#include "gallery_player.h"
#include "app.h"
#include "develop_queue.h"
#include "draw.h"
#include "input.h"
#include "overview.h"
#include "play.h"
#include "toast.h"
#include "wav.h"

#define NO_TAKE (-1)
#define SEEK_STEP_SECONDS 5
#define VOLUME_SLIDER_MAX 63
#define MARKER_AHEAD_MIN_SAMPLES 1600 // a marker right at the play head is not the next one
#define MARKER_BACK_GRACE_SAMPLES 16000 // about 0.5 s: press twice to go further back

// The scrub bar on the bottom screen.
#define SCRUB_Y 156
#define SCRUB_HEIGHT 18
#define SCRUB_TOUCH_SLACK 4 // touches this far above or below the bar still scrub
#define SCRUB_TRACK_Y (SCRUB_Y + 7)
#define SCRUB_TRACK_HEIGHT 4
#define SCRUB_TRACK_RADIUS 2
#define SCRUB_TICK_Y (SCRUB_Y + 3)
#define SCRUB_TICK_WIDTH 1
#define SCRUB_TICK_HALF_WIDTH (SCRUB_TICK_WIDTH / 2.0f)
#define SCRUB_TICK_HEIGHT 12
#define SCRUB_KNOB_Y (SCRUB_TRACK_Y + SCRUB_TRACK_HEIGHT / 2)
#define SCRUB_KNOB_RADIUS 6
#define SCRUB_SEEK_EVERY_FRAMES 3

static bool g_preferClean = true; // play the clean file when there is one
static int g_playerTakeId = NO_TAKE;
static bool g_playerIsClean;
static u64 g_parkedPosition; // the player's place while a develop replaces its clean file

bool gallery_player_plays_clean(int takeId)
{
	return g_playerTakeId == takeId && g_playerIsClean;
}

// Where a reloaded take starts: the same take keeps its place, a parked player gets its place back.
static u64 start_position(int takeId, bool keepPosition)
{
	if (!keepPosition) {
		return 0;
	}
	if (g_playerTakeId == takeId) {
		return play_position();
	}
	if (g_playerTakeId == NO_TAKE) {
		return g_parkedPosition;
	}
	return 0;
}

void gallery_player_close(void)
{
	play_close();
	g_playerTakeId = NO_TAKE;
}

void gallery_player_load(const Take *take, bool keepPosition)
{
	if (!take) {
		gallery_player_close();
		return;
	}
	bool useClean = g_preferClean && take->hasClean && !develop_queue_has(take->id);
	if (!take->hasRaw) {
		useClean = true;
	}
	if (g_playerTakeId == take->id && g_playerIsClean == useClean && play_loaded()) {
		return;
	}
	u64 startPosition = start_position(take->id, keepPosition);
	g_parkedPosition = 0;
	bool keepPlaying = keepPosition && play_is_playing();
	char path[APP_PATH_SIZE];
	takes_path(take->id, useClean ? TAKE_CLEAN : TAKE_RAW, path, sizeof path);
	if (!play_open(path)) {
		toast_warning("Cannot read REC_%04d", take->id);
		g_playerTakeId = NO_TAKE;
		return;
	}
	g_playerTakeId = take->id;
	g_playerIsClean = useClean;
	overview_request(path);
	if (startPosition) {
		play_seek(startPosition);
	}
	if (keepPlaying) {
		play_set_playing(true);
	}
}

void gallery_player_park(void)
{
	g_parkedPosition = play_position();
	play_close();
	g_playerTakeId = NO_TAKE;
}

void gallery_player_request_overview(void)
{
	if (g_playerTakeId == NO_TAKE) {
		return;
	}
	char path[APP_PATH_SIZE];
	takes_path(g_playerTakeId, g_playerIsClean ? TAKE_CLEAN : TAKE_RAW, path, sizeof path);
	overview_request(path);
}

void gallery_player_toggle(const Take *take)
{
	gallery_player_load(take, true);
	play_set_playing(!play_is_playing());
}

void gallery_player_warn_if_muted(void)
{
	u8 volume = VOLUME_SLIDER_MAX;
	if (play_is_playing() && R_SUCCEEDED(HIDUSER_GetSoundVolume(&volume)) && volume == 0) {
		toast_warning("The volume slider is all the way down");
	}
}

void gallery_player_switch_raw_or_clean(const Take *take)
{
	if (develop_queue_has(take->id)) {
		toast_warning("REC_%04d is being developed", take->id);
		return;
	}
	if (!take->hasClean) {
		toast_warning("Not developed yet");
		return;
	}
	if (!take->hasRaw) {
		toast_warning("No raw file to compare with");
		return;
	}
	g_preferClean = !g_preferClean;
	gallery_player_load(take, true);
	toast(g_preferClean ? "Listening to the clean file" : "Listening to the raw take");
}

void gallery_player_jump_to_marker(int direction)
{
	u32 cues[WAV_MAX_CUES];
	int cueCount = play_cues(cues, WAV_MAX_CUES);
	u64 position = play_position();
	s64 target = -1;
	for (int i = 0; i < cueCount; i++) {
		s64 cue = cues[i];
		bool isAhead = cue > (s64)position + MARKER_AHEAD_MIN_SAMPLES;
		bool isBehind = cue + MARKER_BACK_GRACE_SAMPLES < (s64)position;
		if (direction > 0 && isAhead && (target < 0 || cue < target)) {
			target = cue;
		}
		if (direction < 0 && isBehind && (target < 0 || cue > target)) {
			target = cue;
		}
	}
	if (target < 0) {
		target = direction < 0 ? 0 : (s64)play_length();
	}
	play_seek((u64)target);
}

// 1 to seek forward, -1 back, 0 not at all.
static int held_seek_direction(u32 held)
{
	circlePosition stick = {0};
	hidCircleRead(&stick);
	if ((held & KEY_DRIGHT) || stick.dx > INPUT_STICK_THRESHOLD) {
		return 1;
	}
	if ((held & KEY_DLEFT) || stick.dx < -INPUT_STICK_THRESHOLD) {
		return -1;
	}
	return 0;
}

void gallery_player_seek_input(u32 held)
{
	static int backRepeat;
	static int forwardRepeat;
	int direction = held_seek_direction(held);
	if (input_repeat(direction > 0, &forwardRepeat, INPUT_REPEAT_SLOW) ||
	    input_repeat(direction < 0, &backRepeat, INPUT_REPEAT_SLOW)) {
		s64 target = (s64)play_position() + direction * (s64)(SEEK_STEP_SECONDS * play_sample_rate());
		play_seek(target < 0 ? 0 : (u64)target);
	}
}

bool gallery_player_scrub_input(u32 held, touchPosition touch)
{
	static int touchFrames;
	static int lastScrubX = -1;
	if (!(held & KEY_TOUCH)) {
		touchFrames = 0;
		lastScrubX = -1;
		return false;
	}
	bool onBar =
	        touch.py >= SCRUB_Y - SCRUB_TOUCH_SLACK && touch.py < SCRUB_Y + SCRUB_HEIGHT + SCRUB_TOUCH_SLACK;
	if (!onBar || !play_length()) {
		return false;
	}
	// Every move while paused; while playing only every few frames, as each seek refills the buffers from the
	// card.
	if (touch.px != lastScrubX && (!play_is_playing() || touchFrames % SCRUB_SEEK_EVERY_FRAMES == 0)) {
		lastScrubX = touch.px;
		float fraction = (touch.px - PANEL_TEXT_X) / (float)PANEL_TEXT_WIDTH;
		if (fraction < 0) {
			fraction = 0;
		} else if (fraction > 1) {
			fraction = 1;
		}
		play_seek((u64)(fraction * play_length()));
	}
	touchFrames++;
	return true;
}

void gallery_player_draw_scrub_bar(void)
{
	u64 length = play_length();
	u64 position = play_position();
	draw_rounded_rect(PANEL_TEXT_X, SCRUB_TRACK_Y, PANEL_TEXT_WIDTH, SCRUB_TRACK_HEIGHT, SCRUB_TRACK_RADIUS,
	                  COLOR_TILE, DEPTH_CONTROL);
	if (!length) {
		return;
	}
	u32 cues[WAV_MAX_CUES];
	int cueCount = play_cues(cues, WAV_MAX_CUES);
	for (int i = 0; i < cueCount; i++) {
		float tickX = PANEL_TEXT_X + (float)PANEL_TEXT_WIDTH * cues[i] / length - SCRUB_TICK_HALF_WIDTH;
		C2D_DrawRectSolid(tickX, SCRUB_TICK_Y, DEPTH_CONTROL_RAISED, SCRUB_TICK_WIDTH, SCRUB_TICK_HEIGHT,
		                  COLOR_ACCENT_DIM);
	}
	float knobX = PANEL_TEXT_X + (float)PANEL_TEXT_WIDTH * position / length;
	float filledWidth = knobX - PANEL_TEXT_X;
	draw_rounded_rect(PANEL_TEXT_X, SCRUB_TRACK_Y, filledWidth, SCRUB_TRACK_HEIGHT, SCRUB_TRACK_RADIUS,
	                  COLOR_ACCENT, DEPTH_CONTROL_RAISED);
	draw_circle(knobX, SCRUB_KNOB_Y, SCRUB_KNOB_RADIUS, COLOR_INK, DEPTH_CONTROL_TOP);
}
