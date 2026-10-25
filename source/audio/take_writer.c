#include "take_writer.h"
#include "mic.h"
#include "threads.h"
#include "wav.h"
#include <stdlib.h>
#include <string.h>

#define WRITE_CHUNK_SAMPLES 8192
#define NO_SAMPLE (~0ull)

#define WRITER_PASS_NS (20 * NANOSECONDS_PER_MS)
// Sizes go into the header this often, so a crash loses only the last moments.
#define HEADER_PATCH_INTERVAL_MS 2000

#define WRITER_STACK_SIZE 0x4000
#define WRITER_PRIORITY_OFFSET (-3) // runs before the main thread

static WavWriter g_writer;
static Thread g_writerThread;
static LightLock g_stateLock; // guards the state below that both threads touch
static bool g_stateLockReady;
static _Atomic bool g_active;
static _Atomic bool g_failed;
static u64 g_nextSample; // next mic sample to write
// A stretch is the audio between a start or resume and the next pause or stop.
static u64 g_stretchStart; // mic sample where the current stretch began
static u64 g_stretchFileStart; // where the current stretch begins in the file
static u64 g_pauseAt = NO_SAMPLE;
static u64 g_stopAt = NO_SAMPLE;
// The writer makes a resume's jump itself once it has written up to the pause. Moving g_nextSample from the
// main thread would race with a writer held up by the card.
static u64 g_jumpFrom = NO_SAMPLE;
static u64 g_jumpTo;
static _Atomic u32 g_lostSamples;
static u32 g_markers[TAKE_WRITER_MAX_MARKERS];
static u32 g_events[TAKE_WRITER_MAX_EVENTS];
static int g_markerCount;
static int g_eventCount;

static void lock_state(void)
{
	LightLock_Lock(&g_stateLock);
}

static void unlock_state(void)
{
	LightLock_Unlock(&g_stateLock);
}

static u64 earlier(u64 a, u64 b)
{
	return a < b ? a : b;
}

static u64 later(u64 a, u64 b)
{
	return a > b ? a : b;
}

static void append_position(u32 *list, int *count, int capacity, u32 position)
{
	if (*count >= capacity) {
		return;
	}
	list[*count] = position;
	*count += 1;
}

// When the writer falls a whole ring behind, silence keeps the timing and the markers right.
static u64 write_silence_for_lost(s16 *chunk, u64 position, u64 limit)
{
	u64 oldest = mic_oldest_sample();
	if (position >= oldest || position >= limit) {
		return position;
	}
	u64 gap = earlier(oldest, limit) - position;
	memset(chunk, 0, WRITE_CHUNK_SAMPLES * sizeof(s16));
	for (u64 written = 0; written < gap && !g_failed; written += WRITE_CHUNK_SAMPLES) {
		int count = (int)earlier(gap - written, WRITE_CHUNK_SAMPLES);
		if (!wav_write_s16(&g_writer, chunk, count)) {
			g_failed = true;
		}
	}
	g_lostSamples += (u32)gap;
	return position + gap;
}

static u64 write_captured(s16 *chunk, u64 position, u64 limit)
{
	while (position < limit && !g_failed) {
		int count = (int)earlier(limit - position, WRITE_CHUNK_SAMPLES);
		int copied = mic_copy(position, chunk, count);
		if (copied <= 0) {
			break;
		}
		if (!wav_write_s16(&g_writer, chunk, copied)) {
			g_failed = true;
		}
		position += copied;
	}
	return position;
}

static void writer_loop(void *arg)
{
	(void)arg;
	s16 *chunk = malloc(WRITE_CHUNK_SAMPLES * sizeof(s16));
	if (!chunk) {
		g_failed = true;
	}
	u64 lastPatch = osGetTime();
	u64 patchedFrames = 0;
	while (chunk) {
		lock_state();
		u64 position = g_nextSample;
		// Write up to the pause, the stop or a pending jump, whichever comes first.
		u64 end = earlier(earlier(g_pauseAt, g_stopAt), g_jumpFrom);
		bool stopping = g_stopAt != NO_SAMPLE;
		unlock_state();
		u64 limit = earlier(mic_total_samples(), end);
		position = write_silence_for_lost(chunk, position, limit);
		position = write_captured(chunk, position, limit);

		lock_state();
		// The stretch before the pause is complete: go on with the resumed one.
		if (g_jumpFrom != NO_SAMPLE && position >= g_jumpFrom) {
			position = g_jumpTo;
			g_jumpFrom = NO_SAMPLE;
		}
		g_nextSample = position;
		bool done = stopping && g_jumpFrom == NO_SAMPLE && position >= g_stopAt;
		unlock_state();
		if (done || g_failed) {
			break;
		}

		u64 now = osGetTime();
		if (now - lastPatch > HEADER_PATCH_INTERVAL_MS && g_writer.frames != patchedFrames) {
			lastPatch = now;
			patchedFrames = g_writer.frames;
			if (!wav_patch(&g_writer)) {
				g_failed = true;
			}
		}
		svcSleepThread(WRITER_PASS_NS);
	}
	free(chunk);
}

bool take_writer_start(const char *path, u64 fromSample)
{
	if (g_active) {
		return false;
	}
	if (!g_stateLockReady) {
		LightLock_Init(&g_stateLock);
		g_stateLockReady = true;
	}
	if (!wav_create(&g_writer, path, WAV_S16, MIC_RATE_HZ)) {
		return false;
	}
	u64 oldest = mic_oldest_sample();
	if (fromSample < oldest) {
		fromSample = oldest;
	}
	g_stretchStart = fromSample;
	g_nextSample = fromSample;
	g_stretchFileStart = 0;
	g_jumpFrom = NO_SAMPLE;
	g_stopAt = NO_SAMPLE;
	g_pauseAt = NO_SAMPLE;
	g_lostSamples = 0;
	g_failed = false;
	g_eventCount = 0;
	g_markerCount = 0;
	s32 priority = threads_priority_from_main(WRITER_PRIORITY_OFFSET);
	g_writerThread = threadCreate(writer_loop, NULL, WRITER_STACK_SIZE, priority, THREAD_CORE_DEFAULT, false);
	if (!g_writerThread) {
		wav_close_writer(&g_writer, NULL, 0);
		return false;
	}
	g_active = true;
	return true;
}

void take_writer_pause(u64 atSample)
{
	if (!g_active) {
		return;
	}
	lock_state();
	if (g_pauseAt == NO_SAMPLE && g_stopAt == NO_SAMPLE) {
		g_pauseAt = later(atSample, g_stretchStart);
	}
	unlock_state();
}

bool take_writer_resume(u64 atSample)
{
	if (!g_active) {
		return false;
	}
	lock_state();
	// A second resume waits until the writer has made the first jump.
	bool canResume = g_pauseAt != NO_SAMPLE && g_stopAt == NO_SAMPLE && g_jumpFrom == NO_SAMPLE;
	if (canResume) {
		u64 pauseInFile = g_stretchFileStart + (g_pauseAt - g_stretchStart);
		u64 resumeAt = later(atSample, mic_oldest_sample());
		resumeAt = later(resumeAt, g_pauseAt);
		g_jumpFrom = g_pauseAt;
		g_jumpTo = resumeAt;
		g_stretchFileStart = pauseInFile;
		g_stretchStart = resumeAt;
		g_pauseAt = NO_SAMPLE;
		append_position(g_markers, &g_markerCount, TAKE_WRITER_MAX_MARKERS, (u32)pauseInFile);
	}
	unlock_state();
	return canResume;
}

bool take_writer_stop(u64 atSample, u64 *frames)
{
	*frames = 0;
	if (!g_active) {
		return false;
	}
	lock_state();
	if (g_stopAt == NO_SAMPLE) {
		// A paused recording ends at its pause.
		g_stopAt = g_pauseAt != NO_SAMPLE ? g_pauseAt : later(atSample, g_stretchStart);
	}
	unlock_state();
	// The writer is at most one pass behind.
	threadJoin(g_writerThread, U64_MAX);
	threadFree(g_writerThread);
	g_writerThread = NULL;
	*frames = take_writer_frames();
	lock_state();
	int markerCount = g_markerCount;
	unlock_state();
	bool ok = wav_close_writer(&g_writer, g_markers, markerCount) && !g_failed;
	g_active = false;
	return ok;
}

bool take_writer_is_active(void)
{
	return g_active;
}

bool take_writer_is_paused(void)
{
	if (!g_active) {
		return false;
	}
	lock_state();
	bool paused = g_pauseAt != NO_SAMPLE;
	unlock_state();
	return paused;
}

u32 take_writer_lost_samples(void)
{
	return g_lostSamples;
}

bool take_writer_failed(void)
{
	return g_failed;
}

u64 take_writer_frames(void)
{
	if (!g_active) {
		return 0;
	}
	lock_state();
	u64 end;
	if (g_pauseAt != NO_SAMPLE) {
		end = g_pauseAt;
	} else if (g_stopAt != NO_SAMPLE) {
		end = g_stopAt;
	} else {
		end = mic_total_samples();
	}
	u64 frames = g_stretchFileStart + (end > g_stretchStart ? end - g_stretchStart : 0);
	unlock_state();
	return frames;
}

u32 take_writer_backlog(void)
{
	if (!g_active) {
		return 0;
	}
	lock_state();
	u64 position = g_nextSample;
	u64 end = earlier(g_pauseAt, g_stopAt);
	u64 jumpFrom = g_jumpFrom;
	unlock_state();
	u64 captured = earlier(end, mic_total_samples());
	// Still finishing the stretch before the pause.
	if (jumpFrom != NO_SAMPLE && position < jumpFrom) {
		return (u32)(jumpFrom - position);
	}
	return captured > position ? (u32)(captured - position) : 0;
}

// Where a mic sample lands in the file, -1 outside the recorded stretches.
static s64 sample_file_position(u64 sample)
{
	if (!g_active) {
		return -1;
	}
	lock_state();
	s64 filePosition = -1;
	u64 stretchEnd = g_pauseAt != NO_SAMPLE ? g_pauseAt : g_stopAt;
	if (sample >= g_stretchStart && sample <= stretchEnd) {
		filePosition = (s64)(g_stretchFileStart + (sample - g_stretchStart));
	}
	unlock_state();
	return filePosition;
}

static void add_sample_position(u32 *list, int *count, int capacity, u64 sample)
{
	s64 filePosition = sample_file_position(sample);
	if (filePosition < 0) {
		return;
	}
	lock_state();
	append_position(list, count, capacity, (u32)filePosition);
	unlock_state();
}

void take_writer_add_marker(u64 sample)
{
	add_sample_position(g_markers, &g_markerCount, TAKE_WRITER_MAX_MARKERS, sample);
}

void take_writer_add_event(u64 sample)
{
	add_sample_position(g_events, &g_eventCount, TAKE_WRITER_MAX_EVENTS, sample);
}

static int copy_positions(const u32 *list, const int *count, u32 *out, int max)
{
	lock_state();
	int copied = *count < max ? *count : max;
	memcpy(out, list, copied * sizeof(u32));
	unlock_state();
	return copied;
}

int take_writer_markers(u32 *out, int max)
{
	return copy_positions(g_markers, &g_markerCount, out, max);
}

int take_writer_events(u32 *out, int max)
{
	return copy_positions(g_events, &g_eventCount, out, max);
}
