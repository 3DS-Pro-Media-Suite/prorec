#include "develop_queue.h"
#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "develop.h"
#include "noise_profile.h"
#include "overview.h"
#include "profile_store.h"
#include "takes.h"
#include "threads.h"

#define DEVELOP_QUEUE_SIZE 16
#define DEVELOP_STACK_SIZE (128 * 1024)
#define DEVELOP_IDLE_SLEEP_NS (50 * NANOSECONDS_PER_MS) // the worker looks for a new job this often
#define DEVELOP_CORE_NEW_3DS 2
#define DEVELOP_CORE_OLD_3DS 1
#define DEVELOP_PRIORITY_OFFSET 2 // runs after the main thread: the worker yields to it
#define RESULT_MESSAGE_MAX 90 // leaves room for "; NR .. dB; .. s" in the take's result

typedef struct {
	int id;
	DevelopParams params;
} DevelopJob;

typedef struct {
	DevelopJob jobs[DEVELOP_QUEUE_SIZE];
	int count;
	LightLock lock;
	Thread thread;
	_Atomic bool running;
	_Atomic int currentId; // take being developed, 0 = none
	DevelopState state;
	_Atomic int doneId; // set when a job ends (the main loop picks it up)
	bool doneOk;
	char doneMessage[DEVELOP_MESSAGE_SIZE];
	TakeMeta jobMeta; // the worker's copy of the current take's meta
} DevelopQueue;

static DevelopQueue g_developQueue;

bool develop_queue_active(void)
{
	LightLock_Lock(&g_developQueue.lock);
	bool active = g_developQueue.currentId != 0 || g_developQueue.count > 0;
	LightLock_Unlock(&g_developQueue.lock);
	return active;
}

// Under the lock: pop_job moves a job from the queue to current in one step.
bool develop_queue_has(int id)
{
	LightLock_Lock(&g_developQueue.lock);
	bool queued = g_developQueue.currentId == id;
	for (int i = 0; i < g_developQueue.count; i++) {
		queued |= g_developQueue.jobs[i].id == id;
	}
	LightLock_Unlock(&g_developQueue.lock);
	return queued;
}

// Takes the first job off the queue and makes it the current one. False when there is none.
static bool pop_job(DevelopJob *job)
{
	LightLock_Lock(&g_developQueue.lock);
	bool found = g_developQueue.count != 0 && g_developQueue.running;
	if (found) {
		*job = g_developQueue.jobs[0];
		memmove(g_developQueue.jobs, g_developQueue.jobs + 1,
		        (g_developQueue.count - 1) * sizeof(DevelopJob));
		g_developQueue.count--;
		g_developQueue.currentId = job->id;
	}
	LightLock_Unlock(&g_developQueue.lock);
	return found;
}

// The new clean file replaces the old one only once it is complete.
static bool replace_clean_file(const char *partialPath, const char *cleanPath)
{
	remove(cleanPath);
	bool renamed = rename(partialPath, cleanPath) == 0;
	// The cached overview belongs to the old clean file.
	char overviewPath[OVERVIEW_PATH_SIZE];
	overview_cache_path(cleanPath, overviewPath, sizeof overviewPath);
	remove(overviewPath);
	if (!renamed) {
		snprintf(g_developQueue.state.message, sizeof g_developQueue.state.message, "Cannot rename %s",
		         partialPath);
	}
	return renamed;
}

static bool develop_job(const DevelopJob *job)
{
	char rawPath[APP_PATH_SIZE];
	char partialPath[APP_PATH_SIZE];
	char cleanPath[APP_PATH_SIZE];
	takes_path(job->id, TAKE_RAW, rawPath, sizeof rawPath);
	takes_path(job->id, TAKE_PARTIAL, partialPath, sizeof partialPath);
	takes_path(job->id, TAKE_CLEAN, cleanPath, sizeof cleanPath);
	TakeMeta *meta = &g_developQueue.jobMeta;
	bool haveMeta = takes_read_meta(job->id, meta);
	NoiseProfile profileCopy;
	const NoiseProfile *profile = haveMeta ? profile_store_for_gain(meta->gain, &profileCopy) : NULL;
	// Not a memset: that could clear the cancel flag set by an exit meanwhile.
	g_developQueue.state.progress = 0;
	g_developQueue.state.message[0] = 0;
	u64 startMs = osGetTime();
	int eventCount = haveMeta ? meta->eventCount : 0;
	bool ok = develop_run(rawPath, partialPath, DEVELOP_SCRATCH_PATH, &job->params, profile, meta->events,
	                      eventCount, &g_developQueue.state);
	if (ok) {
		ok = replace_clean_file(partialPath, cleanPath);
	}
	double seconds = (double)(osGetTime() - startMs) / 1000;
	if (haveMeta && ok) {
		snprintf(meta->result, sizeof meta->result, "%.*s; NR %d dB; %.1f s", RESULT_MESSAGE_MAX,
		         g_developQueue.state.message, job->params.noiseReductionDb, seconds);
		meta->developParams = job->params;
		meta->haveDevelopParams = true;
		takes_write_meta(job->id, meta);
	}
	return ok;
}

static void finish_job(int id, bool ok)
{
	LightLock_Lock(&g_developQueue.lock);
	snprintf(g_developQueue.doneMessage, sizeof g_developQueue.doneMessage, "%s",
	         g_developQueue.state.message);
	g_developQueue.doneOk = ok;
	g_developQueue.doneId = id;
	g_developQueue.currentId = 0;
	LightLock_Unlock(&g_developQueue.lock);
}

static void develop_worker(void *unused)
{
	(void)unused;
	while (g_developQueue.running) {
		DevelopJob job;
		if (!pop_job(&job)) {
			if (g_developQueue.running) {
				svcSleepThread(DEVELOP_IDLE_SLEEP_NS);
			}
			continue;
		}
		bool ok = develop_job(&job);
		finish_job(job.id, ok);
	}
}

void develop_queue_init(void)
{
	LightLock_Init(&g_developQueue.lock);
	g_developQueue.running = true;
	s32 workerPriority = threads_priority_from_main(DEVELOP_PRIORITY_OFFSET);
	int core = g_isNew3ds ? DEVELOP_CORE_NEW_3DS : DEVELOP_CORE_OLD_3DS;
	g_developQueue.thread =
	        threadCreate(develop_worker, NULL, DEVELOP_STACK_SIZE, workerPriority, core, false);
	if (!g_developQueue.thread) {
		g_developQueue.thread = threadCreate(develop_worker, NULL, DEVELOP_STACK_SIZE, workerPriority,
		                                     THREAD_CORE_DEFAULT, false);
	}
}

void develop_queue_exit(void)
{
	g_developQueue.running = false; // first: a job popped from now on starts cancelled
	g_developQueue.state.cancel = true;
	if (g_developQueue.thread) {
		threadJoin(g_developQueue.thread, U64_MAX);
		threadFree(g_developQueue.thread);
	}
}

bool develop_queue_add(int id, const DevelopParams *params)
{
	if (develop_queue_has(id)) {
		return false;
	}
	LightLock_Lock(&g_developQueue.lock);
	bool hasRoom = g_developQueue.count < DEVELOP_QUEUE_SIZE;
	if (hasRoom) {
		g_developQueue.jobs[g_developQueue.count] = (DevelopJob){.id = id, .params = *params};
		g_developQueue.count++;
	}
	LightLock_Unlock(&g_developQueue.lock);
	return hasRoom;
}

int develop_queue_current(void)
{
	return g_developQueue.currentId;
}

int develop_queue_progress(void)
{
	return g_developQueue.state.progress;
}

// The message is copied under the lock: the worker may finish the next job meanwhile.
bool develop_queue_take_done(int *id, bool *ok, char *message, size_t messageSize)
{
	if (!g_developQueue.doneId) {
		return false;
	}
	LightLock_Lock(&g_developQueue.lock);
	*id = g_developQueue.doneId;
	*ok = g_developQueue.doneOk;
	snprintf(message, messageSize, "%s", g_developQueue.doneMessage);
	g_developQueue.doneId = 0;
	LightLock_Unlock(&g_developQueue.lock);
	return true;
}
