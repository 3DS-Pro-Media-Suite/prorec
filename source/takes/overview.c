#include "overview.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "app.h"
#include "live_analysis.h"
#include "threads.h"
#include "wav.h"

#define OVERVIEW_STACK_SIZE (16 * 1024)
#define OVERVIEW_PRIORITY_OFFSET 1 // just after the main thread
#define READ_CHUNK_SAMPLES 8192
#define IDLE_SLEEP_MS 30 // how often an idle worker looks for a new request
#define CACHE_SUFFIX ".ov" // the cache file holds the WAV's size, then the column heights

// A request is a path and a number; the worker drops a read when a newer number comes in.
static LightLock g_requestLock;
static char g_requestedPath[APP_PATH_SIZE];
static _Atomic int g_requestNumber;
static _Atomic int g_finishedRequestNumber;
static u8 g_columns[WAVE_COLUMNS]; // what overview_columns shows
static u8 g_workColumns[WAVE_COLUMNS]; // the worker's result before it is shown
static _Atomic bool g_columnsReady;
static _Atomic int g_readPercent;
static Thread g_workerThread;
static _Atomic bool g_workerRunning;

void overview_cache_path(const char *wavPath, char *out, size_t outSize)
{
	const char *lastSlash = strrchr(wavPath, '/');
	const char *fileName = lastSlash ? lastSlash + 1 : wavPath;
	snprintf(out, outSize, CACHE_DIR "/%s" CACHE_SUFFIX, fileName);
}

// Copies the requested path. Returns the request's number.
static int take_request(char *path, size_t pathSize)
{
	LightLock_Lock(&g_requestLock);
	int requestNumber = g_requestNumber;
	snprintf(path, pathSize, "%s", g_requestedPath);
	LightLock_Unlock(&g_requestLock);
	return requestNumber;
}

static s64 file_size(const char *path)
{
	struct stat fileInfo;
	return stat(path, &fileInfo) == 0 ? (s64)fileInfo.st_size : -1;
}

// True when the cache belongs to a WAV of this size; g_workColumns then holds its columns.
static bool read_cache(const char *cachePath, s64 wavSize)
{
	FILE *file = fopen(cachePath, "rb");
	if (!file) {
		return false;
	}
	s64 cachedSize = 0;
	bool sizeMatches = fread(&cachedSize, sizeof cachedSize, 1, file) == 1 && cachedSize == wavSize;
	bool columnsRead = sizeMatches && fread(g_workColumns, 1, WAVE_COLUMNS, file) == WAVE_COLUMNS;
	fclose(file);
	return columnsRead;
}

static void write_cache(const char *cachePath, s64 wavSize)
{
	FILE *file = fopen(cachePath, "wb");
	if (!file) {
		return;
	}
	fwrite(&wavSize, sizeof wavSize, 1, file);
	fwrite(g_workColumns, 1, WAVE_COLUMNS, file);
	fclose(file);
}

// The first sample of the column after this one. The columns split the take evenly, rounded up.
static u64 column_end(int column, u64 frames)
{
	return ((u64)(column + 1) * frames + WAVE_COLUMNS - 1) / WAVE_COLUMNS;
}

// Reads the whole WAV into g_workColumns. False when it has no samples or a newer request came in.
static bool read_wav_columns(const char *path, int requestNumber)
{
	static float samples[READ_CHUNK_SAMPLES];
	static float peaks[WAVE_COLUMNS];
	WavReader reader;
	if (!wav_open(&reader, path)) {
		return false;
	}
	if (!reader.frames) {
		wav_close_reader(&reader);
		return false;
	}
	memset(peaks, 0, sizeof peaks);
	u64 position = 0;
	u64 nextColumnStart = 0;
	int column = -1;
	int count;
	bool aborted = false;
	while ((count = wav_read_f32(&reader, samples, READ_CHUNK_SAMPLES)) > 0) {
		for (int i = 0; i < count; i++) {
			while (position + i >= nextColumnStart && column < WAVE_COLUMNS - 1) {
				column++;
				nextColumnStart = column_end(column, reader.frames);
			}
			float magnitude = fabsf(samples[i]);
			if (magnitude > peaks[column]) {
				peaks[column] = magnitude;
			}
		}
		position += count;
		g_readPercent = (int)(position * 100 / reader.frames);
		if (g_requestNumber != requestNumber || !g_workerRunning) {
			aborted = true;
			break;
		}
	}
	wav_close_reader(&reader);
	if (aborted) {
		return false;
	}
	// live_analysis_wave_height rises with the level, so the peak's height is the column's height.
	for (int i = 0; i < WAVE_COLUMNS; i++) {
		g_workColumns[i] = live_analysis_wave_height(peaks[i]);
	}
	return true;
}

// Shows the result unless a newer request came in meanwhile.
static void publish_columns(int requestNumber, bool haveColumns)
{
	LightLock_Lock(&g_requestLock);
	if (requestNumber == g_requestNumber) {
		if (haveColumns) {
			memcpy(g_columns, g_workColumns, WAVE_COLUMNS);
		} else {
			memset(g_columns, 0, WAVE_COLUMNS);
		}
		g_columnsReady = true;
		g_finishedRequestNumber = requestNumber;
	}
	LightLock_Unlock(&g_requestLock);
}

static void overview_thread(void *arg)
{
	(void)arg;
	while (g_workerRunning) {
		if (g_requestNumber == g_finishedRequestNumber) {
			svcSleepThread(IDLE_SLEEP_MS * NANOSECONDS_PER_MS);
			continue;
		}
		char path[APP_PATH_SIZE];
		int requestNumber = take_request(path, sizeof path);
		s64 wavSize = file_size(path);
		char cachePath[OVERVIEW_PATH_SIZE];
		overview_cache_path(path, cachePath, sizeof cachePath);
		bool haveColumns = read_cache(cachePath, wavSize);
		if (!haveColumns && wavSize > 0) {
			haveColumns = read_wav_columns(path, requestNumber);
			if (haveColumns) {
				write_cache(cachePath, wavSize);
			}
		}
		publish_columns(requestNumber, haveColumns);
	}
}

void overview_request(const char *wavPath)
{
	LightLock_Lock(&g_requestLock);
	snprintf(g_requestedPath, sizeof g_requestedPath, "%s", wavPath);
	g_columnsReady = false;
	g_readPercent = 0;
	g_requestNumber++;
	LightLock_Unlock(&g_requestLock);
}

void overview_start(void)
{
	LightLock_Init(&g_requestLock);
	g_workerRunning = true;
	s32 priority = threads_priority_from_main(OVERVIEW_PRIORITY_OFFSET);
	g_workerThread =
	        threadCreate(overview_thread, NULL, OVERVIEW_STACK_SIZE, priority, THREAD_CORE_DEFAULT, false);
}

void overview_stop(void)
{
	g_workerRunning = false;
	if (g_workerThread) {
		threadJoin(g_workerThread, U64_MAX);
		threadFree(g_workerThread);
	}
}

bool overview_is_ready(void)
{
	return g_columnsReady;
}

int overview_progress(void)
{
	return g_readPercent;
}

const u8 *overview_columns(void)
{
	return g_columns;
}
