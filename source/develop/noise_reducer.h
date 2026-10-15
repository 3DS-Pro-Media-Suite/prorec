// Noise reducer: an STFT with a sqrt-Hann window and a Wiener gain on a decision-directed a-priori SNR
// (Ephraim-Malah), floored and smoothed across neighbouring bins. This keeps speech natural and the
// residual noise free of the "musical" chirps that plain spectral subtraction leaves.
// The same pass repairs button clicks, cleans up the pauses and applies the voice EQ.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "fft.h"

// What one develop asks of the noise reducer.
typedef struct {
	double sampleRate;
	bool reduceNoise; // false: no noise reduction, only the other stages
	const float *noisePower; // FFT_BINS values, the noise each bin is reduced against; NULL without reduction
	int reductionDb; // the most a bin is turned down
	int voiceEq; // VOICE_EQ_*
	const uint32_t *events; // sample positions of button presses whose clicks are repaired; NULL for none
	int eventCount;
	int pauseCleanupDb; // this much more reduction while nobody speaks, 0 = off
} NoiseReducerOptions;

typedef struct {
	float input[FFT_SIZE];
	float overlapAdd[FFT_SIZE];
	float noisePower[FFT_BINS];
	float previousSpeech[FFT_BINS];
	float eqGain[FFT_BINS];
	bool eqEnabled;
	// Output magnitude of the last frame without a button press, for click repair.
	float lastMagnitude[FFT_BINS];
	float floorGain;
	float overEstimate;
	bool enabled;
	long frameStart; // input sample index of input[0]
	const uint32_t *events; // sample positions of button presses
	int eventCount;
	int nextEvent;
	int clickBefore; // samples before a press that its click reaches
	int clickAfter; // samples after a press that its click reaches
	// Pause cleanup: a downward expander after the noise reducer. Speech presence comes from the share of
	// bins well above the noise and is held for holdFrames. In pauses the whole frame goes down by pauseGain.
	float pauseGain;
	float speechPresence;
	int holdLeft;
	int holdFrames;
	int speechBinLow;
	int speechBinHigh;
} NoiseReducer;

void noise_reducer_init(NoiseReducer *reducer, const NoiseReducerOptions *options);
// One hop of input in, one hop of output out, delayed by FFT_OVERLAP samples.
void noise_reducer_hop(NoiseReducer *reducer, const float *input, float *output);
