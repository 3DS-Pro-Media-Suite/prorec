// The voice EQ: the gain of each STFT bin for a VOICE_EQ_* mode. Portable C.
#pragma once

// Gain per bin (FFT_BINS values) for a VOICE_EQ_* mode; 1 everywhere when the EQ is off.
void voice_eq_curve(int mode, float rate, float *binGain);
