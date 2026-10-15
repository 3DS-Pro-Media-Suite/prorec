#include "noise_learning.h"
#include "app.h"
#include "mic.h"
#include "noise_profile.h"
#include "profile_store.h"
#include "recorder.h"
#include "settings.h"
#include "take_writer.h"
#include "toast.h"

#define BUTTON_CLICK_SECONDS 0.5f // skipped at the start: the click of the button that started it
#define COPY_CHUNK_SAMPLES 4096

static bool g_learningNoise;
static u64 g_learnStart;
static u64 g_learnPosition;
static NoiseProfileBuilder g_profileBuilder;

bool noise_learning_is_active(void)
{
	return g_learningNoise;
}

void noise_learning_cancel(void)
{
	g_learningNoise = false;
	toast("Noise learning cancelled");
}

float noise_learning_progress(void)
{
	u64 learned = g_learnPosition > g_learnStart ? g_learnPosition - g_learnStart : 0;
	return (float)learned / (NOISE_LEARNING_SECONDS * MIC_RATE);
}

void noise_learning_start(void)
{
	if (take_writer_is_active()) {
		toast_warning("Stop the recording first");
		return;
	}
	if (!g_micAvailable) {
		toast_warning("No microphone");
		return;
	}
	if (recorder_timer_running()) {
		recorder_cancel_timer();
	}
	noise_profile_begin(&g_profileBuilder);
	g_learnStart = mic_total_samples() + (u64)(BUTTON_CLICK_SECONDS * MIC_RATE);
	g_learnPosition = g_learnStart;
	g_learningNoise = true;
}

// Feeds the profile builder the mic samples up to total, but not past end.
static void feed_new_samples(u64 total, u64 end)
{
	static s16 samples[COPY_CHUNK_SAMPLES];
	static float levels[COPY_CHUNK_SAMPLES];
	while (g_learnPosition < total && g_learnPosition < end) {
		u64 stop = end < total ? end : total;
		int wanted = (int)(stop - g_learnPosition);
		int chunk = wanted > COPY_CHUNK_SAMPLES ? COPY_CHUNK_SAMPLES : wanted;
		int copied = mic_copy(g_learnPosition, samples, chunk);
		if (copied <= 0) {
			g_learnPosition = total;
			break;
		}
		for (int i = 0; i < copied; i++) {
			levels[i] = samples[i] * (1.0f / MIC_FULL_SCALE);
		}
		noise_profile_feed(&g_profileBuilder, levels, copied);
		g_learnPosition += copied;
	}
}

static void save_learned_profile(void)
{
	int gain = settings_mic_gain(g_settings.gainStep);
	NoiseProfile profile;
	if (!noise_profile_finish(&g_profileBuilder, &profile, gain, MIC_RATE)) {
		toast_warning("Noise learning failed");
		return;
	}
	char path[APP_PATH_SIZE];
	profile_store_path(gain, path, sizeof path);
	if (!noise_profile_save(&profile, path)) {
		toast_warning("Cannot write %s", path);
		return;
	}
	profile_store_set(gain, &profile);
	toast("Noise learned at %.0f dB gain: %.1f dBFS", mic_gain_db(gain), noise_profile_level_db(&profile));
}

void noise_learning_poll(void)
{
	if (!g_learningNoise) {
		return;
	}
	u64 total = mic_total_samples();
	u64 end = g_learnStart + (u64)(NOISE_LEARNING_SECONDS * MIC_RATE);
	feed_new_samples(total, end);
	if (g_learnPosition < end) {
		return;
	}
	g_learningNoise = false;
	save_learned_profile();
}
