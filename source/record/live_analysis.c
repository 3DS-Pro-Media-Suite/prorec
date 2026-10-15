#include "live_analysis.h"
#include <math.h>
#include <string.h>
#include "decibels.h"
#include "fft.h"
#include "mic.h"
#include "settings.h"
#include "take_writer.h"

#define FRAMES_PER_SECOND 60 // live_analysis_update runs once per frame
#define SILENCE_LEVEL 1e-9f // keeps log10 finite at silence
#define BACKLOG_MAX_SAMPLES ((u64)32768) // after a stall only about the newest second is analysed
#define COPY_CHUNK_SAMPLES 4096
#define CLIP_THRESHOLD 32700 // a sample at least this large counts as clipped
#define RMS_WINDOW_SECONDS 0.3f
#define PEAK_FALL_PER_FRAME 0.9624f // about 20 dB per second at 60 frames per second
#define PEAK_HOLD_MS 2000
#define CLIP_INDICATOR_MS 3000
#define SPECTRUM_FALL_DB_PER_SECOND 90.0f
#define SPECTRUM_FALL_DB_PER_FRAME (SPECTRUM_FALL_DB_PER_SECOND / FRAMES_PER_SECOND)

// What the samples that arrived since the last frame contained.
typedef struct {
	float peak;
	double sumOfSquares;
	int count;
	bool clipped;
} SampleStats;

static WaveColumn g_waveColumns[WAVE_COLUMNS];
static u64 g_waveColumnStart[WAVE_COLUMNS]; // the first mic sample of each column
static int g_waveHead; // next column to fill (the oldest shown)

// The column being filled.
static float g_columnPeak;
static int g_columnSamples;
static bool g_columnClipped;
static u64 g_columnStart;

static u64 g_analysedUntil; // the next mic sample to analyse
static float g_peakLevel;
static float g_meanSquare;
static float g_peakHold;
static u64 g_peakHoldUntil;
static u64 g_clipUntil;
static float g_spectrumDb[FFT_BINS];

void live_analysis_init(void)
{
	for (int i = 0; i < FFT_BINS; i++) {
		g_spectrumDb[i] = SPECTRUM_SILENCE_DB;
	}
}

float live_analysis_level_db(float level)
{
	return amplitude_to_db(level + SILENCE_LEVEL);
}

u8 live_analysis_wave_height(float level)
{
	float db = live_analysis_level_db(level);
	float fraction = (db + WAVE_RANGE_DB) / WAVE_RANGE_DB;
	if (fraction <= 0) {
		return 0;
	}
	if (fraction >= 1) {
		return WAVE_HEIGHT_MAX;
	}
	return (u8)(fraction * WAVE_HEIGHT_MAX);
}

static void push_column(void)
{
	WaveColumn *column = &g_waveColumns[g_waveHead];
	column->height = live_analysis_wave_height(g_columnPeak);
	bool recording = take_writer_is_active() && !take_writer_is_paused();
	column->flags = (recording ? WAVE_RECORDED : 0) | (g_columnClipped ? WAVE_CLIPPED : 0);
	g_waveColumnStart[g_waveHead] = g_columnStart;
	g_waveHead = (g_waveHead + 1) % WAVE_COLUMNS;
	g_columnPeak = 0;
	g_columnSamples = 0;
	g_columnClipped = false;
}

static void add_to_column(float level, bool clipped, u64 sampleNumber, int samplesPerColumn)
{
	if (level > g_columnPeak) {
		g_columnPeak = level;
	}
	if (clipped) {
		g_columnClipped = true;
	}
	if (g_columnSamples == 0) {
		g_columnStart = sampleNumber;
	}
	g_columnSamples++;
	if (g_columnSamples >= samplesPerColumn) {
		push_column();
	}
}

static int samples_per_column(void)
{
	int spanSeconds = WAVEFORM_SPAN_SECONDS[(int)g_settings.waveformSpan];
	return (int)(spanSeconds * MIC_RATE / WAVE_COLUMNS);
}

// Reads the mic up to total, fills waveform columns and measures the new samples.
static SampleStats analyse_new_samples(u64 total)
{
	static s16 samples[COPY_CHUNK_SAMPLES];
	SampleStats stats = {0};
	int samplesPerColumn = samples_per_column();
	while (g_analysedUntil < total) {
		u64 available = total - g_analysedUntil;
		int wanted = available > COPY_CHUNK_SAMPLES ? COPY_CHUNK_SAMPLES : (int)available;
		int copied = mic_copy(g_analysedUntil, samples, wanted);
		if (copied <= 0) {
			g_analysedUntil = total;
			break;
		}
		for (int i = 0; i < copied; i++) {
			int sample = samples[i];
			int magnitude = sample < 0 ? -sample : sample;
			float level = magnitude * (1.0f / MIC_FULL_SCALE);
			bool clipped = magnitude >= CLIP_THRESHOLD;
			if (level > stats.peak) {
				stats.peak = level;
			}
			stats.sumOfSquares += (double)level * level;
			if (clipped) {
				stats.clipped = true;
			}
			add_to_column(level, clipped, g_analysedUntil + i, samplesPerColumn);
		}
		stats.count += copied;
		g_analysedUntil += copied;
	}
	return stats;
}

static void update_meters(const SampleStats *stats)
{
	u64 now = osGetTime();
	if (stats->count > 0) {
		// A running average that forgets over about RMS_WINDOW_SECONDS.
		float smoothing = 1.0f - expf(-stats->count / (RMS_WINDOW_SECONDS * MIC_RATE));
		float meanSquare = (float)(stats->sumOfSquares / stats->count);
		g_meanSquare += (meanSquare - g_meanSquare) * smoothing;
	}
	g_peakLevel *= PEAK_FALL_PER_FRAME;
	if (stats->peak > g_peakLevel) {
		g_peakLevel = stats->peak;
	}
	if (stats->peak >= g_peakHold || now > g_peakHoldUntil) {
		g_peakHold = stats->peak;
		g_peakHoldUntil = now + PEAK_HOLD_MS;
	}
	if (stats->clipped) {
		g_clipUntil = now + CLIP_INDICATOR_MS;
	}
}

// The spectrum of the newest FFT_SIZE samples. A bin rises at once and falls slowly.
static void update_spectrum(u64 total)
{
	static float levels[FFT_SIZE];
	static s16 samples[FFT_SIZE];
	if (total < FFT_SIZE) {
		return;
	}
	if (mic_copy(total - FFT_SIZE, samples, FFT_SIZE) != FFT_SIZE) {
		return;
	}
	for (int i = 0; i < FFT_SIZE; i++) {
		levels[i] = samples[i] * (1.0f / MIC_FULL_SCALE);
	}
	float binDb[FFT_BINS];
	fft_spectrum_db(levels, binDb);
	for (int i = 0; i < FFT_BINS; i++) {
		float fallen = g_spectrumDb[i] - SPECTRUM_FALL_DB_PER_FRAME;
		g_spectrumDb[i] = binDb[i] > fallen ? binDb[i] : fallen;
	}
}

void live_analysis_update(void)
{
	u64 total = mic_total_samples();
	if (total < g_analysedUntil) {
		g_analysedUntil = total;
	}
	if (total - g_analysedUntil > BACKLOG_MAX_SAMPLES) {
		g_analysedUntil = total - BACKLOG_MAX_SAMPLES;
	}
	SampleStats stats = analyse_new_samples(total);
	update_meters(&stats);
	if (g_settings.liveView != LIVE_VIEW_WAVEFORM) {
		update_spectrum(total);
	}
}

void live_analysis_flag_recorded(u64 firstSample)
{
	for (int i = 0; i < WAVE_COLUMNS; i++) {
		if (g_waveColumnStart[i] >= firstSample && g_waveColumns[i].height) {
			g_waveColumns[i].flags |= WAVE_RECORDED;
		}
	}
}

void live_analysis_mark_newest_column(void)
{
	int newest = (g_waveHead + WAVE_COLUMNS - 1) % WAVE_COLUMNS;
	g_waveColumns[newest].flags |= WAVE_MARKER;
}

void live_analysis_clear_waveform(void)
{
	memset(g_waveColumns, 0, sizeof g_waveColumns);
}

const WaveColumn *live_analysis_wave_column(int index)
{
	return &g_waveColumns[(g_waveHead + index) % WAVE_COLUMNS];
}

const float *live_analysis_spectrum_db(void)
{
	return g_spectrumDb;
}

float live_analysis_peak(void)
{
	return g_peakLevel;
}

float live_analysis_rms(void)
{
	return sqrtf(g_meanSquare);
}

float live_analysis_peak_hold(void)
{
	return g_peakHold;
}

bool live_analysis_clipping(u64 now)
{
	return now < g_clipUntil;
}
