#include "play.h"
#include "mic.h"
#include "threads.h"
#include "wav.h"
#include <stdlib.h>
#include <string.h>

#define PLAYBACK_CHANNEL 0
#define MONITOR_CHANNEL 1

#define PLAYBACK_BUFFER_SAMPLES 4096
#define PLAYBACK_BUFFER_COUNT 4
// Monitor buffers are small to keep the delay between mic and headphones short.
#define MONITOR_BUFFER_SAMPLES 192
#define MONITOR_BUFFER_COUNT 8
// Monitor buffers kept queued: more adds delay, fewer risk dropouts.
#define MONITOR_QUEUE_DEPTH 3

// Every 4th sample is enough for the level meter.
#define PEAK_SCAN_STEP 4

// ndspChnSetMix takes 12 volumes; the first two are front left and front right.
#define MIX_VOLUME_COUNT 12
#define MIX_FRONT_LEFT 0
#define MIX_FRONT_RIGHT 1

// Monitoring needs short passes to keep its small queue filled.
#define MONITOR_PASS_NS (2 * NANOSECONDS_PER_MS)
#define PLAYBACK_PASS_NS (10 * NANOSECONDS_PER_MS)

#define PLAYER_STACK_SIZE 0x4000
#define PLAYER_PRIORITY_OFFSET (-2) // runs before the main thread

#define NO_FRAME (~0ull)

static bool g_dspReady;
static Thread g_playerThread;
static _Atomic bool g_playerRunning;
static LightLock g_playerLock;

static WavReader g_reader;
static bool g_loaded;
static bool g_endOfFile;
static _Atomic bool g_playing;
static s16 *g_playbackSamples;
static ndspWaveBuf g_playbackBuffers[PLAYBACK_BUFFER_COUNT];
static u64 g_bufferStartFrame[PLAYBACK_BUFFER_COUNT]; // file frame of each buffer's first sample
static u64 g_nextQueueFrame; // next file frame to queue
static u64 g_heldFrame; // the position while nothing is playing
static u64 g_shownFrame; // play_position's last answer (main thread only)
static _Atomic float g_peakLevel;

static _Atomic bool g_monitorRequested;
static bool g_monitorOn;
static s16 *g_monitorSamples;
static ndspWaveBuf g_monitorBuffers[MONITOR_BUFFER_COUNT];
static u64 g_monitorNextSample; // next mic sample to queue

bool play_available(void)
{
	return g_dspReady;
}

bool play_loaded(void)
{
	return g_loaded;
}

bool play_is_playing(void)
{
	return g_playing;
}

u64 play_length(void)
{
	return g_loaded ? g_reader.frames : 0;
}

u32 play_sample_rate(void)
{
	return g_loaded ? g_reader.rate : MIC_RATE_HZ;
}

float play_level(void)
{
	return g_playing ? g_peakLevel : 0;
}

void play_set_monitor(bool on)
{
	g_monitorRequested = on;
}

int play_cues(u32 *out, int max)
{
	if (!g_loaded) {
		return 0;
	}
	int count = g_reader.cueCount < max ? g_reader.cueCount : max;
	memcpy(out, g_reader.cues, count * sizeof(u32));
	return count;
}

static void setup_channel(int channel, float rate)
{
	ndspChnReset(channel);
	ndspChnSetFormat(channel, NDSP_FORMAT_MONO_PCM16);
	// The DSP runs at the mic's rate, so takes recorded here play as they are.
	bool isMicRate = rate == MIC_RATE_HZ;
	ndspChnSetInterp(channel, isMicRate ? NDSP_INTERP_NONE : NDSP_INTERP_POLYPHASE);
	ndspChnSetRate(channel, isMicRate ? NDSP_SAMPLE_RATE : rate);
	float mix[MIX_VOLUME_COUNT] = {0};
	mix[MIX_FRONT_RIGHT] = 1.0f;
	mix[MIX_FRONT_LEFT] = 1.0f;
	ndspChnSetMix(channel, mix);
}

static bool buffer_in_use(const ndspWaveBuf *buffer)
{
	return buffer->status == NDSP_WBUF_QUEUED || buffer->status == NDSP_WBUF_PLAYING;
}

static void queue_buffer(int channel, ndspWaveBuf *buffer, s16 *samples, int count)
{
	memset(buffer, 0, sizeof *buffer);
	buffer->data_pcm16 = samples;
	buffer->nsamples = count;
	DSP_FlushDataCache(samples, count * sizeof(s16));
	ndspChnWaveBufAdd(channel, buffer);
}

// The frame being heard: the playing buffer's first frame plus the DSP's position in it. Lock held.
static u64 position_locked(void)
{
	if (!g_playing) {
		return g_heldFrame;
	}
	// The DSP's buffer is found by its sequence id. Right after a start or a seek the DSP still describes
	// the old queue; then the answer is the start of the first queued buffer.
	u16 playingSequence = ndspChnGetWaveBufSeq(PLAYBACK_CHANNEL);
	u64 firstQueued = NO_FRAME;
	for (int i = 0; i < PLAYBACK_BUFFER_COUNT; i++) {
		if (!buffer_in_use(&g_playbackBuffers[i])) {
			continue;
		}
		if (playingSequence && g_playbackBuffers[i].sequence_id == playingSequence) {
			return g_bufferStartFrame[i] + ndspChnGetSamplePos(PLAYBACK_CHANNEL);
		}
		if (g_bufferStartFrame[i] < firstQueued) {
			firstQueued = g_bufferStartFrame[i];
		}
	}
	if (firstQueued != NO_FRAME) {
		return firstQueued;
	}
	return g_nextQueueFrame < g_reader.frames ? g_nextQueueFrame : g_reader.frames;
}

// The player thread holds the lock while it reads the card: then the UI gets the last answer, not a wait.
u64 play_position(void)
{
	if (!g_loaded) {
		return 0;
	}
	if (LightLock_TryLock(&g_playerLock) == 0) {
		u64 position = position_locked();
		g_shownFrame = position > g_reader.frames ? g_reader.frames : position;
		LightLock_Unlock(&g_playerLock);
	}
	return g_shownFrame;
}

static void stop_locked(void)
{
	g_playing = false;
	if (!g_dspReady) {
		return;
	}
	ndspChnWaveBufClear(PLAYBACK_CHANNEL);
	for (int i = 0; i < PLAYBACK_BUFFER_COUNT; i++) {
		g_playbackBuffers[i].status = NDSP_WBUF_FREE;
	}
}

static void set_position_locked(u64 frame)
{
	g_shownFrame = frame;
	g_nextQueueFrame = frame;
	g_heldFrame = frame;
}

static void seek_locked(u64 frame)
{
	set_position_locked(frame);
	wav_seek(&g_reader, frame);
	g_endOfFile = false;
}

void play_seek(u64 frame)
{
	if (!g_loaded) {
		return;
	}
	LightLock_Lock(&g_playerLock);
	bool wasPlaying = g_playing;
	stop_locked();
	if (frame > g_reader.frames) {
		frame = g_reader.frames;
	}
	seek_locked(frame);
	g_playing = wasPlaying && frame < g_reader.frames;
	LightLock_Unlock(&g_playerLock);
}

void play_set_playing(bool playing)
{
	if (!g_loaded || !g_dspReady) {
		return;
	}
	LightLock_Lock(&g_playerLock);
	if (playing && !g_playing) {
		// Playing from the end starts again from the beginning.
		u64 startFrame = g_heldFrame >= g_reader.frames ? 0 : g_heldFrame;
		stop_locked();
		seek_locked(startFrame);
		g_playing = true;
	} else if (!playing && g_playing) {
		g_heldFrame = position_locked();
		stop_locked();
	}
	LightLock_Unlock(&g_playerLock);
}

bool play_open(const char *path)
{
	play_close();
	LightLock_Lock(&g_playerLock);
	g_loaded = wav_open(&g_reader, path);
	if (g_loaded) {
		if (g_dspReady) {
			setup_channel(PLAYBACK_CHANNEL, (float)g_reader.rate);
		}
		set_position_locked(0);
		g_endOfFile = false;
	}
	LightLock_Unlock(&g_playerLock);
	return g_loaded;
}

void play_close(void)
{
	LightLock_Lock(&g_playerLock);
	stop_locked();
	if (g_loaded) {
		wav_close_reader(&g_reader);
	}
	g_loaded = false;
	LightLock_Unlock(&g_playerLock);
}

static float peak_level(const s16 *samples, int count)
{
	int peak = 0;
	for (int i = 0; i < count; i += PEAK_SCAN_STEP) {
		int magnitude = abs(samples[i]);
		if (magnitude > peak) {
			peak = magnitude;
		}
	}
	return peak / MIC_FULL_SCALE;
}

// Fills every free playback buffer from the file and stops once all of it has played. Lock held.
static void queue_playback_locked(void)
{
	bool anyQueued = false;
	for (int i = 0; i < PLAYBACK_BUFFER_COUNT; i++) {
		ndspWaveBuf *buffer = &g_playbackBuffers[i];
		if (buffer_in_use(buffer)) {
			anyQueued = true;
			continue;
		}
		if (g_endOfFile) {
			continue;
		}
		s16 *samples = g_playbackSamples + i * PLAYBACK_BUFFER_SAMPLES;
		int count = wav_read_s16(&g_reader, samples, PLAYBACK_BUFFER_SAMPLES);
		if (count <= 0) {
			g_endOfFile = true;
			continue;
		}
		g_peakLevel = peak_level(samples, count);
		g_bufferStartFrame[i] = g_nextQueueFrame;
		g_nextQueueFrame += count;
		queue_buffer(PLAYBACK_CHANNEL, buffer, samples, count);
		anyQueued = true;
	}
	if (!anyQueued && g_endOfFile) {
		g_heldFrame = g_reader.frames;
		stop_locked();
	}
}

static void feed_playback(void)
{
	LightLock_Lock(&g_playerLock);
	if (g_playing && g_loaded) {
		queue_playback_locked();
	}
	LightLock_Unlock(&g_playerLock);
}

// Monitoring follows the request, but only on headphones: through the speakers it would howl.
static void switch_monitor(void)
{
	bool wanted = g_monitorRequested && osIsHeadsetConnected();
	if (wanted == g_monitorOn) {
		return;
	}
	g_monitorOn = wanted;
	ndspChnWaveBufClear(MONITOR_CHANNEL);
	for (int i = 0; i < MONITOR_BUFFER_COUNT; i++) {
		g_monitorBuffers[i].status = NDSP_WBUF_FREE;
	}
	if (wanted) {
		setup_channel(MONITOR_CHANNEL, MIC_RATE_HZ);
		u64 total = mic_total_samples();
		g_monitorNextSample = total > MONITOR_BUFFER_SAMPLES ? total - MONITOR_BUFFER_SAMPLES : 0;
	}
}

static void feed_monitor(void)
{
	switch_monitor();
	if (!g_monitorOn) {
		return;
	}
	int queued = 0;
	for (int i = 0; i < MONITOR_BUFFER_COUNT; i++) {
		if (buffer_in_use(&g_monitorBuffers[i])) {
			queued++;
		}
	}
	u64 total = mic_total_samples();
	u64 newestBufferStart = total - MONITOR_BUFFER_SAMPLES;
	// Starved or fallen behind: catch up with the mic.
	if (queued == 0 && total > g_monitorNextSample + MONITOR_BUFFER_SAMPLES * MONITOR_QUEUE_DEPTH) {
		g_monitorNextSample = newestBufferStart;
	}
	for (int i = 0; i < MONITOR_BUFFER_COUNT; i++) {
		bool queueFull = queued >= MONITOR_QUEUE_DEPTH;
		bool waitingForMic = g_monitorNextSample + MONITOR_BUFFER_SAMPLES > total;
		if (queueFull || waitingForMic) {
			break;
		}
		ndspWaveBuf *buffer = &g_monitorBuffers[i];
		if (buffer_in_use(buffer)) {
			continue;
		}
		s16 *samples = g_monitorSamples + i * MONITOR_BUFFER_SAMPLES;
		if (mic_copy(g_monitorNextSample, samples, MONITOR_BUFFER_SAMPLES) != MONITOR_BUFFER_SAMPLES) {
			g_monitorNextSample = newestBufferStart;
			break;
		}
		g_monitorNextSample += MONITOR_BUFFER_SAMPLES;
		queue_buffer(MONITOR_CHANNEL, buffer, samples, MONITOR_BUFFER_SAMPLES);
		queued++;
	}
}

static void player_loop(void *arg)
{
	(void)arg;
	while (g_playerRunning) {
		feed_monitor();
		feed_playback();
		s64 passNs = g_monitorOn ? MONITOR_PASS_NS : PLAYBACK_PASS_NS;
		svcSleepThread(passNs);
	}
}

bool play_init(void)
{
	LightLock_Init(&g_playerLock);
	if (R_FAILED(ndspInit())) {
		return false; // the DSP firmware sdmc:/3ds/dspfirm.cdc is missing
	}
	g_playbackSamples = linearAlloc(PLAYBACK_BUFFER_COUNT * PLAYBACK_BUFFER_SAMPLES * sizeof(s16));
	g_monitorSamples = linearAlloc(MONITOR_BUFFER_COUNT * MONITOR_BUFFER_SAMPLES * sizeof(s16));
	if (!g_playbackSamples || !g_monitorSamples) {
		ndspExit();
		return false;
	}
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	setup_channel(PLAYBACK_CHANNEL, MIC_RATE_HZ);
	setup_channel(MONITOR_CHANNEL, MIC_RATE_HZ);
	g_dspReady = true;
	g_playerRunning = true;
	s32 priority = threads_priority_from_main(PLAYER_PRIORITY_OFFSET);
	g_playerThread = threadCreate(player_loop, NULL, PLAYER_STACK_SIZE, priority, THREAD_CORE_DEFAULT, false);
	return true;
}

void play_exit(void)
{
	g_playerRunning = false;
	if (g_playerThread) {
		threadJoin(g_playerThread, U64_MAX);
		threadFree(g_playerThread);
		g_playerThread = NULL;
	}
	play_close();
	if (g_dspReady) {
		ndspChnWaveBufClear(MONITOR_CHANNEL);
		ndspExit();
	}
	if (g_playbackSamples) {
		linearFree(g_playbackSamples);
	}
	if (g_monitorSamples) {
		linearFree(g_monitorSamples);
	}
	g_monitorSamples = NULL;
	g_playbackSamples = NULL;
	g_dspReady = false;
}
