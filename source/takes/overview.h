// The whole-take overview: a worker thread reads a WAV into one peak per waveform column, cached on the card.
#pragma once
#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

#define OVERVIEW_PATH_SIZE 96 // the cache folder and a take's file name

void overview_start(void);
void overview_stop(void);
void overview_cache_path(const char *wavPath, char *out, size_t outSize);
void overview_request(const char *wavPath); // a newer request replaces an unfinished one
bool overview_is_ready(void);
int overview_progress(void); // percent of the take read so far
const u8 *overview_columns(void); // WAVE_COLUMNS heights (0..WAVE_HEIGHT_MAX), once overview_is_ready
