#include "voice_eq.h"
#include <math.h>
#include "array_count.h"
#include "decibels.h"
#include "develop.h"
#include "fft.h"

// A point of both EQ curves.
typedef struct {
	float hz;
	float naturalDb;
	float broadcastDb;
} EqPoint;

// The 3DS mic is about flat up to 2 kHz, then lifts the treble steeply, from +8 dB at 2.5 kHz to +25 dB at
// 8 kHz. That is its thin, hissy sound. "Natural" undoes the lift, capped at -16 dB. "Broadcast" adds a
// little warmth and presence on top.
static const EqPoint EQ_POINTS[] = {
        {60, 0, 0},       {125, 1, 2},         {200, 2, 3.5f},       {315, -1, -1},
        {500, -2, -2},    {800, -1, -1},       {1000, 0, 0},         {1600, 1, 1.5f},
        {2000, 0, 1},     {2500, -6, -4},      {3150, -7.5f, -4.5f}, {4000, -11.5f, -8.5f},
        {5000, -15, -12}, {6300, -16, -13.5f}, {8000, -16, -14},     {16000, -16, -14},
};
#define EQ_POINT_COUNT ARRAY_COUNT(EQ_POINTS)

static float point_db(const EqPoint *point, int mode)
{
	return mode == VOICE_EQ_BROADCAST ? point->broadcastDb : point->naturalDb;
}

// Interpolates the mode's curve on a log-frequency scale.
static float eq_db_at(int mode, float hz)
{
	if (hz <= EQ_POINTS[0].hz) {
		return point_db(&EQ_POINTS[0], mode);
	}
	if (hz >= EQ_POINTS[EQ_POINT_COUNT - 1].hz) {
		return point_db(&EQ_POINTS[EQ_POINT_COUNT - 1], mode);
	}
	int i = 1;
	while (hz > EQ_POINTS[i].hz) {
		i++;
	}
	const EqPoint *low = &EQ_POINTS[i - 1];
	const EqPoint *high = &EQ_POINTS[i];
	float t = logf(hz / low->hz) / logf(high->hz / low->hz);
	float lowDb = point_db(low, mode);
	return lowDb + t * (point_db(high, mode) - lowDb);
}

void voice_eq_curve(int mode, float rate, float *binGain)
{
	for (int k = 0; k < FFT_BINS; k++) {
		float binHz = rate * k / FFT_SIZE;
		float db = eq_db_at(mode, binHz);
		binGain[k] = mode != VOICE_EQ_OFF ? db_to_amplitude(db) : 1.0f;
	}
}
