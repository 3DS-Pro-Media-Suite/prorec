// Decibels and linear factors. Portable C.
#pragma once
#include <math.h>

// amplitude = 10^(dB / 20)
static inline float db_to_amplitude(float db)
{
	return powf(10.0f, db / 20.0f);
}

// dB = 20 log10(amplitude)
static inline float amplitude_to_db(float amplitude)
{
	return 20.0f * log10f(amplitude);
}

// power = 10^(dB / 10)
static inline float db_to_power(float db)
{
	return powf(10.0f, db / 10.0f);
}

// dB = 10 log10(power)
static inline float power_to_db(float power)
{
	return 10.0f * log10f(power);
}
