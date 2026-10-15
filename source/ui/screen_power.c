#include "screen_power.h"
#include "input.h"
#include "settings.h"
#include "take_writer.h"

#define LID_POLL_MS 250
#define IDLE_OFF_MS (SCREEN_POWER_IDLE_SECONDS * 1000)
#define WAKE_STICK_MS 500 // after a wake, a drifting circle pad is ignored this long
// PTMU_GetShellState.
#define SHELL_STATE_CLOSED 0
#define SHELL_STATE_OPEN 1

static bool g_screensOff;
static bool g_lidClosed;
static u64 g_lidPolledAt;
static u64 g_lastInputAt;
static bool g_waking; // a press woke the screens and its keys are still down
static u64 g_wokeAt;

// gsp::Lcd is opened only for the switch: the HOME Menu needs it and hangs while an app holds it.
static void set_backlight(bool on)
{
	if (R_FAILED(gspLcdInit())) {
		return;
	}
	if (on) {
		GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTH);
	} else {
		GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTH);
	}
	gspLcdExit();
}

// Lid closed while recording: the screens go dark, the take goes on.
static void poll_lid(u64 now)
{
	if (now - g_lidPolledAt <= LID_POLL_MS) {
		return;
	}
	g_lidPolledAt = now;
	u8 shell = SHELL_STATE_OPEN;
	if (R_FAILED(PTMU_GetShellState(&shell))) {
		return;
	}
	bool closed = shell == SHELL_STATE_CLOSED;
	if (closed == g_lidClosed) {
		return;
	}
	g_lidClosed = closed;
	if (closed && take_writer_is_active() && !g_screensOff) {
		g_screensOff = true;
		set_backlight(false);
	} else if (!closed && g_screensOff) {
		g_screensOff = false;
		set_backlight(true);
		g_lastInputAt = now;
	}
}

void screen_power_poll(u64 now)
{
	poll_lid(now);
	if (g_settings.screensOff && take_writer_is_active() && !g_screensOff &&
	    now - g_lastInputAt > IDLE_OFF_MS) {
		g_screensOff = true;
		set_backlight(false);
	}
}

void screen_power_note_activity(u64 now)
{
	g_lastInputAt = now;
}

bool screen_power_input(u32 down, u32 held, u64 now)
{
	if (g_screensOff) {
		if (down && !g_lidClosed) {
			g_screensOff = false;
			set_backlight(true);
			g_waking = true;
			g_wokeAt = now;
		}
		return true;
	}
	if (!g_waking) {
		return false;
	}
	bool buttonsUp = !(held & ~INPUT_STICK_KEYS);
	bool sticksSettled = !(held & INPUT_STICK_KEYS) || now - g_wokeAt > WAKE_STICK_MS;
	if (buttonsUp && sticksSettled) {
		g_waking = false;
	}
	return true;
}

bool screen_power_is_off(void)
{
	return g_screensOff;
}

void screen_power_on(void)
{
	if (g_screensOff) {
		g_screensOff = false;
		set_backlight(true);
	}
}

void screen_power_suspend(void)
{
	if (g_screensOff) {
		set_backlight(true); // or the system's menu stays invisible
	}
}

void screen_power_restore(void)
{
	if (g_screensOff) {
		set_backlight(false);
	}
}
