#include "filters.h"
#include <math.h>
#include <string.h>

// A 4th-order Butterworth low cut is two biquads with these Q values.
#define BUTTERWORTH_FIRST_Q 0.54119610
#define BUTTERWORTH_SECOND_Q 1.30656296
#define DC_BLOCKER_HZ 5.0 // runs when the low cut is off
#define HUM_HARMONICS 8 // notches on the mains frequency and its harmonics
#define HUM_NOTCH_Q 16.0
#define POWER_EPSILON 1e-30 // keeps the division finite where the denominator is zero

float filters_biquad_run(Biquad *biquad, float x)
{
	double y = biquad->b0 * x + biquad->z1;
	biquad->z1 = biquad->b1 * x - biquad->a1 * y + biquad->z2;
	biquad->z2 = biquad->b2 * x - biquad->a2 * y;
	return (float)y;
}

void filters_biquad_set(Biquad *biquad, double b0, double b1, double b2, double a0, double a1, double a2)
{
	biquad->b0 = b0 / a0;
	biquad->b1 = b1 / a0;
	biquad->b2 = b2 / a0;
	biquad->a1 = a1 / a0;
	biquad->a2 = a2 / a0;
	biquad->z2 = 0;
	biquad->z1 = 0;
}

// Audio EQ Cookbook high-pass.
static void biquad_high_pass(Biquad *biquad, double sampleRate, double cutoffHz, double q)
{
	double omega = 2 * M_PI * cutoffHz / sampleRate;
	double cosOmega = cos(omega);
	double alpha = sin(omega) / (2 * q);
	filters_biquad_set(biquad, (1 + cosOmega) / 2, -(1 + cosOmega), (1 + cosOmega) / 2, 1 + alpha,
	                   -2 * cosOmega, 1 - alpha);
}

// Audio EQ Cookbook notch.
static void biquad_notch(Biquad *biquad, double sampleRate, double centerHz, double q)
{
	double omega = 2 * M_PI * centerHz / sampleRate;
	double cosOmega = cos(omega);
	double alpha = sin(omega) / (2 * q);
	filters_biquad_set(biquad, 1, -2 * cosOmega, 1, 1 + alpha, -2 * cosOmega, 1 - alpha);
}

// |H(f)|^2 of one biquad.
static double biquad_power(const Biquad *biquad, double sampleRate, double hz)
{
	double omega = 2 * M_PI * hz / sampleRate;
	double cos1 = cos(omega);
	double sin1 = sin(omega);
	double cos2 = cos(2 * omega);
	double sin2 = sin(2 * omega);
	double numeratorReal = biquad->b0 + biquad->b1 * cos1 + biquad->b2 * cos2;
	double numeratorImag = -(biquad->b1 * sin1 + biquad->b2 * sin2);
	double denominatorReal = 1 + biquad->a1 * cos1 + biquad->a2 * cos2;
	double denominatorImag = -(biquad->a1 * sin1 + biquad->a2 * sin2);
	double numeratorPower = numeratorReal * numeratorReal + numeratorImag * numeratorImag;
	double denominatorPower = denominatorReal * denominatorReal + denominatorImag * denominatorImag;
	return numeratorPower / (denominatorPower + POWER_EPSILON);
}

static Biquad *add_section(PreFilters *filters)
{
	Biquad *section = &filters->sections[filters->count];
	filters->count++;
	return section;
}

void filters_init(PreFilters *filters, double sampleRate, const DevelopParams *params)
{
	memset(filters, 0, sizeof *filters);
	if (params->lowCutHz > 0) {
		biquad_high_pass(add_section(filters), sampleRate, params->lowCutHz, BUTTERWORTH_FIRST_Q);
		biquad_high_pass(add_section(filters), sampleRate, params->lowCutHz, BUTTERWORTH_SECOND_Q);
	} else {
		filters->dcBlocker = true;
		filters->dcPole = 1.0 - 2 * M_PI * DC_BLOCKER_HZ / sampleRate;
	}
	bool mainsHum = params->humHz == MAINS_HZ_EUROPE || params->humHz == MAINS_HZ_AMERICAS;
	if (!mainsHum) {
		return;
	}
	for (int harmonic = 1; harmonic <= HUM_HARMONICS && filters->count < PREFILTER_MAX_SECTIONS; harmonic++) {
		biquad_notch(add_section(filters), sampleRate, params->humHz * harmonic, HUM_NOTCH_Q);
	}
}

float filters_run(PreFilters *filters, float x)
{
	if (filters->dcBlocker) {
		double y = x - filters->dcLastInput + filters->dcPole * filters->dcLastOutput;
		filters->dcLastInput = x;
		filters->dcLastOutput = y;
		x = (float)y;
	}
	for (int i = 0; i < filters->count; i++) {
		x = filters_biquad_run(&filters->sections[i], x);
	}
	return x;
}

// |H(f)|^2 of the DC blocker (1 - z^-1) / (1 - R z^-1), R being the pole radius.
static double dc_blocker_power(double pole, double sampleRate, double hz)
{
	double omega = 2 * M_PI * hz / sampleRate;
	double numeratorReal = 1 - cos(omega);
	double numeratorImag = sin(omega);
	double denominatorReal = 1 - pole * cos(omega);
	double denominatorImag = pole * sin(omega);
	double numeratorPower = numeratorReal * numeratorReal + numeratorImag * numeratorImag;
	double denominatorPower = denominatorReal * denominatorReal + denominatorImag * denominatorImag;
	return numeratorPower / (denominatorPower + POWER_EPSILON);
}

double filters_power_gain(const PreFilters *filters, double sampleRate, double hz)
{
	double gain = 1;
	if (filters->dcBlocker) {
		gain = dc_blocker_power(filters->dcPole, sampleRate, hz);
	}
	for (int i = 0; i < filters->count; i++) {
		gain *= biquad_power(&filters->sections[i], sampleRate, hz);
	}
	return gain;
}
