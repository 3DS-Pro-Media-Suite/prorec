#include "settings.h"
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "array_count.h"
#include "develop.h"
#include "mic.h"
#include "setting_row.h"
#include "wav.h"

#define SETTINGS_PATH APP_DIR "/prorec.cfg"
#define SETTINGS_FILE_MAGIC 0x50524332u // "PRC2"; change it when Settings changes, so old files are ignored
#define SAVE_DELAY_MS 1500 // a change is saved after this long without another change

#define MIC_GAIN_PER_STEP 6 // the MICU gain counts in 0.5 dB; a gain step is 3 dB

typedef struct {
	u32 magic;
	u32 size;
	Settings settings;
} SettingsFile;

// The clean-up settings a preset sets. settings_is_preset_field lists the same fields.
typedef struct {
	s8 noiseReduction;
	s8 noiseSource;
	s8 lowCut;
	s8 humFilter;
	s8 voiceEq;
	s8 leveler;
	s8 loudness;
	s8 pauseCleanup;
} Preset;

static const Preset PRESETS[PRESET_COUNT] = {
        [PRESET_VOICE] =
                {
                        .noiseReduction = NOISE_REDUCTION_12_DB,
                        .noiseSource = NOISE_SOURCE_BOTH,
                        .lowCut = LOW_CUT_120_HZ,
                        .humFilter = HUM_FILTER_OFF,
                        .voiceEq = VOICE_EQ_BROADCAST,
                        .leveler = true,
                        .loudness = LOUDNESS_MINUS_16_LUFS,
                        .pauseCleanup = PAUSE_CLEANUP_12_DB,
                },
        [PRESET_MEETING] =
                {
                        .noiseReduction = NOISE_REDUCTION_18_DB,
                        .noiseSource = NOISE_SOURCE_BOTH,
                        .lowCut = LOW_CUT_120_HZ,
                        .humFilter = HUM_FILTER_OFF,
                        .voiceEq = VOICE_EQ_BROADCAST,
                        .leveler = true,
                        .loudness = LOUDNESS_MINUS_16_LUFS,
                        .pauseCleanup = PAUSE_CLEANUP_12_DB,
                },
        [PRESET_MUSIC] =
                {
                        .noiseReduction = NOISE_REDUCTION_6_DB,
                        .noiseSource = NOISE_SOURCE_PROFILE,
                        .lowCut = LOW_CUT_40_HZ,
                        .humFilter = HUM_FILTER_OFF,
                        .voiceEq = VOICE_EQ_NATURAL,
                        .leveler = false,
                        .loudness = LOUDNESS_MINUS_19_LUFS,
                        .pauseCleanup = PAUSE_CLEANUP_OFF,
                },
        [PRESET_NATURE] =
                {
                        .noiseReduction = NOISE_REDUCTION_6_DB,
                        .noiseSource = NOISE_SOURCE_PROFILE,
                        .lowCut = LOW_CUT_40_HZ,
                        .humFilter = HUM_FILTER_OFF,
                        .voiceEq = VOICE_EQ_NATURAL,
                        .leveler = false,
                        .loudness = LOUDNESS_OFF,
                        .pauseCleanup = PAUSE_CLEANUP_OFF,
                },
};
static const int PAUSE_CLEANUP_DB[PAUSE_CLEANUP_COUNT] = {0, 6, 12, 18};
static const int NOISE_REDUCTION_DB[NOISE_REDUCTION_COUNT] = {0, 6, 12, 18, 24};
static const int LOW_CUT_HZ[LOW_CUT_COUNT] = {0, 40, 80, 120, 160};
static const int HUM_FILTER_HZ[HUM_FILTER_COUNT] = {
        [HUM_FILTER_OFF] = 0,
        [HUM_FILTER_50_HZ] = MAINS_HZ_EUROPE,
        [HUM_FILTER_60_HZ] = MAINS_HZ_AMERICAS,
};
static const int LOUDNESS_LUFS[LOUDNESS_COUNT] = {0, -23, -19, -16, -14};
static const int CLEAN_FORMAT_WAV[CLEAN_FORMAT_COUNT] = {
        [CLEAN_FORMAT_24_BIT] = WAV_S24,
        [CLEAN_FORMAT_FLOAT] = WAV_F32,
        [CLEAN_FORMAT_16_BIT] = WAV_S16,
};
const int PRE_RECORD_SECONDS[PRE_RECORD_COUNT] = {0, 2, 5, 10};
const int SELF_TIMER_SECONDS[SELF_TIMER_COUNT] = {0, 3, 10};
const int WAVEFORM_SPAN_SECONDS[WAVEFORM_SPAN_COUNT] = {5, 10, 30};

Settings g_settings;

int settings_mic_gain(int gainStep)
{
	int micGain = gainStep * MIC_GAIN_PER_STEP;
	if (micGain > MIC_GAIN_MAX) {
		return MIC_GAIN_MAX;
	}
	return micGain;
}

static void copy_preset(Settings *settings, const Preset *preset)
{
	settings->noiseReduction = preset->noiseReduction;
	settings->noiseSource = preset->noiseSource;
	settings->lowCut = preset->lowCut;
	settings->humFilter = preset->humFilter;
	settings->voiceEq = preset->voiceEq;
	settings->leveler = preset->leveler;
	settings->loudness = preset->loudness;
	settings->pauseCleanup = preset->pauseCleanup;
}

void settings_set_defaults(Settings *settings)
{
	memset(settings, 0, sizeof *settings);
	settings->gainStep = DEFAULT_GAIN_STEP;
	settings->preset = PRESET_VOICE;
	copy_preset(settings, &PRESETS[PRESET_VOICE]);
	settings->cleanFormat = CLEAN_FORMAT_24_BIT;
	settings->preRecord = PRE_RECORD_2_S;
	settings->autoDevelop = true;
	settings->removeClicks = true;
	settings->waveformSpan = WAVEFORM_SPAN_10_S;
	settings->liveView = LIVE_VIEW_BOTH;
}

bool settings_is_preset_field(size_t fieldOffset)
{
	static const size_t PRESET_FIELDS[] = {
	        offsetof(Settings, noiseReduction), offsetof(Settings, noiseSource),  offsetof(Settings, lowCut),
	        offsetof(Settings, humFilter),      offsetof(Settings, voiceEq),      offsetof(Settings, leveler),
	        offsetof(Settings, loudness),       offsetof(Settings, pauseCleanup),
	};
	for (int i = 0; i < ARRAY_COUNT(PRESET_FIELDS); i++) {
		if (PRESET_FIELDS[i] == fieldOffset) {
			return true;
		}
	}
	return false;
}

bool settings_noise_reduction_on(const Settings *settings)
{
	return settings->noiseReduction != NOISE_REDUCTION_OFF;
}

static bool preset_matches(const Settings *settings, const Preset *preset)
{
	return settings->noiseReduction == preset->noiseReduction &&
	       settings->noiseSource == preset->noiseSource && settings->lowCut == preset->lowCut &&
	       settings->humFilter == preset->humFilter && settings->voiceEq == preset->voiceEq &&
	       settings->leveler == preset->leveler && settings->loudness == preset->loudness &&
	       settings->pauseCleanup == preset->pauseCleanup;
}

int settings_match_preset(const Settings *settings)
{
	for (int preset = 0; preset < PRESET_COUNT; preset++) {
		if (preset_matches(settings, &PRESETS[preset])) {
			return preset;
		}
	}
	return PRESET_CUSTOM;
}

void settings_apply_preset(Settings *settings, int preset)
{
	if (preset < 0 || preset >= PRESET_COUNT) {
		return;
	}
	copy_preset(settings, &PRESETS[preset]);
	settings->preset = preset;
}

// A file on the card may hold values out of range.
static void make_valid(Settings *settings)
{
	setting_row_clamp_all(settings);
	settings->preset = settings_match_preset(settings);
}

void settings_load(void)
{
	settings_set_defaults(&g_settings);
	FILE *file = fopen(SETTINGS_PATH, "rb");
	if (!file) {
		return;
	}
	SettingsFile saved;
	size_t bytesRead = fread(&saved, 1, sizeof saved, file);
	bool isValid =
	        bytesRead == sizeof saved && saved.magic == SETTINGS_FILE_MAGIC && saved.size == sizeof saved;
	if (isValid) {
		g_settings = saved.settings;
		make_valid(&g_settings);
	}
	fclose(file);
}

static u64 g_settingsChangedAt; // when an unsaved change happened; 0 when everything is saved

void settings_save(void)
{
	g_settingsChangedAt = 0;
	SettingsFile saved = {.magic = SETTINGS_FILE_MAGIC, .size = sizeof saved, .settings = g_settings};
	FILE *file = fopen(SETTINGS_PATH, "wb");
	if (!file) {
		return;
	}
	fwrite(&saved, 1, sizeof saved, file);
	fclose(file);
}

// Held keys step a setting every few frames: the main loop saves once they stop, not on every step.
void settings_changed(void)
{
	g_settingsChangedAt = osGetTime();
}

void settings_autosave(u64 now)
{
	if (g_settingsChangedAt && now - g_settingsChangedAt > SAVE_DELAY_MS) {
		settings_save();
	}
}

DevelopParams settings_develop_params(const Settings *settings)
{
	DevelopParams params;
	memset(&params, 0, sizeof params);
	params.lowCutHz = LOW_CUT_HZ[settings->lowCut];
	params.humHz = HUM_FILTER_HZ[settings->humFilter];
	params.noiseReductionDb = NOISE_REDUCTION_DB[settings->noiseReduction];
	params.noiseSource = settings->noiseSource;
	params.voiceEq = settings->voiceEq;
	params.leveler = settings->leveler;
	params.targetLufs = LOUDNESS_LUFS[settings->loudness];
	params.outputFormat = CLEAN_FORMAT_WAV[settings->cleanFormat];
	params.removeClicks = settings->removeClicks;
	params.pauseCleanupDb = PAUSE_CLEANUP_DB[settings->pauseCleanup];
	return params;
}
