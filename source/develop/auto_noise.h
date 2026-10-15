// Automatic noise estimate: a low percentile of each bin's power over the whole take.
// Speech fills a bin only part of the time, so its quiet moments set the estimate. Portable C.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "fft.h"

typedef struct {
	uint32_t *histogram; // FFT_BINS rows of frame counts, one count per power step
	long frames;
	float input[FFT_SIZE];
	int fill;
	int hopFill;
} AutoNoise;

// False when the histogram does not fit in memory.
bool auto_noise_begin(AutoNoise *autoNoise);
// The samples go into overlapping STFT frames, as the noise reducer sees them.
void auto_noise_feed(AutoNoise *autoNoise, const float *samples, int count);
// The noise power per bin (FFT_BINS values) from all frames so far. False when too few frames came in.
bool auto_noise_result(const AutoNoise *autoNoise, float *noisePower);
void auto_noise_free(AutoNoise *autoNoise);
