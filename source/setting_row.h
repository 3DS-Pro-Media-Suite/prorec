// The menu rows of the settings: every setting is a row with a label, a range and the names of its values.
// The quick menu shows QUICK_TILE_COUNT of them as tiles; the setup menu lists the rest on its pages.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "settings.h"

typedef enum {
	ROW_CHOICE,
	ROW_ON_OFF,
	ROW_ACTION
} RowKind;

typedef enum {
	ACTION_NONE,
	ACTION_LEARN_NOISE,
	ACTION_FORGET_NOISE,
	ACTION_RECORDINGS,
	ACTION_FTP,
	ACTION_INFORMATION,
	ACTION_RESET,
	ACTION_EXIT
} RowAction;

typedef struct {
	const char *label; // full name, in menu rows and the quick menu's value bar
	const char *tag; // short name on a quick-menu tile
	RowKind kind;
	size_t offset; // the field in Settings; not used by ROW_ACTION
	int min;
	int max;
	int defaultValue;
	const char *const *valueNames; // ROW_CHOICE and ROW_ON_OFF
	const char *const *shortValueNames; // names on a quick-menu tile; NULL uses valueNames
	RowAction action;
} SettingRow;

#define SETTING_OFFSET(field) offsetof(Settings, field)
#define QUICK_TILE_COUNT 12
#define QUICK_TILE_GAIN 0

typedef struct {
	const char *name;
	const SettingRow *rows;
	int count;
} MenuPage;

enum {
	MENU_PAGE_RECORDING,
	MENU_PAGE_DISPLAY,
	MENU_PAGE_SYSTEM,
	MENU_PAGE_COUNT
};

extern const SettingRow QUICK_ROWS[QUICK_TILE_COUNT];
extern const MenuPage MENU_PAGES[MENU_PAGE_COUNT];
extern const char *const PRE_RECORD_NAMES[PRE_RECORD_COUNT];

// The labels that are built from constants; call before anything shows them.
void setting_row_init_labels(void);
// True when the row edits the field of Settings at fieldOffset. Action rows edit no field.
bool setting_row_edits(const SettingRow *row, size_t fieldOffset);
// The row that edits the field at fieldOffset.
const SettingRow *setting_row_for_field(size_t fieldOffset);
// The field of Settings that a row edits.
s8 *setting_row_field(const SettingRow *row, Settings *settings);
// Brings every field into its row's range: a file on the card may hold values out of range.
void setting_row_clamp_all(Settings *settings);
// The name of a stored value of a row; shortForm picks the name on a quick-menu tile.
void setting_row_value_name(const SettingRow *row, int storedValue, char *out, size_t outSize,
                            bool shortForm);
// The name of the row's current value.
void setting_row_value(const SettingRow *row, char *out, size_t outSize, bool shortForm);
