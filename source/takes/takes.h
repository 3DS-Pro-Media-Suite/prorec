// Takes on the card: REC_nnnn_RAW.WAV (the mic, untouched), REC_nnnn.WAV (developed), REC_nnnn.TXT (facts),
// and the list of them in memory.
#pragma once
#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>
#include "develop.h"
#include "take_writer.h"

#define TAKE_ID_MAX 9999 // REC_nnnn has four digits
#define TAKE_DATE_SIZE 20 // "YYYY-MM-DD hh:mm:ss" and its terminator
#define TAKE_RESULT_SIZE 128

typedef struct {
	u16 id;
	bool hasRaw;
	bool hasClean;
	u32 frames;
	char date[TAKE_DATE_SIZE];
} Take;

// The files of one take, for takes_path.
enum {
	TAKE_RAW,
	TAKE_CLEAN,
	TAKE_TXT,
	TAKE_PARTIAL, // a develop writes here and renames it when done
	TAKE_KIND_COUNT
};

// What the TXT file of a take holds.
typedef struct {
	char date[TAKE_DATE_SIZE];
	int gain;
	float preRecordSeconds;
	u32 frames;
	u32 lostSamples;
	int eventCount;
	int markerCount;
	u32 events[TAKE_WRITER_MAX_EVENTS];
	u32 markers[TAKE_WRITER_MAX_MARKERS];
	char result[TAKE_RESULT_SIZE]; // what the last develop reported
	DevelopParams developParams; // what the last develop used
	bool haveDevelopParams;
} TakeMeta;

// The list of takes, sorted by number.
int takes_count(void);
const Take *takes_at(int index);
void takes_path(int id, int kind, char *out, size_t outSize);
bool takes_write_meta(int id, const TakeMeta *meta);
bool takes_read_meta(int id, TakeMeta *meta);
int takes_next_id(void); // the number the next take gets
void takes_advance_next_id(void); // a take started with takes_next_id
void takes_add(int id, u32 frames, const char *date);
void takes_remove_at(int index); // once the take's files are gone
void takes_refresh_clean(int id); // a develop of this take ended
void takes_scan(void);
void takes_delete_partials(void); // develops that a crash interrupted
void takes_recover(void); // fixes the raw files' headers after a crash
