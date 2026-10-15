// The microphone, raw. mic:u streams signed 16-bit PCM at the codec's own rate into a shared buffer, and a
// capture thread copies every sample into a RAM ring. Readers address the ring by absolute sample number, so
// nothing is converted or lost on the way. No UI and no files.
#pragma once
#include <3ds.h>
#include <stdbool.h>

#define MIC_RATE 32728.498f // MICU_SAMPLE_RATE_32730, the highest rate mic:u offers
#define MIC_RATE_HZ 32728 // the rate as a whole number, for a WAV header
#define MIC_FULL_SCALE 32768.0f // a sample of this size is 0 dBFS
#define MIC_GAIN_MAX 119 // MICU_SetGain range: the codec's amplifier in 0.5 dB steps

bool mic_init(u8 gain); // false when mic:u is unavailable (mic_error says why)
void mic_exit(void);
Result mic_error(void);
const char *mic_error_where(void);

void mic_set_gain(u8 gain); // applied by the capture thread (never blocks the caller)
void mic_hold(bool hold); // true stops sampling while the app is in the background
u8 mic_gain(void); // the value last read back from mic:u
float mic_gain_db(u8 gain); // nominal analog gain in dB

u64 mic_total_samples(void); // samples captured since mic_init (monotonic)
// Copies samples [from, from + n) that are captured and still in the ring; returns the count.
int mic_copy(u64 from, s16 *dst, int n);
u64 mic_oldest_sample(void); // first sample still in the ring
u32 mic_gaps(void); // how often samples got lost because the capture thread fell behind
u32 mic_restarts(void); // how often sampling had to be started again (after sleep, for example)
