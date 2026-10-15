#include "takes.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "app.h"
#include "array_count.h"
#include "mic.h"
#include "take_writer.h"
#include "wav.h"

#define MAX_TAKES 1000

// File names: "REC_" and four digits, then the suffix of the file kind.
#define TAKE_NAME_PREFIX "REC_"
#define TAKE_STEM_LENGTH 8 // "REC_nnnn"
#define SCAN_PATH_SIZE (sizeof RECORDINGS_DIR + 1 + NAME_MAX) // the folder, a slash and any entry's name

// The TXT file: a format line, then one "key=value" per line.
#define META_FORMAT_LINE "prorec-take 1\n"
#define META_READ_SIZE 16384 // far more than a TXT holds

// A length from the file size until the TXT gives the exact one: a raw take is 16-bit after a 44-byte header,
// a clean file is counted as 24-bit after 68 bytes.
#define RAW_HEADER_BYTES 44
#define RAW_BYTES_PER_FRAME 2
#define CLEAN_HEADER_BYTES_GUESS 68
#define CLEAN_BYTES_PER_FRAME_GUESS 3

static Take *g_takes; // sorted by number
static int g_takeCount;
static int g_nextTakeId = 1;

static const char *const TAKE_FILE_SUFFIXES[TAKE_KIND_COUNT] = {"_RAW.WAV", ".WAV", ".TXT", "_NEW.WAV"};

int takes_count(void)
{
	return g_takeCount;
}

const Take *takes_at(int index)
{
	return &g_takes[index];
}

int takes_next_id(void)
{
	return g_nextTakeId;
}

void takes_advance_next_id(void)
{
	g_nextTakeId++;
}

void takes_path(int id, int kind, char *out, size_t outSize)
{
	snprintf(out, outSize, RECORDINGS_DIR "/" TAKE_NAME_PREFIX "%04d%s", id, TAKE_FILE_SUFFIXES[kind]);
}

static void write_number_list(FILE *file, const char *key, const u32 *values, int count)
{
	fprintf(file, "%s=", key);
	for (int i = 0; i < count; i++) {
		fprintf(file, i ? ",%lu" : "%lu", (unsigned long)values[i]);
	}
	fprintf(file, "\n");
}

// The develop settings in the TXT: one named key per DevelopParams field. A plain number of 0 is written "off".
typedef struct {
	const char *key;
	size_t offset; // of the int in DevelopParams
	const char *const *names; // the value's names, NULL for a plain number
	int nameCount;
} DevelopKey;

static const char *const OFF_ON_NAMES[] = {"off", "on"};
static const char *const NOISE_SOURCE_NAMES[] = {
        [NOISE_SOURCE_PROFILE] = "profile", [NOISE_SOURCE_AUTO] = "auto", [NOISE_SOURCE_BOTH] = "both"};
static const char *const VOICE_EQ_NAMES[] = {
        [VOICE_EQ_OFF] = "off", [VOICE_EQ_NATURAL] = "natural", [VOICE_EQ_BROADCAST] = "broadcast"};
static const char *const FORMAT_NAMES[] = {[WAV_S16] = "16-bit", [WAV_S24] = "24-bit", [WAV_F32] = "float"};

#define NUMBER_KEY(key, field) {key, offsetof(DevelopParams, field), NULL, 0}
#define NAMED_KEY(key, field, names) {key, offsetof(DevelopParams, field), names, ARRAY_COUNT(names)}

static const DevelopKey DEVELOP_KEYS[] = {
        NUMBER_KEY("develop_low_cut_hz=", lowCutHz),
        NUMBER_KEY("develop_hum_hz=", humHz),
        NUMBER_KEY("develop_noise_reduction_db=", noiseReductionDb),
        NAMED_KEY("develop_noise_source=", noiseSource, NOISE_SOURCE_NAMES),
        NAMED_KEY("develop_voice_eq=", voiceEq, VOICE_EQ_NAMES),
        NAMED_KEY("develop_leveler=", leveler, OFF_ON_NAMES),
        NUMBER_KEY("develop_target_lufs=", targetLufs),
        NAMED_KEY("develop_format=", outputFormat, FORMAT_NAMES),
        NAMED_KEY("develop_remove_clicks=", removeClicks, OFF_ON_NAMES),
        NUMBER_KEY("develop_pause_cleanup_db=", pauseCleanupDb),
};

static void write_develop_params(FILE *file, const DevelopParams *params)
{
	for (int i = 0; i < ARRAY_COUNT(DEVELOP_KEYS); i++) {
		const DevelopKey *key = &DEVELOP_KEYS[i];
		int value = *(const int *)((const char *)params + key->offset);
		if (key->names && value >= 0 && value < key->nameCount) {
			fprintf(file, "%s%s\n", key->key, key->names[value]);
		} else if (value) {
			fprintf(file, "%s%d\n", key->key, value);
		} else {
			fprintf(file, "%soff\n", key->key);
		}
	}
}

bool takes_write_meta(int id, const TakeMeta *meta)
{
	char path[APP_PATH_SIZE];
	takes_path(id, TAKE_TXT, path, sizeof path);
	FILE *file = fopen(path, "w");
	if (!file) {
		return false;
	}
	fprintf(file, META_FORMAT_LINE);
	fprintf(file,
	        "date=%s\n"
	        "rate=%.3f\n"
	        "gain=%d\n"
	        "gain_db=%.1f\n"
	        "prerecord_s=%.2f\n"
	        "frames=%lu\n"
	        "seconds=%.2f\n"
	        "lost=%lu\n",
	        meta->date, MIC_RATE, meta->gain, mic_gain_db(meta->gain), meta->preRecordSeconds,
	        (unsigned long)meta->frames, meta->frames / MIC_RATE, (unsigned long)meta->lostSamples);
	write_number_list(file, "events", meta->events, meta->eventCount);
	write_number_list(file, "markers", meta->markers, meta->markerCount);
	if (meta->result[0]) {
		fprintf(file, "develop=%s\n", meta->result);
	}
	if (meta->haveDevelopParams) {
		write_develop_params(file, &meta->developParams);
	}
	return fclose(file) == 0;
}

// Reads "1,2,3" up to the end of the line. Returns how many numbers it read.
static int parse_number_list(const char *text, u32 *out, int max)
{
	int count = 0;
	while (*text && *text != '\n' && count < max) {
		char *end;
		unsigned long value = strtoul(text, &end, 10);
		if (end == text) {
			break;
		}
		out[count] = (u32)value;
		count++;
		text = *end == ',' ? end + 1 : end;
	}
	return count;
}

// Copies the value up to the end of its line. An empty value leaves out unchanged.
static void copy_rest_of_line(const char *value, char *out, size_t outSize)
{
	size_t length = strcspn(value, "\n");
	if (length == 0) {
		return;
	}
	if (length >= outSize) {
		length = outSize - 1;
	}
	memcpy(out, value, length);
	out[length] = 0;
}

// True when the line starts with key; value then points just after it.
static bool line_has_key(const char *line, const char *key, const char **value)
{
	size_t keyLength = strlen(key);
	if (strncmp(line, key, keyLength) != 0) {
		return false;
	}
	*value = line + keyLength;
	return true;
}

// The index of the value's name, or the value read as a number.
static int named_value(const DevelopKey *key, const char *value)
{
	size_t length = strcspn(value, "\r\n");
	for (int i = 0; i < key->nameCount; i++) {
		if (strlen(key->names[i]) == length && strncmp(value, key->names[i], length) == 0) {
			return i;
		}
	}
	return atoi(value);
}

// False when the line is not a develop setting. "off" reads as 0.
static bool parse_develop_line(const char *line, TakeMeta *meta)
{
	for (int i = 0; i < ARRAY_COUNT(DEVELOP_KEYS); i++) {
		const DevelopKey *key = &DEVELOP_KEYS[i];
		const char *value;
		if (!line_has_key(line, key->key, &value)) {
			continue;
		}
		int *field = (int *)((char *)&meta->developParams + key->offset);
		*field = key->names ? named_value(key, value) : atoi(value);
		meta->haveDevelopParams = true;
		return true;
	}
	return false;
}

static void parse_meta_line(const char *line, TakeMeta *meta)
{
	const char *value;
	if (parse_develop_line(line, meta)) {
		return;
	}
	if (line_has_key(line, "date=", &value)) {
		copy_rest_of_line(value, meta->date, sizeof meta->date);
	} else if (line_has_key(line, "gain=", &value)) {
		meta->gain = atoi(value);
	} else if (line_has_key(line, "prerecord_s=", &value)) {
		meta->preRecordSeconds = (float)atof(value);
	} else if (line_has_key(line, "frames=", &value)) {
		meta->frames = strtoul(value, NULL, 10);
	} else if (line_has_key(line, "lost=", &value)) {
		meta->lostSamples = strtoul(value, NULL, 10);
	} else if (line_has_key(line, "events=", &value)) {
		meta->eventCount = parse_number_list(value, meta->events, TAKE_WRITER_MAX_EVENTS);
	} else if (line_has_key(line, "markers=", &value)) {
		meta->markerCount = parse_number_list(value, meta->markers, TAKE_WRITER_MAX_MARKERS);
	} else if (line_has_key(line, "develop=", &value)) {
		copy_rest_of_line(value, meta->result, sizeof meta->result);
	}
}

// The line after this one, or NULL at the end of the text.
static char *next_line(char *line)
{
	char *newline = strchr(line, '\n');
	return newline ? newline + 1 : NULL;
}

bool takes_read_meta(int id, TakeMeta *meta)
{
	memset(meta, 0, sizeof *meta);
	char path[APP_PATH_SIZE];
	takes_path(id, TAKE_TXT, path, sizeof path);
	FILE *file = fopen(path, "r");
	if (!file) {
		return false;
	}
	// Not a static buffer: the develop thread reads TXT files too.
	char *text = malloc(META_READ_SIZE);
	if (!text) {
		fclose(file);
		return false;
	}
	size_t length = fread(text, 1, META_READ_SIZE - 1, file);
	fclose(file);
	text[length] = 0;
	for (char *line = text; line && *line; line = next_line(line)) {
		parse_meta_line(line, meta);
	}
	free(text);
	return true;
}

static int compare_take_ids(const void *a, const void *b)
{
	const Take *takeA = a;
	const Take *takeB = b;
	return takeA->id - takeB->id;
}

static Take *takes_find(int id)
{
	for (int i = 0; i < g_takeCount; i++) {
		if (g_takes[i].id == id) {
			return &g_takes[i];
		}
	}
	return NULL;
}

// The take with this number, appended when it is not listed yet. NULL when the list is full.
static Take *find_or_append_take(int id)
{
	Take *take = takes_find(id);
	if (take) {
		return take;
	}
	if (!g_takes || g_takeCount >= MAX_TAKES) {
		return NULL;
	}
	take = &g_takes[g_takeCount];
	g_takeCount++;
	memset(take, 0, sizeof *take);
	take->id = id;
	return take;
}

// A new take has the highest number, so appending keeps the list sorted without a rescan.
void takes_add(int id, u32 frames, const char *date)
{
	Take *take = find_or_append_take(id);
	if (!take) {
		return;
	}
	take->hasRaw = true;
	take->frames = frames;
	snprintf(take->date, sizeof take->date, "%s", date);
}

// TAKE_RAW, TAKE_CLEAN or TAKE_TXT for a file name's suffix, else -1.
static int take_kind_of_suffix(const char *suffix)
{
	for (int kind = TAKE_RAW; kind <= TAKE_TXT; kind++) {
		if (!strcasecmp(suffix, TAKE_FILE_SUFFIXES[kind])) {
			return kind;
		}
	}
	return -1;
}

// Lists one file of the recordings folder. Returns its take number, or 0 when it is not a take's file.
static int list_take_file(const char *fileName)
{
	int id;
	int stemLength = 0;
	int fieldsRead = sscanf(fileName, TAKE_NAME_PREFIX "%4d%n", &id, &stemLength);
	if (fieldsRead != 1 || stemLength != TAKE_STEM_LENGTH || id <= 0 || id > TAKE_ID_MAX) {
		return 0;
	}
	int kind = take_kind_of_suffix(fileName + TAKE_STEM_LENGTH);
	if (kind < 0) {
		return 0;
	}
	Take *take = find_or_append_take(id);
	if (!take) {
		return id;
	}
	char path[SCAN_PATH_SIZE];
	snprintf(path, sizeof path, RECORDINGS_DIR "/%s", fileName);
	struct stat fileInfo;
	if (kind == TAKE_RAW) {
		take->hasRaw = true;
		if (stat(path, &fileInfo) == 0 && fileInfo.st_size > RAW_HEADER_BYTES) {
			take->frames = (u32)((fileInfo.st_size - RAW_HEADER_BYTES) / RAW_BYTES_PER_FRAME);
		}
	} else if (kind == TAKE_CLEAN) {
		take->hasClean = true;
		if (!take->hasRaw && stat(path, &fileInfo) == 0 && fileInfo.st_size > CLEAN_HEADER_BYTES_GUESS) {
			take->frames = (u32)((fileInfo.st_size - CLEAN_HEADER_BYTES_GUESS) / CLEAN_BYTES_PER_FRAME_GUESS);
		}
	}
	return id;
}

// A take is its raw or clean audio; a lone TXT (the take never got audio) is not listed.
static void drop_takes_without_audio(void)
{
	int keptCount = 0;
	for (int i = 0; i < g_takeCount; i++) {
		if (g_takes[i].hasRaw || g_takes[i].hasClean) {
			g_takes[keptCount] = g_takes[i];
			keptCount++;
		}
	}
	g_takeCount = keptCount;
}

static void read_dates_and_lengths(void)
{
	for (int i = 0; i < g_takeCount; i++) {
		Take *take = &g_takes[i];
		TakeMeta meta;
		if (!takes_read_meta(take->id, &meta)) {
			continue;
		}
		snprintf(take->date, sizeof take->date, "%s", meta.date);
		if (meta.frames) {
			take->frames = meta.frames;
		}
	}
}

void takes_scan(void)
{
	if (!g_takes) {
		g_takes = calloc(MAX_TAKES, sizeof(Take));
	}
	if (!g_takes) {
		return;
	}
	g_takeCount = 0;
	DIR *folder = opendir(RECORDINGS_DIR);
	if (!folder) {
		return;
	}
	int highestId = 0;
	struct dirent *entry;
	while ((entry = readdir(folder))) {
		int id = list_take_file(entry->d_name);
		if (id > highestId) {
			highestId = id;
		}
	}
	closedir(folder);
	drop_takes_without_audio();
	qsort(g_takes, g_takeCount, sizeof(Take), compare_take_ids);
	read_dates_and_lengths();
	if (highestId + 1 > g_nextTakeId) {
		g_nextTakeId = highestId + 1;
	}
}

// After a crash or a flat battery the raw takes' headers get their real sizes.
void takes_recover(void)
{
	for (int i = 0; i < g_takeCount; i++) {
		if (!g_takes[i].hasRaw) {
			continue;
		}
		char path[APP_PATH_SIZE];
		takes_path(g_takes[i].id, TAKE_RAW, path, sizeof path);
		wav_repair(path);
	}
}

void takes_delete_partials(void)
{
	for (int i = 0; i < g_takeCount; i++) {
		char path[APP_PATH_SIZE];
		takes_path(g_takes[i].id, TAKE_PARTIAL, path, sizeof path);
		remove(path);
	}
}

// Only this take's clean file is checked: a full rescan stalls the UI when there are many takes.
void takes_refresh_clean(int id)
{
	Take *take = takes_find(id);
	if (!take) {
		takes_scan();
		return;
	}
	char path[APP_PATH_SIZE];
	struct stat fileInfo;
	takes_path(id, TAKE_CLEAN, path, sizeof path);
	take->hasClean = stat(path, &fileInfo) == 0;
}

void takes_remove_at(int index)
{
	int takesAfter = g_takeCount - index - 1;
	memmove(&g_takes[index], &g_takes[index + 1], takesAfter * sizeof(Take));
	g_takeCount--;
}
