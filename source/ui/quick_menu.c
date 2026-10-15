#include "quick_menu.h"
#include <stdlib.h>
#include "draw.h"
#include "input.h"
#include "menu_frame.h"
#include "mic.h"
#include "picker.h"
#include "profile_store.h"
#include "recorder.h"
#include "setting_edit.h"
#include "setting_row.h"
#include "settings.h"
#include "take_writer.h"

// A grid of tiles.
#define QUICK_GRID_COLUMNS 4
#define QUICK_GRID_ROWS 3
#define QUICK_TILES_Y 26
#define QUICK_TILE_WIDTH 74
#define QUICK_TILE_HEIGHT 44
#define QUICK_TILE_GAP_X 4
#define QUICK_TILE_GAP_Y 5
#define QUICK_TAG_X 7 // from the top left corner of the tile
#define QUICK_TAG_Y 5
#define QUICK_VALUE_Y 20
#define QUICK_VALUE_PADDING 5
#define QUICK_VALUE_SIZE 40
#define QUICK_CHANGED_DOT_INSET 8 // from the right edge of the tile
#define QUICK_CHANGED_DOT_Y 10
#define QUICK_CHANGED_DOT_RADIUS 2.2f

// The value bar under the tiles: the selected setting in full, a hint and two arrows that step it.
#define VALUE_BAR_LINE_Y 174
#define VALUE_BAR_LABEL_Y 179
#define VALUE_BAR_VALUE_Y 193
#define VALUE_BAR_VALUE_WIDTH 172
#define VALUE_BAR_VALUE_SIZE 48
#define VALUE_BAR_HINT_Y 216
#define VALUE_BAR_ARROW_LEFT_X 190
#define VALUE_BAR_ARROW_RIGHT_X 226
#define VALUE_BAR_ARROW_Y 202
#define VALUE_BAR_ARROW_WIDTH 32
#define VALUE_BAR_ARROW_HEIGHT 30
#define VALUE_BAR_ARROW_SYMBOL_SIZE 5

#define NOISY_GAIN_BELOW_DB 12 // below this gain the converter adds noise
#define DIAL_FAST_TRAVEL 130 // the circle pad pushed this far turns the setting faster
#define STICK_AXIS_DOMINANCE 1.5f // a push counts only when it is this much stronger along the axis

static int g_quickSelected;

static void quick_tile_position(int index, float *x, float *y)
{
	*x = PANEL_MARGIN + (index % QUICK_GRID_COLUMNS) * (QUICK_TILE_WIDTH + QUICK_TILE_GAP_X);
	*y = QUICK_TILES_Y + (index / QUICK_GRID_COLUMNS) * (QUICK_TILE_HEIGHT + QUICK_TILE_GAP_Y);
}

static void draw_quick_tile(int index)
{
	const SettingRow *row = &QUICK_ROWS[index];
	float x;
	float y;
	quick_tile_position(index, &x, &y);
	bool selected = index == g_quickSelected;
	bool disabled = setting_edit_disabled_reason(row) != NULL;
	if (selected) {
		draw_rounded_rect(x - TILE_SELECT_GROW, y - TILE_SELECT_GROW, QUICK_TILE_WIDTH + 2 * TILE_SELECT_GROW,
		                  QUICK_TILE_HEIGHT + 2 * TILE_SELECT_GROW, TILE_SELECT_RADIUS, COLOR_ACCENT,
		                  DEPTH_PANEL);
	}
	u32 tileColor = selected ? COLOR_TILE_SELECTED : COLOR_TILE;
	draw_rounded_rect(x, y, QUICK_TILE_WIDTH, QUICK_TILE_HEIGHT, TILE_RADIUS, tileColor, DEPTH_PANEL_RAISED);
	u32 tagColor = menu_frame_setting_color(disabled, selected, COLOR_ACCENT, COLOR_MUTED);
	draw_text(g_fontSmall, x + QUICK_TAG_X, y + QUICK_TAG_Y, tagColor, ALIGN_LEFT, "%s", row->tag);
	if (!disabled && *setting_row_field(row, &g_settings) != row->defaultValue) {
		draw_circle(x + QUICK_TILE_WIDTH - QUICK_CHANGED_DOT_INSET, y + QUICK_CHANGED_DOT_Y,
		            QUICK_CHANGED_DOT_RADIUS, COLOR_ACCENT, DEPTH_CONTROL);
	}
	char value[QUICK_VALUE_SIZE];
	setting_row_value(row, value, sizeof value, true);
	u32 valueColor = menu_frame_setting_color(disabled, selected, COLOR_INK, COLOR_SOFT);
	draw_text_fit(g_fontBody, x + QUICK_TILE_WIDTH / 2, y + QUICK_VALUE_Y, 1.0f, valueColor, ALIGN_CENTER,
	              QUICK_TILE_WIDTH - 2 * QUICK_VALUE_PADDING, value);
}

// Which learned noise profile the clean-up uses at the current gain.
static void draw_noise_profile_hint(void)
{
	int gain = settings_mic_gain(g_settings.gainStep);
	int profileGain = profile_store_nearest(gain);
	if (profileGain < 0) {
		draw_text(g_fontSmall, PANEL_TEXT_X, VALUE_BAR_HINT_Y, COLOR_WARNING, ALIGN_LEFT,
		          "No mic profile yet: START > Learn");
	} else if (profileGain == gain) {
		draw_text(g_fontSmall, PANEL_TEXT_X, VALUE_BAR_HINT_Y, COLOR_FAINT, ALIGN_LEFT,
		          "Mic profile learned at this gain");
	} else {
		draw_text(g_fontSmall, PANEL_TEXT_X, VALUE_BAR_HINT_Y, COLOR_FAINT, ALIGN_LEFT,
		          "Mic profile from %.0f dB gain", mic_gain_db(profileGain));
	}
}

static void draw_value_hint(const SettingRow *row)
{
	bool isNoiseSetting = setting_row_edits(row, SETTING_OFFSET(noiseSource)) ||
	                      setting_row_edits(row, SETTING_OFFSET(noiseReduction));
	bool isGainSetting = setting_row_edits(row, SETTING_OFFSET(gainStep));
	float gainDb = mic_gain_db(settings_mic_gain(g_settings.gainStep));
	if (isNoiseSetting) {
		draw_noise_profile_hint();
	} else if (isGainSetting && gainDb < NOISY_GAIN_BELOW_DB) {
		draw_text(g_fontSmall, PANEL_TEXT_X, VALUE_BAR_HINT_Y, COLOR_WARNING, ALIGN_LEFT,
		          "Below %d dB is noisy. Use 18-24 dB", NOISY_GAIN_BELOW_DB);
	} else if (isGainSetting) {
		draw_text(g_fontSmall, PANEL_TEXT_X, VALUE_BAR_HINT_Y, COLOR_FAINT, ALIGN_LEFT,
		          "Speech should peak near -12 dB");
	} else if (take_writer_is_active()) {
		draw_text(g_fontSmall, PANEL_TEXT_X, VALUE_BAR_HINT_Y, COLOR_FAINT, ALIGN_LEFT, "L marker   X pause");
	}
}

static void draw_step_arrow(float x, int direction)
{
	draw_rounded_rect(x, VALUE_BAR_ARROW_Y, VALUE_BAR_ARROW_WIDTH, VALUE_BAR_ARROW_HEIGHT, TILE_RADIUS,
	                  COLOR_TILE, DEPTH_CONTROL);
	float centerX = x + VALUE_BAR_ARROW_WIDTH / 2;
	float centerY = VALUE_BAR_ARROW_Y + VALUE_BAR_ARROW_HEIGHT / 2;
	draw_arrow(centerX, centerY, VALUE_BAR_ARROW_SYMBOL_SIZE, direction, COLOR_INK, DEPTH_CONTROL_RAISED);
}

static void draw_value_bar(void)
{
	draw_hairline(VALUE_BAR_LINE_Y);
	const SettingRow *row = &QUICK_ROWS[g_quickSelected];
	char value[VALUE_BAR_VALUE_SIZE];
	setting_row_value(row, value, sizeof value, false);
	draw_text(g_fontSmall, PANEL_TEXT_X, VALUE_BAR_LABEL_Y, COLOR_MUTED, ALIGN_LEFT, "%s", row->label);
	u32 valueColor = setting_edit_disabled_reason(row) ? COLOR_FAINT : COLOR_INK;
	draw_text_fit(g_fontBody, PANEL_TEXT_X, VALUE_BAR_VALUE_Y, 1.0f, valueColor, ALIGN_LEFT,
	              VALUE_BAR_VALUE_WIDTH, value);
	draw_value_hint(row);
	draw_step_arrow(VALUE_BAR_ARROW_LEFT_X, ARROW_LEFT);
	draw_step_arrow(VALUE_BAR_ARROW_RIGHT_X, ARROW_RIGHT);
}

void quick_menu_draw(void)
{
	menu_frame_draw_header("Q", "QUICK MENU");
	for (int i = 0; i < QUICK_TILE_COUNT; i++) {
		draw_quick_tile(i);
	}
	draw_value_bar();
	menu_frame_draw_record_button();
}

// A stick push along one axis: -1, 0 or 1. It counts only when the stick lies mostly along that axis.
static int stick_push(int along, int across)
{
	int travel = abs(along);
	if (travel <= INPUT_STICK_THRESHOLD || travel <= abs(across) * STICK_AXIS_DOMINANCE) {
		return 0;
	}
	return along > 0 ? 1 : -1;
}

// The gain from the circle pad (up / down) or the C-stick (up / down alone).
static int gain_push(circlePosition circle, u32 held)
{
	int padPush = stick_push(circle.dy, circle.dx);
	if (padPush) {
		return padPush;
	}
	bool cStickVertical = held & (KEY_CSTICK_UP | KEY_CSTICK_DOWN);
	bool cStickHorizontal = held & (KEY_CSTICK_LEFT | KEY_CSTICK_RIGHT);
	if (!cStickVertical || cStickHorizontal) {
		return 0;
	}
	return held & KEY_CSTICK_UP ? 1 : -1;
}

static void quick_select_input(u32 held)
{
	int column = g_quickSelected % QUICK_GRID_COLUMNS;
	int row = g_quickSelected / QUICK_GRID_COLUMNS;
	if (input_dpad_repeat(held, KEY_DRIGHT, INPUT_REPEAT_NORMAL)) {
		column = (column + 1) % QUICK_GRID_COLUMNS;
	}
	if (input_dpad_repeat(held, KEY_DLEFT, INPUT_REPEAT_NORMAL)) {
		column = (column + QUICK_GRID_COLUMNS - 1) % QUICK_GRID_COLUMNS;
	}
	if (input_dpad_repeat(held, KEY_DDOWN, INPUT_REPEAT_NORMAL)) {
		row = (row + 1) % QUICK_GRID_ROWS;
	}
	if (input_dpad_repeat(held, KEY_DUP, INPUT_REPEAT_NORMAL)) {
		row = (row + QUICK_GRID_ROWS - 1) % QUICK_GRID_ROWS;
	}
	g_quickSelected = row * QUICK_GRID_COLUMNS + column;
}

// Holding an arrow of the value bar keeps stepping the selected tile.
static void quick_arrows_input(u32 held, touchPosition touch)
{
	static int arrowRepeat;
	if (!(held & KEY_TOUCH)) {
		arrowRepeat = 0;
		return;
	}
	int direction = 0; // no arrow touched
	if (input_in_rect(touch, VALUE_BAR_ARROW_LEFT_X, VALUE_BAR_ARROW_Y, VALUE_BAR_ARROW_WIDTH,
	                  VALUE_BAR_ARROW_HEIGHT)) {
		direction = ARROW_LEFT;
	} else if (input_in_rect(touch, VALUE_BAR_ARROW_RIGHT_X, VALUE_BAR_ARROW_Y, VALUE_BAR_ARROW_WIDTH,
	                         VALUE_BAR_ARROW_HEIGHT)) {
		direction = ARROW_RIGHT;
	}
	if (input_repeat(direction != 0, &arrowRepeat, INPUT_REPEAT_FAST)) {
		setting_edit_step(&QUICK_ROWS[g_quickSelected], direction);
	}
}

// A tap selects a tile; a second tap on the selected tile opens its picker.
static void quick_tiles_touch_input(touchPosition touch)
{
	for (int i = 0; i < QUICK_TILE_COUNT; i++) {
		float x;
		float y;
		quick_tile_position(i, &x, &y);
		if (!input_in_rect(touch, x, y, QUICK_TILE_WIDTH, QUICK_TILE_HEIGHT)) {
			continue;
		}
		if (i == g_quickSelected) {
			picker_open(&QUICK_ROWS[i]);
		}
		g_quickSelected = i;
	}
}

void quick_menu_input(u32 down, u32 held, touchPosition touch)
{
	static int gainRepeat;
	static int dialRepeat;
	circlePosition circle = {0};
	hidCircleRead(&circle);
	int gainDirection = gain_push(circle, held);
	if (input_repeat(gainDirection != 0, &gainRepeat, INPUT_REPEAT_SLOW)) {
		setting_edit_step(&QUICK_ROWS[QUICK_TILE_GAIN], gainDirection);
	}
	quick_select_input(held);
	int dialDirection = stick_push(circle.dx, circle.dy);
	int dialRepeatFrames = abs(circle.dx) > DIAL_FAST_TRAVEL ? INPUT_REPEAT_FAST : INPUT_REPEAT_SLOWEST;
	if (input_repeat(dialDirection != 0, &dialRepeat, dialRepeatFrames)) {
		setting_edit_step(&QUICK_ROWS[g_quickSelected], dialDirection);
	}
	quick_arrows_input(held, touch);
	if (!(down & KEY_TOUCH)) {
		return;
	}
	if (menu_frame_record_button_tapped(down, touch)) {
		recorder_record_button();
	}
	quick_tiles_touch_input(touch);
}
