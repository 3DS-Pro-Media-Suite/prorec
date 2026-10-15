#include "live_view.h"
#include <math.h>
#include <time.h>
#include "app.h"
#include "array_count.h"
#include "decibels.h"
#include "develop_queue.h"
#include "draw.h"
#include "fft.h"
#include "live_analysis.h"
#include "mic.h"
#include "noise_learning.h"
#include "noise_profile.h"
#include "play.h"
#include "profile_store.h"
#include "recorder.h"
#include "setting_row.h"
#include "settings.h"
#include "status.h"
#include "take_writer.h"
#include "takes.h"
#include "toast.h"

// Levels from here on are coloured: amber when loud, red near clipping.
#define LEVEL_LOUD_DB (-12)
#define LEVEL_HOT_DB (-3)

// The level meter: a bar from METER_MIN_DB to 0 dBFS with its scale below and the clip light to the right.
#define METER_Y 204
#define METER_X 40
#define METER_WIDTH 330
#define METER_HEIGHT 10
#define METER_RANGE_DB 60
#define METER_MIN_DB (-METER_RANGE_DB)
#define METER_ZONE_COUNT 3
#define METER_PEAK_INSET 2 // the peak strip is this much thinner than the bar at the top and the bottom
#define METER_PEAK_ALPHA 0.45f
#define METER_HOLD_WIDTH 2
#define METER_HOLD_OVERHANG 2
#define METER_TICK_GAP 1
#define METER_TICK_LENGTH 3
#define METER_LABEL_GAP (METER_TICK_GAP + METER_TICK_LENGTH) // from the bar's bottom to the scale labels
#define METER_READOUT_GAP 6
#define METER_READOUT_RAISE 1
#define METER_DEPTH 0.5f
#define METER_PEAK_DEPTH 0.51f
#define METER_RMS_DEPTH 0.52f
#define METER_HOLD_DEPTH 0.53f
#define CLIP_LIGHT_GAP 6
#define CLIP_LIGHT_X (METER_X + METER_WIDTH + CLIP_LIGHT_GAP)
#define CLIP_LIGHT_WIDTH 18
#define CLIP_LIGHT_OVERHANG 1
#define CLIP_LIGHT_RADIUS 3
#define CLIP_LABEL_X (CLIP_LIGHT_X + CLIP_LIGHT_WIDTH / 2)

// The waveform and the spectrum share the space between the info line and the meter.
#define GRAPH_Y 84
#define GRAPH_HEIGHT 104
#define SPLIT_WAVE_HEIGHT 52 // the live view with both
#define SPLIT_GAP 4
#define SPLIT_SPECTRUM_Y (GRAPH_Y + SPLIT_WAVE_HEIGHT + SPLIT_GAP)
#define SPLIT_SPECTRUM_HEIGHT (GRAPH_HEIGHT - SPLIT_WAVE_HEIGHT - SPLIT_GAP)

#define WAVE_GUIDE_COUNT 2
#define WAVE_MIN_HALF_HEIGHT 0.5f // a quiet column still shows
#define WAVE_GUIDE_DEPTH 0.41f
#define WAVE_MARKER_DEPTH 0.45f
#define WAVE_COLUMN_DEPTH 0.46f
#define COLOR_WAVE_GUIDE COLOR_RGBA(255, 255, 255, 18)

// The spectrum: bars on a log frequency axis, and the learned noise of this gain as a red line.
#define SPECTRUM_BARS 94
#define SPECTRUM_LOW_HZ 40.0f
#define SPECTRUM_HIGH_HZ 16000.0f
#define SPECTRUM_RANGE_DB 100 // the bars show -100..0 dB
#define SPECTRUM_COLOR_BOOST_DB 6 // bars change colour this much below the meter's levels
#define SPECTRUM_BAR_GAP 1
#define SPECTRUM_MIN_BAR_HEIGHT 0.5f
#define SPECTRUM_MARK_COUNT 3
#define SPECTRUM_LABEL_GAP 3
#define NOISE_LINE_THICKNESS 2
#define SPECTRUM_GUIDE_DEPTH 0.41f
#define SPECTRUM_BAR_DEPTH 0.45f
#define SPECTRUM_NOISE_DEPTH 0.47f
#define COLOR_SPECTRUM_GUIDE COLOR_RGBA(255, 255, 255, 16)
// Noise power through the sqrt-Hann window, times this and over FFT_SIZE squared, reads like noise in the
// live spectrum, which uses a Hann window.
#define NOISE_POWER_TO_DISPLAY 12.0f
#define POWER_FLOOR 1e-20f // keeps log10f away from zero

// The header strip.
#define HEADER_TEXT_Y 7
#define HEADER_BADGE_Y 5
#define HEADER_BADGE_HEIGHT 15
#define HEADER_BADGE_RADIUS 3
#define HEADER_BADGE_GAP 6
#define HEADER_TEXT_GAP 10
#define REC_BADGE_WIDTH 34
#define PAUSE_BADGE_WIDTH 44
#define PAUSE_BADGE_THICKNESS 1.5f
#define HEADER_CLOCK_X 232
#define HEADER_CARD_TIME_RIGHT_X 358
#define HEADER_BATTERY_X 366
#define HEADER_BATTERY_Y 8
#define CARD_LOW_SECONDS (10 * SECONDS_PER_MINUTE) // the card's time left turns amber below this

// The take time and the info line under it.
#define TAKE_TIME_Y 36
#define REC_DOT_GAP 14 // from the left edge of the time to the dot's centre
#define REC_DOT_Y 51
#define REC_DOT_RADIUS 6
#define REC_DOT_DEPTH 0.7f
#define INFO_LINE_Y 66

#define REC_EDGE_HEIGHT 2
#define REC_EDGE_DEPTH 0.95f
#define LIVE_TOAST_Y 150
#define SAVING_Y 120

#define UNAVAILABLE_TITLE_Y 80
#define UNAVAILABLE_ERROR_Y 118
#define UNAVAILABLE_HINT_Y 140

// The self-timer: a ring that empties as the seconds count down.
#define TIMER_CENTER_Y 130
#define TIMER_RADIUS 40
#define TIMER_RING_RADIUS 36
#define TIMER_TRACK_THICKNESS 2
#define TIMER_PROGRESS_THICKNESS 4
#define TIMER_FULL_TURN 6.2832f // a full turn, rounded a little above 2 pi
#define TIMER_DIGIT_Y 116
#define TIMER_DEPTH 0.8f
#define TIMER_TRACK_DEPTH 0.81f
#define TIMER_PROGRESS_DEPTH 0.82f
#define COLOR_TIMER_TRACK COLOR_RGBA(255, 255, 255, 60)

// The noise learning box.
#define LEARN_BOX_WIDTH 260
#define LEARN_BOX_X ((TOP_SCREEN_WIDTH - LEARN_BOX_WIDTH) / 2)
#define LEARN_BOX_Y 82
#define LEARN_BOX_HEIGHT 104
#define LEARN_BOX_RADIUS 12
#define LEARN_TITLE_Y 92
#define LEARN_HINT_Y 112
#define LEARN_GAIN_Y 126
#define LEARN_BAR_WIDTH 200
#define LEARN_BAR_X ((TOP_SCREEN_WIDTH - LEARN_BAR_WIDTH) / 2)
#define LEARN_BAR_Y 150
#define LEARN_BAR_HEIGHT 4
#define LEARN_BAR_RADIUS 2
#define LEARN_CANCEL_Y 162
#define LEARN_BOX_DEPTH 0.85f
#define LEARN_BAR_DEPTH 0.86f
#define LEARN_FILL_DEPTH 0.87f
#define COLOR_LEARN_BOX COLOR_RGBA(14, 14, 16, 245)

// The key hints.
#define CONTROLS_X 20
#define CONTROLS_WIDTH (TOP_SCREEN_WIDTH - 2 * CONTROLS_X)
#define CONTROLS_LINE_HEIGHT 14
#define CONTROLS_PADDING_X 10
#define CONTROLS_PADDING_Y 7
#define CONTROLS_RADIUS 8
#define CONTROLS_ACTION_SHARE 0.42f // the action column starts at this share of the box width
#define CONTROLS_DEPTH 0.9f
#define COLOR_CONTROLS COLOR_RGBA(0, 0, 0, 215)

typedef struct {
	float fromDb;
	float toDb;
	u32 color;
} MeterZone;

typedef struct {
	int hz;
	const char *label;
} FrequencyMark;

typedef struct {
	const char *keys;
	const char *action;
} ControlHint;

static const int METER_MARKS_DB[] = {-48, -36, -24, -18, -12, -6, -3, 0};
#define METER_MARK_COUNT ARRAY_COUNT(METER_MARKS_DB)
static const float WAVE_GUIDES_DB[WAVE_GUIDE_COUNT] = {-6.0f, -20.0f};
static const FrequencyMark SPECTRUM_MARKS[SPECTRUM_MARK_COUNT] = {{100, "100"}, {1000, "1k"}, {10000, "10k"}};

static bool g_showControls;

static int g_barFirstBin[SPECTRUM_BARS];
static int g_barLastBin[SPECTRUM_BARS];
static float g_barNoiseDb[SPECTRUM_BARS]; // SPECTRUM_SILENCE_DB: no noise line
static int g_barsGain = -1; // the gain and the profile version the bars belong to
static int g_barsProfileVersion = -1;

void live_view_show_controls(bool show)
{
	g_showControls = show;
}

static u32 level_color(float db)
{
	if (db >= LEVEL_HOT_DB) {
		return COLOR_ACCENT;
	}
	if (db >= LEVEL_LOUD_DB) {
		return COLOR_WARNING;
	}
	return COLOR_INK;
}

static float meter_x(float db)
{
	if (db < METER_MIN_DB) {
		db = METER_MIN_DB;
	}
	if (db > 0) {
		db = 0;
	}
	return METER_X + METER_WIDTH * (db + METER_RANGE_DB) / METER_RANGE_DB;
}

// The RMS bar and the lighter peak strip beyond it, coloured by the zones they reach.
static void draw_meter_bars(float y, float rmsX, float peakX)
{
	const MeterZone zones[METER_ZONE_COUNT] = {
	        {METER_MIN_DB, LEVEL_LOUD_DB, COLOR_INK},
	        {LEVEL_LOUD_DB, LEVEL_HOT_DB, COLOR_WARNING},
	        {LEVEL_HOT_DB, 0, COLOR_ACCENT},
	};
	for (int i = 0; i < METER_ZONE_COUNT; i++) {
		float zoneStart = meter_x(zones[i].fromDb);
		float zoneEnd = meter_x(zones[i].toDb);
		if (rmsX > zoneStart) {
			float rmsEnd = rmsX < zoneEnd ? rmsX : zoneEnd;
			C2D_DrawRectSolid(zoneStart, y, METER_RMS_DEPTH, rmsEnd - zoneStart, METER_HEIGHT,
			                  zones[i].color);
		}
		float peakStart = rmsX > zoneStart ? rmsX : zoneStart;
		if (peakX > peakStart && peakStart < zoneEnd) {
			float peakEnd = peakX < zoneEnd ? peakX : zoneEnd;
			u32 peakColor = draw_scale_alpha(zones[i].color, METER_PEAK_ALPHA);
			C2D_DrawRectSolid(peakStart, y + METER_PEAK_INSET, METER_PEAK_DEPTH, peakEnd - peakStart,
			                  METER_HEIGHT - 2 * METER_PEAK_INSET, peakColor);
		}
	}
}

static void draw_meter_scale(float y)
{
	for (int i = 0; i < METER_MARK_COUNT; i++) {
		float x = meter_x(METER_MARKS_DB[i]);
		C2D_DrawRectSolid(floorf(x), y + METER_HEIGHT + METER_TICK_GAP, METER_DEPTH, 1, METER_TICK_LENGTH,
		                  COLOR_FAINT);
		draw_text(g_fontSmall, x, y + METER_HEIGHT + METER_LABEL_GAP, COLOR_MUTED, ALIGN_CENTER, "%d",
		          METER_MARKS_DB[i]);
	}
}

void live_view_draw_meter(float y, float peak, float rms, float hold, bool clip)
{
	C2D_DrawRectSolid(METER_X, y, METER_DEPTH, METER_WIDTH, METER_HEIGHT, COLOR_TILE);
	float peakDb = live_analysis_level_db(peak);
	float rmsDb = live_analysis_level_db(rms);
	float holdDb = live_analysis_level_db(hold);
	draw_meter_bars(y, meter_x(rmsDb), meter_x(peakDb));
	bool hasHold = holdDb > METER_MIN_DB;
	if (hasHold) {
		float holdX = meter_x(holdDb) - METER_HOLD_WIDTH / 2;
		C2D_DrawRectSolid(holdX, y - METER_HOLD_OVERHANG, METER_HOLD_DEPTH, METER_HOLD_WIDTH,
		                  METER_HEIGHT + 2 * METER_HOLD_OVERHANG, level_color(holdDb));
	}
	draw_meter_scale(y);

	u32 readoutColor = hasHold ? level_color(holdDb) : COLOR_FAINT;
	const char *readoutFormat = hasHold ? "%.0f" : "-inf";
	draw_text(g_fontSmall, METER_X - METER_READOUT_GAP, y - METER_READOUT_RAISE, readoutColor, ALIGN_RIGHT,
	          readoutFormat, holdDb);

	u32 lightColor = clip ? COLOR_ACCENT : COLOR_TILE;
	float lightY = y - CLIP_LIGHT_OVERHANG;
	float lightHeight = METER_HEIGHT + 2 * CLIP_LIGHT_OVERHANG;
	draw_rounded_rect(CLIP_LIGHT_X, lightY, CLIP_LIGHT_WIDTH, lightHeight, CLIP_LIGHT_RADIUS, lightColor,
	                  METER_DEPTH);
	if (clip) {
		float labelY = y + METER_HEIGHT + METER_LABEL_GAP;
		draw_text(g_fontSmall, CLIP_LABEL_X, labelY, COLOR_ACCENT, ALIGN_CENTER, "CLIP");
	}
}

static u32 wave_column_color(u8 flags)
{
	if (flags & WAVE_CLIPPED) {
		return COLOR_ACCENT;
	}
	if (flags & WAVE_RECORDED) {
		return COLOR_INK;
	}
	return COLOR_WAVE_IDLE;
}

static void draw_wave(float top, float height)
{
	float centerY = top + height / 2;
	float maxHalfHeight = height / 2 - 1;
	C2D_DrawRectSolid(WAVE_X, floorf(centerY), DEPTH_WAVE, WAVE_COLUMNS, 1, COLOR_LINE);
	for (int i = 0; i < WAVE_COLUMNS; i++) {
		const WaveColumn *column = live_analysis_wave_column(i);
		if (column->flags & WAVE_MARKER) {
			C2D_DrawRectSolid(WAVE_X + i, top, WAVE_MARKER_DEPTH, 1, height, COLOR_ACCENT_DIM);
		}
		if (!column->height) {
			continue;
		}
		float halfHeight = maxHalfHeight * column->height / (float)WAVE_HEIGHT_MAX;
		if (halfHeight < WAVE_MIN_HALF_HEIGHT) {
			halfHeight = WAVE_MIN_HALF_HEIGHT;
		}
		C2D_DrawRectSolid(WAVE_X + i, centerY - halfHeight, WAVE_COLUMN_DEPTH, 1, halfHeight * 2,
		                  wave_column_color(column->flags));
	}
	for (int i = 0; i < WAVE_GUIDE_COUNT; i++) {
		float guideHalfHeight = maxHalfHeight * (WAVE_GUIDES_DB[i] + WAVE_RANGE_DB) / WAVE_RANGE_DB;
		C2D_DrawRectSolid(WAVE_X, floorf(centerY - guideHalfHeight), WAVE_GUIDE_DEPTH, WAVE_COLUMNS, 1,
		                  COLOR_WAVE_GUIDE);
		C2D_DrawRectSolid(WAVE_X, floorf(centerY + guideHalfHeight), WAVE_GUIDE_DEPTH, WAVE_COLUMNS, 1,
		                  COLOR_WAVE_GUIDE);
	}
}

// The lower frequency edge of a bar: the bars split the axis evenly on a log scale.
static float spectrum_bar_edge_hz(int bar)
{
	return SPECTRUM_LOW_HZ * powf(SPECTRUM_HIGH_HZ / SPECTRUM_LOW_HZ, (float)bar / SPECTRUM_BARS);
}

static float frequency_x(int hz)
{
	return WAVE_X + WAVE_COLUMNS * logf(hz / SPECTRUM_LOW_HZ) / logf(SPECTRUM_HIGH_HZ / SPECTRUM_LOW_HZ);
}

// The loudest noise in the bins, in the live spectrum's dB scale.
static float loudest_noise_db(const NoiseProfile *profile, int firstBin, int lastBin)
{
	float loudestDb = SPECTRUM_SILENCE_DB;
	if (!profile) {
		return loudestDb;
	}
	for (int bin = firstBin; bin <= lastBin; bin++) {
		float displayPower = profile->binPower[bin] * NOISE_POWER_TO_DISPLAY / ((float)FFT_SIZE * FFT_SIZE);
		float binDb = power_to_db(displayPower + POWER_FLOOR);
		if (binDb > loudestDb) {
			loudestDb = binDb;
		}
	}
	return loudestDb;
}

// The bins under each bar and the noise line. They change only with the gain or the learned profiles.
static void update_spectrum_bars(void)
{
	int gain = settings_mic_gain(g_settings.gainStep);
	if (gain == g_barsGain && profile_store_version() == g_barsProfileVersion) {
		return;
	}
	g_barsGain = gain;
	g_barsProfileVersion = profile_store_version();
	NoiseProfile storage;
	const NoiseProfile *profile = profile_store_for_gain(gain, &storage);
	const float binHz = MIC_RATE / FFT_SIZE;
	for (int bar = 0; bar < SPECTRUM_BARS; bar++) {
		int firstBin = (int)(spectrum_bar_edge_hz(bar) / binHz);
		int lastBin = (int)(spectrum_bar_edge_hz(bar + 1) / binHz);
		if (lastBin < firstBin) {
			lastBin = firstBin;
		}
		if (lastBin >= FFT_BINS) {
			lastBin = FFT_BINS - 1;
		}
		g_barFirstBin[bar] = firstBin;
		g_barLastBin[bar] = lastBin;
		g_barNoiseDb[bar] = loudest_noise_db(profile, firstBin, lastBin);
	}
}

static float bar_level_db(const float *spectrumDb, int bar)
{
	float levelDb = SPECTRUM_SILENCE_DB;
	for (int bin = g_barFirstBin[bar]; bin <= g_barLastBin[bar]; bin++) {
		if (spectrumDb[bin] > levelDb) {
			levelDb = spectrumDb[bin];
		}
	}
	return levelDb;
}

static void draw_spectrum(float top, float height)
{
	update_spectrum_bars();
	const float *spectrumDb = live_analysis_spectrum_db();
	const float barWidth = WAVE_COLUMNS / (float)SPECTRUM_BARS;
	float bottom = top + height;
	for (int bar = 0; bar < SPECTRUM_BARS; bar++) {
		float barX = WAVE_X + bar * barWidth;
		float levelDb = bar_level_db(spectrumDb, bar);
		float fill = (levelDb + SPECTRUM_RANGE_DB) / SPECTRUM_RANGE_DB;
		if (fill < 0) {
			fill = 0;
		}
		if (fill > 1) {
			fill = 1;
		}
		float barHeight = fill * height;
		if (barHeight >= SPECTRUM_MIN_BAR_HEIGHT) {
			u32 barColor = level_color(levelDb + SPECTRUM_COLOR_BOOST_DB);
			C2D_DrawRectSolid(barX, bottom - barHeight, SPECTRUM_BAR_DEPTH, barWidth - SPECTRUM_BAR_GAP,
			                  barHeight, barColor);
		}
		float noiseDb = g_barNoiseDb[bar];
		if (noiseDb > -SPECTRUM_RANGE_DB) {
			float noiseHeight = (noiseDb + SPECTRUM_RANGE_DB) / SPECTRUM_RANGE_DB * height;
			if (noiseHeight > height) {
				noiseHeight = height;
			}
			C2D_DrawRectSolid(barX, bottom - noiseHeight - NOISE_LINE_THICKNESS / 2, SPECTRUM_NOISE_DEPTH,
			                  barWidth, NOISE_LINE_THICKNESS, COLOR_ACCENT);
		}
	}
	C2D_DrawRectSolid(WAVE_X, bottom, DEPTH_WAVE, WAVE_COLUMNS, 1, COLOR_LINE);
	for (int i = 0; i < SPECTRUM_MARK_COUNT; i++) {
		float x = frequency_x(SPECTRUM_MARKS[i].hz);
		C2D_DrawRectSolid(floorf(x), top, SPECTRUM_GUIDE_DEPTH, 1, height, COLOR_SPECTRUM_GUIDE);
		draw_text(g_fontSmall, x + SPECTRUM_LABEL_GAP, top + 1, COLOR_FAINT, ALIGN_LEFT, "%s",
		          SPECTRUM_MARKS[i].label);
	}
}

// The REC or PAUSE badge, or READY. Returns how far the next item starts to the right.
static float draw_record_state(float x)
{
	if (take_writer_is_active() && !take_writer_is_paused()) {
		draw_rounded_rect(x, HEADER_BADGE_Y, REC_BADGE_WIDTH, HEADER_BADGE_HEIGHT, HEADER_BADGE_RADIUS,
		                  COLOR_ACCENT, DEPTH_HEADER_RAISED);
		draw_text(g_fontSmall, x + REC_BADGE_WIDTH / 2, HEADER_TEXT_Y, COLOR_BLACK, ALIGN_CENTER, "REC");
		return REC_BADGE_WIDTH + HEADER_BADGE_GAP;
	}
	if (take_writer_is_active()) {
		draw_frame(x, HEADER_BADGE_Y, PAUSE_BADGE_WIDTH, HEADER_BADGE_HEIGHT, PAUSE_BADGE_THICKNESS,
		           COLOR_ACCENT, DEPTH_HEADER_RAISED);
		draw_text(g_fontSmall, x + PAUSE_BADGE_WIDTH / 2, HEADER_TEXT_Y, COLOR_ACCENT, ALIGN_CENTER, "PAUSE");
		return PAUSE_BADGE_WIDTH + HEADER_BADGE_GAP;
	}
	return draw_text(g_fontSmall, x, HEADER_TEXT_Y, COLOR_MUTED, ALIGN_LEFT, "READY") + HEADER_TEXT_GAP;
}

static void draw_card_time_left(double secondsLeft)
{
	int hours = (int)(secondsLeft / SECONDS_PER_HOUR);
	int minutes = (int)(secondsLeft / SECONDS_PER_MINUTE) % MINUTES_PER_HOUR;
	u32 color = secondsLeft < CARD_LOW_SECONDS ? COLOR_WARNING : COLOR_INK;
	if (hours) {
		draw_text(g_fontSmall, HEADER_CARD_TIME_RIGHT_X, HEADER_TEXT_Y, color, ALIGN_RIGHT, "%dh %02dm",
		          hours, minutes);
	} else {
		draw_text(g_fontSmall, HEADER_CARD_TIME_RIGHT_X, HEADER_TEXT_Y, color, ALIGN_RIGHT, "%dm", minutes);
	}
}

static void draw_header_strip(void)
{
	draw_top_header_strip();
	float x = TOP_MARGIN;
	x += draw_record_state(x);
	float rateKhz = MIC_RATE / 1000;
	x += draw_text(g_fontSmall, x, HEADER_TEXT_Y, COLOR_MUTED, ALIGN_LEFT, "%.1f kHz  16-bit", rateKhz) +
	     HEADER_TEXT_GAP;
	// The take keeps the gain it started with.
	int gain =
	        take_writer_is_active() ? (u8)recorder_take_meta()->gain : settings_mic_gain(g_settings.gainStep);
	draw_text(g_fontSmall, x, HEADER_TEXT_Y, COLOR_INK, ALIGN_LEFT, "GAIN %.0f dB", mic_gain_db(gain));

	time_t now = time(NULL);
	struct tm *localNow = localtime(&now);
	draw_text(g_fontSmall, HEADER_CLOCK_X, HEADER_TEXT_Y, COLOR_INK, ALIGN_CENTER, "%02d:%02d",
	          localNow->tm_hour, localNow->tm_min);
	double secondsLeft = status_card_seconds_left();
	if (secondsLeft >= 0) {
		draw_card_time_left(secondsLeft);
	}
	status_draw_battery(HEADER_BATTERY_X, HEADER_BATTERY_Y, DEPTH_HEADER_RAISED);
}

static void draw_controls(void)
{
	const ControlHint hints[] = {
	        {g_isNew3ds ? "A / R / ZR" : "A / R", "Record / stop"},
	        {"X", "Pause / go on (while recording)"},
	        {"L", "Marker (while recording)"},
	        {"Circle Pad up / down", "Mic gain"},
	        {"Circle Pad left / right", "Turn the selected setting"},
	        {"D-pad", "Select a setting"},
	        {"Y", g_settings.monitor ? "Monitor off" : "Monitor on headphones"},
	        {"B", "Live view: waveform / spectrum / both"},
	        {"SELECT tap / hold", "Recordings / these hints"},
	        {"START", "Menu (noise learning, settings)"},
	        {"START + SELECT", "Exit"},
	};
	int hintCount = ARRAY_COUNT(hints);
	float boxHeight = 2 * CONTROLS_PADDING_Y + hintCount * CONTROLS_LINE_HEIGHT;
	float boxY = SCREEN_HEIGHT / 2 - boxHeight / 2;
	draw_rounded_rect(CONTROLS_X, boxY, CONTROLS_WIDTH, boxHeight, CONTROLS_RADIUS, COLOR_CONTROLS,
	                  CONTROLS_DEPTH);
	float keysX = CONTROLS_X + CONTROLS_PADDING_X;
	float actionX = CONTROLS_X + CONTROLS_WIDTH * CONTROLS_ACTION_SHARE;
	for (int i = 0; i < hintCount; i++) {
		float lineY = boxY + CONTROLS_PADDING_Y + i * CONTROLS_LINE_HEIGHT;
		draw_text(g_fontSmall, keysX, lineY, COLOR_ACCENT, ALIGN_LEFT, "%s", hints[i].keys);
		draw_text(g_fontSmall, actionX, lineY, COLOR_INK, ALIGN_LEFT, "%s", hints[i].action);
	}
}

static void draw_mic_unavailable(void)
{
	draw_text(g_fontLarge, TOP_CENTER_X, UNAVAILABLE_TITLE_Y, COLOR_INK, ALIGN_CENTER,
	          "Microphone unavailable");
	draw_text(g_fontSmall, TOP_CENTER_X, UNAVAILABLE_ERROR_Y, COLOR_MUTED, ALIGN_CENTER, "%s: 0x%08lX",
	          mic_error_where(), mic_error());
	draw_text(g_fontSmall, TOP_CENTER_X, UNAVAILABLE_HINT_Y, COLOR_MUTED, ALIGN_CENTER,
	          "START + SELECT to exit");
}

// The take's length while recording, with a red dot that blinks while paused.
static void draw_take_time(u64 now, bool isRecording)
{
	char timeText[APP_TIME_TEXT_SIZE];
	app_format_time(timeText, sizeof timeText, isRecording ? take_writer_frames() / MIC_RATE : 0, true);
	u32 color = isRecording ? COLOR_INK : COLOR_FAINT;
	float timeWidth = draw_text(g_fontLarge, TOP_CENTER_X, TAKE_TIME_Y, color, ALIGN_CENTER, "%s", timeText);
	if (isRecording && (!take_writer_is_paused() || draw_blink_is_on(now))) {
		draw_circle(TOP_CENTER_X - timeWidth / 2 - REC_DOT_GAP, REC_DOT_Y, REC_DOT_RADIUS, COLOR_ACCENT,
		            REC_DOT_DEPTH);
	}
}

// The take being recorded, or the name the next take gets.
static void draw_take_line(bool isRecording)
{
	if (isRecording) {
		draw_text(g_fontSmall, TOP_CENTER_X, INFO_LINE_Y, COLOR_MUTED, ALIGN_CENTER,
		          "REC_%04d   pre-record %.1f s", recorder_take_id(), recorder_take_meta()->preRecordSeconds);
		u32 backlog = take_writer_backlog();
		if (backlog > MIC_RATE) { // more than a second of sound waits for the card
			draw_text(g_fontSmall, TOP_SCREEN_WIDTH - TOP_MARGIN, INFO_LINE_Y, COLOR_WARNING, ALIGN_RIGHT,
			          "card %.1f s behind", backlog / MIC_RATE);
		}
	} else {
		draw_text(g_fontSmall, TOP_CENTER_X, INFO_LINE_Y, COLOR_FAINT, ALIGN_CENTER,
		          "next: REC_%04d   pre-record %s", takes_next_id(),
		          PRE_RECORD_NAMES[(int)g_settings.preRecord]);
	}
}

// The monitor needs the DSP and headphones.
static void draw_monitor_state(void)
{
	const char *text = "MONITOR";
	u32 color = COLOR_ACCENT;
	if (!play_available()) {
		text = "MONITOR: no DSP";
		color = COLOR_FAINT;
	} else if (!osIsHeadsetConnected()) {
		text = "MONITOR: no headphones";
		color = COLOR_FAINT;
	}
	draw_text(g_fontSmall, TOP_MARGIN, INFO_LINE_Y, color, ALIGN_LEFT, text);
}

static void draw_graphs(void)
{
	if (g_settings.liveView == LIVE_VIEW_WAVEFORM) {
		draw_wave(GRAPH_Y, GRAPH_HEIGHT);
	} else if (g_settings.liveView == LIVE_VIEW_SPECTRUM) {
		draw_spectrum(GRAPH_Y, GRAPH_HEIGHT);
	} else {
		draw_wave(GRAPH_Y, SPLIT_WAVE_HEIGHT);
		draw_spectrum(SPLIT_SPECTRUM_Y, SPLIT_SPECTRUM_HEIGHT);
	}
}

// Red lines at the top and the bottom edge say that a take is recording, whatever else is on screen.
static void draw_recording_edges(void)
{
	C2D_DrawRectSolid(0, 0, REC_EDGE_DEPTH, TOP_SCREEN_WIDTH, REC_EDGE_HEIGHT, COLOR_ACCENT);
	C2D_DrawRectSolid(0, SCREEN_HEIGHT - REC_EDGE_HEIGHT, REC_EDGE_DEPTH, TOP_SCREEN_WIDTH, REC_EDGE_HEIGHT,
	                  COLOR_ACCENT);
}

static void draw_self_timer(u64 now)
{
	u64 endsAt = recorder_timer_ends_at();
	float totalMs = (float)(endsAt - recorder_timer_started_at());
	float msLeft = now < endsAt ? (float)(endsAt - now) : 0;
	draw_circle(TOP_CENTER_X, TIMER_CENTER_Y, TIMER_RADIUS, COLOR_SHADE, TIMER_DEPTH);
	draw_arc(TOP_CENTER_X, TIMER_CENTER_Y, TIMER_RING_RADIUS, 0, TIMER_FULL_TURN, TIMER_TRACK_THICKNESS,
	         COLOR_TIMER_TRACK, TIMER_TRACK_DEPTH);
	float progressAngle = TIMER_FULL_TURN * msLeft / totalMs;
	draw_arc(TOP_CENTER_X, TIMER_CENTER_Y, TIMER_RING_RADIUS, 0, progressAngle, TIMER_PROGRESS_THICKNESS,
	         COLOR_ACCENT, TIMER_PROGRESS_DEPTH);
	int secondsShown = (int)(msLeft / 1000) + 1; // counts down to 1, never 0
	draw_text(g_fontLarge, TOP_CENTER_X, TIMER_DIGIT_Y, COLOR_INK, ALIGN_CENTER, "%d", secondsShown);
}

static void draw_learning_box(void)
{
	float progress = noise_learning_progress();
	draw_rounded_rect(LEARN_BOX_X, LEARN_BOX_Y, LEARN_BOX_WIDTH, LEARN_BOX_HEIGHT, LEARN_BOX_RADIUS,
	                  COLOR_LEARN_BOX, LEARN_BOX_DEPTH);
	draw_text(g_fontBody, TOP_CENTER_X, LEARN_TITLE_Y, COLOR_INK, ALIGN_CENTER, "Learning the mic's noise");
	draw_text(g_fontSmall, TOP_CENTER_X, LEARN_HINT_Y, COLOR_MUTED, ALIGN_CENTER,
	          "Quiet please: no talking, no touching");
	draw_text(g_fontSmall, TOP_CENTER_X, LEARN_GAIN_Y, COLOR_MUTED, ALIGN_CENTER,
	          "Gain %.0f dB (each gain has its own profile)",
	          mic_gain_db(settings_mic_gain(g_settings.gainStep)));
	draw_rounded_rect(LEARN_BAR_X, LEARN_BAR_Y, LEARN_BAR_WIDTH, LEARN_BAR_HEIGHT, LEARN_BAR_RADIUS,
	                  COLOR_TILE, LEARN_BAR_DEPTH);
	float done = progress > 1 ? 1 : progress;
	draw_rounded_rect(LEARN_BAR_X, LEARN_BAR_Y, LEARN_BAR_WIDTH * done, LEARN_BAR_HEIGHT, LEARN_BAR_RADIUS,
	                  COLOR_ACCENT, LEARN_FILL_DEPTH);
	draw_text(g_fontSmall, TOP_CENTER_X, LEARN_CANCEL_Y, COLOR_FAINT, ALIGN_CENTER, "B cancels");
}

static void draw_develop_progress(void)
{
	int takeId = develop_queue_current();
	if (!takeId) {
		return;
	}
	draw_text(g_fontSmall, TOP_SCREEN_WIDTH - TOP_MARGIN, INFO_LINE_Y, COLOR_ACCENT, ALIGN_RIGHT,
	          "DEV REC_%04d  %d%%", takeId, develop_queue_progress());
}

static void draw_recording_view(u64 now)
{
	draw_header_strip();
	bool isRecording = take_writer_is_active();
	draw_take_time(now, isRecording);
	draw_take_line(isRecording);
	if (g_settings.monitor) {
		draw_monitor_state();
	}
	draw_graphs();
	live_view_draw_meter(METER_Y, live_analysis_peak(), live_analysis_rms(), live_analysis_peak_hold(),
	                     live_analysis_clipping(now));
	if (isRecording && !take_writer_is_paused()) {
		draw_recording_edges();
	}
	if (recorder_timer_running()) {
		draw_self_timer(now);
	}
	if (noise_learning_is_active()) {
		draw_learning_box();
	}
	if (recorder_is_saving()) {
		draw_text(g_fontBody, TOP_CENTER_X, SAVING_Y, COLOR_INK, ALIGN_CENTER, "Saving...");
	}
	if (develop_queue_active() && !isRecording) {
		draw_develop_progress();
	}
}

void live_view_draw(void)
{
	u64 now = osGetTime();
	if (g_micAvailable) {
		draw_recording_view(now);
	} else {
		draw_mic_unavailable();
	}
	toast_draw(LIVE_TOAST_Y);
	if (g_showControls) {
		draw_controls();
	}
}
