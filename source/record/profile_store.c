#include "profile_store.h"
#include <stdio.h>
#include <stdlib.h>
#include "app.h"
#include "mic.h"
#include "noise_profile.h"
#include "toast.h"

static NoiseProfile *g_profiles[MIC_GAIN_MAX + 1]; // indexed by gain, NULL when not learned
static int g_profilesVersion; // changes whenever g_profiles does

void profile_store_path(int gain, char *path, size_t pathSize)
{
	snprintf(path, pathSize, NOISE_PROFILE_DIR "/g%03d.bin", gain);
}

void profile_store_load_all(void)
{
	for (int gain = 0; gain <= MIC_GAIN_MAX; gain++) {
		char path[APP_PATH_SIZE];
		profile_store_path(gain, path, sizeof path);
		NoiseProfile profile;
		if (!noise_profile_load(&profile, path)) {
			continue;
		}
		g_profiles[gain] = malloc(sizeof profile);
		if (g_profiles[gain]) {
			*g_profiles[gain] = profile;
		}
	}
	g_profilesVersion++;
}

int profile_store_count(void)
{
	int count = 0;
	for (int gain = 0; gain <= MIC_GAIN_MAX; gain++) {
		if (g_profiles[gain]) {
			count++;
		}
	}
	return count;
}

int profile_store_nearest(int gain)
{
	int best = -1;
	for (int learned = 0; learned <= MIC_GAIN_MAX; learned++) {
		if (!g_profiles[learned]) {
			continue;
		}
		if (best < 0 || abs(learned - gain) < abs(best - gain)) {
			best = learned;
		}
	}
	return best;
}

const NoiseProfile *profile_store_for_gain(int gain, NoiseProfile *storage)
{
	int nearest = profile_store_nearest(gain);
	if (nearest < 0) {
		return NULL;
	}
	*storage = *g_profiles[nearest];
	if (nearest != gain) {
		noise_profile_regain(storage, mic_gain_db(nearest), mic_gain_db(gain));
	}
	return storage;
}

void profile_store_forget_all(void)
{
	int forgotten = 0;
	for (int gain = 0; gain <= MIC_GAIN_MAX; gain++) {
		if (!g_profiles[gain]) {
			continue;
		}
		char path[APP_PATH_SIZE];
		profile_store_path(gain, path, sizeof path);
		remove(path);
		free(g_profiles[gain]);
		g_profiles[gain] = NULL;
		forgotten++;
	}
	g_profilesVersion++;
	toast(forgotten ? "Forgot %d noise profiles" : "No noise profiles learned yet", forgotten);
}

void profile_store_set(int gain, const NoiseProfile *profile)
{
	if (!g_profiles[gain]) {
		g_profiles[gain] = malloc(sizeof *profile);
	}
	if (g_profiles[gain]) {
		*g_profiles[gain] = *profile;
	}
	g_profilesVersion++;
}

bool profile_store_has(int gain)
{
	return g_profiles[gain] != NULL;
}

int profile_store_version(void)
{
	return g_profilesVersion;
}
