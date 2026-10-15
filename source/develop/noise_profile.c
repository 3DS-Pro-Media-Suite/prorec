#include "noise_profile.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "array_count.h"
#include "decibels.h"
#include "fft.h"

#define NOISE_PROFILE_MAGIC 0x31524E50u // "PNR1"
#define MIN_PROFILE_FRAMES 8
#define FULL_SCALE_SINE_MEAN_SQUARE 0.5
#define LOG_EPSILON 1e-20 // keeps the logarithm finite on digital silence
// A bin keeps at least this share of its power as acoustic noise, so a gain change always moves it a little.
#define MIN_ACOUSTIC_SHARE 0.05f

// A point of the converter's noise floor: its bin power in dB at 0 dB gain.
typedef struct {
	float hz;
	float db;
} FloorPoint;

// The floor in roughly third-octave bands. Below the first point the value is held; the low cut removes that
// region anyway.
static const FloorPoint ADC_FLOOR[] = {
        {100, -42.6f},  {160, -46.9f},  {250, -49.0f},  {400, -51.3f},  {630, -54.4f},   {1000, -55.6f},
        {1600, -57.6f}, {2500, -58.7f}, {4000, -58.7f}, {6300, -58.8f}, {10000, -59.1f}, {16000, -60.6f},
};
#define ADC_FLOOR_POINT_COUNT ARRAY_COUNT(ADC_FLOOR)

void noise_profile_begin(NoiseProfileBuilder *builder)
{
	memset(builder, 0, sizeof *builder);
}

// Adds the power spectrum of the full frame and makes room for the next hop.
static void add_frame(NoiseProfileBuilder *builder)
{
	float real[FFT_BINS];
	float imaginary[FFT_BINS];
	float power[FFT_BINS];
	fft_frame_power(builder->frame, real, imaginary, power);
	for (int k = 0; k < FFT_BINS; k++) {
		builder->powerSum[k] += power[k];
	}
	builder->frames++;
	builder->sinceLastFrame = 0;
	memmove(builder->frame, builder->frame + FFT_HOP, FFT_OVERLAP * sizeof(float));
}

void noise_profile_feed(NoiseProfileBuilder *builder, const float *samples, int count)
{
	for (int i = 0; i < count; i++) {
		if (builder->fill < FFT_SIZE) {
			// The first frame fills the whole window.
			builder->frame[builder->fill] = samples[i];
			builder->fill++;
			if (builder->fill < FFT_SIZE) {
				continue;
			}
		} else {
			// Every later frame waits for one more hop.
			builder->frame[FFT_OVERLAP + builder->sinceLastFrame] = samples[i];
			builder->sinceLastFrame++;
			if (builder->sinceLastFrame < FFT_HOP) {
				continue;
			}
		}
		add_frame(builder);
	}
}

bool noise_profile_finish(NoiseProfileBuilder *builder, NoiseProfile *profile, int gain, float rate)
{
	if (builder->frames < MIN_PROFILE_FRAMES) {
		return false;
	}
	memset(profile, 0, sizeof *profile);
	profile->magic = NOISE_PROFILE_MAGIC;
	profile->gain = gain;
	profile->frames = builder->frames;
	profile->rate = rate;
	for (int k = 0; k < FFT_BINS; k++) {
		profile->binPower[k] = (float)(builder->powerSum[k] / builder->frames);
	}
	// The DC bin holds the converter's offset, not noise.
	profile->binPower[0] = profile->binPower[1];
	return true;
}

// Parseval through the sqrt-Hann window (sum of w^2 = N / 2): mean square = 2 S / N^2,
// with S the power summed over all N bins of the full spectrum.
static float spectrum_level_db(const float *binPower)
{
	// Every bin between DC and Nyquist stands for two bins of the full spectrum.
	double allBinsSum = binPower[0] + binPower[FFT_BINS - 1];
	for (int k = 1; k < FFT_BINS - 1; k++) {
		allBinsSum += 2.0 * binPower[k];
	}
	double meanSquare = 2.0 * allBinsSum / ((double)FFT_SIZE * FFT_SIZE);
	double sineRelativePower = meanSquare / FULL_SCALE_SINE_MEAN_SQUARE;
	return (float)(10 * log10(sineRelativePower + LOG_EPSILON)); // in double: power_to_db() works in float
}

float noise_profile_level_db(const NoiseProfile *profile)
{
	return spectrum_level_db(profile->binPower);
}

// Interpolates the converter floor table on a log-frequency scale.
static float adc_floor(float hz)
{
	if (hz <= ADC_FLOOR[0].hz) {
		return db_to_power(ADC_FLOOR[0].db);
	}
	for (int i = 1; i < ADC_FLOOR_POINT_COUNT; i++) {
		if (hz <= ADC_FLOOR[i].hz) {
			const FloorPoint *low = &ADC_FLOOR[i - 1];
			const FloorPoint *high = &ADC_FLOOR[i];
			float t = logf(hz / low->hz) / logf(high->hz / low->hz);
			return db_to_power(low->db + t * (high->db - low->db));
		}
	}
	return db_to_power(ADC_FLOOR[ADC_FLOOR_POINT_COUNT - 1].db);
}

void noise_profile_regain(NoiseProfile *profile, float fromDb, float toDb)
{
	float gainScale = db_to_power(toDb - fromDb);
	for (int i = 0; i < FFT_BINS; i++) {
		float binHz = profile->rate * i / FFT_SIZE;
		float converterPower = adc_floor(binHz);
		float acousticPower = profile->binPower[i] - converterPower;
		float minAcousticPower = profile->binPower[i] * MIN_ACOUSTIC_SHARE;
		if (acousticPower < minAcousticPower) {
			acousticPower = minAcousticPower;
		}
		profile->binPower[i] = acousticPower * gainScale + converterPower;
	}
}

bool noise_profile_save(const NoiseProfile *profile, const char *path)
{
	FILE *file = fopen(path, "wb");
	if (!file) {
		return false;
	}
	bool written = fwrite(profile, 1, sizeof *profile, file) == sizeof *profile;
	bool closed = fclose(file) == 0;
	return closed && written;
}

bool noise_profile_load(NoiseProfile *profile, const char *path)
{
	FILE *file = fopen(path, "rb");
	if (!file) {
		return false;
	}
	bool complete = fread(profile, 1, sizeof *profile, file) == sizeof *profile;
	bool valid = complete && profile->magic == NOISE_PROFILE_MAGIC && profile->frames > 0;
	fclose(file);
	return valid;
}
