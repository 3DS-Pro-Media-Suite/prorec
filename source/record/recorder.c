#include "recorder.h"
#include <string.h>
#include <time.h>
#include "app.h"
#include "develop.h"
#include "develop_queue.h"
#include "live_analysis.h"
#include "mic.h"
#include "screen_power.h"
#include "settings.h"
#include "status.h"
#include "take_writer.h"
#include "takes.h"
#include "toast.h"

#define TAKE_MAX_HOURS 9 // develop's float files must fit in FAT32's 4 GiB
#define STOP_DELAY_FRAMES 2 // the frame saying "Saving" is drawn before the take is closed
#define PRESS_HISTORY_SIZE 32

// Presses and taps noted while not recording, as mic sample numbers. A later take's pre-record may contain
// their clicks.
static u64 g_recentPresses[PRESS_HISTORY_SIZE];
static int g_pressCount; // all presses noted; the ring keeps the newest PRESS_HISTORY_SIZE

static bool g_selfTimerRunning;
static u64 g_selfTimerPress; // the mic sample of the record press that started it
static u64 g_selfTimerEnd;
static u64 g_selfTimerStart;

static int g_recordingTakeId;
static TakeMeta g_recordingMeta;
static bool g_stopRequested; // the stop starts on the next poll
static int g_stopFramesLeft; // not 0 while a stop is pending
static u64 g_firstSampleAfterResume; // the first mic sample after the app came back from the background

static aptHookCookie g_aptCookie;

void recorder_note_press(u64 sample)
{
	if (take_writer_is_active()) {
		take_writer_add_event(sample);
		return;
	}
	int slot = g_pressCount % PRESS_HISTORY_SIZE;
	g_pressCount++;
	g_recentPresses[slot] = sample;
}

bool recorder_timer_running(void)
{
	return g_selfTimerRunning;
}

bool recorder_timer_due(u64 now)
{
	return g_selfTimerRunning && now >= g_selfTimerEnd;
}

u64 recorder_timer_started_at(void)
{
	return g_selfTimerStart;
}

u64 recorder_timer_ends_at(void)
{
	return g_selfTimerEnd;
}

void recorder_cancel_timer(void)
{
	g_selfTimerRunning = false;
	g_selfTimerPress = 0;
	toast("Self-timer cancelled");
}

int recorder_take_id(void)
{
	return g_recordingTakeId;
}

const TakeMeta *recorder_take_meta(void)
{
	return &g_recordingMeta;
}

bool recorder_is_saving(void)
{
	return g_stopFramesLeft != 0;
}

// As far back as the pre-record setting asks, but only to samples the ring still holds and that came after
// the app last returned from the background.
static u64 pre_record_start(u64 now)
{
	int preRecordSeconds = PRE_RECORD_SECONDS[(int)g_settings.preRecord];
	u64 preRecordSamples = (u64)(preRecordSeconds * MIC_RATE);
	u64 start = now > preRecordSamples ? now - preRecordSamples : 0;
	if (start < mic_oldest_sample()) {
		start = mic_oldest_sample();
	}
	if (start < g_firstSampleAfterResume) {
		start = g_firstSampleAfterResume;
	}
	return start;
}

// Oldest first, so the take's events stay sorted.
static void add_recent_presses(u64 from, u64 to)
{
	int oldest = g_pressCount > PRESS_HISTORY_SIZE ? g_pressCount - PRESS_HISTORY_SIZE : 0;
	for (int i = oldest; i < g_pressCount; i++) {
		u64 press = g_recentPresses[i % PRESS_HISTORY_SIZE];
		if (press >= from && press < to) {
			take_writer_add_event(press);
		}
	}
}

// Written at the start as well, so a take keeps its metadata if the battery dies mid-take.
static void write_start_meta(int takeId, u64 start, u64 now)
{
	memset(&g_recordingMeta, 0, sizeof g_recordingMeta);
	time_t startTime = time(NULL);
	struct tm *localStart = localtime(&startTime);
	strftime(g_recordingMeta.date, sizeof g_recordingMeta.date, "%Y-%m-%d %H:%M:%S", localStart);
	g_recordingMeta.gain = mic_gain();
	g_recordingMeta.preRecordSeconds = (now - start) / MIC_RATE;
	takes_write_meta(takeId, &g_recordingMeta);
}

static void recorder_start(void)
{
	if (take_writer_is_active()) {
		return;
	}
	u64 timerPress = g_selfTimerPress;
	g_selfTimerPress = 0;
	if (!g_micAvailable) {
		toast_warning("No microphone (0x%08lX in %s)", mic_error(), mic_error_where());
		return;
	}
	int takeId = takes_next_id();
	if (takeId > TAKE_ID_MAX) {
		toast_warning("REC_%04d was the last number: empty " RECORDINGS_DIR " and restart", TAKE_ID_MAX);
		return;
	}
	char path[APP_PATH_SIZE];
	takes_path(takeId, TAKE_RAW, path, sizeof path);
	u64 now = mic_total_samples();
	u64 start = pre_record_start(now);
	if (!take_writer_start(path, start)) {
		toast_warning("Cannot write %s", path);
		return;
	}
	add_recent_presses(start, now);
	// The record press: the one that started the self-timer, else this moment.
	take_writer_add_event(timerPress ? timerPress : now);
	takes_advance_next_id();
	g_recordingTakeId = takeId;
	write_start_meta(takeId, start, now);
	live_analysis_flag_recorded(start);
	// The take goes on when the lid closes.
	aptSetSleepAllowed(false);
	aptSetHomeAllowed(false);
	screen_power_note_activity(osGetTime());
}

void recorder_start_after_timer(void)
{
	g_selfTimerRunning = false;
	recorder_start();
}

// The TXT gets what only the stop knows.
static void write_stop_meta(u64 frames, u32 lostSamples)
{
	g_recordingMeta.frames = (u32)frames;
	g_recordingMeta.lostSamples = lostSamples;
	g_recordingMeta.eventCount = take_writer_events(g_recordingMeta.events, TAKE_WRITER_MAX_EVENTS);
	g_recordingMeta.markerCount = take_writer_markers(g_recordingMeta.markers, TAKE_WRITER_MAX_MARKERS);
	takes_write_meta(g_recordingTakeId, &g_recordingMeta);
}

static void report_saved_take(bool written, u64 frames, u32 lostSamples)
{
	if (!written) {
		toast_warning("REC_%04d: write trouble (card full?)", g_recordingTakeId);
	} else if (lostSamples) {
		toast_warning("REC_%04d saved, %.1f s lost (card too slow)", g_recordingTakeId,
		              lostSamples / MIC_RATE);
	} else {
		char duration[APP_TIME_TEXT_SIZE];
		app_format_time(duration, sizeof duration, frames / MIC_RATE, true);
		toast("Saved REC_%04d  %s", g_recordingTakeId, duration);
	}
}

static void queue_auto_develop(u64 frames)
{
	if (!g_settings.autoDevelop || frames <= DEVELOP_MIN_FRAMES) {
		return;
	}
	DevelopParams params = settings_develop_params(&g_settings);
	if (!develop_queue_add(g_recordingTakeId, &params)) {
		toast_warning("REC_%04d saved, not developed: the develop queue is full", g_recordingTakeId);
	}
}

static void recorder_stop(void)
{
	if (!take_writer_is_active()) {
		return;
	}
	u64 stopSample = mic_total_samples();
	take_writer_add_event(stopSample); // the stop press
	u64 frames = take_writer_frames();
	u32 lostSamples = take_writer_lost_samples();
	bool written = take_writer_stop(stopSample);
	write_stop_meta(frames, lostSamples);
	aptSetSleepAllowed(true);
	aptSetHomeAllowed(true);
	screen_power_on();
	takes_add(g_recordingTakeId, (u32)frames, g_recordingMeta.date);
	report_saved_take(written, frames, lostSamples);
	if (written) {
		queue_auto_develop(frames);
	}
	status_refresh(true);
}

void recorder_record_button(void)
{
	if (g_selfTimerRunning) {
		recorder_cancel_timer();
		return;
	}
	if (take_writer_is_active()) {
		g_stopRequested = true;
		return;
	}
	if (!g_micAvailable) {
		toast_warning("No microphone (0x%08lX in %s)", mic_error(), mic_error_where());
		return;
	}
	int timerSeconds = SELF_TIMER_SECONDS[(int)g_settings.selfTimer];
	if (timerSeconds == 0) {
		recorder_start();
		return;
	}
	g_selfTimerRunning = true;
	g_selfTimerPress = mic_total_samples();
	g_selfTimerStart = osGetTime();
	g_selfTimerEnd = g_selfTimerStart + timerSeconds * 1000;
}

void recorder_toggle_pause(void)
{
	if (!take_writer_is_active()) {
		return;
	}
	if (!take_writer_is_paused()) {
		take_writer_add_event(mic_total_samples());
		take_writer_pause(mic_total_samples());
		toast("Paused");
		return;
	}
	if (!take_writer_resume(mic_total_samples())) {
		toast_warning("Still saving the part before the pause: press X again");
		return;
	}
	take_writer_add_event(mic_total_samples());
	toast("Recording again (marker)");
}

void recorder_add_marker(void)
{
	if (!take_writer_is_active() || take_writer_is_paused()) {
		return;
	}
	take_writer_add_marker(mic_total_samples());
	live_analysis_mark_newest_column();
	u32 markers[TAKE_WRITER_MAX_MARKERS];
	int markerCount = take_writer_markers(markers, TAKE_WRITER_MAX_MARKERS);
	toast("Marker %d", markerCount);
}

void recorder_poll(void)
{
	u64 maxFrames = (u64)(TAKE_MAX_HOURS * SECONDS_PER_HOUR * MIC_RATE);
	bool takeTooLong = take_writer_is_active() && take_writer_frames() >= maxFrames;
	if (takeTooLong && g_stopFramesLeft == 0 && !g_stopRequested) {
		toast_warning("%d hours: the take was stopped (file size limit)", TAKE_MAX_HOURS);
		g_stopRequested = true;
	}
	if (g_stopRequested) {
		g_stopRequested = false;
		g_stopFramesLeft = STOP_DELAY_FRAMES;
	} else if (g_stopFramesLeft != 0) {
		g_stopFramesLeft--;
		if (g_stopFramesLeft == 0) {
			recorder_stop();
		}
	}
	if (take_writer_is_active() && take_writer_failed()) {
		toast_warning("Writing failed: card full or removed");
		recorder_stop();
	}
}

// Runs inside aptMainLoop, or on APT's own thread for sleep, so it only switches things and sets flags.
static void apt_hook(APT_HookType hook, void *param)
{
	(void)param;
	if (hook == APTHOOK_ONSUSPEND) {
		screen_power_suspend();
		if (!take_writer_is_active()) {
			mic_hold(true);
		}
	} else if (hook == APTHOOK_ONRESTORE) {
		screen_power_restore();
		if (!take_writer_is_active()) {
			g_firstSampleAfterResume = mic_total_samples(); // older samples are from before the hold
		}
		mic_hold(false);
	}
}

void recorder_init(void)
{
	aptHook(&g_aptCookie, apt_hook, NULL);
}

void recorder_exit(void)
{
	aptUnhook(&g_aptCookie);
	recorder_stop(); // does nothing when no take is running
}
