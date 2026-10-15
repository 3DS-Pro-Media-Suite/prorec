// WAV files, mono only. The recorder writes 16-bit PCM takes that survive a crash.
// Developed copies are 24-bit PCM, 32-bit float or dithered 16-bit.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

// Sample formats.
enum {
	WAV_S16,
	WAV_S24,
	WAV_F32
};

#define WAV_MAX_CUES 64

typedef struct {
	FILE *file;
	int format;
	uint32_t rate;
	uint64_t frames; // samples written
	off_t dataStart; // file offset of the first sample, 64-bit because developed files can pass 2 GiB
	uint32_t ditherState; // TPDF dither noise, for 16-bit output from float
	char *buffer; // large stdio buffer: the SD card is slow with small writes
} WavWriter;

typedef struct {
	FILE *file;
	int format;
	uint32_t rate;
	uint64_t frames; // samples in the file
	uint64_t position; // next sample to read
	off_t dataStart;
	int cueCount;
	uint32_t cues[WAV_MAX_CUES]; // marker positions in samples
	char *buffer;
} WavReader;

int wav_bytes_per_sample(int format);
bool wav_create(WavWriter *writer, const char *path, int format, uint32_t rate);
// Only for WAV_S16 files.
bool wav_write_s16(WavWriter *writer, const int16_t *samples, int count);
// Any format. Full scale is +-1.0. 16-bit output gets TPDF dither.
bool wav_write_f32(WavWriter *writer, const float *samples, int count);
// Writes the sizes so far into the header, so a crash loses little. Writing goes on afterwards.
bool wav_patch(WavWriter *writer);
// The markers go into 'cue ' and 'LIST' 'adtl' chunks after the data.
bool wav_close_writer(WavWriter *writer, const uint32_t *cues, int cueCount);

bool wav_open(WavReader *reader, const char *path);
// Returns the number of samples read, 0 at the end.
int wav_read_f32(WavReader *reader, float *samples, int count);
// Converts other formats, rounded and clipped.
int wav_read_s16(WavReader *reader, int16_t *samples, int count);
bool wav_seek(WavReader *reader, uint64_t frame);
void wav_close_reader(WavReader *reader);

// Fixes the RIFF and data sizes after a crash or power loss, when the header says 0 or too much.
// Returns true when it changed the file.
bool wav_repair(const char *path);
