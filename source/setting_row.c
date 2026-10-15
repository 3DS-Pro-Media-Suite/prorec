#include "setting_row.h"
#include <stdio.h>
#include "app.h"
#include "array_count.h"
#include "ftp_control.h"
#include "mic.h"
#include "noise_learning.h"
#include "screen_power.h"

#define GAIN_STEP_COUNT (GAIN_STEP_MAX + 1)
#define GAIN_LABEL_SIZE 12 // a gain as text, such as "18 dB"
#define BUILT_LABEL_SIZE 48 // a label built from a constant

static char g_gainLabelText[GAIN_STEP_COUNT][GAIN_LABEL_SIZE];
static const char *g_gainLabels[GAIN_STEP_COUNT];
static char g_learnNoiseLabel[BUILT_LABEL_SIZE];
static char g_screensOffLabel[BUILT_LABEL_SIZE];

static const char *const ON_OFF_NAMES[] = {"Off", "On"};
static const char *const ON_OFF_SHORT_NAMES[] = {"OFF", "ON"};
static const char *const PRESET_NAMES[] = {"Voice (speech, close)", "Meeting / lecture (distant speech)",
                                           "Music", "Nature / ambience", "Custom"};
static const char *const PRESET_SHORT_NAMES[] = {"VOICE", "MEETING", "MUSIC", "NATURE", "CUSTOM"};
static const char *const NOISE_REDUCTION_NAMES[] = {"Off", "Light (6 dB)", "Medium (12 dB)", "Strong (18 dB)",
                                                    "Maximum (24 dB)"};
static const char *const NOISE_REDUCTION_SHORT_NAMES[] = {"OFF", "6 dB", "12 dB", "18 dB", "24 dB"};
static const char *const NOISE_SOURCE_NAMES[] = {"Learned mic profile", "Estimated from the take",
                                                 "Both (the larger per frequency)"};
static const char *const NOISE_SOURCE_SHORT_NAMES[] = {"PROFILE", "AUTO", "BOTH"};
static const char *const LOW_CUT_NAMES[] = {"Off", "40 Hz", "80 Hz", "120 Hz", "160 Hz"};
static const char *const LOW_CUT_SHORT_NAMES[] = {"OFF", "40 Hz", "80 Hz", "120 Hz", "160 Hz"};
static const char *const HUM_FILTER_NAMES[] = {"Off", "50 Hz mains (Europe)", "60 Hz mains (Americas)"};
static const char *const HUM_FILTER_SHORT_NAMES[] = {"OFF", "50 Hz", "60 Hz"};
static const char *const LOUDNESS_NAMES[] = {"Off (level kept)", "-23 LUFS (broadcast)", "-19 LUFS",
                                             "-16 LUFS (podcast, voice)", "-14 LUFS (loud)"};
static const char *const LOUDNESS_SHORT_NAMES[] = {"OFF", "-23", "-19", "-16", "-14"};
static const char *const CLEAN_FORMAT_NAMES[] = {"24-bit PCM", "32-bit float", "16-bit PCM (dithered)"};
static const char *const CLEAN_FORMAT_SHORT_NAMES[] = {"24-BIT", "FLOAT", "16-BIT"};
const char *const PRE_RECORD_NAMES[PRE_RECORD_COUNT] = {"Off", "2 s", "5 s", "10 s"};
static const char *const PRE_RECORD_SHORT_NAMES[] = {"OFF", "2 s", "5 s", "10 s"};
static const char *const SELF_TIMER_NAMES[] = {"Off", "3 s", "10 s"};
static const char *const WAVEFORM_SPAN_NAMES[] = {"5 s", "10 s", "30 s"};
static const char *const LIVE_VIEW_NAMES[] = {"Waveform", "Spectrum", "Waveform and spectrum"};
static const char *const SCREENS_OFF_NAMES[] = {"Off", g_screensOffLabel};
static const char *const VOICE_EQ_NAMES[] = {"Off (the mic as it is)",
                                             "Natural (undoes the 3DS mic's tinny treble)",
                                             "Broadcast (natural + warmth and presence)"};
static const char *const VOICE_EQ_SHORT_NAMES[] = {"OFF", "NATURAL", "BROADCAST"};
static const char *const PAUSE_CLEANUP_NAMES[] = {
        "Off", "6 dB quieter between words", "12 dB quieter between words", "18 dB quieter between words"};
static const char *const PAUSE_CLEANUP_SHORT_NAMES[] = {"OFF", "6 dB", "12 dB", "18 dB"};

// A setting with named values; the list of names gives its range.
#define VALUE_ROW(rowKind, rowLabel, rowTag, field, defaultChoice, names, shortNames) \
	{                                                                                 \
	        .label = rowLabel,                                                        \
	        .tag = rowTag,                                                            \
	        .kind = rowKind,                                                          \
	        .offset = SETTING_OFFSET(field),                                          \
	        .min = 0,                                                                 \
	        .max = ARRAY_COUNT(names) - 1,                                            \
	        .defaultValue = defaultChoice,                                            \
	        .valueNames = names,                                                      \
	        .shortValueNames = shortNames,                                            \
	        .action = ACTION_NONE,                                                    \
	}
#define CHOICE_ROW(rowLabel, rowTag, field, defaultChoice, names, shortNames) \
	VALUE_ROW(ROW_CHOICE, rowLabel, rowTag, field, defaultChoice, names, shortNames)
#define ON_OFF_ROW(rowLabel, rowTag, field, defaultChoice, names, shortNames) \
	VALUE_ROW(ROW_ON_OFF, rowLabel, rowTag, field, defaultChoice, names, shortNames)
// A row that runs an action and holds no value.
#define ACTION_ROW(rowLabel, rowAction) {.label = rowLabel, .kind = ROW_ACTION, .action = rowAction}

const SettingRow QUICK_ROWS[QUICK_TILE_COUNT] = {
        [QUICK_TILE_GAIN] = CHOICE_ROW("Mic gain (analog, before the converter)", "GAIN", gainStep,
                                       DEFAULT_GAIN_STEP, g_gainLabels, NULL),
        CHOICE_ROW("Preset (sets the clean-up tiles)", "PRESET", preset, PRESET_VOICE, PRESET_NAMES,
                   PRESET_SHORT_NAMES),
        CHOICE_ROW("Noise reduction", "NR", noiseReduction, NOISE_REDUCTION_12_DB, NOISE_REDUCTION_NAMES,
                   NOISE_REDUCTION_SHORT_NAMES),
        CHOICE_ROW("Noise estimate", "NOISE", noiseSource, NOISE_SOURCE_BOTH, NOISE_SOURCE_NAMES,
                   NOISE_SOURCE_SHORT_NAMES),
        CHOICE_ROW("Low cut (rumble, handling)", "LOW CUT", lowCut, LOW_CUT_80_HZ, LOW_CUT_NAMES,
                   LOW_CUT_SHORT_NAMES),
        CHOICE_ROW("Hum filter", "HUM", humFilter, HUM_FILTER_OFF, HUM_FILTER_NAMES, HUM_FILTER_SHORT_NAMES),
        CHOICE_ROW("Voice EQ (the 3DS mic is very bright: measured)", "VOICE EQ", voiceEq, VOICE_EQ_BROADCAST,
                   VOICE_EQ_NAMES, VOICE_EQ_SHORT_NAMES),
        ON_OFF_ROW("Leveler (gentle compressor)", "LEVEL", leveler, true, ON_OFF_NAMES, ON_OFF_SHORT_NAMES),
        CHOICE_ROW("Loudness of the clean file", "LOUDNESS", loudness, LOUDNESS_MINUS_16_LUFS, LOUDNESS_NAMES,
                   LOUDNESS_SHORT_NAMES),
        CHOICE_ROW("Clean file format", "FORMAT", cleanFormat, CLEAN_FORMAT_24_BIT, CLEAN_FORMAT_NAMES,
                   CLEAN_FORMAT_SHORT_NAMES),
        CHOICE_ROW("Pre-record (the take starts this much earlier)", "PRE-REC", preRecord, PRE_RECORD_2_S,
                   PRE_RECORD_NAMES, PRE_RECORD_SHORT_NAMES),
        CHOICE_ROW("Pause cleanup (needs noise reduction)", "PAUSES", pauseCleanup, PAUSE_CLEANUP_12_DB,
                   PAUSE_CLEANUP_NAMES, PAUSE_CLEANUP_SHORT_NAMES),
};

static const SettingRow RECORDING_ROWS[] = {
        CHOICE_ROW("Self-timer", NULL, selfTimer, SELF_TIMER_OFF, SELF_TIMER_NAMES, NULL),
        ON_OFF_ROW("Monitor on headphones  (Y)", NULL, monitor, false, ON_OFF_NAMES, NULL),
        ON_OFF_ROW("Develop each take when it stops", NULL, autoDevelop, true, ON_OFF_NAMES, NULL),
        ON_OFF_ROW("Remove button clicks", NULL, removeClicks, true, ON_OFF_NAMES, NULL),
        ACTION_ROW(g_learnNoiseLabel, ACTION_LEARN_NOISE),
        ACTION_ROW("Forget all learned noise profiles", ACTION_FORGET_NOISE),
};
static const SettingRow DISPLAY_ROWS[] = {
        CHOICE_ROW("Live view", NULL, liveView, LIVE_VIEW_BOTH, LIVE_VIEW_NAMES, NULL),
        CHOICE_ROW("Waveform span", NULL, waveformSpan, WAVEFORM_SPAN_10_S, WAVEFORM_SPAN_NAMES, NULL),
        ON_OFF_ROW("Screens off while recording", NULL, screensOff, false, SCREENS_OFF_NAMES, NULL),
};
static const SettingRow SYSTEM_ROWS[] = {
        ACTION_ROW("Recordings", ACTION_RECORDINGS),
        ACTION_ROW(g_ftpLabel, ACTION_FTP), // the label shows whether the server runs
        ACTION_ROW("Information", ACTION_INFORMATION),
        ACTION_ROW("Reset all settings", ACTION_RESET),
        ACTION_ROW("Exit Pro Rec", ACTION_EXIT),
};

#define MENU_PAGE(pageName, pageRows) {.name = pageName, .rows = pageRows, .count = ARRAY_COUNT(pageRows)}
const MenuPage MENU_PAGES[MENU_PAGE_COUNT] = {
        [MENU_PAGE_RECORDING] = MENU_PAGE("RECORDING", RECORDING_ROWS),
        [MENU_PAGE_DISPLAY] = MENU_PAGE("DISPLAY", DISPLAY_ROWS),
        [MENU_PAGE_SYSTEM] = MENU_PAGE("SYSTEM", SYSTEM_ROWS),
};

void setting_row_init_labels(void)
{
	for (int gainStep = 0; gainStep <= GAIN_STEP_MAX; gainStep++) {
		float gainDb = mic_gain_db(settings_mic_gain(gainStep));
		snprintf(g_gainLabelText[gainStep], sizeof g_gainLabelText[gainStep], "%.0f dB", gainDb);
		g_gainLabels[gainStep] = g_gainLabelText[gainStep];
	}
	snprintf(g_learnNoiseLabel, sizeof g_learnNoiseLabel, "Learn the mic's noise (%d s of quiet)",
	         NOISE_LEARNING_SECONDS);
	snprintf(g_screensOffLabel, sizeof g_screensOffLabel, "After %d s without a key",
	         SCREEN_POWER_IDLE_SECONDS);
}

bool setting_row_edits(const SettingRow *row, size_t fieldOffset)
{
	return row->kind != ROW_ACTION && row->offset == fieldOffset;
}

const SettingRow *setting_row_for_field(size_t fieldOffset)
{
	for (int i = 0; i < QUICK_TILE_COUNT; i++) {
		if (setting_row_edits(&QUICK_ROWS[i], fieldOffset)) {
			return &QUICK_ROWS[i];
		}
	}
	for (int page = 0; page < MENU_PAGE_COUNT; page++) {
		for (int i = 0; i < MENU_PAGES[page].count; i++) {
			if (setting_row_edits(&MENU_PAGES[page].rows[i], fieldOffset)) {
				return &MENU_PAGES[page].rows[i];
			}
		}
	}
	return NULL;
}

s8 *setting_row_field(const SettingRow *row, Settings *settings)
{
	return (s8 *)((char *)settings + row->offset);
}

static void clamp_to_row_range(const SettingRow *row, Settings *settings)
{
	s8 *value = setting_row_field(row, settings);
	*value = app_clamp(*value, row->min, row->max);
}

void setting_row_clamp_all(Settings *settings)
{
	for (int i = 0; i < QUICK_TILE_COUNT; i++) {
		clamp_to_row_range(&QUICK_ROWS[i], settings);
	}
	for (int page = 0; page < MENU_PAGE_COUNT; page++) {
		for (int i = 0; i < MENU_PAGES[page].count; i++) {
			const SettingRow *row = &MENU_PAGES[page].rows[i];
			if (row->kind == ROW_ACTION) {
				continue;
			}
			clamp_to_row_range(row, settings);
		}
	}
}

void setting_row_value_name(const SettingRow *row, int storedValue, char *out, size_t outSize, bool shortForm)
{
	if (row->kind == ROW_ACTION) {
		snprintf(out, outSize, "%s", "");
		return;
	}
	bool useShortNames = shortForm && row->shortValueNames;
	const char *const *names = useShortNames ? row->shortValueNames : row->valueNames;
	snprintf(out, outSize, "%s", names[storedValue - row->min]);
}

void setting_row_value(const SettingRow *row, char *out, size_t outSize, bool shortForm)
{
	setting_row_value_name(row, *setting_row_field(row, &g_settings), out, outSize, shortForm);
}
