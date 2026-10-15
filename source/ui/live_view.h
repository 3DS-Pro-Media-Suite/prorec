// The top screen while recording: header strip, take time, level meter, waveform, spectrum with the noise
// line, self-timer and learning overlays, and the control hints.
#pragma once
#include <stdbool.h>
#include "draw.h"
#include "live_analysis.h"

#define WAVE_X ((TOP_SCREEN_WIDTH - WAVE_COLUMNS) / 2) // the waveform is centred on the top screen

// The level meter at y: RMS bar, peak strip, peak hold and the clip light. Levels are 0..1.
void live_view_draw_meter(float y, float peak, float rms, float hold, bool clip);
void live_view_draw(void);
void live_view_show_controls(bool show); // the key hints over the live view
