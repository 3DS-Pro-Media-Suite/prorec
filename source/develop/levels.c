#include "levels.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "decibels.h"
#include "filters.h"

// K-weighting, stage 1: a high shelf.
#define SHELF_HZ 1681.974450955533
#define SHELF_GAIN_DB 3.999843853973347
#define SHELF_Q 0.7071752369554196
#define SHELF_BAND_EXPONENT 0.4996667741545416
#define AMPLITUDE_DB_PER_DECADE 20 // amplitude = 10^(dB / 20)
// K-weighting, stage 2: a high-pass.
#define HIGH_PASS_HZ 38.13547087602444
#define HIGH_PASS_Q 0.5003270373238773

// A gating block is four pieces; a new block starts with every piece.
#define PIECE_SECONDS 0.1
#define PIECES_PER_BLOCK 4
#define FIRST_PIECE_CAPACITY 1024
#define ABSOLUTE_GATE_LUFS (-70)
#define RELATIVE_GATE_LU (-10.0)
#define LOUDNESS_OFFSET 0.691 // loudness = -0.691 + 10 log10(mean square)

// A leveler, not a syllable squasher: a slow detector and a gentle ratio.
#define LEVELER_ATTACK_SECONDS 0.030
#define LEVELER_RELEASE_SECONDS 0.400
#define LEVELER_RATIO 2.5f
#define LEVELER_KNEE_DB 10
#define LEVEL_POWER_FLOOR 1e-12f // keeps the logarithm finite on silence

#define LIMITER_LOOKAHEAD_SECONDS 0.005
#define LIMITER_MIN_DELAY 8
#define LIMITER_RELEASE_SECONDS 0.080

// The coefficient of a one-pole smoother with this time constant.
static double smoothing_coefficient(double seconds, double sampleRate)
{
	return 1 - exp(-1 / (seconds * sampleRate));
}

static void set_shelf(Biquad *shelf, double sampleRate)
{
	double k = tan(M_PI * SHELF_HZ / sampleRate); // the bilinear transform's pre-warped frequency
	// In double: db_to_amplitude() works in float.
	double highGain = pow(10, SHELF_GAIN_DB / AMPLITUDE_DB_PER_DECADE);
	double bandGain = pow(highGain, SHELF_BAND_EXPONENT);
	double a0 = 1.0 + k / SHELF_Q + k * k;
	double b0 = (highGain + bandGain * k / SHELF_Q + k * k) / a0;
	double b1 = 2.0 * (k * k - highGain) / a0;
	double b2 = (highGain - bandGain * k / SHELF_Q + k * k) / a0;
	double a1 = 2.0 * (k * k - 1.0) / a0;
	double a2 = (1.0 - k / SHELF_Q + k * k) / a0;
	filters_biquad_set(shelf, b0, b1, b2, 1.0, a1, a2);
}

static void set_high_pass(Biquad *highPass, double sampleRate)
{
	double k = tan(M_PI * HIGH_PASS_HZ / sampleRate);
	double a0 = 1.0 + k / HIGH_PASS_Q + k * k;
	double a1 = 2.0 * (k * k - 1.0) / a0;
	double a2 = (1.0 - k / HIGH_PASS_Q + k * k) / a0;
	filters_biquad_set(highPass, 1.0, -2.0, 1.0, 1.0, a1, a2);
}

void levels_loudness_init(LoudnessMeter *meter, double sampleRate)
{
	memset(meter, 0, sizeof *meter);
	set_shelf(&meter->shelf, sampleRate);
	set_high_pass(&meter->highPass, sampleRate);
	meter->pieceLength = (int)(sampleRate * PIECE_SECONDS + 0.5);
}

static bool make_room_for_piece(LoudnessMeter *meter)
{
	if (meter->pieceCount != meter->pieceCapacity) {
		return true;
	}
	int capacity = meter->pieceCapacity ? meter->pieceCapacity * 2 : FIRST_PIECE_CAPACITY;
	float *grown = realloc(meter->meanSquares, capacity * sizeof(float));
	if (!grown) {
		return false;
	}
	meter->meanSquares = grown;
	meter->pieceCapacity = capacity;
	return true;
}

void levels_loudness_feed(LoudnessMeter *meter, const float *samples, int count)
{
	for (int i = 0; i < count; i++) {
		float shelved = filters_biquad_run(&meter->shelf, samples[i]);
		float weighted = filters_biquad_run(&meter->highPass, shelved);
		meter->pieceSum += (double)weighted * weighted;
		meter->pieceFill++;
		if (meter->pieceFill != meter->pieceLength) {
			continue;
		}
		// Without memory the piece is lost, the meter goes on.
		if (make_room_for_piece(meter)) {
			meter->meanSquares[meter->pieceCount] = (float)(meter->pieceSum / meter->pieceLength);
			meter->pieceCount++;
		}
		meter->pieceSum = 0;
		meter->pieceFill = 0;
	}
}

static double lufs_to_mean_square(double lufs)
{
	return pow(10, (lufs + LOUDNESS_OFFSET) / 10);
}

static double mean_square_to_lufs(double meanSquare)
{
	return -LOUDNESS_OFFSET + 10 * log10(meanSquare);
}

// The mean square of the gating block that starts at this piece.
static double block_mean_square(const float *pieces)
{
	float sum = pieces[0];
	for (int piece = 1; piece < PIECES_PER_BLOCK; piece++) {
		sum += pieces[piece];
	}
	return (double)sum / PIECES_PER_BLOCK;
}

// Sums the mean squares of the gating blocks above the gate. Returns how many there are.
static int sum_blocks_above(const LoudnessMeter *meter, double gate, double *sum)
{
	int blockCount = meter->pieceCount - (PIECES_PER_BLOCK - 1);
	double gatedSum = 0;
	int gatedCount = 0;
	for (int block = 0; block < blockCount; block++) {
		double meanSquare = block_mean_square(meter->meanSquares + block);
		if (meanSquare > gate) {
			gatedSum += meanSquare;
			gatedCount++;
		}
	}
	*sum = gatedSum;
	return gatedCount;
}

float levels_loudness_result(LoudnessMeter *meter)
{
	double absoluteGate = lufs_to_mean_square(ABSOLUTE_GATE_LUFS);
	double sum = 0;
	int count = sum_blocks_above(meter, absoluteGate, &sum);
	if (count > 0) {
		double relativeGate = sum / count * pow(10, RELATIVE_GATE_LU / 10);
		double gate = relativeGate > absoluteGate ? relativeGate : absoluteGate;
		count = sum_blocks_above(meter, gate, &sum);
	}
	float loudness = count > 0 ? (float)mean_square_to_lufs(sum / count) : LOUDNESS_SILENCE_LUFS;
	levels_loudness_free(meter);
	return loudness;
}

void levels_loudness_free(LoudnessMeter *meter)
{
	free(meter->meanSquares);
	meter->meanSquares = NULL;
	meter->pieceCount = 0;
	meter->pieceCapacity = 0;
}

void levels_compressor_init(Compressor *compressor, double sampleRate, float thresholdDb)
{
	compressor->envelope = 0;
	compressor->attack = smoothing_coefficient(LEVELER_ATTACK_SECONDS, sampleRate);
	compressor->release = smoothing_coefficient(LEVELER_RELEASE_SECONDS, sampleRate);
	compressor->thresholdDb = thresholdDb;
	compressor->ratio = LEVELER_RATIO;
	compressor->kneeDb = LEVELER_KNEE_DB;
}

// Soft knee: the reduction fades in quadratically across the knee.
static float gain_reduction_db(const Compressor *compressor, float overDb)
{
	float halfKnee = compressor->kneeDb / 2;
	float slope = 1 - 1 / compressor->ratio;
	if (overDb > halfKnee) {
		return overDb * slope;
	}
	if (overDb > -halfKnee) {
		float intoKnee = overDb + halfKnee;
		return slope * intoKnee * intoKnee / (2 * compressor->kneeDb);
	}
	return 0;
}

float levels_compressor_run(Compressor *compressor, float x)
{
	double power = (double)x * x;
	double coefficient = power > compressor->envelope ? compressor->attack : compressor->release;
	compressor->envelope += coefficient * (power - compressor->envelope);
	// A sine's peak level.
	float levelDb = power_to_db((float)compressor->envelope * 2.0f + LEVEL_POWER_FLOOR);
	float reductionDb = gain_reduction_db(compressor, levelDb - compressor->thresholdDb);
	return x * db_to_amplitude(-reductionDb);
}

void levels_limiter_init(Limiter *limiter, double sampleRate, float ceilingDb)
{
	memset(limiter, 0, sizeof *limiter);
	limiter->ceiling = db_to_amplitude(ceilingDb);
	limiter->delay = (int)(sampleRate * LIMITER_LOOKAHEAD_SECONDS);
	if (limiter->delay < LIMITER_MIN_DELAY) {
		limiter->delay = LIMITER_MIN_DELAY;
	}
	if (limiter->delay > LIMITER_MAX_DELAY) {
		limiter->delay = LIMITER_MAX_DELAY;
	}
	for (int i = 0; i < limiter->delay; i++) {
		limiter->minima[i] = 1;
		limiter->neededGains[i] = 1;
	}
	limiter->minimaSum = limiter->delay;
	limiter->minimum = 1;
	limiter->gain = 1;
	limiter->release = (float)smoothing_coefficient(LIMITER_RELEASE_SECONDS, sampleRate);
}

// The smallest needed gain in the line. When the old minimum leaves the line, the line is searched again.
static void update_minimum(Limiter *limiter, int position, float neededGain)
{
	if (neededGain <= limiter->minimum) {
		limiter->minimum = neededGain;
		limiter->minimumIndex = position;
		return;
	}
	if (limiter->minimumIndex != position) {
		return;
	}
	limiter->minimum = 1;
	for (int k = 0; k < limiter->delay; k++) {
		if (limiter->neededGains[k] <= limiter->minimum) {
			limiter->minimum = limiter->neededGains[k];
			limiter->minimumIndex = k;
		}
	}
}

int levels_limiter_latency(const Limiter *limiter)
{
	return limiter->delay - 1;
}

float levels_limiter_run(Limiter *limiter, float x)
{
	int length = limiter->delay;
	int position = limiter->position;
	float magnitude = fabsf(x);
	float neededGain = magnitude > limiter->ceiling ? limiter->ceiling / magnitude : 1.0f;
	limiter->samples[position] = x;
	limiter->neededGains[position] = neededGain;
	update_minimum(limiter, position, neededGain);
	// Box average of the last `length` minima: at the oldest sample every one of them saw that sample's need.
	limiter->minimaSum += limiter->minimum - limiter->minima[position];
	limiter->minima[position] = limiter->minimum;
	float smoothed = (float)(limiter->minimaSum / length);
	if (smoothed > 1) {
		smoothed = 1;
	}
	if (smoothed < limiter->gain) {
		limiter->gain = smoothed;
	} else {
		limiter->gain += (smoothed - limiter->gain) * limiter->release;
	}
	limiter->position = (position + 1) % length;
	float y = limiter->samples[limiter->position] * limiter->gain; // the oldest sample in the line
	// Guards against float rounding.
	if (y > limiter->ceiling) {
		y = limiter->ceiling;
	}
	if (y < -limiter->ceiling) {
		y = -limiter->ceiling;
	}
	return y;
}
