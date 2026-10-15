// Live analysis of the mic, once per frame: the meters, the waveform history and the spectrum.
#pragma once
#include <3ds.h>
#include "fft.h"

#define WAVE_COLUMNS 376
#define WAVE_HEIGHT_MAX 255 // a column's height at full scale
#define WAVE_RANGE_DB 60.0f // the waveform shows this many dB below full scale
#define SPECTRUM_SILENCE_DB (-200) // a spectrum bin before anything is heard

typedef struct {
	u8 height; // 0..WAVE_HEIGHT_MAX on a log scale
	u8 flags; // WAVE_RECORDED, WAVE_CLIPPED, WAVE_MARKER
} WaveColumn;

enum {
	WAVE_RECORDED = 1,
	WAVE_CLIPPED = 2,
	WAVE_MARKER = 4
};

void live_analysis_init(void);
void live_analysis_update(void);
float live_analysis_level_db(float level); // level 0..1 to dB full scale
u8 live_analysis_wave_height(float level); // level 0..1 to a column height

void live_analysis_flag_recorded(u64 firstSample); // columns from that mic sample on belong to the take
void live_analysis_mark_newest_column(void);
void live_analysis_clear_waveform(void);
const WaveColumn *live_analysis_wave_column(int index); // 0 is the oldest column shown

const float *live_analysis_spectrum_db(void); // FFT_BINS values
float live_analysis_peak(void); // falls 20 dB per second
float live_analysis_rms(void); // averaged over about a third of a second
float live_analysis_peak_hold(void);
bool live_analysis_clipping(u64 now); // a sample touched full scale in the last seconds
