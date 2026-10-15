// The level stages of develop: the loudness meter, the leveler (a slow compressor) and the look-ahead peak
// limiter.
#pragma once
#include "filters.h"

#define LOUDNESS_SILENCE_LUFS (-99.0f) // the loudness when nothing is above the gates

// Integrated loudness after ITU-R BS.1770-4: K-weighting, then gated blocks of four overlapping pieces.
typedef struct {
	Biquad shelf;
	Biquad highPass;
	double pieceSum;
	int pieceFill;
	int pieceLength;
	float *meanSquares; // mean square of each piece
	int pieceCount;
	int pieceCapacity;
} LoudnessMeter;

typedef struct {
	double envelope;
	double attack;
	double release;
	float thresholdDb;
	float ratio;
	float kneeDb;
} Compressor;

// Look-ahead peak limiter: the gain is the smallest needed gain over the next `delay` samples, smoothed by an
// average over as many samples, so it is down in time for every peak. The output is delayed: the caller drops
// the first levels_limiter_latency samples.
#define LIMITER_MAX_DELAY 512

typedef struct {
	float ceiling;
	int delay;
	int position;
	int minimumIndex;
	float samples[LIMITER_MAX_DELAY];
	float neededGains[LIMITER_MAX_DELAY];
	float minima[LIMITER_MAX_DELAY];
	double minimaSum;
	float minimum;
	float gain;
	float release;
} Limiter;

void levels_loudness_init(LoudnessMeter *meter, double sampleRate);
void levels_loudness_feed(LoudnessMeter *meter, const float *samples, int count);
// Integrated loudness in LUFS, or LOUDNESS_SILENCE_LUFS. Frees the meter's pieces.
float levels_loudness_result(LoudnessMeter *meter);
// Frees the meter's pieces without a result.
void levels_loudness_free(LoudnessMeter *meter);
void levels_compressor_init(Compressor *compressor, double sampleRate, float thresholdDb);
float levels_compressor_run(Compressor *compressor, float x);
void levels_limiter_init(Limiter *limiter, double sampleRate, float ceilingDb);
float levels_limiter_run(Limiter *limiter, float x);
int levels_limiter_latency(const Limiter *limiter);
