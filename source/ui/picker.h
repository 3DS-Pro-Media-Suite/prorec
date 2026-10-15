// The value picker: a grid of all values of one setting, opened from a quick-menu tile or a setup-menu row.
#pragma once
#include <3ds.h>
#include <stdbool.h>
#include "setting_row.h"

bool picker_is_open(void);
void picker_open(const SettingRow *row);
void picker_close(void);
void picker_draw(void);
void picker_input(u32 down, u32 held, touchPosition touch);
