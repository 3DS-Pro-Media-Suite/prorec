// Develop: the passes that turn a raw take into the clean one (low cut, hum notches, noise reduction,
// click removal, clarity EQ, leveler, loudness normalisation, peak limiter, 24-bit / float / dithered 16-bit
// output). Portable C.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "noise_profile.h"

// Where the noise estimate comes from.
enum {
	NOISE_SOURCE_PROFILE,
	NOISE_SOURCE_AUTO,
	NOISE_SOURCE_BOTH
};

enum {
	VOICE_EQ_OFF,
	VOICE_EQ_NATURAL,
	VOICE_EQ_BROADCAST
};

// The hum filter's two mains frequencies.
#define MAINS_HZ_EUROPE 50
#define MAINS_HZ_AMERICAS 60
#define DEVELOP_MESSAGE_SIZE 128
#define DEVELOP_MIN_FRAMES FFT_SIZE // a shorter take is too short to develop

typedef struct {
	int lowCutHz; // 4th-order Butterworth, 0 = off (a DC blocker still runs)
	int humHz; // 0 = off, MAINS_HZ_EUROPE or MAINS_HZ_AMERICAS: notches on the mains frequency and harmonics
	int noiseReductionDb; // the most a bin is turned down, 0 = off
	// NOISE_SOURCE_*: the learned profile, an estimate from the take itself, or the larger of the two per bin
	int noiseSource;
	// VOICE_EQ_*: off, natural (undoes the 3DS mic's treble lift), broadcast (natural + warmth, presence)
	int voiceEq;
	int leveler; // a slow compressor that evens out the level around the target loudness
	int targetLufs; // loudness target, 0 = keep the level (only the limiter acts)
	int outputFormat; // WAV_S24, WAV_F32 or WAV_S16 (dithered)
	int removeClicks; // silence the button presses and taps logged during the take (events)
	// Pause cleanup: this much more broadband reduction while nobody speaks, 0 = off (needs noise reduction).
	int pauseCleanupDb;
} DevelopParams;

typedef struct {
	_Atomic int progress; // 0..100
	_Atomic bool cancel;
	char message[DEVELOP_MESSAGE_SIZE]; // what happened (or why it failed)
	float inLufs; // loudness after noise reduction, before levelling
	float outLufs;
	float peakDb; // output sample peak
} DevelopState;

// Develops inPath (any WAV this app writes) into outPath. scratchPath: a scratch file (floats). events:
// sample positions of button presses / taps in the take. Markers ('cue ' chunk) are carried over. Runs at any
// thread priority; checks state->cancel between blocks.
bool develop_run(const char *inPath, const char *outPath, const char *scratchPath,
                 const DevelopParams *params, const NoiseProfile *profile, const uint32_t *events,
                 int eventCount, DevelopState *state);
