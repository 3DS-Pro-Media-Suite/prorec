// The real FFT of FFT_SIZE samples with its windows, one frame's power spectrum and the live spectrum.
// Portable C.
#pragma once

#define FFT_SIZE 1024 // samples per STFT frame
#define FFT_HOP 256 // 75 % overlap
#define FFT_OVERLAP (FFT_SIZE - FFT_HOP) // samples a frame shares with the next one
#define FFT_BINS (FFT_SIZE / 2 + 1)
// Smoothing across neighbouring bins.
#define FFT_SMOOTH_CENTER_WEIGHT 0.5f
#define FFT_SMOOTH_SIDE_WEIGHT 0.25f

void fft_init(void); // FFT tables; call once before anything else (not thread-safe itself)
// The sqrt-Hann window, FFT_SIZE values: the STFT's analysis and synthesis window.
const float *fft_sqrt_hann(void);

// Inverse real FFT: bins 0..FFT_SIZE/2 back to FFT_SIZE samples (scaled: an exact round trip of the forward
// FFT).
void fft_inverse_real(const float *real, const float *imag, float *samples);

// Live spectrum of the last FFT_SIZE samples: dB per bin, a full-scale sine reads 0 dB (Hann window).
void fft_spectrum_db(const float *samples, float *db);

// Power spectrum of one frame through the analysis window (as the noise reducer sees it).
void fft_frame_power(const float *frame, float *real, float *imag, float *power);

// A bin averaged with its two neighbours. The edge bins stand in for their missing neighbour.
static inline float fft_smoothed_bin(const float *values, int k)
{
	float left = k > 0 ? values[k - 1] : values[k];
	float right = k < FFT_BINS - 1 ? values[k + 1] : values[k];
	return values[k] * FFT_SMOOTH_CENTER_WEIGHT + left * FFT_SMOOTH_SIDE_WEIGHT +
	       right * FFT_SMOOTH_SIDE_WEIGHT;
}
