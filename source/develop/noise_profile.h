// Noise profile: the mean power of each STFT bin while nobody speaks.
// That is the mic's own hiss and whine, and the room.
// It uses the same window and frame size as the noise reducer.
// The level depends on the mic gain, so there is one profile per gain.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "fft.h"

// Saved to disk as is.
typedef struct {
	uint32_t magic;
	int32_t gain; // mic gain setting the profile belongs to
	int32_t frames; // STFT frames averaged
	float rate;
	float binPower[FFT_BINS];
} NoiseProfile;

typedef struct {
	float frame[FFT_SIZE];
	int fill;
	int sinceLastFrame;
	double powerSum[FFT_BINS];
	int frames;
} NoiseProfileBuilder;

void noise_profile_begin(NoiseProfileBuilder *builder);
void noise_profile_feed(NoiseProfileBuilder *builder, const float *samples, int count);
// Returns false when the input is too short for a profile.
bool noise_profile_finish(NoiseProfileBuilder *builder, NoiseProfile *profile, int gain, float rate);
// Overall noise level in dB relative to a full-scale sine.
float noise_profile_level_db(const NoiseProfile *profile);
// Moves the profile to another gain. The room and capsule noise follow the gain.
// The converter's own floor, which dominates at low gain, stays where it is.
void noise_profile_regain(NoiseProfile *profile, float fromDb, float toDb);
bool noise_profile_save(const NoiseProfile *profile, const char *path);
bool noise_profile_load(NoiseProfile *profile, const char *path);
