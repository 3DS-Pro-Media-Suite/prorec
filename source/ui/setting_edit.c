#include "setting_edit.h"
#include "app.h"
#include "gallery.h"
#include "live_analysis.h"
#include "mic.h"
#include "play.h"
#include "settings.h"
#include "take_writer.h"
#include "toast.h"

const char *setting_edit_disabled_reason(const SettingRow *row)
{
	if (setting_row_edits(row, SETTING_OFFSET(gainStep)) && take_writer_is_active()) {
		return "The gain stays fixed during a take";
	}
	bool needsNoiseReduction = setting_row_edits(row, SETTING_OFFSET(noiseSource)) ||
	                           setting_row_edits(row, SETTING_OFFSET(pauseCleanup));
	if (needsNoiseReduction && !settings_noise_reduction_on(&g_settings)) {
		return "Noise reduction is off";
	}
	bool needsIdleMic = row->action == ACTION_LEARN_NOISE || row->action == ACTION_RESET;
	if (needsIdleMic && take_writer_is_active()) {
		return "Stop the recording first";
	}
	return NULL;
}

bool setting_edit_refuse_disabled(const SettingRow *row)
{
	const char *disabledReason = setting_edit_disabled_reason(row);
	if (disabledReason) {
		toast_warning("%s", disabledReason);
	}
	return disabledReason != NULL;
}

// A clean-up setting that the presets set.
static bool sets_preset_field(const SettingRow *row)
{
	return row->kind != ROW_ACTION && settings_is_preset_field(row->offset);
}

void setting_edit_apply_change(const SettingRow *row)
{
	if (setting_row_edits(row, SETTING_OFFSET(preset))) {
		if (g_settings.preset < PRESET_CUSTOM) {
			settings_apply_preset(&g_settings, g_settings.preset);
		} else {
			// Custom only says that no preset matches. It is not a choice.
			g_settings.preset = settings_match_preset(&g_settings);
		}
	} else if (setting_row_edits(row, SETTING_OFFSET(gainStep))) {
		mic_set_gain(settings_mic_gain(g_settings.gainStep));
	} else if (setting_row_edits(row, SETTING_OFFSET(monitor))) {
		play_set_monitor(g_settings.monitor && !gallery_is_open());
	} else if (setting_row_edits(row, SETTING_OFFSET(waveformSpan))) {
		live_analysis_clear_waveform();
	}
	if (sets_preset_field(row)) {
		g_settings.preset = settings_match_preset(&g_settings);
	}
	settings_changed();
}

// Custom is never stepped onto. From it, a step up goes to the first preset, a step down to the last.
static int next_preset(int preset, int direction)
{
	if (preset >= PRESET_CUSTOM) {
		return direction > 0 ? 0 : PRESET_COUNT - 1;
	}
	return (preset + direction + PRESET_COUNT) % PRESET_COUNT;
}

// The presets and most settings go round; the gain stops at its ends.
void setting_edit_step(const SettingRow *row, int direction)
{
	if (row->kind == ROW_ACTION || setting_edit_refuse_disabled(row)) {
		return;
	}
	s8 *value = setting_row_field(row, &g_settings);
	if (setting_row_edits(row, SETTING_OFFSET(preset))) {
		*value = next_preset(*value, direction);
		setting_edit_apply_change(row);
		return;
	}
	if (setting_row_edits(row, SETTING_OFFSET(gainStep))) {
		int stepped = app_clamp(*value + direction, row->min, row->max);
		if (stepped == *value) {
			return;
		}
		*value = stepped;
	} else {
		int valueCount = row->max - row->min + 1;
		*value = row->min + ((*value - row->min + direction + valueCount) % valueCount);
	}
	setting_edit_apply_change(row);
}
