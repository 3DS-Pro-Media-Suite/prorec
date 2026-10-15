// Pro Rec: a recorder for the 3DS microphone.
// A take is saved raw. A worker thread then develops a clean copy, and the raw file stays so the take can
// be developed again with other settings.

#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <sys/stat.h>
#include "app.h"
#include "develop_queue.h"
#include "draw.h"
#include "fft.h"
#include "ftp.h"
#include "ftp_control.h"
#include "gallery.h"
#include "live_analysis.h"
#include "mic.h"
#include "noise_learning.h"
#include "overview.h"
#include "picker.h"
#include "play.h"
#include "profile_store.h"
#include "recorder.h"
#include "screen_power.h"
#include "setting_row.h"
#include "settings.h"
#include "status.h"
#include "takes.h"
#include "toast.h"
#include "ui.h"

#define DRAW_MAX_OBJECTS (C2D_DEFAULT_MAX_OBJECTS * 4) // anti-aliased shapes and a waveform column per pixel
#define FOLDER_MODE 0777
#define OLD_3DS_CPU_LIMIT_PERCENT 30 // the develop thread runs on the system core of an Old 3DS

static void make_folders(void)
{
	mkdir(APPS_DIR, FOLDER_MODE);
	mkdir(APP_DIR, FOLDER_MODE);
	mkdir(NOISE_PROFILE_DIR, FOLDER_MODE);
	mkdir(CACHE_DIR, FOLDER_MODE);
	mkdir(RECORDINGS_DIR, FOLDER_MODE);
}

// The self-timer ran out: the take starts, unless the recordings or noise learning took over meanwhile.
static void poll_self_timer(u64 now)
{
	if (!recorder_timer_due(now)) {
		return;
	}
	if (gallery_is_open() || noise_learning_is_active()) {
		recorder_cancel_timer();
		return;
	}
	picker_close();
	recorder_start_after_timer();
}

static void poll_develop_done(void)
{
	int takeId = 0;
	bool succeeded = false;
	char message[DEVELOP_MESSAGE_SIZE];
	if (!develop_queue_take_done(&takeId, &succeeded, message, sizeof message)) {
		return;
	}
	if (succeeded) {
		toast("REC_%04d developed: %s", takeId, message);
	} else {
		toast_warning("REC_%04d: %s", takeId, message);
	}
	takes_refresh_clean(takeId);
	gallery_take_developed(takeId);
	status_refresh(true);
}

static void render_frame(C3D_RenderTarget *topScreen, C3D_RenderTarget *bottomScreen)
{
	draw_clear_text();
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	C2D_TargetClear(topScreen, COLOR_BLACK);
	C2D_SceneBegin(topScreen);
	ui_draw_top();
	C2D_TargetClear(bottomScreen, COLOR_BLACK);
	C2D_SceneBegin(bottomScreen);
	ui_draw_bottom();
	C3D_FrameEnd(0);
}

static void start_up(void)
{
	draw_init();
	ptmuInit();
	APT_CheckNew3DS(&g_isNew3ds);
	osSetSpeedupEnable(true);
	make_folders();
	fft_init();
	setting_row_init_labels();
	settings_load();
	ftp_control_update_label();
	remove(DEVELOP_SCRATCH_PATH); // a develop cut off by a crash leaves this file behind
	takes_scan();
	takes_delete_partials();
	takes_recover();
	profile_store_load_all();
	if (!g_isNew3ds) {
		APT_SetAppCpuTimeLimit(OLD_3DS_CPU_LIMIT_PERCENT);
	}
	develop_queue_init();
	overview_start();
	g_micAvailable = mic_init((u8)settings_mic_gain(g_settings.gainStep));
	play_init(); // without the DSP firmware there is no sound; play_available() says so
	play_set_monitor(g_settings.monitor);
	recorder_init();
	status_refresh(true);
	screen_power_note_activity(osGetTime());
	live_analysis_init();
}

// Runs every frame, also while the screens are off.
static void poll_modules(u64 now)
{
	if (g_micAvailable) {
		live_analysis_update();
	}
	noise_learning_poll();
	poll_self_timer(now);
	recorder_poll();
	poll_develop_done();
	screen_power_poll(now);
	play_set_monitor(g_settings.monitor && !gallery_is_open());
	status_refresh(false);
	ftp_control_watch(now);
	settings_autosave(now);
}

static void shut_down(void)
{
	recorder_exit();
	screen_power_on();
	develop_queue_exit();
	overview_stop();
	play_exit();
	mic_exit();
	settings_save();
	ftp_exit();
	draw_exit();
	ptmuExit();
	osSetSpeedupEnable(false);
}

int main(void)
{
	gfxInitDefault();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(DRAW_MAX_OBJECTS);
	C2D_Prepare();
	C3D_RenderTarget *topScreen = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	C3D_RenderTarget *bottomScreen = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	start_up();

	bool quit = false;
	while (aptMainLoop() && !quit) {
		hidScanInput();
		ui_handle_input(&quit);
		poll_modules(osGetTime());
		if (screen_power_is_off()) {
			gspWaitForVBlank(); // nothing to see: skip drawing
			continue;
		}
		render_frame(topScreen, bottomScreen);
	}

	shut_down();
	C2D_Fini();
	C3D_Fini();
	gfxExit();
	return 0;
}
