#include "setup_menu.h"
#include "app.h"
#include "draw.h"
#include "ftp_control.h"
#include "gallery.h"
#include "info_page.h"
#include "input.h"
#include "menu_frame.h"
#include "mic.h"
#include "noise_learning.h"
#include "picker.h"
#include "play.h"
#include "profile_store.h"
#include "recorder.h"
#include "setting_edit.h"
#include "setting_row.h"
#include "settings.h"
#include "toast.h"

// Section tabs under the header, then a list of rows that scrolls.
#define SETUP_VISIBLE_ROWS 6
#define SETUP_TAB_WIDTH 107
#define SETUP_TABS_TOP (PANEL_HEADER_LINE_Y + 1)
#define SETUP_TABS_BOTTOM 45
#define SETUP_TAB_TEXT_Y 27
#define SETUP_TAB_UNDERLINE_Y 41
#define SETUP_TAB_UNDERLINE_WIDTH 36
#define SETUP_TAB_UNDERLINE_HEIGHT 2
#define SETUP_TAB_UNDERLINE_RADIUS 1
#define SETUP_ROWS_Y 50
#define SETUP_ROW_TEXT_Y 3 // from the top of the row
#define SETUP_ROW_ARROW_Y 11.5f
#define SETUP_LABEL_WIDTH 170
#define SETUP_ACTION_LABEL_WIDTH 280
#define SETUP_ACTION_ARROW_X 302
#define SETUP_ACTION_ARROW_SIZE 4
#define SETUP_VALUE_WIDTH 110
#define SETUP_VALUE_SIZE 40
#define SETUP_SELECTED_VALUE_RIGHT_X 296 // the selected value moves left to make room for its arrows
#define SETUP_VALUE_ARROW_RIGHT_X 303
#define SETUP_VALUE_ARROW_GAP 8
#define SETUP_VALUE_ARROW_SIZE 3.5f
#define SETUP_SCROLL_MARK_SIZE 2.5f
#define SETUP_SCROLL_UP_Y 47.6f
#define SETUP_SCROLL_DOWN_TIP_Y 205
#define SETUP_SCROLL_DOWN_BASE_Y 201.5f
#define SETUP_FOOTER_LINE_Y 206

static bool g_setupOpen;
static int g_setupPage;
static int g_setupSelected[MENU_PAGE_COUNT];
static int g_setupFirstRow[MENU_PAGE_COUNT];
static u64 g_forgetArmedUntil; // forgetting the noise profiles waits for a second press until then

bool setup_menu_is_open(void)
{
	return g_setupOpen;
}

void setup_menu_open(void)
{
	g_setupOpen = true;
}

void setup_menu_close(void)
{
	g_setupOpen = false;
}

// Scrolls the setup list so that the selected row shows, and returns the first row on screen.
static int setup_first_visible_row(void)
{
	const MenuPage *page = &MENU_PAGES[g_setupPage];
	int *firstRow = &g_setupFirstRow[g_setupPage];
	int selected = g_setupSelected[g_setupPage];
	if (selected < *firstRow) {
		*firstRow = selected;
	}
	if (selected >= *firstRow + SETUP_VISIBLE_ROWS) {
		*firstRow = selected - SETUP_VISIBLE_ROWS + 1;
	}
	int maxFirstRow = page->count > SETUP_VISIBLE_ROWS ? page->count - SETUP_VISIBLE_ROWS : 0;
	*firstRow = app_clamp(*firstRow, 0, maxFirstRow);
	return *firstRow;
}

static void draw_setup_tabs(void)
{
	for (int i = 0; i < MENU_PAGE_COUNT; i++) {
		float tabCenterX = SETUP_TAB_WIDTH / 2 + i * SETUP_TAB_WIDTH;
		bool current = i == g_setupPage;
		u32 textColor = current ? COLOR_INK : COLOR_MUTED;
		draw_text(g_fontSmall, tabCenterX, SETUP_TAB_TEXT_Y, textColor, ALIGN_CENTER, "%s",
		          MENU_PAGES[i].name);
		if (current) {
			draw_rounded_rect(tabCenterX - SETUP_TAB_UNDERLINE_WIDTH / 2, SETUP_TAB_UNDERLINE_Y,
			                  SETUP_TAB_UNDERLINE_WIDTH, SETUP_TAB_UNDERLINE_HEIGHT,
			                  SETUP_TAB_UNDERLINE_RADIUS, COLOR_ACCENT, DEPTH_CONTROL);
		}
	}
	draw_hairline(SETUP_TABS_BOTTOM);
}

// The value of a setup row, right-aligned. The selected one gets an arrow on each side.
static void draw_setup_value(const SettingRow *row, float y, bool selected)
{
	char value[SETUP_VALUE_SIZE];
	setting_row_value(row, value, sizeof value, false);
	float rightX = selected ? SETUP_SELECTED_VALUE_RIGHT_X : LIST_VALUE_RIGHT_X;
	u32 textColor = selected ? COLOR_ACCENT : COLOR_MUTED;
	float width = draw_text_fit(g_fontBody, rightX, y + SETUP_ROW_TEXT_Y, 1, textColor, ALIGN_RIGHT,
	                            SETUP_VALUE_WIDTH, value);
	if (!selected) {
		return;
	}
	float arrowY = y + SETUP_ROW_ARROW_Y;
	float leftArrowX = SETUP_SELECTED_VALUE_RIGHT_X - width - SETUP_VALUE_ARROW_GAP;
	draw_arrow(SETUP_VALUE_ARROW_RIGHT_X, arrowY, SETUP_VALUE_ARROW_SIZE, ARROW_RIGHT, COLOR_ACCENT,
	           DEPTH_CONTROL);
	draw_arrow(leftArrowX, arrowY, SETUP_VALUE_ARROW_SIZE, ARROW_LEFT, COLOR_ACCENT, DEPTH_CONTROL);
}

static void draw_setup_row(const SettingRow *row, float y, bool selected)
{
	if (selected) {
		draw_list_row_highlight(y);
	}
	bool disabled = setting_edit_disabled_reason(row) != NULL;
	u32 labelColor = menu_frame_setting_color(disabled, selected, COLOR_INK, COLOR_SOFT);
	float labelWidth = row->kind == ROW_ACTION ? SETUP_ACTION_LABEL_WIDTH : SETUP_LABEL_WIDTH;
	draw_text_fit(g_fontBody, LIST_TEXT_X, y + SETUP_ROW_TEXT_Y, 1, labelColor, ALIGN_LEFT, labelWidth,
	              row->label);
	if (row->kind != ROW_ACTION) {
		draw_setup_value(row, y, selected);
		return;
	}
	u32 arrowColor = selected ? COLOR_ACCENT : COLOR_FAINT;
	draw_arrow(SETUP_ACTION_ARROW_X, y + SETUP_ROW_ARROW_Y, SETUP_ACTION_ARROW_SIZE, ARROW_RIGHT, arrowColor,
	           DEPTH_CONTROL);
}

// Marks above and below the list when it scrolls further.
static void draw_scroll_marks(int firstRow, int rowCount)
{
	if (firstRow > 0) {
		draw_arrow(BOTTOM_CENTER_X, SETUP_SCROLL_UP_Y, SETUP_SCROLL_MARK_SIZE, ARROW_UP, COLOR_MUTED,
		           DEPTH_CONTROL);
	}
	if (firstRow + SETUP_VISIBLE_ROWS < rowCount) {
		draw_triangle(BOTTOM_CENTER_X, SETUP_SCROLL_DOWN_TIP_Y, BOTTOM_CENTER_X - SETUP_SCROLL_MARK_SIZE,
		              SETUP_SCROLL_DOWN_BASE_Y, BOTTOM_CENTER_X + SETUP_SCROLL_MARK_SIZE,
		              SETUP_SCROLL_DOWN_BASE_Y, COLOR_MUTED, DEPTH_CONTROL);
	}
}

void setup_menu_draw(void)
{
	menu_frame_draw_header("M", "SETUP");
	draw_setup_tabs();
	const MenuPage *page = &MENU_PAGES[g_setupPage];
	int firstRow = setup_first_visible_row();
	for (int i = firstRow; i < page->count && i < firstRow + SETUP_VISIBLE_ROWS; i++) {
		float y = SETUP_ROWS_Y + (i - firstRow) * LIST_ROW_HEIGHT;
		bool selected = i == g_setupSelected[g_setupPage];
		draw_setup_row(&page->rows[i], y, selected);
	}
	draw_scroll_marks(firstRow, page->count);
	draw_hairline(SETUP_FOOTER_LINE_Y);
	draw_text(g_fontSmall, PANEL_TEXT_X, PANEL_HINT_Y, COLOR_MUTED, ALIGN_LEFT,
	          "A select   B back   L / R section");
	if (g_setupPage == MENU_PAGE_SYSTEM) {
		draw_text(g_fontSmall, PANEL_TEXT_X, PANEL_HINT_SECOND_Y, COLOR_FAINT, ALIGN_LEFT,
		          "Version " APP_VERSION);
	}
	menu_frame_draw_record_button();
}

// Forgetting the noise profiles asks for a second press first.
static void forget_noise_profiles(void)
{
	if (profile_store_count() && !input_confirm(&g_forgetArmedUntil, osGetTime())) {
		toast_warning("Press again to forget %d noise profiles", profile_store_count());
		return;
	}
	profile_store_forget_all();
}

static void reset_settings(void)
{
	settings_set_defaults(&g_settings);
	mic_set_gain(settings_mic_gain(g_settings.gainStep));
	play_set_monitor(g_settings.monitor);
	settings_save();
	toast("All settings reset");
}

static void run_action(RowAction action, bool *quit)
{
	switch (action) {
	case ACTION_LEARN_NOISE:
		g_setupOpen = false;
		noise_learning_start();
		break;
	case ACTION_FORGET_NOISE:
		forget_noise_profiles();
		break;
	case ACTION_RECORDINGS:
		if (gallery_open_newest_if_idle()) {
			g_setupOpen = false;
		}
		break;
	case ACTION_FTP:
		ftp_control_toggle();
		break;
	case ACTION_INFORMATION:
		info_page_open();
		break;
	case ACTION_RESET:
		reset_settings();
		break;
	case ACTION_EXIT:
		*quit = true;
		break;
	default:
		break;
	}
}

// An action row: greyed out, or done.
static void run_action_row(const SettingRow *row, bool *quit)
{
	const char *disabledReason = setting_edit_disabled_reason(row);
	if (disabledReason) {
		toast_warning("%s", disabledReason);
		return;
	}
	run_action(row->action, quit);
}

// A tap on a section tab, the record button or a row (a second tap on the selected row opens it).
static void setup_touch_input(u32 down, touchPosition touch, bool *quit)
{
	const MenuPage *page = &MENU_PAGES[g_setupPage];
	int *selected = &g_setupSelected[g_setupPage];
	if (touch.py < SETUP_TABS_BOTTOM && touch.py > SETUP_TABS_TOP) {
		g_setupPage = app_clamp(touch.px / SETUP_TAB_WIDTH, 0, MENU_PAGE_COUNT - 1);
		return;
	}
	if (menu_frame_record_button_tapped(down, touch)) {
		g_setupOpen = false;
		recorder_record_button();
		return;
	}
	if (touch.py < SETUP_ROWS_Y || touch.py >= SETUP_ROWS_Y + SETUP_VISIBLE_ROWS * LIST_ROW_HEIGHT) {
		return;
	}
	int rowIndex = setup_first_visible_row() + (touch.py - SETUP_ROWS_Y) / LIST_ROW_HEIGHT;
	if (rowIndex >= page->count) {
		return;
	}
	bool secondTap = rowIndex == *selected;
	*selected = rowIndex;
	const SettingRow *row = &page->rows[rowIndex];
	if (row->kind == ROW_ACTION) {
		run_action_row(row, quit);
	} else if (secondTap) {
		picker_open(row);
	}
}

void setup_menu_input(u32 down, u32 held, touchPosition touch, bool *quit)
{
	if (down & (KEY_B | KEY_START)) {
		g_setupOpen = false;
		return;
	}
	if (down & KEY_R) {
		g_setupPage = (g_setupPage + 1) % MENU_PAGE_COUNT;
	}
	if (down & KEY_L) {
		g_setupPage = (g_setupPage + MENU_PAGE_COUNT - 1) % MENU_PAGE_COUNT;
	}
	const MenuPage *page = &MENU_PAGES[g_setupPage];
	int *selected = &g_setupSelected[g_setupPage];
	if (input_dpad_repeat(held, KEY_DDOWN, INPUT_REPEAT_FAST)) {
		*selected = (*selected + 1) % page->count;
	}
	if (input_dpad_repeat(held, KEY_DUP, INPUT_REPEAT_FAST)) {
		*selected = (*selected + page->count - 1) % page->count;
	}
	const SettingRow *row = &page->rows[*selected];
	if (input_dpad_repeat(held, KEY_DRIGHT, INPUT_REPEAT_FAST)) {
		setting_edit_step(row, ARROW_RIGHT);
	}
	if (input_dpad_repeat(held, KEY_DLEFT, INPUT_REPEAT_FAST)) {
		setting_edit_step(row, ARROW_LEFT);
	}
	if (down & KEY_A) {
		if (row->kind == ROW_ACTION) {
			run_action_row(row, quit);
		} else {
			picker_open(row);
		}
	}
	if (down & KEY_TOUCH) {
		setup_touch_input(down, touch, quit);
	}
}
