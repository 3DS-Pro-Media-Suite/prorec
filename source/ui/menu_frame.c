#include "menu_frame.h"
#include "develop_queue.h"
#include "draw.h"
#include "input.h"
#include "recorder.h"
#include "take_writer.h"

// The header of both menus.
#define RECORDINGS_BUTTON_X 222
#define RECORDINGS_BUTTON_Y 2
#define RECORDINGS_BUTTON_WIDTH 92
#define RECORDINGS_BUTTON_HEIGHT 17
#define RECORDINGS_BUTTON_RADIUS 4
#define RECORDINGS_LABEL_Y 3 // from the top of the button
#define DEVELOP_PROGRESS_GAP 8 // between the develop progress and the RECORDINGS button

// The record button, at the bottom right of both menus.
#define RECORD_BUTTON_X 288
#define RECORD_BUTTON_Y 212
#define RECORD_BUTTON_TOUCH_RADIUS 24
#define RECORD_RING_RADIUS 21
#define RECORD_RING_INNER_RADIUS 18.5f
#define RECORD_DOT_RADIUS 14
#define RECORD_STOP_SIZE 16
#define RECORD_STOP_RADIUS 3

void menu_frame_draw_header(const char *badge, const char *title)
{
	draw_panel_header(badge, title);
	if (develop_queue_current()) {
		draw_text(g_fontSmall, RECORDINGS_BUTTON_X - DEVELOP_PROGRESS_GAP, PANEL_TITLE_Y, COLOR_ACCENT,
		          ALIGN_RIGHT, "DEV %d%%", develop_queue_progress());
	}
	draw_rounded_rect(RECORDINGS_BUTTON_X, RECORDINGS_BUTTON_Y, RECORDINGS_BUTTON_WIDTH,
	                  RECORDINGS_BUTTON_HEIGHT, RECORDINGS_BUTTON_RADIUS, COLOR_TILE_SELECTED, DEPTH_PANEL);
	float labelX = RECORDINGS_BUTTON_X + RECORDINGS_BUTTON_WIDTH / 2;
	float labelY = RECORDINGS_BUTTON_Y + RECORDINGS_LABEL_Y;
	draw_text(g_fontSmall, labelX, labelY, COLOR_ACCENT, ALIGN_CENTER, "RECORDINGS");
}

bool menu_frame_recordings_tapped(u32 down, touchPosition touch)
{
	return (down & KEY_TOUCH) && input_in_rect(touch, RECORDINGS_BUTTON_X, RECORDINGS_BUTTON_Y,
	                                           RECORDINGS_BUTTON_WIDTH, RECORDINGS_BUTTON_HEIGHT);
}

void menu_frame_draw_record_button(void)
{
	u64 now = osGetTime();
	bool recording = take_writer_is_active();
	bool paused = take_writer_is_paused();
	bool armed = recorder_timer_running() || recording;
	u32 ringColor = armed ? COLOR_ACCENT : COLOR_INK;
	draw_circle(RECORD_BUTTON_X, RECORD_BUTTON_Y, RECORD_RING_RADIUS, ringColor, DEPTH_CONTROL);
	draw_circle(RECORD_BUTTON_X, RECORD_BUTTON_Y, RECORD_RING_INNER_RADIUS, COLOR_BLACK,
	            DEPTH_CONTROL_RAISED);
	if (recording && !paused) {
		draw_rounded_rect(RECORD_BUTTON_X - RECORD_STOP_SIZE / 2, RECORD_BUTTON_Y - RECORD_STOP_SIZE / 2,
		                  RECORD_STOP_SIZE, RECORD_STOP_SIZE, RECORD_STOP_RADIUS, COLOR_ACCENT,
		                  DEPTH_CONTROL_TOP);
	} else if (paused) {
		if (draw_blink_is_on(now)) {
			draw_circle(RECORD_BUTTON_X, RECORD_BUTTON_Y, RECORD_DOT_RADIUS, COLOR_ACCENT, DEPTH_CONTROL_TOP);
		}
	} else {
		draw_circle(RECORD_BUTTON_X, RECORD_BUTTON_Y, RECORD_DOT_RADIUS, COLOR_ACCENT, DEPTH_CONTROL_TOP);
	}
}

bool menu_frame_record_button_tapped(u32 down, touchPosition touch)
{
	int dx = touch.px - RECORD_BUTTON_X;
	int dy = touch.py - RECORD_BUTTON_Y;
	return (down & KEY_TOUCH) && dx * dx + dy * dy < RECORD_BUTTON_TOUCH_RADIUS * RECORD_BUTTON_TOUCH_RADIUS;
}

u32 menu_frame_setting_color(bool disabled, bool selected, u32 selectedColor, u32 normalColor)
{
	if (disabled) {
		return COLOR_FAINT;
	}
	return selected ? selectedColor : normalColor;
}
