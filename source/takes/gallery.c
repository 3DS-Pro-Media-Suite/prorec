#include "gallery.h"
#include <stdio.h>
#include <sys/stat.h>
#include "app.h"
#include "develop.h"
#include "develop_queue.h"
#include "draw.h"
#include "ftp.h"
#include "ftp_control.h"
#include "gallery_player.h"
#include "gallery_view.h"
#include "input.h"
#include "mic.h"
#include "overview.h"
#include "recorder.h"
#include "settings.h"
#include "take_writer.h"
#include "takes.h"
#include "toast.h"

#define DATE_WITHOUT_SECONDS_LENGTH 16 // "YYYY-MM-DD hh:mm"

// The list of takes on the bottom screen.
#define LIST_Y 25
#define LIST_VISIBLE_ROWS 5
#define ROW_NAME_Y 3 // offsets from the top of a row
#define ROW_SMALL_TEXT_Y 6
#define ROW_DATE_X 96
#define ROW_BADGE_X 206
#define ROW_BADGE_Y 5
#define ROW_BADGE_WIDTH 46
#define ROW_BADGE_HEIGHT 13
#define ROW_BADGE_RADIUS 3
#define DEPTH_ROW_BADGE 0.25f
#define EMPTY_LIST_Y 70
#define MORE_ABOVE_ARROW_Y 23.5f
#define MORE_ABOVE_ARROW_SIZE 2.5f

// The row of buttons on the bottom screen.
#define BUTTON_X 8
#define BUTTON_Y 180
#define BUTTON_WIDTH 72
#define BUTTON_HEIGHT 28
#define BUTTON_GAP 4
#define BUTTON_RADIUS 7
#define BUTTON_TEXT_Y (BUTTON_Y + 6)

enum {
	BUTTON_BACK,
	BUTTON_DEVELOP,
	BUTTON_DELETE,
	BUTTON_FTP,
	BUTTON_COUNT
};

static bool g_galleryOpen;
static int g_selectedIndex;
static int g_firstVisibleRow;
static u64 g_deleteArmedUntil;
static int g_deleteArmedTakeId;
static TakeMeta g_selectedMeta; // the selected take's TXT, read when the selection changes

static const char *const BUTTON_LABELS[BUTTON_COUNT] = {"Back", "Develop", "Delete", "FTP"};

bool gallery_is_open(void)
{
	return g_galleryOpen;
}

static const Take *selected_take(void)
{
	int takeCount = takes_count();
	bool hasSelection = takeCount && g_selectedIndex >= 0 && g_selectedIndex < takeCount;
	return hasSelection ? takes_at(g_selectedIndex) : NULL;
}

// The selected take's TXT changes with the selection and with a develop of the take.
static void refresh_selected_meta(void)
{
	const Take *take = selected_take();
	if (take) {
		takes_read_meta(take->id, &g_selectedMeta);
	}
}

static void open_with_newest_take(void)
{
	if (recorder_timer_running()) {
		recorder_cancel_timer();
	}
	takes_scan();
	g_galleryOpen = true;
	g_selectedIndex = takes_count() - 1;
	gallery_player_close();
	gallery_player_load(selected_take(), false);
	refresh_selected_meta();
}

bool gallery_open_newest_if_idle(void)
{
	if (take_writer_is_active()) {
		toast_warning("Stop the recording first");
		return false;
	}
	open_with_newest_take();
	return true;
}

static void close_gallery(void)
{
	gallery_player_close();
	g_galleryOpen = false;
}

static void select_take(int index)
{
	if (!takes_count()) {
		return;
	}
	g_selectedIndex = app_clamp(index, 0, takes_count() - 1);
	gallery_player_load(selected_take(), false);
	refresh_selected_meta();
}

// The first press arms the delete; a second press on the same take within INPUT_CONFIRM_MS confirms it.
static bool delete_confirmed(int takeId)
{
	if (g_deleteArmedTakeId != takeId) {
		g_deleteArmedUntil = 0;
	}
	g_deleteArmedTakeId = takeId;
	return input_confirm(&g_deleteArmedUntil, osGetTime());
}

// Deletes every file of the take and their overview caches. False when a file stays.
static bool delete_take_files(int takeId)
{
	bool allDeleted = true;
	for (int kind = TAKE_RAW; kind < TAKE_KIND_COUNT; kind++) {
		char path[APP_PATH_SIZE];
		char cachePath[OVERVIEW_PATH_SIZE];
		takes_path(takeId, kind, path, sizeof path);
		overview_cache_path(path, cachePath, sizeof cachePath);
		remove(cachePath);
		struct stat fileInfo;
		if (stat(path, &fileInfo) == 0 && remove(path) != 0) {
			allDeleted = false;
		}
	}
	return allDeleted;
}

static void delete_selected_take(void)
{
	const Take *take = selected_take();
	if (!take) {
		return;
	}
	if (develop_queue_has(take->id)) {
		toast_warning("REC_%04d is being developed", take->id);
		return;
	}
	if (!delete_confirmed(take->id)) {
		return;
	}
	int takeId = take->id;
	int index = g_selectedIndex;
	gallery_player_close();
	if (delete_take_files(takeId)) {
		toast("Deleted REC_%04d", takeId);
		takes_remove_at(index);
	} else {
		toast_warning("Could not delete all of REC_%04d", takeId);
		takes_scan();
	}
	select_take(index < takes_count() ? index : takes_count() - 1);
}

static void develop_selected_take(void)
{
	const Take *take = selected_take();
	if (!take) {
		return;
	}
	if (!take->hasRaw) {
		toast_warning("REC_%04d has no raw file to develop from", take->id);
		return;
	}
	if (develop_queue_has(take->id)) {
		toast_warning("REC_%04d is already being developed", take->id);
		return;
	}
	if (gallery_player_plays_clean(take->id)) {
		gallery_player_park(); // the clean file is about to be replaced
	}
	DevelopParams params = settings_develop_params(&g_settings);
	if (develop_queue_add(take->id, &params)) {
		toast("Developing REC_%04d with the current settings", take->id);
	} else {
		toast_warning("The develop queue is full");
	}
}

// The take's TXT has a new develop result, and the player and the overview pick up the new clean file.
void gallery_take_developed(int id)
{
	if (!g_galleryOpen) {
		return;
	}
	refresh_selected_meta(); // the end of a develop can also rescan the list
	const Take *take = selected_take();
	if (!take || take->id != id) {
		return;
	}
	if (gallery_player_plays_clean(id)) {
		gallery_player_park(); // the player holds the old clean file
	}
	gallery_player_load(take, true);
	gallery_player_request_overview();
}

void gallery_draw_take(void)
{
	gallery_view_draw(selected_take(), &g_selectedMeta);
}

static void scroll_list_to_selection(void)
{
	if (g_selectedIndex < g_firstVisibleRow) {
		g_firstVisibleRow = g_selectedIndex;
	}
	if (g_selectedIndex >= g_firstVisibleRow + LIST_VISIBLE_ROWS) {
		g_firstVisibleRow = g_selectedIndex - LIST_VISIBLE_ROWS + 1;
	}
	int takeCount = takes_count();
	int lastFirstRow = takeCount > LIST_VISIBLE_ROWS ? takeCount - LIST_VISIBLE_ROWS : 0;
	g_firstVisibleRow = app_clamp(g_firstVisibleRow, 0, lastFirstRow);
}

// DEV while a develop has the take, CLEAN once it has a clean file, else RAW.
static void draw_row_badge(const Take *take, float rowY)
{
	const char *label = "RAW";
	u32 color = COLOR_MUTED;
	if (develop_queue_has(take->id)) {
		label = "DEV";
		color = COLOR_WARNING;
	} else if (take->hasClean) {
		label = "CLEAN";
		color = COLOR_ACCENT;
	}
	draw_rounded_rect(ROW_BADGE_X, rowY + ROW_BADGE_Y, ROW_BADGE_WIDTH, ROW_BADGE_HEIGHT, ROW_BADGE_RADIUS,
	                  color, DEPTH_ROW_BADGE);
	draw_text(g_fontSmall, ROW_BADGE_X + ROW_BADGE_WIDTH / 2, rowY + ROW_BADGE_Y, COLOR_BLACK, ALIGN_CENTER,
	          "%s", label);
}

static void draw_take_row(const Take *take, float rowY, bool isSelected)
{
	if (isSelected) {
		draw_list_row_highlight(rowY);
	}
	draw_text(g_fontBody, LIST_TEXT_X, rowY + ROW_NAME_Y, isSelected ? COLOR_INK : COLOR_SOFT, ALIGN_LEFT,
	          "REC_%04d", take->id);
	draw_text(g_fontSmall, ROW_DATE_X, rowY + ROW_SMALL_TEXT_Y, COLOR_MUTED, ALIGN_LEFT, "%.*s",
	          DATE_WITHOUT_SECONDS_LENGTH, take->date);
	char duration[APP_TIME_TEXT_SIZE];
	app_format_time(duration, sizeof duration, take->frames / MIC_RATE, false);
	draw_text(g_fontSmall, LIST_VALUE_RIGHT_X, rowY + ROW_SMALL_TEXT_Y, isSelected ? COLOR_INK : COLOR_MUTED,
	          ALIGN_RIGHT, "%s", duration);
	draw_row_badge(take, rowY);
}

// Newest at the bottom, scrolled to keep the selection visible.
static void draw_take_list(void)
{
	scroll_list_to_selection();
	int takeCount = takes_count();
	if (!takeCount) {
		draw_text(g_fontBody, BOTTOM_CENTER_X, EMPTY_LIST_Y, COLOR_MUTED, ALIGN_CENTER, "No recordings yet");
	}
	for (int i = g_firstVisibleRow; i < takeCount && i < g_firstVisibleRow + LIST_VISIBLE_ROWS; i++) {
		float rowY = LIST_Y + (i - g_firstVisibleRow) * LIST_ROW_HEIGHT;
		draw_take_row(takes_at(i), rowY, i == g_selectedIndex);
	}
	if (g_firstVisibleRow > 0) {
		draw_arrow(BOTTOM_CENTER_X, MORE_ABOVE_ARROW_Y, MORE_ABOVE_ARROW_SIZE, ARROW_UP, COLOR_MUTED,
		           DEPTH_CONTROL);
	}
}

static int button_x(int button)
{
	return BUTTON_X + button * (BUTTON_WIDTH + BUTTON_GAP);
}

// Delete lights up while it waits for the second press, FTP while the server runs.
static bool button_is_lit(int button, bool deleteArmed)
{
	if (button == BUTTON_DELETE) {
		return deleteArmed;
	}
	if (button == BUTTON_FTP) {
		return ftp_running();
	}
	return false;
}

static void draw_buttons(void)
{
	bool deleteArmed = osGetTime() < g_deleteArmedUntil;
	for (int button = 0; button < BUTTON_COUNT; button++) {
		float x = button_x(button);
		u32 background = COLOR_TILE;
		u32 textColor = COLOR_INK;
		if (button_is_lit(button, deleteArmed)) {
			background = COLOR_ACCENT;
			textColor = COLOR_BLACK;
		}
		if (button == BUTTON_BACK) {
			background = COLOR_TILE_SELECTED;
		}
		const char *label = BUTTON_LABELS[button];
		if (button == BUTTON_DELETE && deleteArmed) {
			label = "Sure?";
		}
		draw_rounded_rect(x, BUTTON_Y, BUTTON_WIDTH, BUTTON_HEIGHT, BUTTON_RADIUS, background, DEPTH_CONTROL);
		draw_text(g_fontBody, x + BUTTON_WIDTH / 2, BUTTON_TEXT_Y, textColor, ALIGN_CENTER, "%s", label);
	}
}

static void draw_panel_hints(void)
{
	if (toast_is_showing()) {
		toast_draw_line(BOTTOM_CENTER_X, PANEL_HINT_Y);
	} else if (ftp_running()) {
		const char *ftpAddress = ftp_address();
		if (ftpAddress) {
			draw_text(g_fontSmall, BOTTOM_CENTER_X, PANEL_HINT_Y, COLOR_ACCENT, ALIGN_CENTER, "ftp://%s",
			          ftpAddress);
		} else {
			draw_text(g_fontSmall, BOTTOM_CENTER_X, PANEL_HINT_Y, COLOR_ACCENT, ALIGN_CENTER,
			          "FTP: waiting for Wi-Fi");
		}
	} else {
		draw_text(g_fontSmall, BOTTOM_CENTER_X, PANEL_HINT_Y, COLOR_FAINT, ALIGN_CENTER,
		          "A play   Y raw / clean   L / R markers   left / right 5 s");
	}
	draw_text(g_fontSmall, BOTTOM_CENTER_X, PANEL_HINT_SECOND_Y, COLOR_FAINT, ALIGN_CENTER,
	          "X delete   START develop again   B back");
}

void gallery_draw_panel(void)
{
	draw_panel_header("R", "RECORDINGS");
	if (takes_count()) {
		draw_text(g_fontSmall, PANEL_RIGHT_X, PANEL_TITLE_Y, COLOR_FAINT, ALIGN_RIGHT, "%d of %d",
		          g_selectedIndex + 1, takes_count());
	}
	draw_take_list();
	gallery_player_draw_scrub_bar();
	draw_buttons();
	draw_panel_hints();
}

static void handle_key_presses(u32 down)
{
	const Take *take = selected_take();
	if (down & KEY_A) {
		gallery_player_toggle(take);
		gallery_player_warn_if_muted();
	}
	if ((down & KEY_Y) && take) {
		gallery_player_switch_raw_or_clean(take);
	}
	if (down & KEY_X) {
		delete_selected_take();
	}
	if (down & KEY_START) {
		develop_selected_take();
	}
	if (down & KEY_R) {
		gallery_player_jump_to_marker(1);
	}
	if (down & KEY_L) {
		gallery_player_jump_to_marker(-1);
	}
}

static void select_with_held_keys(u32 held)
{
	static int upRepeat;
	static int downRepeat;
	if (input_repeat(held & KEY_DDOWN, &downRepeat, INPUT_REPEAT_NORMAL) &&
	    g_selectedIndex < takes_count() - 1) {
		select_take(g_selectedIndex + 1);
	}
	if (input_repeat(held & KEY_DUP, &upRepeat, INPUT_REPEAT_NORMAL) && g_selectedIndex > 0) {
		select_take(g_selectedIndex - 1);
	}
}

// A tap on the selected take plays or pauses it; a tap on another one selects it.
static void tap_list(touchPosition touch)
{
	if (touch.py < LIST_Y || touch.py >= LIST_Y + LIST_VISIBLE_ROWS * LIST_ROW_HEIGHT) {
		return;
	}
	int index = g_firstVisibleRow + (touch.py - LIST_Y) / LIST_ROW_HEIGHT;
	if (index >= takes_count()) {
		return;
	}
	if (index == g_selectedIndex) {
		gallery_player_toggle(selected_take());
	} else {
		select_take(index);
	}
}

static bool button_touched(touchPosition touch, int button)
{
	return input_in_rect(touch, button_x(button), BUTTON_Y, BUTTON_WIDTH, BUTTON_HEIGHT);
}

static void tap_buttons(touchPosition touch)
{
	for (int button = 0; button < BUTTON_COUNT; button++) {
		if (!button_touched(touch, button)) {
			continue;
		}
		switch (button) {
		case BUTTON_BACK:
			close_gallery();
			break;
		case BUTTON_DEVELOP:
			develop_selected_take();
			break;
		case BUTTON_DELETE:
			delete_selected_take();
			break;
		case BUTTON_FTP:
			ftp_control_toggle();
			break;
		}
	}
}

void gallery_input(u32 down, u32 held, touchPosition touch)
{
	if (down & KEY_B) {
		close_gallery();
		return;
	}
	if (!takes_count()) {
		if ((down & KEY_TOUCH) && button_touched(touch, BUTTON_BACK)) {
			close_gallery();
		}
		if ((down & KEY_TOUCH) && button_touched(touch, BUTTON_FTP)) {
			ftp_control_toggle();
		}
		return;
	}
	handle_key_presses(down);
	gallery_player_seek_input(held);
	select_with_held_keys(held);
	if (gallery_player_scrub_input(held, touch)) {
		return;
	}
	if (down & KEY_TOUCH) {
		tap_list(touch);
		tap_buttons(touch);
	}
}
