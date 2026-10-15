// Biquad sections and develop's pre-filters (DC blocker or low cut, hum notches) with their power response.
#pragma once
#include <stdbool.h>
#include "develop.h"

// Transposed direct form II.
typedef struct {
	double b0;
	double b1;
	double b2;
	double a1;
	double a2;
	double z1;
	double z2;
} Biquad;

#define PREFILTER_MAX_SECTIONS 12

// The pre-filters run on the noise estimate and on the signal alike.
typedef struct {
	Biquad sections[PREFILTER_MAX_SECTIONS];
	int count;
	double dcPole; // the DC blocker's pole radius
	double dcLastInput;
	double dcLastOutput;
	bool dcBlocker;
} PreFilters;

float filters_biquad_run(Biquad *biquad, float x);
// Coefficients normalised by a0; the state starts at zero.
void filters_biquad_set(Biquad *biquad, double b0, double b1, double b2, double a0, double a1, double a2);
void filters_init(PreFilters *filters, double sampleRate, const DevelopParams *params);
float filters_run(PreFilters *filters, float x);
// |H(f)|^2 of the whole chain at frequency hz.
double filters_power_gain(const PreFilters *filters, double sampleRate, double hz);
