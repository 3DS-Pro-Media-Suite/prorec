#include "mic.h"
#include "threads.h"
#include <malloc.h>
#include <stdatomic.h>
#include <string.h>

// mic:u's shared buffer holds about 3 s of samples, so the capture thread may be held up for a while before
// anything is lost. Its last 4 bytes hold the offset mic:u writes at.
#define SHARED_BUFFER_SIZE 0x30000
#define SHARED_BUFFER_ALIGN 0x1000 // mic:u maps the buffer in whole pages

// Samples kept (32 s): room for pre-record and for a slow card.
#define MIC_RING_SAMPLES (1u << 20)
// Readers keep this far away from the part of the ring the capture thread overwrites next.
#define RING_OVERWRITE_MARGIN 16384
#define RING_READABLE_SAMPLES (MIC_RING_SAMPLES - RING_OVERWRITE_MARGIN)

#define GAIN_STEP_DB 0.5f // the codec's mic amplifier works in half-decibel steps
#define NO_GAIN_REQUEST (-1)

#define CAPTURE_PASS_MS 4
// A longer pause between passes may have let mic:u lap the reader in its 3 s buffer.
#define LATE_PASS_MS 2500
#define SAMPLING_CHECK_MS 1000

#define CAPTURE_STACK_SIZE 0x4000
#define CAPTURE_PRIORITY_OFFSET (-6) // runs before every other thread of the app

#define ERROR_OUT_OF_MEMORY (-1)
#define ERROR_THREAD_CREATE (-2)

static u8 *g_sharedBuffer;
static s16 *g_sampleRing;
static Thread g_captureThread;
static _Atomic bool g_captureRunning;
// A 64-bit store is not atomic on the ARM11, so g_totalSamples needs a lock.
static LightLock g_totalLock;
static u64 g_totalSamples;
static Result g_error;
static const char *g_errorWhere = "";
static _Atomic int g_requestedGain = NO_GAIN_REQUEST;
static _Atomic bool g_holdRequested;
static _Atomic u8 g_currentGain;
static _Atomic u32 g_gapCount;
static _Atomic u32 g_restartCount;

Result mic_error(void)
{
	return g_error;
}

const char *mic_error_where(void)
{
	return g_errorWhere;
}

u8 mic_gain(void)
{
	return g_currentGain;
}

u32 mic_gaps(void)
{
	return g_gapCount;
}

u32 mic_restarts(void)
{
	return g_restartCount;
}

static u8 clamp_gain(u8 gain)
{
	return gain > MIC_GAIN_MAX ? MIC_GAIN_MAX : gain;
}

void mic_set_gain(u8 gain)
{
	g_requestedGain = clamp_gain(gain);
}

void mic_hold(bool hold)
{
	g_holdRequested = hold;
}

float mic_gain_db(u8 gain)
{
	return gain * GAIN_STEP_DB;
}

u64 mic_total_samples(void)
{
	LightLock_Lock(&g_totalLock);
	u64 total = g_totalSamples;
	LightLock_Unlock(&g_totalLock);
	return total;
}

static u64 oldest_readable_sample(u64 total)
{
	return total > RING_READABLE_SAMPLES ? total - RING_READABLE_SAMPLES : 0;
}

u64 mic_oldest_sample(void)
{
	return oldest_readable_sample(mic_total_samples());
}

int mic_copy(u64 from, s16 *dst, int n)
{
	u64 total = mic_total_samples();
	u64 oldest = oldest_readable_sample(total);
	if (from < oldest || from >= total || n <= 0) {
		return 0;
	}
	if ((u64)n > total - from) {
		n = (int)(total - from);
	}
	u32 ringIndex = (u32)(from % MIC_RING_SAMPLES);
	u32 untilRingEnd = MIC_RING_SAMPLES - ringIndex;
	if ((u32)n <= untilRingEnd) {
		memcpy(dst, g_sampleRing + ringIndex, n * sizeof(s16));
	} else {
		memcpy(dst, g_sampleRing + ringIndex, untilRingEnd * sizeof(s16));
		memcpy(dst + untilRingEnd, g_sampleRing, (n - untilRingEnd) * sizeof(s16));
	}
	return n;
}

static void set_error(Result error, const char *where)
{
	g_error = error;
	g_errorWhere = where;
}

static bool start_sampling(void)
{
	Result rc = MICU_StartSampling(MICU_ENCODING_PCM16_SIGNED, MICU_SAMPLE_RATE_32730, 0,
	                               micGetSampleDataSize(), true);
	if (R_FAILED(rc)) {
		set_error(rc, "MICU_StartSampling");
		return false;
	}
	return true;
}

static bool sampling_stopped(void)
{
	bool sampling = true;
	return R_SUCCEEDED(MICU_IsSampling(&sampling)) && !sampling;
}

static void read_back_gain(void)
{
	u8 gain = 0;
	if (R_SUCCEEDED(MICU_GetGain(&gain))) {
		g_currentGain = gain;
	}
}

static void apply_requested_gain(void)
{
	// One step, so a request made meanwhile is not cleared unseen.
	int requested = atomic_exchange(&g_requestedGain, NO_GAIN_REQUEST);
	if (requested < 0) {
		return;
	}
	MICU_SetGain((u8)requested);
	read_back_gain();
}

// How far mic:u has written into the shared buffer, rounded down to a whole sample.
static u32 shared_write_offset(void)
{
	return micGetLastSampleOffset() & ~1u;
}

// Copies what mic:u wrote since readOffset into the ring and returns the new read offset.
static u32 copy_new_samples(u32 readOffset, u32 bufferBytes)
{
	u32 writeOffset = shared_write_offset();
	if (writeOffset >= bufferBytes || writeOffset == readOffset) {
		return readOffset;
	}
	u32 newBytes;
	if (writeOffset > readOffset) {
		newBytes = writeOffset - readOffset;
	} else {
		// mic:u wrapped around to the start of its buffer.
		newBytes = bufferBytes - readOffset + writeOffset;
	}
	u32 newSamples = newBytes / sizeof(s16);
	u64 total = mic_total_samples();
	const u8 *source = g_sharedBuffer;
	u32 copied = 0;
	// Copy in runs that end at the end of the shared buffer or of the ring, whichever comes first.
	while (copied < newSamples) {
		u32 untilBufferEnd = (bufferBytes - readOffset) / sizeof(s16);
		u32 ringIndex = (u32)(total % MIC_RING_SAMPLES);
		u32 untilRingEnd = MIC_RING_SAMPLES - ringIndex;
		u32 run = newSamples - copied;
		if (run > untilBufferEnd) {
			run = untilBufferEnd;
		}
		if (run > untilRingEnd) {
			run = untilRingEnd;
		}
		memcpy(g_sampleRing + ringIndex, source + readOffset, run * sizeof(s16));
		total += run;
		copied += run;
		readOffset += run * sizeof(s16);
		if (readOffset >= bufferBytes) {
			readOffset = 0;
		}
	}
	LightLock_Lock(&g_totalLock);
	g_totalSamples = total;
	LightLock_Unlock(&g_totalLock);
	return readOffset;
}

static void capture_loop(void *arg)
{
	(void)arg;
	// The part of the shared buffer that holds samples, in bytes, rounded down to a whole sample.
	const u32 bufferBytes = micGetSampleDataSize() & ~1u;
	u32 readOffset = shared_write_offset();
	u64 lastPass = osGetTime();
	u64 lastSamplingCheck = lastPass;
	bool held = false;
	while (g_captureRunning) {
		svcSleepThread(CAPTURE_PASS_MS * NANOSECONDS_PER_MS);
		// Only this thread starts and stops sampling, so a hold cannot race the restart below.
		if (g_holdRequested != held) {
			held = g_holdRequested;
			if (held) {
				MICU_StopSampling();
			} else if (start_sampling()) {
				readOffset = shared_write_offset();
			}
			lastPass = osGetTime();
			lastSamplingCheck = lastPass;
			continue;
		}
		if (held) {
			continue;
		}
		apply_requested_gain();
		u64 now = osGetTime();
		if (now - lastPass > LATE_PASS_MS) {
			g_gapCount++;
		}
		lastPass = now;
		// mic:u stops sampling after sleep mode and in other cases: start it again.
		if (now - lastSamplingCheck > SAMPLING_CHECK_MS) {
			lastSamplingCheck = now;
			if (sampling_stopped()) {
				if (start_sampling()) {
					g_restartCount++;
					g_gapCount++;
					readOffset = shared_write_offset();
				}
				continue;
			}
		}
		readOffset = copy_new_samples(readOffset, bufferBytes);
	}
}

static void free_shared_buffer(void)
{
	free(g_sharedBuffer);
	g_sharedBuffer = NULL;
}

bool mic_init(u8 gain)
{
	LightLock_Init(&g_totalLock);
	g_error = 0;
	g_sampleRing = malloc(MIC_RING_SAMPLES * sizeof(s16));
	g_sharedBuffer = memalign(SHARED_BUFFER_ALIGN, SHARED_BUFFER_SIZE);
	if (!g_sampleRing || !g_sharedBuffer) {
		set_error(ERROR_OUT_OF_MEMORY, "out of memory");
		return false;
	}
	memset(g_sampleRing, 0, MIC_RING_SAMPLES * sizeof(s16));
	memset(g_sharedBuffer, 0, SHARED_BUFFER_SIZE);
	// Maps the buffer and powers the mic on.
	Result rc = micInit(g_sharedBuffer, SHARED_BUFFER_SIZE);
	if (R_FAILED(rc)) {
		set_error(rc, "micInit");
		free_shared_buffer();
		return false;
	}
	// A recording continues with the lid closed.
	MICU_SetAllowShellClosed(true);
	MICU_SetGain(clamp_gain(gain));
	read_back_gain();
	if (!start_sampling()) {
		micExit();
		free_shared_buffer();
		return false;
	}
	g_captureRunning = true;
	// Nothing may hold the copy up for long.
	s32 priority = threads_priority_from_main(CAPTURE_PRIORITY_OFFSET);
	g_captureThread =
	        threadCreate(capture_loop, NULL, CAPTURE_STACK_SIZE, priority, THREAD_CORE_DEFAULT, false);
	if (!g_captureThread) {
		set_error(ERROR_THREAD_CREATE, "threadCreate");
		MICU_StopSampling();
		micExit();
		free_shared_buffer();
		return false;
	}
	return true;
}

void mic_exit(void)
{
	g_captureRunning = false;
	if (g_captureThread) {
		threadJoin(g_captureThread, U64_MAX);
		threadFree(g_captureThread);
		g_captureThread = NULL;
	}
	if (g_sharedBuffer) {
		MICU_StopSampling();
		MICU_SetAllowShellClosed(false);
		// Unmaps the buffer, so it may be freed only after this.
		micExit();
		free_shared_buffer();
	}
	free(g_sampleRing);
	g_sampleRing = NULL;
}
