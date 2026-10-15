#include "gallery_view.h"
#include <math.h>
#include "app.h"
#include "develop_queue.h"
#include "draw.h"
#include "gallery_player.h"
#include "live_analysis.h"
#include "live_view.h"
#include "mic.h"
#include "overview.h"
#include "play.h"
#include "toast.h"
#include "wav.h"

// The header strip.
#define EMPTY_TEXT_Y 108
#define EMPTY_HINT_Y 130
#define TAKE_NAME_Y 6
#define HEADER_SMALL_TEXT_Y 9
#define TAKE_DATE_X 92
#define GAIN_RIGHT_X 322
#define FILE_BADGE_X 330
#define FILE_BADGE_Y 6
#define FILE_BADGE_WIDTH 58
#define FILE_BADGE_HEIGHT 15
#define FILE_BADGE_TEXT_Y 8
#define FILE_BADGE_RADIUS 3

// The overview of the whole take, its markers and the play head.
#define OVERVIEW_Y 44
#define OVERVIEW_HEIGHT 112
#define OVERVIEW_CENTER_Y (OVERVIEW_Y + OVERVIEW_HEIGHT / 2)
#define OVERVIEW_HALF_HEIGHT (OVERVIEW_HEIGHT / 2 - 2) // the tallest column stays inside the box
#define SMALL_TEXT_HALF_HEIGHT 6
#define MARKER_LINE_RISE 4 // above the overview
#define MARKER_FLAG_HALF_WIDTH 3.5f
#define MARKER_FLAG_TOP_Y (OVERVIEW_Y - 6)
#define MARKER_FLAG_TIP_Y (OVERVIEW_Y - 1)
#define PLAY_HEAD_WIDTH 2
#define PLAY_HEAD_OVERHANG 2 // above and below the overview
#define DEPTH_OVERVIEW 0.45f
#define DEPTH_MARKER 0.5f
#define DEPTH_PLAY_HEAD 0.55f

// The play time, the develop state and the level meter.
#define TIME_Y 162
#define LENGTH_Y 175
#define LENGTH_GAP 8
#define PLAY_ICON_GAP 8
#define PLAY_ICON_WIDTH 12
#define PLAY_ICON_CENTER_Y 175
#define PLAY_ICON_HALF_HEIGHT 8
#define DEPTH_PLAY_ICON 0.6f
#define STATUS_Y 196
#define PROGRESS_WIDTH 200
#define PROGRESS_X (TOP_CENTER_X - PROGRESS_WIDTH / 2)
#define PROGRESS_Y 210
#define PROGRESS_HEIGHT 3
#define PROGRESS_RADIUS 1.5f
#define PROGRESS_PER_PERCENT ((float)PROGRESS_WIDTH / 100)
#define DEPTH_PROGRESS 0.6f
#define DEPTH_PROGRESS_FILL 0.61f
#define NO_SOUND_Y 214
#define METER_Y 216
#define TOAST_Y 166

static void draw_take_header(const Take *take, const TakeMeta *meta)
{
	draw_text(g_fontBody, TOP_MARGIN, TAKE_NAME_Y, COLOR_INK, ALIGN_LEFT, "REC_%04d", take->id);
	draw_text(g_fontSmall, TAKE_DATE_X, HEADER_SMALL_TEXT_Y, COLOR_MUTED, ALIGN_LEFT, "%s", take->date);
	bool playingClean = gallery_player_plays_clean(take->id);
	draw_rounded_rect(FILE_BADGE_X, FILE_BADGE_Y, FILE_BADGE_WIDTH, FILE_BADGE_HEIGHT, FILE_BADGE_RADIUS,
	                  playingClean ? COLOR_ACCENT : COLOR_MUTED, DEPTH_HEADER_RAISED);
	draw_text(g_fontSmall, FILE_BADGE_X + FILE_BADGE_WIDTH / 2, FILE_BADGE_TEXT_Y, COLOR_BLACK, ALIGN_CENTER,
	          playingClean ? "CLEAN" : "RAW");
	if (meta->gain) {
		draw_text(g_fontSmall, GAIN_RIGHT_X, HEADER_SMALL_TEXT_Y, COLOR_MUTED, ALIGN_RIGHT, "gain %.0f dB",
		          mic_gain_db(meta->gain));
	}
}

// The part already played is drawn in ink.
static void draw_overview_columns(float playHeadX)
{
	const u8 *heights = overview_columns();
	for (int i = 0; i < WAVE_COLUMNS; i++) {
		if (!heights[i]) {
			continue;
		}
		float halfColumn = (float)OVERVIEW_HALF_HEIGHT * heights[i] / WAVE_HEIGHT_MAX;
		u32 color = WAVE_X + i < playHeadX ? COLOR_INK : COLOR_WAVE_IDLE;
		C2D_DrawRectSolid(WAVE_X + i, OVERVIEW_CENTER_Y - halfColumn, DEPTH_OVERVIEW, 1, halfColumn * 2,
		                  color);
	}
}

static void draw_overview_markers(u64 length)
{
	u32 cues[WAV_MAX_CUES];
	int cueCount = play_cues(cues, WAV_MAX_CUES);
	if (!length) {
		return;
	}
	for (int i = 0; i < cueCount; i++) {
		float x = WAVE_X + WAVE_COLUMNS * (float)cues[i] / length;
		C2D_DrawRectSolid(floorf(x), OVERVIEW_Y - MARKER_LINE_RISE, DEPTH_MARKER, 1,
		                  OVERVIEW_HEIGHT + MARKER_LINE_RISE, COLOR_ACCENT_DIM);
		float flagLeft = x - MARKER_FLAG_HALF_WIDTH;
		float flagRight = x + MARKER_FLAG_HALF_WIDTH;
		draw_triangle(flagLeft, MARKER_FLAG_TOP_Y, flagRight, MARKER_FLAG_TOP_Y, x, MARKER_FLAG_TIP_Y,
		              COLOR_ACCENT, DEPTH_MARKER);
	}
}

static void draw_overview(u64 length, u64 position)
{
	float playHeadX = length ? WAVE_X + WAVE_COLUMNS * (float)position / length : WAVE_X;
	C2D_DrawRectSolid(WAVE_X, OVERVIEW_CENTER_Y, DEPTH_WAVE, WAVE_COLUMNS, 1, COLOR_LINE);
	if (overview_is_ready()) {
		draw_overview_columns(playHeadX);
	} else {
		draw_text(g_fontSmall, TOP_CENTER_X, OVERVIEW_CENTER_Y - SMALL_TEXT_HALF_HEIGHT, COLOR_MUTED,
		          ALIGN_CENTER, "Reading the take  %d%%", overview_progress());
	}
	draw_overview_markers(length);
	C2D_DrawRectSolid(playHeadX - PLAY_HEAD_WIDTH / 2, OVERVIEW_Y - PLAY_HEAD_OVERHANG, DEPTH_PLAY_HEAD,
	                  PLAY_HEAD_WIDTH, OVERVIEW_HEIGHT + 2 * PLAY_HEAD_OVERHANG, COLOR_ACCENT);
}

// The position in large type, the length after it, and a play icon in front while playing.
static void draw_play_time(u64 length, u64 position)
{
	char positionText[APP_TIME_TEXT_SIZE];
	char lengthText[APP_TIME_TEXT_SIZE];
	app_format_time(positionText, sizeof positionText, position / (double)play_sample_rate(), true);
	app_format_time(lengthText, sizeof lengthText, length / (double)play_sample_rate(), true);
	u32 timeColor = play_is_playing() ? COLOR_INK : COLOR_SOFT;
	float timeWidth =
	        draw_text(g_fontLarge, TOP_CENTER_X, TIME_Y, timeColor, ALIGN_CENTER, "%s", positionText);
	float timeLeft = TOP_CENTER_X - timeWidth / 2;
	float timeRight = TOP_CENTER_X + timeWidth / 2;
	draw_text(g_fontSmall, timeRight + LENGTH_GAP, LENGTH_Y, COLOR_MUTED, ALIGN_LEFT, "/ %s", lengthText);
	if (play_is_playing()) {
		float iconLeft = timeLeft - (PLAY_ICON_GAP + PLAY_ICON_WIDTH);
		float iconRight = timeLeft - PLAY_ICON_GAP;
		float iconTop = PLAY_ICON_CENTER_Y - PLAY_ICON_HALF_HEIGHT;
		float iconBottom = PLAY_ICON_CENTER_Y + PLAY_ICON_HALF_HEIGHT;
		draw_triangle(iconLeft, iconTop, iconLeft, iconBottom, iconRight, PLAY_ICON_CENTER_Y, COLOR_ACCENT,
		              DEPTH_PLAY_ICON);
	}
}

static void draw_develop_status(const Take *take, const TakeMeta *meta)
{
	if (develop_queue_has(take->id)) {
		bool isDeveloping = develop_queue_current() == take->id;
		int percent = isDeveloping ? develop_queue_progress() : 0;
		draw_text(g_fontSmall, TOP_CENTER_X, STATUS_Y, COLOR_ACCENT, ALIGN_CENTER,
		          isDeveloping ? "Developing  %d%%" : "Waiting to be developed", percent);
		draw_rounded_rect(PROGRESS_X, PROGRESS_Y, PROGRESS_WIDTH, PROGRESS_HEIGHT, PROGRESS_RADIUS,
		                  COLOR_TILE, DEPTH_PROGRESS);
		if (percent) {
			float filledWidth = PROGRESS_PER_PERCENT * percent;
			draw_rounded_rect(PROGRESS_X, PROGRESS_Y, filledWidth, PROGRESS_HEIGHT, PROGRESS_RADIUS,
			                  COLOR_ACCENT, DEPTH_PROGRESS_FILL);
		}
	} else if (take->hasClean && meta->result[0]) {
		draw_text_fit(g_fontSmall, TOP_CENTER_X, STATUS_Y, 1, COLOR_MUTED, ALIGN_CENTER,
		              TOP_SCREEN_WIDTH - 2 * TOP_MARGIN, meta->result);
	} else if (!take->hasClean) {
		draw_text(g_fontSmall, TOP_CENTER_X, STATUS_Y, COLOR_MUTED, ALIGN_CENTER,
		          "Not developed yet: START develops it with the current settings");
	}
}

static void draw_playback_level(void)
{
	if (!play_available()) {
		draw_text(g_fontSmall, TOP_CENTER_X, NO_SOUND_Y, COLOR_WARNING, ALIGN_CENTER, PLAY_NO_DSP_MESSAGE);
		return;
	}
	if (!play_is_playing()) {
		return;
	}
	float level = play_level();
	// The player measures only the peak; half of it stands in for the RMS.
	live_view_draw_meter(METER_Y, level, level * 0.5f, level, false);
}

void gallery_view_draw(const Take *take, const TakeMeta *meta)
{
	draw_top_header_strip();
	if (!take) {
		draw_text(g_fontBody, TOP_CENTER_X, EMPTY_TEXT_Y, COLOR_MUTED, ALIGN_CENTER, "No recordings yet");
		draw_text(g_fontSmall, TOP_CENTER_X, EMPTY_HINT_Y, COLOR_FAINT, ALIGN_CENTER,
		          "They are saved in " RECORDINGS_DIR);
		return;
	}
	draw_take_header(take, meta);
	u64 length = play_length();
	u64 position = play_position();
	draw_overview(length, position);
	draw_play_time(length, position);
	draw_develop_status(take, meta);
	draw_playback_level();
	toast_draw(TOAST_Y);
}
