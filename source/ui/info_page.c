#include "info_page.h"
#include <stdio.h>
#include "app.h"
#include "draw.h"
#include "ftp.h"
#include "mic.h"
#include "play.h"
#include "profile_store.h"
#include "status.h"
#include "takes.h"

#define INFO_LINE_HEIGHT 16
#define INFO_LINE_SIZE 160
#define INFO_CLOSE_HINT_Y 224
#define PROFILE_LIST_SIZE 96
#define PROFILE_LIST_ENTRY_ROOM 12 // a separator and one gain in dB
#define BYTES_PER_MEGABYTE (1024.0 * 1024.0)

static bool g_infoOpen;

bool info_page_is_open(void)
{
	return g_infoOpen;
}

void info_page_open(void)
{
	g_infoOpen = true;
}

// The gains that have a learned noise profile.
static void draw_noise_profiles_line(float y)
{
	if (profile_store_count() == 0) {
		draw_text(g_fontSmall, PANEL_TEXT_X, y, COLOR_WARNING, ALIGN_LEFT,
		          "No noise profile learned yet (START > Learn the mic's noise)");
		return;
	}
	char gains[PROFILE_LIST_SIZE] = "";
	size_t length = 0;
	for (int gain = 0; gain <= MIC_GAIN_MAX && length < sizeof gains - PROFILE_LIST_ENTRY_ROOM; gain++) {
		if (!profile_store_has(gain)) {
			continue;
		}
		const char *separator = length > 0 ? ", " : "";
		length += snprintf(gains + length, sizeof gains - length, "%s%.0f", separator, mic_gain_db(gain));
	}
	char line[INFO_LINE_SIZE];
	snprintf(line, sizeof line, "Noise profiles learned at gain (dB): %s", gains);
	draw_text_fit(g_fontSmall, PANEL_TEXT_X, y, 1, COLOR_INK, ALIGN_LEFT, PANEL_TEXT_WIDTH, line);
}

static void draw_mic_line(float y)
{
	if (g_micAvailable) {
		draw_text(g_fontSmall, PANEL_TEXT_X, y, COLOR_INK, ALIGN_LEFT,
		          "Mic: %.1f Hz, 16-bit signed, gain %u (%.1f dB)", MIC_RATE, mic_gain(),
		          mic_gain_db(mic_gain()));
	} else {
		draw_text(g_fontSmall, PANEL_TEXT_X, y, COLOR_WARNING, ALIGN_LEFT, "Mic: %s failed, 0x%08lX",
		          mic_error_where(), mic_error());
	}
}

static void draw_playback_line(float y)
{
	if (play_available()) {
		draw_text(g_fontSmall, PANEL_TEXT_X, y, COLOR_INK, ALIGN_LEFT, "Playback: DSP ready");
	} else {
		draw_text(g_fontSmall, PANEL_TEXT_X, y, COLOR_WARNING, ALIGN_LEFT,
		          "Playback: no sdmc:/3ds/dspfirm.cdc (dump it with DSP1)");
	}
}

static void draw_card_line(float y)
{
	double secondsLeft = status_card_seconds_left();
	if (secondsLeft < 0) {
		return;
	}
	double freeMegabytes = status_card_free_bytes() / BYTES_PER_MEGABYTE;
	draw_text(g_fontSmall, PANEL_TEXT_X, y, COLOR_INK, ALIGN_LEFT,
	          "Card: %.0f MB free, about %.1f h of recording", freeMegabytes, secondsLeft / SECONDS_PER_HOUR);
}

static void draw_ftp_line(float y)
{
	const char *ftpAddress = ftp_address();
	if (ftpAddress) {
		draw_text(g_fontSmall, PANEL_TEXT_X, y, COLOR_ACCENT, ALIGN_LEFT, "FTP: ftp://%s", ftpAddress);
	} else if (ftp_running()) {
		draw_text(g_fontSmall, PANEL_TEXT_X, y, COLOR_ACCENT, ALIGN_LEFT, "FTP: waiting for Wi-Fi");
	}
}

void info_page_draw(void)
{
	draw_panel_header("i", "INFORMATION");
	float y = PANEL_CONTENT_Y;
	draw_mic_line(y);
	y += INFO_LINE_HEIGHT;
	draw_text(g_fontSmall, PANEL_TEXT_X, y, COLOR_INK, ALIGN_LEFT, "Captured %.0f s, gaps %lu, restarts %lu",
	          mic_total_samples() / MIC_RATE, (unsigned long)mic_gaps(), (unsigned long)mic_restarts());
	y += INFO_LINE_HEIGHT;
	draw_playback_line(y);
	y += INFO_LINE_HEIGHT;
	draw_noise_profiles_line(y);
	y += INFO_LINE_HEIGHT;
	draw_text(g_fontSmall, PANEL_TEXT_X, y, COLOR_INK, ALIGN_LEFT, "Takes: %d on the card, next REC_%04d",
	          takes_count(), takes_next_id());
	y += INFO_LINE_HEIGHT;
	draw_card_line(y);
	y += INFO_LINE_HEIGHT;
	draw_text(g_fontSmall, PANEL_TEXT_X, y, COLOR_MUTED, ALIGN_LEFT,
	          "Files: " RECORDINGS_DIR " (RAW, clean WAV, TXT)");
	y += INFO_LINE_HEIGHT;
	draw_text(g_fontSmall, PANEL_TEXT_X, y, COLOR_MUTED, ALIGN_LEFT, "Version " APP_VERSION);
	y += INFO_LINE_HEIGHT;
	draw_ftp_line(y);
	draw_text(g_fontSmall, BOTTOM_CENTER_X, INFO_CLOSE_HINT_Y, COLOR_FAINT, ALIGN_CENTER, "B to close");
}

void info_page_input(u32 down)
{
	if (down & (KEY_A | KEY_B | KEY_START | KEY_TOUCH)) {
		g_infoOpen = false;
	}
}
