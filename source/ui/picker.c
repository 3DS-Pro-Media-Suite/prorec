#include "picker.h"
#include "draw.h"
#include "input.h"
#include "setting_edit.h"
#include "setting_row.h"
#include "settings.h"
#include "toast.h"

// Up to PICKER_ONE_COLUMN_MAX values stand in one column, up to PICKER_TWO_COLUMNS_MAX in two, more in three.
#define PICKER_ONE_COLUMN_MAX 6
#define PICKER_TWO_COLUMNS_MAX 14
#define PICKER_MAX_COLUMNS 3
#define PICKER_AREA_HEIGHT 204.0f
#define PICKER_CELL_GAP 4
#define PICKER_CELL_MAX_HEIGHT 30
#define PICKER_FULL_TEXT_CELL_HEIGHT 22.0f // lower cells shrink their text
#define PICKER_TEXT_HEIGHT 16 // the body font at full size
#define PICKER_TEXT_PADDING 6
#define PICKER_VALUE_SIZE 48
#define PICKER_DEFAULT_DOT_X 7 // marks the default value
#define PICKER_DEFAULT_DOT_RADIUS 2

static const SettingRow *g_pickerRow;
static int g_pickerSelected;

bool picker_is_open(void)
{
	return g_pickerRow != NULL;
}

void picker_close(void)
{
	g_pickerRow = NULL;
}

static int value_count(const SettingRow *row)
{
	return row->max - row->min + 1;
}

static int column_count(const SettingRow *row)
{
	int valueCount = value_count(row);
	if (valueCount <= PICKER_ONE_COLUMN_MAX) {
		return 1;
	}
	if (valueCount <= PICKER_TWO_COLUMNS_MAX) {
		return 2;
	}
	return PICKER_MAX_COLUMNS;
}

// The values fill each column from top to bottom.
static void cell_rect(const SettingRow *row, int index, float *x, float *y, float *width, float *height)
{
	int valueCount = value_count(row);
	int columns = column_count(row);
	int rowsPerColumn = (valueCount + columns - 1) / columns;
	float cellHeight = (PICKER_AREA_HEIGHT - (rowsPerColumn - 1) * PICKER_CELL_GAP) / rowsPerColumn;
	if (cellHeight > PICKER_CELL_MAX_HEIGHT) {
		cellHeight = PICKER_CELL_MAX_HEIGHT;
	}
	*width = ((float)PANEL_WIDTH - (columns - 1) * PICKER_CELL_GAP) / columns;
	*height = cellHeight;
	*x = PANEL_MARGIN + (index / rowsPerColumn) * (*width + PICKER_CELL_GAP);
	*y = PANEL_CONTENT_Y + (index % rowsPerColumn) * (cellHeight + PICKER_CELL_GAP);
}

static void draw_cell(const SettingRow *row, int index, bool isCurrent)
{
	float x;
	float y;
	float width;
	float height;
	cell_rect(row, index, &x, &y, &width, &height);
	if (index == g_pickerSelected) {
		draw_rounded_rect(x - TILE_SELECT_GROW, y - TILE_SELECT_GROW, width + 2 * TILE_SELECT_GROW,
		                  height + 2 * TILE_SELECT_GROW, TILE_SELECT_RADIUS, COLOR_ACCENT, DEPTH_PANEL);
	}
	u32 tileColor = isCurrent ? COLOR_TILE_SELECTED : COLOR_TILE;
	draw_rounded_rect(x, y, width, height, TILE_RADIUS, tileColor, DEPTH_PANEL_RAISED);
	char valueName[PICKER_VALUE_SIZE];
	setting_row_value_name(row, row->min + index, valueName, sizeof valueName, false);
	float textScale = height < PICKER_FULL_TEXT_CELL_HEIGHT ? height / PICKER_FULL_TEXT_CELL_HEIGHT : 1.0f;
	float textY = y + (height - PICKER_TEXT_HEIGHT * textScale) / 2 - 1;
	u32 textColor = isCurrent ? COLOR_ACCENT : COLOR_INK;
	draw_text_fit(g_fontBody, x + width / 2, textY, textScale, textColor, ALIGN_CENTER,
	              width - 2 * PICKER_TEXT_PADDING, valueName);
	if (row->min + index == row->defaultValue) {
		draw_circle(x + PICKER_DEFAULT_DOT_X, y + height / 2, PICKER_DEFAULT_DOT_RADIUS, COLOR_FAINT,
		            DEPTH_CONTROL);
	}
}

void picker_draw(void)
{
	const SettingRow *row = g_pickerRow;
	draw_panel_header("P", row->label);
	draw_text(g_fontSmall, PANEL_RIGHT_X, PANEL_TITLE_Y, COLOR_FAINT, ALIGN_RIGHT, "B back");
	int currentIndex = *setting_row_field(row, &g_settings) - row->min;
	for (int i = 0; i < value_count(row); i++) {
		draw_cell(row, i, i == currentIndex);
	}
}

void picker_open(const SettingRow *row)
{
	if (row->kind == ROW_ACTION) {
		return;
	}
	const char *disabledReason = setting_edit_disabled_reason(row);
	if (disabledReason) {
		toast_warning("%s", disabledReason);
		return;
	}
	g_pickerRow = row;
	g_pickerSelected = *setting_row_field(row, &g_settings) - row->min;
}

static void choose_value(int index)
{
	const SettingRow *row = g_pickerRow;
	g_pickerRow = NULL;
	// A take can start while the picker is open.
	const char *disabledReason = setting_edit_disabled_reason(row);
	if (disabledReason) {
		toast_warning("%s", disabledReason);
		return;
	}
	s8 *value = setting_row_field(row, &g_settings);
	if (index < 0 || index >= value_count(row) || *value == row->min + index) {
		return;
	}
	*value = row->min + index;
	setting_edit_apply_change(row);
}

// D-pad and A pick a value, a tap on a cell picks it, a tap elsewhere or B closes the picker.
void picker_input(u32 down, u32 held, touchPosition touch)
{
	const SettingRow *row = g_pickerRow;
	int valueCount = value_count(row);
	if (down & (KEY_B | KEY_START)) {
		g_pickerRow = NULL;
		return;
	}
	if (input_dpad_repeat(held, KEY_DDOWN, INPUT_REPEAT_FAST)) {
		g_pickerSelected = (g_pickerSelected + 1) % valueCount;
	}
	if (input_dpad_repeat(held, KEY_DUP, INPUT_REPEAT_FAST)) {
		g_pickerSelected = (g_pickerSelected + valueCount - 1) % valueCount;
	}
	if (down & KEY_A) {
		choose_value(g_pickerSelected);
		return;
	}
	if (!(down & KEY_TOUCH)) {
		return;
	}
	int touchedIndex = -1;
	for (int i = 0; i < valueCount && touchedIndex < 0; i++) {
		float x;
		float y;
		float width;
		float height;
		cell_rect(row, i, &x, &y, &width, &height);
		if (input_in_rect(touch, x, y, width, height)) {
			touchedIndex = i;
		}
	}
	if (touchedIndex >= 0) {
		choose_value(touchedIndex);
	} else {
		g_pickerRow = NULL;
	}
}
