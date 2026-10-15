#include "wav.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Large stdio buffers: the SD card is slow with small writes.
#define WRITE_BUFFER_BYTES (256 * 1024)
#define READ_BUFFER_BYTES (64 * 1024)

#define HEADER_BUFFER_BYTES 96 // room for the largest header, the float one
#define S16_WRITE_BUFFER_BYTES 2048
#define CONVERT_BUFFER_BYTES 3072 // a whole number of 2, 3 and 4 byte samples
#define S16_CONVERT_SAMPLES 1024

#define BITS_PER_BYTE 8
#define S16_BYTES 2
#define S24_BYTES 3
#define F32_BYTES 4
#define S16_BITS (S16_BYTES * BITS_PER_BYTE)
#define S24_BITS (S24_BYTES * BITS_PER_BYTE)
#define F32_BITS (F32_BYTES * BITS_PER_BYTE)

// Integer full scale and clip limits, as floats.
#define S16_FULL_SCALE 32768.0f
#define S16_MAX_FLOAT (S16_FULL_SCALE - 1.0f)
#define S16_MIN_FLOAT (-S16_FULL_SCALE)
#define S24_FULL_SCALE 8388608.0f
#define S24_MAX_FLOAT (S24_FULL_SCALE - 1.0f)
#define S24_MIN_FLOAT (-S24_FULL_SCALE)
#define S32_FULL_SCALE 2147483648.0f

#define DITHER_SEED 0x12345678u // xorshift needs a nonzero start
#define DITHER_NOISE_SHIFT 8 // keeps 24 random bits, which a float holds exactly
#define DITHER_NOISE_SCALE (1.0f / 16777216.0f) // 2^-24, so each noise spans 0 to 1

// A chunk is a 4-byte id, a 4-byte little-endian size and a body padded to an even length.
#define CHUNK_ID_BYTES 4
#define CHUNK_SIZE_AT 4
#define CHUNK_HEADER_BYTES 8
#define RIFF_TYPE_AT CHUNK_HEADER_BYTES
#define RIFF_HEADER_BYTES (CHUNK_HEADER_BYTES + CHUNK_ID_BYTES)

#define FORMAT_TAG_PCM 1
#define FORMAT_TAG_FLOAT 3
#define FORMAT_TAG_EXTENSIBLE 0xFFFE
#define MONO_CHANNELS 1

// Byte positions inside the fmt chunk body.
#define FMT_TAG_AT 0
#define FMT_CHANNELS_AT 2
#define FMT_RATE_AT 4
#define FMT_BYTE_RATE_AT 8
#define FMT_BLOCK_ALIGN_AT 12
#define FMT_BITS_AT 14
#define FMT_SUBFORMAT_TAG_AT 24 // the extensible sub-format GUID starts with the plain format tag
#define FMT_PCM_BYTES 16
#define FMT_EXTENSION_SIZE_BYTES 2
#define FMT_FLOAT_BYTES (FMT_PCM_BYTES + FMT_EXTENSION_SIZE_BYTES)
#define FMT_EXTENSIBLE_BYTES 40

#define FACT_BODY_BYTES 4 // the sample count

// A 'cue ' chunk holds a count and then one cue point per marker.
#define CUE_COUNT_BYTES 4
#define CUE_HEADER_BYTES (CHUNK_HEADER_BYTES + CUE_COUNT_BYTES)
#define CUE_POINT_BYTES 24
#define CUE_POINT_ID_AT 0
#define CUE_POINT_POSITION_AT 4
#define CUE_POINT_CHUNK_AT 8
#define CUE_POINT_CHUNK_START_AT 12
#define CUE_POINT_BLOCK_START_AT 16
#define CUE_POINT_SAMPLE_AT 20

// A 'LIST' 'adtl' chunk holds one 'labl' chunk per marker: the cue id and the name.
#define LIST_TYPE_BYTES 4
#define LIST_HEADER_BYTES (CHUNK_HEADER_BYTES + LIST_TYPE_BYTES)
#define LABEL_CUE_ID_BYTES 4
#define LABEL_HEADER_BYTES (CHUNK_HEADER_BYTES + LABEL_CUE_ID_BYTES)
#define MARKER_NAME_BYTES 16

#define REPAIR_MAX_DATA_BYTES 0xFFFFFFF0u // just under the 4 GiB limit of a 32-bit size

static void put_le16(uint8_t *bytes, uint32_t value)
{
	bytes[0] = value;
	bytes[1] = value >> 8;
}

static void put_le24(uint8_t *bytes, uint32_t value)
{
	bytes[0] = value;
	bytes[1] = value >> 8;
	bytes[2] = value >> 16;
}

static void put_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = value;
	bytes[1] = value >> 8;
	bytes[2] = value >> 16;
	bytes[3] = value >> 24;
}

static uint32_t get_le16(const uint8_t *bytes)
{
	return bytes[0] | bytes[1] << 8;
}

static uint32_t get_le24(const uint8_t *bytes)
{
	return bytes[0] | bytes[1] << 8 | bytes[2] << 16;
}

static uint32_t get_le32(const uint8_t *bytes)
{
	return bytes[0] | bytes[1] << 8 | bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static void put_chunk_header(uint8_t *bytes, const char *id, uint32_t size)
{
	memcpy(bytes, id, CHUNK_ID_BYTES);
	put_le32(bytes + CHUNK_SIZE_AT, size);
}

static bool chunk_has_id(const uint8_t *chunkHeader, const char *id)
{
	return memcmp(chunkHeader, id, CHUNK_ID_BYTES) == 0;
}

static uint32_t float_to_bits(float value)
{
	union {
		float asFloat;
		uint32_t asBits;
	} pun = {value};

	return pun.asBits;
}

static float float_from_bits(uint32_t bits)
{
	union {
		uint32_t asBits;
		float asFloat;
	} pun = {bits};

	return pun.asFloat;
}

int wav_bytes_per_sample(int format)
{
	if (format == WAV_S16) {
		return S16_BYTES;
	}
	return format == WAV_S24 ? S24_BYTES : F32_BYTES;
}

static float clamp_float(float value, float low, float high)
{
	if (value >= high) {
		return high;
	}
	if (value <= low) {
		return low;
	}
	return value;
}

// Plain format tags, not WAVE_FORMAT_EXTENSIBLE: some tools refuse the extensible header.
static int make_header(uint8_t *header, int format, uint32_t rate, uint64_t frames, uint32_t riffExtra)
{
	int bytesPerSample = wav_bytes_per_sample(format);
	uint32_t dataBytes = (uint32_t)(frames * bytesPerSample);
	bool isFloat = format == WAV_F32;
	memcpy(header, "RIFF", CHUNK_ID_BYTES);
	memcpy(header + RIFF_TYPE_AT, "WAVE", CHUNK_ID_BYTES);
	int length = RIFF_HEADER_BYTES;

	uint8_t *fmtBody = header + length + CHUNK_HEADER_BYTES;
	int fmtBytes = isFloat ? FMT_FLOAT_BYTES : FMT_PCM_BYTES;
	put_chunk_header(header + length, "fmt ", fmtBytes);
	put_le16(fmtBody + FMT_TAG_AT, isFloat ? FORMAT_TAG_FLOAT : FORMAT_TAG_PCM);
	put_le16(fmtBody + FMT_CHANNELS_AT, MONO_CHANNELS);
	put_le32(fmtBody + FMT_RATE_AT, rate);
	put_le32(fmtBody + FMT_BYTE_RATE_AT, rate * bytesPerSample);
	put_le16(fmtBody + FMT_BLOCK_ALIGN_AT, bytesPerSample);
	put_le16(fmtBody + FMT_BITS_AT, bytesPerSample * BITS_PER_BYTE);
	length += CHUNK_HEADER_BYTES + FMT_PCM_BYTES;

	// Float files need an empty extension in fmt and a fact chunk.
	if (isFloat) {
		put_le16(header + length, 0);
		length += FMT_EXTENSION_SIZE_BYTES;
		put_chunk_header(header + length, "fact", FACT_BODY_BYTES);
		put_le32(header + length + CHUNK_HEADER_BYTES, (uint32_t)frames);
		length += CHUNK_HEADER_BYTES + FACT_BODY_BYTES;
	}

	put_chunk_header(header + length, "data", dataBytes);
	length += CHUNK_HEADER_BYTES;
	uint32_t padding = dataBytes & 1;
	uint32_t riffBytes = (uint32_t)(length - CHUNK_HEADER_BYTES) + dataBytes + padding + riffExtra;
	put_le32(header + CHUNK_SIZE_AT, riffBytes);
	return length;
}

bool wav_create(WavWriter *writer, const char *path, int format, uint32_t rate)
{
	memset(writer, 0, sizeof *writer);
	writer->file = fopen(path, "wb");
	if (!writer->file) {
		return false;
	}
	writer->buffer = malloc(WRITE_BUFFER_BYTES);
	if (writer->buffer) {
		setvbuf(writer->file, writer->buffer, _IOFBF, WRITE_BUFFER_BYTES);
	}
	writer->format = format;
	writer->rate = rate;
	writer->ditherState = DITHER_SEED;
	uint8_t header[HEADER_BUFFER_BYTES];
	int headerBytes = make_header(header, format, rate, 0, 0);
	writer->dataStart = headerBytes;
	if (fwrite(header, 1, headerBytes, writer->file) != (size_t)headerBytes) {
		fclose(writer->file);
		free(writer->buffer);
		writer->file = NULL;
		return false;
	}
	return true;
}

bool wav_write_s16(WavWriter *writer, const int16_t *samples, int count)
{
	if (!writer->file || writer->format != WAV_S16 || count <= 0) {
		return count == 0;
	}
	uint8_t bytes[S16_WRITE_BUFFER_BYTES];
	int batchLimit = (int)sizeof bytes / S16_BYTES;
	int done = 0;
	while (done < count) {
		int batch = count - done;
		if (batch > batchLimit) {
			batch = batchLimit;
		}
		for (int i = 0; i < batch; i++) {
			put_le16(bytes + i * S16_BYTES, (uint16_t)samples[done + i]);
		}
		size_t batchBytes = (size_t)batch * S16_BYTES;
		if (fwrite(bytes, 1, batchBytes, writer->file) != batchBytes) {
			return false;
		}
		done += batch;
	}
	writer->frames += count;
	return true;
}

// Marsaglia's xorshift32.
static uint32_t xorshift32(uint32_t *state)
{
	uint32_t value = *state;
	value ^= value << 13;
	value ^= value >> 17;
	value ^= value << 5;
	*state = value;
	return value;
}

// TPDF dither: two uniform noises add up to a triangle that spans +-1 LSB.
static float tpdf_dither(uint32_t *state)
{
	float first = (float)(xorshift32(state) >> DITHER_NOISE_SHIFT);
	float second = (float)(xorshift32(state) >> DITHER_NOISE_SHIFT);
	return (first + second) * DITHER_NOISE_SCALE - 1.0f;
}

static void encode_sample(WavWriter *writer, float sample, uint8_t *bytes)
{
	if (writer->format == WAV_F32) {
		put_le32(bytes, float_to_bits(sample));
		return;
	}
	if (writer->format == WAV_S24) {
		float scaled = clamp_float(sample * S24_FULL_SCALE, S24_MIN_FLOAT, S24_MAX_FLOAT);
		int32_t quantized = (int32_t)lrintf(scaled);
		put_le24(bytes, (uint32_t)quantized);
		return;
	}
	float dither = tpdf_dither(&writer->ditherState);
	float scaled = clamp_float(sample * S16_FULL_SCALE + dither, S16_MIN_FLOAT, S16_MAX_FLOAT);
	put_le16(bytes, (uint16_t)(int16_t)lrintf(scaled));
}

bool wav_write_f32(WavWriter *writer, const float *samples, int count)
{
	if (!writer->file || count <= 0) {
		return count == 0;
	}
	uint8_t bytes[CONVERT_BUFFER_BYTES];
	int bytesPerSample = wav_bytes_per_sample(writer->format);
	int batchLimit = (int)sizeof bytes / bytesPerSample;
	int done = 0;
	while (done < count) {
		int batch = count - done;
		if (batch > batchLimit) {
			batch = batchLimit;
		}
		for (int i = 0; i < batch; i++) {
			encode_sample(writer, samples[done + i], bytes + i * bytesPerSample);
		}
		size_t batchBytes = (size_t)batch * bytesPerSample;
		if (fwrite(bytes, 1, batchBytes, writer->file) != batchBytes) {
			return false;
		}
		done += batch;
	}
	writer->frames += count;
	return true;
}

static bool write_sizes(WavWriter *writer, uint32_t riffExtra)
{
	uint8_t header[HEADER_BUFFER_BYTES];
	int headerBytes = make_header(header, writer->format, writer->rate, writer->frames, riffExtra);
	off_t writePosition = ftello(writer->file);
	if (writePosition < 0 || fseeko(writer->file, 0, SEEK_SET) != 0) {
		return false;
	}
	bool ok = fwrite(header, 1, headerBytes, writer->file) == (size_t)headerBytes;
	if (fseeko(writer->file, writePosition, SEEK_SET) != 0) {
		return false; // Writing on here would overwrite the take.
	}
	return ok;
}

bool wav_patch(WavWriter *writer)
{
	if (!writer->file) {
		return false;
	}
	bool ok = write_sizes(writer, 0);
	// Without fsync the new file size reaches the card only at fclose.
	if (fflush(writer->file) == 0) {
		fsync(fileno(writer->file));
	}
	return ok;
}

// Cue ids start at 1.
static bool write_cue_chunk(FILE *file, const uint32_t *cues, int cueCount)
{
	uint8_t bytes[CUE_POINT_BYTES];
	put_chunk_header(bytes, "cue ", CUE_COUNT_BYTES + CUE_POINT_BYTES * cueCount);
	put_le32(bytes + CHUNK_HEADER_BYTES, cueCount);
	bool ok = fwrite(bytes, 1, CUE_HEADER_BYTES, file) == CUE_HEADER_BYTES;
	for (int i = 0; i < cueCount; i++) {
		put_le32(bytes + CUE_POINT_ID_AT, i + 1);
		put_le32(bytes + CUE_POINT_POSITION_AT, cues[i]);
		memcpy(bytes + CUE_POINT_CHUNK_AT, "data", CHUNK_ID_BYTES);
		put_le32(bytes + CUE_POINT_CHUNK_START_AT, 0);
		put_le32(bytes + CUE_POINT_BLOCK_START_AT, 0);
		put_le32(bytes + CUE_POINT_SAMPLE_AT, cues[i]);
		ok &= fwrite(bytes, 1, CUE_POINT_BYTES, file) == CUE_POINT_BYTES;
	}
	return ok;
}

// The name keeps its terminating zero.
static uint32_t name_bytes(const char *name)
{
	return (uint32_t)strlen(name) + 1;
}

// Names the markers "Marker 1", "Marker 2" and so on. Audio editors show these names.
static bool write_label_list(FILE *file, int cueCount, uint32_t *chunkBytes)
{
	char names[WAV_MAX_CUES][MARKER_NAME_BYTES];
	uint32_t listBytes = LIST_TYPE_BYTES;
	for (int i = 0; i < cueCount; i++) {
		snprintf(names[i], sizeof names[i], "Marker %d", i + 1);
		uint32_t labelBytes = LABEL_CUE_ID_BYTES + name_bytes(names[i]);
		listBytes += CHUNK_HEADER_BYTES + labelBytes + (labelBytes & 1);
	}
	uint8_t listHeader[LIST_HEADER_BYTES];
	put_chunk_header(listHeader, "LIST", listBytes);
	memcpy(listHeader + CHUNK_HEADER_BYTES, "adtl", CHUNK_ID_BYTES);
	bool ok = fwrite(listHeader, 1, LIST_HEADER_BYTES, file) == LIST_HEADER_BYTES;
	for (int i = 0; i < cueCount; i++) {
		uint32_t nameBytes = name_bytes(names[i]);
		uint32_t labelBytes = LABEL_CUE_ID_BYTES + nameBytes;
		uint8_t labelHeader[LABEL_HEADER_BYTES];
		put_chunk_header(labelHeader, "labl", labelBytes);
		put_le32(labelHeader + CHUNK_HEADER_BYTES, i + 1);
		ok &= fwrite(labelHeader, 1, LABEL_HEADER_BYTES, file) == LABEL_HEADER_BYTES;
		ok &= fwrite(names[i], 1, nameBytes, file) == nameBytes;
		if (labelBytes & 1) {
			ok &= fputc(0, file) != EOF;
		}
	}
	*chunkBytes = CHUNK_HEADER_BYTES + listBytes;
	return ok;
}

bool wav_close_writer(WavWriter *writer, const uint32_t *cues, int cueCount)
{
	if (!writer->file) {
		return false;
	}
	bool ok = true;
	uint32_t dataBytes = (uint32_t)(writer->frames * wav_bytes_per_sample(writer->format));
	if (dataBytes & 1) {
		ok &= fputc(0, writer->file) != EOF; // Chunks start on even offsets.
	}
	if (cueCount > WAV_MAX_CUES) {
		cueCount = WAV_MAX_CUES;
	}
	uint32_t markerBytes = 0;
	if (cueCount > 0) {
		uint32_t labelListBytes = 0;
		ok &= write_cue_chunk(writer->file, cues, cueCount);
		ok &= write_label_list(writer->file, cueCount, &labelListBytes);
		markerBytes = CUE_HEADER_BYTES + CUE_POINT_BYTES * cueCount + labelListBytes;
	}
	ok &= write_sizes(writer, markerBytes);
	ok &= fclose(writer->file) == 0;
	free(writer->buffer);
	writer->file = NULL;
	writer->buffer = NULL;
	return ok;
}

// Reading.

static bool read_riff_header(FILE *file, off_t *fileSize)
{
	if (fseeko(file, 0, SEEK_END) != 0) {
		return false;
	}
	*fileSize = ftello(file);
	if (*fileSize < RIFF_HEADER_BYTES) {
		return false;
	}
	fseeko(file, 0, SEEK_SET);
	uint8_t header[RIFF_HEADER_BYTES];
	if (fread(header, 1, RIFF_HEADER_BYTES, file) != RIFF_HEADER_BYTES) {
		return false;
	}
	return chunk_has_id(header, "RIFF") && chunk_has_id(header + RIFF_TYPE_AT, "WAVE");
}

// Accepts mono 16-bit PCM, 24-bit PCM and 32-bit float.
static bool read_format_chunk(FILE *file, uint32_t chunkBytes, int *format, uint32_t *rate)
{
	uint8_t body[FMT_EXTENSIBLE_BYTES] = {0};
	uint32_t wanted = chunkBytes < FMT_EXTENSIBLE_BYTES ? chunkBytes : FMT_EXTENSIBLE_BYTES;
	if (fread(body, 1, wanted, file) < FMT_PCM_BYTES) {
		return false;
	}
	int tag = get_le16(body + FMT_TAG_AT);
	int channels = get_le16(body + FMT_CHANNELS_AT);
	int bits = get_le16(body + FMT_BITS_AT);
	if (tag == FORMAT_TAG_EXTENSIBLE && chunkBytes >= FMT_EXTENSIBLE_BYTES) {
		tag = get_le16(body + FMT_SUBFORMAT_TAG_AT);
	}
	*rate = get_le32(body + FMT_RATE_AT);
	if (channels != MONO_CHANNELS) {
		return false;
	}
	if (tag == FORMAT_TAG_PCM && bits == S16_BITS) {
		*format = WAV_S16;
	} else if (tag == FORMAT_TAG_PCM && bits == S24_BITS) {
		*format = WAV_S24;
	} else if (tag == FORMAT_TAG_FLOAT && bits == F32_BITS) {
		*format = WAV_F32;
	} else {
		return false;
	}
	return true;
}

static void read_cue_chunk(FILE *file, uint32_t *cues, int *cueCount)
{
	uint8_t countBytes[CUE_COUNT_BYTES];
	if (fread(countBytes, 1, CUE_COUNT_BYTES, file) != CUE_COUNT_BYTES) {
		return;
	}
	uint32_t pointCount = get_le32(countBytes);
	for (uint32_t i = 0; i < pointCount && *cueCount < WAV_MAX_CUES; i++) {
		uint8_t point[CUE_POINT_BYTES];
		if (fread(point, 1, CUE_POINT_BYTES, file) != CUE_POINT_BYTES) {
			break;
		}
		cues[*cueCount] = get_le32(point + CUE_POINT_SAMPLE_AT);
		*cueCount += 1;
	}
}

// Walks the chunks for the format, the data position and size, and the markers.
// cues, cueCount and dataSizeAt may be NULL.
static bool parse_chunks(FILE *file, int *format, uint32_t *rate, off_t *dataStart, uint64_t *frames,
                         uint32_t *cues, int *cueCount, off_t *dataSizeAt)
{
	off_t fileSize;
	if (!read_riff_header(file, &fileSize)) {
		return false;
	}
	bool haveFormat = false;
	*dataStart = -1;
	if (cueCount) {
		*cueCount = 0;
	}
	off_t chunkStart = RIFF_HEADER_BYTES;
	uint64_t dataBytes = 0;
	while (chunkStart + CHUNK_HEADER_BYTES <= fileSize) {
		uint8_t chunkHeader[CHUNK_HEADER_BYTES];
		fseeko(file, chunkStart, SEEK_SET);
		if (fread(chunkHeader, 1, CHUNK_HEADER_BYTES, file) != CHUNK_HEADER_BYTES) {
			break;
		}
		uint32_t chunkBytes = get_le32(chunkHeader + CHUNK_SIZE_AT);
		off_t bodyStart = chunkStart + CHUNK_HEADER_BYTES;
		if (chunk_has_id(chunkHeader, "fmt ")) {
			if (!read_format_chunk(file, chunkBytes, format, rate)) {
				return false;
			}
			haveFormat = true;
		} else if (chunk_has_id(chunkHeader, "data")) {
			*dataStart = bodyStart;
			if (dataSizeAt) {
				*dataSizeAt = chunkStart + CHUNK_SIZE_AT;
			}
			dataBytes = chunkBytes;
			// A take cut off by a crash says 0 or too much: its data runs to the end of the file.
			if (chunkBytes == 0 || (off_t)chunkBytes > fileSize - bodyStart) {
				dataBytes = (uint64_t)(fileSize - bodyStart);
				break;
			}
		} else if (chunk_has_id(chunkHeader, "cue ") && cues && cueCount) {
			read_cue_chunk(file, cues, cueCount);
		}
		uint32_t padding = chunkBytes & 1;
		chunkStart += (off_t)CHUNK_HEADER_BYTES + chunkBytes + padding; // 64-bit, so it always moves forward
	}
	if (!haveFormat || *dataStart < 0) {
		return false;
	}
	*frames = dataBytes / wav_bytes_per_sample(*format);
	return true;
}

bool wav_open(WavReader *reader, const char *path)
{
	memset(reader, 0, sizeof *reader);
	reader->file = fopen(path, "rb");
	if (!reader->file) {
		return false;
	}
	if (!parse_chunks(reader->file, &reader->format, &reader->rate, &reader->dataStart, &reader->frames,
	                  reader->cues, &reader->cueCount, NULL)) {
		fclose(reader->file);
		reader->file = NULL;
		return false;
	}
	reader->buffer = malloc(READ_BUFFER_BYTES);
	if (reader->buffer) {
		setvbuf(reader->file, reader->buffer, _IOFBF, READ_BUFFER_BYTES);
	}
	fseeko(reader->file, reader->dataStart, SEEK_SET);
	return true;
}

bool wav_seek(WavReader *reader, uint64_t frame)
{
	if (!reader->file) {
		return false;
	}
	if (frame > reader->frames) {
		frame = reader->frames;
	}
	reader->position = frame;
	off_t fileOffset = reader->dataStart + (off_t)(frame * wav_bytes_per_sample(reader->format));
	return fseeko(reader->file, fileOffset, SEEK_SET) == 0;
}

static float decode_sample(int format, const uint8_t *bytes)
{
	if (format == WAV_S16) {
		return (int16_t)get_le16(bytes) * (1.0f / S16_FULL_SCALE);
	}
	if (format == WAV_S24) {
		// In the top three bytes the sign of the 24-bit sample lands on bit 31.
		int32_t topAligned = (int32_t)(get_le24(bytes) << BITS_PER_BYTE);
		return topAligned * (1.0f / S32_FULL_SCALE);
	}
	return float_from_bits(get_le32(bytes));
}

int wav_read_f32(WavReader *reader, float *samples, int count)
{
	if (!reader->file) {
		return 0;
	}
	uint64_t remaining = reader->frames - reader->position;
	if ((uint64_t)count > remaining) {
		count = (int)remaining;
	}
	uint8_t bytes[CONVERT_BUFFER_BYTES];
	int bytesPerSample = wav_bytes_per_sample(reader->format);
	int batchLimit = (int)sizeof bytes / bytesPerSample;
	int done = 0;
	while (done < count) {
		int wanted = count - done;
		if (wanted > batchLimit) {
			wanted = batchLimit;
		}
		int got = (int)fread(bytes, bytesPerSample, wanted, reader->file);
		for (int i = 0; i < got; i++) {
			samples[done + i] = decode_sample(reader->format, bytes + i * bytesPerSample);
		}
		done += got;
		if (got < wanted) {
			break;
		}
	}
	reader->position += done;
	return done;
}

static int16_t float_to_s16(float sample)
{
	float scaled = sample * S16_FULL_SCALE;
	if (scaled >= S16_MAX_FLOAT) {
		return INT16_MAX;
	}
	if (scaled <= S16_MIN_FLOAT) {
		return INT16_MIN;
	}
	return (int16_t)lrintf(scaled);
}

int wav_read_s16(WavReader *reader, int16_t *samples, int count)
{
	if (reader->format == WAV_S16) {
		if (!reader->file) {
			return 0;
		}
		uint64_t remaining = reader->frames - reader->position;
		if ((uint64_t)count > remaining) {
			count = (int)remaining;
		}
		// The file and the 3DS are both little-endian.
		int got = (int)fread(samples, S16_BYTES, count, reader->file);
		reader->position += got;
		return got;
	}
	float converted[S16_CONVERT_SAMPLES];
	int done = 0;
	while (done < count) {
		int wanted = count - done;
		if (wanted > S16_CONVERT_SAMPLES) {
			wanted = S16_CONVERT_SAMPLES;
		}
		int got = wav_read_f32(reader, converted, wanted);
		for (int i = 0; i < got; i++) {
			samples[done + i] = float_to_s16(converted[i]);
		}
		done += got;
		if (got < wanted) {
			break;
		}
	}
	return done;
}

void wav_close_reader(WavReader *reader)
{
	if (reader->file) {
		fclose(reader->file);
	}
	free(reader->buffer);
	reader->file = NULL;
	reader->buffer = NULL;
}

static void write_le32_at(FILE *file, off_t position, uint32_t value)
{
	uint8_t bytes[sizeof(uint32_t)];
	put_le32(bytes, value);
	fseeko(file, position, SEEK_SET);
	fwrite(bytes, 1, sizeof bytes, file);
}

static bool repair_sizes(FILE *file)
{
	int format;
	uint32_t rate;
	off_t dataStart;
	off_t dataSizeAt = -1;
	uint64_t frames;
	if (!parse_chunks(file, &format, &rate, &dataStart, &frames, NULL, NULL, &dataSizeAt) ||
	    dataSizeAt <= 0) {
		return false;
	}
	uint8_t storedSize[sizeof(uint32_t)];
	fseeko(file, dataSizeAt, SEEK_SET);
	if (fread(storedSize, 1, sizeof storedSize, file) != sizeof storedSize) {
		return false;
	}
	uint64_t dataBytes = frames * wav_bytes_per_sample(format);
	uint32_t newDataSize = dataBytes > REPAIR_MAX_DATA_BYTES ? REPAIR_MAX_DATA_BYTES : (uint32_t)dataBytes;
	fseeko(file, 0, SEEK_END);
	off_t fileSize = ftello(file);
	// Only a take whose data runs to the end of the file gets repaired. One pad byte may follow it.
	bool dataRunsToEnd = dataStart + (off_t)newDataSize + 1 >= fileSize;
	if (get_le32(storedSize) == newDataSize || !dataRunsToEnd) {
		return false;
	}
	write_le32_at(file, dataSizeAt, newDataSize);
	uint64_t riffBytes = (uint64_t)fileSize - CHUNK_HEADER_BYTES;
	write_le32_at(file, CHUNK_SIZE_AT, riffBytes > UINT32_MAX ? UINT32_MAX : (uint32_t)riffBytes);
	if (format == WAV_F32) {
		// Our float files put the fact chunk right before the data chunk.
		off_t factCountAt = dataStart - CHUNK_HEADER_BYTES - FACT_BODY_BYTES;
		write_le32_at(file, factCountAt, (uint32_t)frames);
	}
	return true;
}

bool wav_repair(const char *path)
{
	FILE *file = fopen(path, "r+b");
	if (!file) {
		return false;
	}
	bool changed = repair_sizes(file);
	fclose(file);
	return changed;
}
