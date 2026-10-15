// App-wide basics: the folders on the card, what start-up found out, and small helpers every screen uses.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define APPS_DIR "sdmc:/3ds"
#define APP_DIR APPS_DIR "/prorec"
#define RECORDINGS_DIR "sdmc:/Recordings"
#define NOISE_PROFILE_DIR APP_DIR "/noise"
#define CACHE_DIR APP_DIR "/cache"
#define DEVELOP_SCRATCH_PATH APP_DIR "/develop.tmp"
#define APP_PATH_SIZE 64 // a card path: a folder, a file name and a suffix

#define SECONDS_PER_MINUTE 60
#define MINUTES_PER_HOUR 60
#define SECONDS_PER_HOUR (SECONDS_PER_MINUTE * MINUTES_PER_HOUR)

extern bool g_isNew3ds;
extern bool g_micAvailable;

#define APP_TIME_TEXT_SIZE 24 // room for app_format_time

int app_clamp(int value, int low, int high);
// Writes a duration as mm:ss or h:mm:ss, with tenths of a second when asked.
void app_format_time(char *out, size_t outSize, double totalSeconds, bool withTenths);
