#include "ui.h"
#include "app.h"
#include "draw.h"
#include "gallery.h"
#include "info_page.h"
#include "input.h"
#include "live_view.h"
#include "menu_frame.h"
#include "mic.h"
#include "noise_learning.h"
#include "picker.h"
#include "play.h"
#include "quick_menu.h"
#include "recorder.h"
#include "screen_power.h"
#include "setting_edit.h"
#include "setting_row.h"
#include "settings.h"
#include "setup_menu.h"
#include "toast.h"

#define HINTS_HOLD_MS 350 // SELECT held this long shows the key hints; a shorter tap opens the recordings

void ui_draw_top(void)
{
	C2D_DrawRectSolid(0, 0, 0, TOP_SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);
	if (gallery_is_open()) {
		gallery_draw_take();
	} else {
		live_view_draw();
	}
}

void ui_draw_bottom(void)
{
	C2D_DrawRectSolid(0, 0, 0, BOTTOM_SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);
	if (gallery_is_open()) {
		gallery_draw_panel();
	} else if (picker_is_open()) {
		picker_draw();
	} else if (info_page_is_open()) {
		info_page_draw();
	} else if (setup_menu_is_open()) {
		setup_menu_draw();
	} else {
		quick_menu_draw();
	}
}

// SELECT shows the key hints while held and opens the recordings on a tap. True when they opened.
static bool select_key_input(u32 down, u32 held, u32 released, u64 now)
{
	static u64 selectDownAt;
	if (down & KEY_SELECT) {
		selectDownAt = now;
	}
	bool heldLong = now - selectDownAt > HINTS_HOLD_MS;
	live_view_show_controls((held & KEY_SELECT) && heldLong);
	bool tapped = (released & KEY_SELECT) && !heldLong;
	if (!tapped) {
		return false;
	}
	return gallery_open_newest_if_idle();
}

// The monitor plays the mic on the headphones. It needs the DSP firmware.
static void toggle_monitor(void)
{
	if (!play_available()) {
		toast_warning(PLAY_NO_DSP_MESSAGE);
		return;
	}
	g_settings.monitor = !g_settings.monitor;
	setting_edit_apply_change(setting_row_for_field(SETTING_OFFSET(monitor)));
	if (!g_settings.monitor) {
		toast("Monitor off");
	} else if (osIsHeadsetConnected()) {
		toast("Monitor on");
	} else {
		toast("Monitor on (plug in headphones)");
	}
}

// The recorder's keys under the quick menu. True when the frame's input ends here.
static bool recording_keys_input(u32 down, u32 held, u32 released, u64 now)
{
	if (select_key_input(down, held, released, now)) {
		return true;
	}
	if (down & (KEY_A | KEY_R | KEY_ZR)) {
		recorder_record_button();
	}
	if (down & KEY_X) {
		recorder_toggle_pause();
	}
	if (down & KEY_L) {
		recorder_add_marker();
	}
	if (down & KEY_Y) {
		toggle_monitor();
	}
	if (down & KEY_B) {
		g_settings.liveView = (g_settings.liveView + 1) % LIVE_VIEW_COUNT;
		settings_changed();
	}
	if (down & KEY_START) {
		setup_menu_open();
		return true;
	}
	return false;
}

void ui_handle_input(bool *quit)
{
	u32 down = hidKeysDown();
	u32 held = hidKeysHeld();
	u32 released = hidKeysUp();
	touchPosition touch = {0};
	if (held & KEY_TOUCH) {
		hidTouchRead(&touch);
	}
	u64 now = osGetTime();
	if ((held & KEY_START) && (held & KEY_SELECT)) {
		*quit = true;
		return;
	}
	if (down) {
		screen_power_note_activity(now);
		// A press clicks in the mic. Develop repairs the clicks at these positions.
		if (down & ~INPUT_STICK_KEYS) {
			recorder_note_press(mic_total_samples());
		}
	}
	if (screen_power_input(down, held, now)) {
		return;
	}
	if (noise_learning_is_active()) {
		if (down & KEY_B) {
			noise_learning_cancel();
		}
		return;
	}
	if (gallery_is_open()) {
		gallery_input(down, held, touch);
		return;
	}
	if (info_page_is_open()) {
		info_page_input(down);
		return;
	}
	if (picker_is_open()) {
		picker_input(down, held, touch);
		return;
	}
	// The RECORDINGS button in the header of both menus.
	if (menu_frame_recordings_tapped(down, touch)) {
		if (gallery_open_newest_if_idle()) {
			setup_menu_close();
		}
		return;
	}
	if (setup_menu_is_open()) {
		setup_menu_input(down, held, touch, quit);
		return;
	}
	if (recording_keys_input(down, held, released, now)) {
		return;
	}
	quick_menu_input(down, held, touch);
}
