// The settings: the live values, their stored values, the presets, loading and saving them, and the develop
// parameters they give.
#pragma once
#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>
#include "develop.h"

#define GAIN_STEP_MAX 20
#define DEFAULT_GAIN_STEP 6 // 18 dB: high enough that the converter's own noise does not show

typedef struct {
	s8 gainStep; // mic gain in 3 dB steps, 0..GAIN_STEP_MAX
	s8 preset; // PRESET_*; PRESET_CUSTOM when the clean-up settings match no preset
	s8 noiseReduction; // NOISE_REDUCTION_*
	s8 noiseSource; // NOISE_SOURCE_*
	s8 lowCut; // LOW_CUT_*
	s8 humFilter; // HUM_FILTER_*
	s8 voiceEq; // VOICE_EQ_*
	s8 leveler;
	s8 loudness; // LOUDNESS_*
	s8 cleanFormat; // CLEAN_FORMAT_*
	s8 preRecord; // PRE_RECORD_*
	s8 monitor;
	s8 selfTimer; // SELF_TIMER_*
	s8 autoDevelop;
	s8 removeClicks;
	s8 waveformSpan; // WAVEFORM_SPAN_*
	s8 liveView; // LIVE_VIEW_*
	s8 screensOff; // turns the screens off while recording
	s8 pauseCleanup; // PAUSE_CLEANUP_*: more reduction while nobody speaks
} Settings;

enum {
	PRESET_VOICE,
	PRESET_MEETING,
	PRESET_MUSIC,
	PRESET_NATURE,
	PRESET_CUSTOM
};

#define PRESET_COUNT PRESET_CUSTOM // the presets to pick from; PRESET_CUSTOM only says that none matches

enum {
	LIVE_VIEW_WAVEFORM,
	LIVE_VIEW_SPECTRUM,
	LIVE_VIEW_BOTH,
	LIVE_VIEW_COUNT
};

// The stored values of the settings below, in the order of their value names.
enum {
	NOISE_REDUCTION_OFF,
	NOISE_REDUCTION_6_DB,
	NOISE_REDUCTION_12_DB,
	NOISE_REDUCTION_18_DB,
	NOISE_REDUCTION_24_DB,
	NOISE_REDUCTION_COUNT
};

enum {
	LOW_CUT_OFF,
	LOW_CUT_40_HZ,
	LOW_CUT_80_HZ,
	LOW_CUT_120_HZ,
	LOW_CUT_160_HZ,
	LOW_CUT_COUNT
};

enum {
	HUM_FILTER_OFF,
	HUM_FILTER_50_HZ,
	HUM_FILTER_60_HZ,
	HUM_FILTER_COUNT
};

enum {
	LOUDNESS_OFF,
	LOUDNESS_MINUS_23_LUFS,
	LOUDNESS_MINUS_19_LUFS,
	LOUDNESS_MINUS_16_LUFS,
	LOUDNESS_MINUS_14_LUFS,
	LOUDNESS_COUNT
};

enum {
	CLEAN_FORMAT_24_BIT,
	CLEAN_FORMAT_FLOAT,
	CLEAN_FORMAT_16_BIT,
	CLEAN_FORMAT_COUNT
};

enum {
	PAUSE_CLEANUP_OFF,
	PAUSE_CLEANUP_6_DB,
	PAUSE_CLEANUP_12_DB,
	PAUSE_CLEANUP_18_DB,
	PAUSE_CLEANUP_COUNT
};

enum {
	PRE_RECORD_OFF,
	PRE_RECORD_2_S,
	PRE_RECORD_5_S,
	PRE_RECORD_10_S,
	PRE_RECORD_COUNT
};

enum {
	SELF_TIMER_OFF,
	SELF_TIMER_3_S,
	SELF_TIMER_10_S,
	SELF_TIMER_COUNT
};

enum {
	WAVEFORM_SPAN_5_S,
	WAVEFORM_SPAN_10_S,
	WAVEFORM_SPAN_30_S,
	WAVEFORM_SPAN_COUNT
};

extern Settings g_settings;
extern const int PRE_RECORD_SECONDS[PRE_RECORD_COUNT];
extern const int SELF_TIMER_SECONDS[SELF_TIMER_COUNT];
extern const int WAVEFORM_SPAN_SECONDS[WAVEFORM_SPAN_COUNT];

// The MICU gain register value of a gain step.
int settings_mic_gain(int gainStep);
void settings_set_defaults(Settings *settings);
// The preset that matches the clean-up settings, or PRESET_CUSTOM.
int settings_match_preset(const Settings *settings);
void settings_apply_preset(Settings *settings, int preset);
// True for the clean-up fields of Settings (offsetof) that the presets set.
bool settings_is_preset_field(size_t fieldOffset);
bool settings_noise_reduction_on(const Settings *settings);
void settings_load(void);
void settings_save(void);
void settings_changed(void);
// Saves a change once a moment has passed without another one.
void settings_autosave(u64 now);
DevelopParams settings_develop_params(const Settings *settings);
