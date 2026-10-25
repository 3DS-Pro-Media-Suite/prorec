// Changing a setting from a menu: whether it can be used now, one step of it, and applying a change.
#pragma once
#include "setting_row.h"

// Why a setting cannot be used right now (it shows greyed out), or NULL.
const char *setting_edit_disabled_reason(const SettingRow *row);
// True when the setting cannot be used right now; a toast then says why.
bool setting_edit_refuse_disabled(const SettingRow *row);
// Puts a changed setting into effect and marks the settings for saving.
void setting_edit_apply_change(const SettingRow *row);
// Steps a value row by one value: ARROW_LEFT steps down, ARROW_RIGHT up.
void setting_edit_step(const SettingRow *row, int direction);
