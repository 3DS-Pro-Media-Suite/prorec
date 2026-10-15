#include "auto_noise.h"
#include <stdlib.h>
#include <string.h>
#include "decibels.h"

// The histogram of each bin's power.
#define HISTOGRAM_LOW_DB (-160.0f)
#define HISTOGRAM_HIGH_DB 20.0f
#define HISTOGRAM_STEP_DB 0.5f
#define HISTOGRAM_STEPS ((int)((HISTOGRAM_HIGH_DB - HISTOGRAM_LOW_DB) / HISTOGRAM_STEP_DB))
#define POWER_EPSILON 1e-30f // keeps the logarithm finite on digital silence

#define NOISE_PERCENTILE 0.15f
// Noise power in a bin is about exponentially distributed: its mean is about 1 / -ln(1 - NOISE_PERCENTILE)
// times the percentile.
#define PERCENTILE_TO_MEAN 6.15f
#define MIN_FRAMES 16 // fewer frames give no usable estimate

bool auto_noise_begin(AutoNoise *autoNoise)
{
	memset(autoNoise, 0, sizeof *autoNoise);
	autoNoise->histogram = calloc((size_t)FFT_BINS * HISTOGRAM_STEPS, sizeof(uint32_t));
	return autoNoise->histogram != NULL;
}

void auto_noise_free(AutoNoise *autoNoise)
{
	free(autoNoise->histogram);
	autoNoise->histogram = NULL;
}

// Counts the full frame in the histogram and makes room for the next hop.
static void add_frame(AutoNoise *autoNoise)
{
	float real[FFT_BINS];
	float imaginary[FFT_BINS];
	float power[FFT_BINS];
	fft_frame_power(autoNoise->input, real, imaginary, power);
	for (int k = 0; k < FFT_BINS; k++) {
		float binDb = power_to_db(power[k] + POWER_EPSILON);
		int step = (int)((binDb - HISTOGRAM_LOW_DB) / HISTOGRAM_STEP_DB);
		if (step < 0) {
			step = 0;
		}
		if (step >= HISTOGRAM_STEPS) {
			step = HISTOGRAM_STEPS - 1;
		}
		autoNoise->histogram[k * HISTOGRAM_STEPS + step]++;
	}
	autoNoise->frames++;
	memmove(autoNoise->input, autoNoise->input + FFT_HOP, FFT_OVERLAP * sizeof(float));
	autoNoise->hopFill = 0;
}

void auto_noise_feed(AutoNoise *autoNoise, const float *samples, int count)
{
	for (int i = 0; i < count; i++) {
		if (autoNoise->fill < FFT_SIZE) {
			// The first frame fills the whole window.
			autoNoise->input[autoNoise->fill] = samples[i];
			autoNoise->fill++;
			if (autoNoise->fill < FFT_SIZE) {
				continue;
			}
		} else {
			// Every later frame waits for one more hop.
			autoNoise->input[FFT_OVERLAP + autoNoise->hopFill] = samples[i];
			autoNoise->hopFill++;
			if (autoNoise->hopFill < FFT_HOP) {
				continue;
			}
		}
		add_frame(autoNoise);
	}
}

// The first histogram step at which more than rank frames have been counted.
static int rank_step(const uint32_t *binHistogram, long rank)
{
	long framesCounted = 0;
	for (int step = 0; step < HISTOGRAM_STEPS; step++) {
		framesCounted += binHistogram[step];
		if (framesCounted > rank) {
			return step;
		}
	}
	return HISTOGRAM_STEPS;
}

bool auto_noise_result(const AutoNoise *autoNoise, float *noisePower)
{
	if (autoNoise->frames < MIN_FRAMES) {
		return false;
	}
	for (int k = 0; k < FFT_BINS; k++) {
		long rank = (long)(autoNoise->frames * NOISE_PERCENTILE);
		int step = rank_step(autoNoise->histogram + k * HISTOGRAM_STEPS, rank);
		float stepCenterDb = HISTOGRAM_LOW_DB + (step + 0.5f) * HISTOGRAM_STEP_DB;
		noisePower[k] = db_to_power(stepCenterDb) * PERCENTILE_TO_MEAN;
	}
	float smoothed[FFT_BINS];
	for (int k = 0; k < FFT_BINS; k++) {
		smoothed[k] = fft_smoothed_bin(noisePower, k);
	}
	memcpy(noisePower, smoothed, sizeof smoothed);
	noisePower[0] = noisePower[1]; // the DC bin holds the converter's offset, not noise
	return true;
}
