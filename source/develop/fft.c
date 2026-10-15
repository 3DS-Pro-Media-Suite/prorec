// The real FFT runs as one complex FFT of half the length (even samples real, odd ones imaginary),
// followed by a split into the real spectrum. Radix-2, float, tables built once.

#include "fft.h"
#include <math.h>
#include "decibels.h"
#include <stdbool.h>
#include <stdint.h>

#define COMPLEX_SIZE (FFT_SIZE / 2)
#define SPECTRUM_POWER_FLOOR 1e-14f // keeps the logarithm finite on silence

static float g_twiddleCos[COMPLEX_SIZE / 2]; // exp(-2 pi i k / COMPLEX_SIZE)
static float g_twiddleSin[COMPLEX_SIZE / 2];
static float g_splitCos[COMPLEX_SIZE + 1]; // exp(-2 pi i k / FFT_SIZE)
static float g_splitSin[COMPLEX_SIZE + 1];
static uint16_t g_bitReverse[COMPLEX_SIZE];
static float g_sqrtHann[FFT_SIZE];
static float g_hann[FFT_SIZE];
static float g_hannSum;

static void build_bit_reverse_table(void)
{
	int bits = 0;
	while ((1 << bits) < COMPLEX_SIZE) {
		bits++;
	}
	for (int i = 0; i < COMPLEX_SIZE; i++) {
		int reversed = 0;
		for (int bit = 0; bit < bits; bit++) {
			if (i & (1 << bit)) {
				reversed |= 1 << (bits - 1 - bit);
			}
		}
		g_bitReverse[i] = reversed;
	}
}

static void build_twiddles(void)
{
	for (int k = 0; k < COMPLEX_SIZE / 2; k++) {
		g_twiddleCos[k] = (float)cos(2 * M_PI * k / COMPLEX_SIZE);
		g_twiddleSin[k] = (float)-sin(2 * M_PI * k / COMPLEX_SIZE);
	}
	for (int k = 0; k <= COMPLEX_SIZE; k++) {
		g_splitCos[k] = (float)cos(2 * M_PI * k / FFT_SIZE);
		g_splitSin[k] = (float)-sin(2 * M_PI * k / FFT_SIZE);
	}
}

// Periodic Hann; its square root is the STFT's analysis and synthesis window.
static void build_windows(void)
{
	g_hannSum = 0;
	for (int n = 0; n < FFT_SIZE; n++) {
		double hann = 0.5 - 0.5 * cos(2 * M_PI * n / FFT_SIZE);
		g_hann[n] = (float)hann;
		g_sqrtHann[n] = (float)sqrt(hann);
		g_hannSum += (float)hann;
	}
}

const float *fft_sqrt_hann(void)
{
	return g_sqrtHann;
}

void fft_init(void)
{
	build_bit_reverse_table();
	build_twiddles();
	build_windows();
}

static void swap_floats(float *a, float *b)
{
	float swap = *a;
	*a = *b;
	*b = swap;
}

// In-place complex FFT of COMPLEX_SIZE points. The inverse is not scaled.
static void complex_fft(float *real, float *imag, bool inverse)
{
	for (int i = 0; i < COMPLEX_SIZE; i++) {
		int j = g_bitReverse[i];
		if (j > i) {
			swap_floats(&real[i], &real[j]);
			swap_floats(&imag[i], &imag[j]);
		}
	}
	for (int span = 2; span <= COMPLEX_SIZE; span <<= 1) {
		int halfSpan = span / 2;
		int twiddleStep = COMPLEX_SIZE / span;
		for (int start = 0; start < COMPLEX_SIZE; start += span) {
			for (int k = 0; k < halfSpan; k++) {
				float twiddleReal = g_twiddleCos[k * twiddleStep];
				float twiddleImag = inverse ? -g_twiddleSin[k * twiddleStep] : g_twiddleSin[k * twiddleStep];
				int top = start + k;
				int bottom = top + halfSpan;
				float productReal = real[bottom] * twiddleReal - imag[bottom] * twiddleImag;
				float productImag = real[bottom] * twiddleImag + imag[bottom] * twiddleReal;
				real[bottom] = real[top] - productReal;
				imag[bottom] = imag[top] - productImag;
				real[top] += productReal;
				imag[top] += productImag;
			}
		}
	}
}

// Real FFT of FFT_SIZE samples: bins 0..FFT_SIZE/2 (unnormalised).
static void fft_real(const float *samples, float *real, float *imag)
{
	float complexReal[COMPLEX_SIZE];
	float complexImag[COMPLEX_SIZE];
	for (int n = 0; n < COMPLEX_SIZE; n++) {
		complexReal[n] = samples[2 * n];
		complexImag[n] = samples[2 * n + 1];
	}
	complex_fft(complexReal, complexImag, false);
	// Z splits into E and O, the spectra of the even and odd samples. Then X = E + W O.
	for (int k = 0; k <= COMPLEX_SIZE; k++) {
		int index = k == COMPLEX_SIZE ? 0 : k;
		int mirror = (COMPLEX_SIZE - k) % COMPLEX_SIZE;
		float zReal = complexReal[index];
		float zImag = complexImag[index];
		// C = conj(Z[COMPLEX_SIZE - k])
		float conjReal = complexReal[mirror];
		float conjImag = -complexImag[mirror];
		float evenReal = 0.5f * (zReal + conjReal);
		float evenImag = 0.5f * (zImag + conjImag);
		// O = (Z - C) / 2i
		float oddReal = 0.5f * (zImag - conjImag);
		float oddImag = -0.5f * (zReal - conjReal);
		float twiddleReal = g_splitCos[k];
		float twiddleImag = g_splitSin[k];
		real[k] = evenReal + twiddleReal * oddReal - twiddleImag * oddImag;
		imag[k] = evenImag + twiddleReal * oddImag + twiddleImag * oddReal;
	}
}

void fft_inverse_real(const float *real, const float *imag, float *samples)
{
	// Z = E + i O, rebuilt from X.
	float complexReal[COMPLEX_SIZE];
	float complexImag[COMPLEX_SIZE];
	for (int k = 0; k < COMPLEX_SIZE; k++) {
		int mirror = COMPLEX_SIZE - k;
		float xReal = real[k];
		float xImag = imag[k];
		// C = conj(X[COMPLEX_SIZE - k])
		float conjReal = real[mirror];
		float conjImag = -imag[mirror];
		float evenReal = 0.5f * (xReal + conjReal);
		float evenImag = 0.5f * (xImag + conjImag);
		float diffReal = xReal - conjReal;
		float diffImag = xImag - conjImag;
		float twiddleReal = g_splitCos[k];
		float twiddleImag = g_splitSin[k];
		// O = (X - C) * conj(W) / 2
		float oddReal = 0.5f * (diffReal * twiddleReal + diffImag * twiddleImag);
		float oddImag = 0.5f * (diffImag * twiddleReal - diffReal * twiddleImag);
		complexReal[k] = evenReal - oddImag;
		complexImag[k] = evenImag + oddReal;
	}
	complex_fft(complexReal, complexImag, true);
	const float scale = 1.0f / COMPLEX_SIZE;
	for (int n = 0; n < COMPLEX_SIZE; n++) {
		samples[2 * n] = complexReal[n] * scale;
		samples[2 * n + 1] = complexImag[n] * scale;
	}
}

void fft_spectrum_db(const float *samples, float *db)
{
	float windowed[FFT_SIZE];
	float real[FFT_BINS];
	float imag[FFT_BINS];
	for (int n = 0; n < FFT_SIZE; n++) {
		windowed[n] = samples[n] * g_hann[n];
	}
	fft_real(windowed, real, imag);
	const float amplitudeScale = 2.0f / g_hannSum; // a full-scale sine: amplitude 1 -> 0 dB
	for (int i = 0; i < FFT_BINS; i++) {
		float power = (real[i] * real[i] + imag[i] * imag[i]) * amplitudeScale * amplitudeScale;
		db[i] = power_to_db(power + SPECTRUM_POWER_FLOOR);
	}
}

void fft_frame_power(const float *frame, float *real, float *imag, float *power)
{
	float windowed[FFT_SIZE];
	for (int n = 0; n < FFT_SIZE; n++) {
		windowed[n] = frame[n] * g_sqrtHann[n];
	}
	fft_real(windowed, real, imag);
	for (int i = 0; i < FFT_BINS; i++) {
		power[i] = real[i] * real[i] + imag[i] * imag[i];
	}
}
