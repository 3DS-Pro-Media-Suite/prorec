#include "noise_reducer.h"
#include <math.h>
#include <string.h>
#include "decibels.h"
#include "develop.h"
#include "voice_eq.h"

// Wiener gain on the decision-directed a-priori SNR.
#define NOISE_OVER_ESTIMATE 1.5f // reduce against a little more than the estimate
#define NOISE_POWER_EPSILON 1e-20f // keeps the SNR finite where the noise estimate is zero
#define MAX_POSTERIOR_SNR 1000 // 30 dB
#define PREVIOUS_FRAME_WEIGHT 0.98f // the a-priori SNR mostly follows the last frame's clean speech
#define CURRENT_FRAME_WEIGHT 0.02f // not 1 - PREVIOUS_FRAME_WEIGHT: in float that lands 10 ulp below 0.02f
#define MIN_PRIOR_SNR 1e-3f // -30 dB

// Pause cleanup. Noise alone lifts about 1 % of bins over LOUD_BIN_SNR.
#define LOUD_BIN_SNR 3
#define SPEECH_LOUD_BIN_PERCENT 6 // share of the speech band that has to be loud for speech
#define GATE_OPEN_RATE 0.6f // the gate opens within a few frames
#define GATE_CLOSE_RATE 0.1f // and closes more slowly
#define PAUSE_HOLD_SECONDS 0.25 // speech presence is held this long before pause cleanup sets in
// The band that tells whether someone speaks.
#define SPEECH_LOW_HZ 150.0
#define SPEECH_HIGH_HZ 8000.0

// Click repair: a logged button press is silenced from CLICK_BEFORE_SECONDS before it to CLICK_AFTER_SECONDS
// after it.
#define CLICK_BEFORE_SECONDS 0.040
#define CLICK_AFTER_SECONDS 0.090
#define CLICK_MAX_RISE 2.0f // magnitude factor, 6 dB
#define CLICK_FLOOR_FACTOR 0.03f // how far below the usual floor a click may go
#define MAGNITUDE_EPSILON 1e-12f

#define OVERLAP_ADD_SCALE 0.5f // the squared sqrt-Hann window sums to 2 at 75 % overlap

void noise_reducer_init(NoiseReducer *reducer, const NoiseReducerOptions *options)
{
	memset(reducer, 0, sizeof *reducer);
	double sampleRate = options->sampleRate;
	reducer->enabled = options->reduceNoise;
	if (options->noisePower) {
		memcpy(reducer->noisePower, options->noisePower, sizeof reducer->noisePower);
	}
	reducer->floorGain = db_to_amplitude(-(float)options->reductionDb);
	reducer->overEstimate = NOISE_OVER_ESTIMATE;
	reducer->frameStart = -FFT_SIZE; // input[] starts as FFT_SIZE zeros before the take
	reducer->events = options->events;
	reducer->eventCount = options->eventCount;
	reducer->clickBefore = (int)(CLICK_BEFORE_SECONDS * sampleRate);
	reducer->clickAfter = (int)(CLICK_AFTER_SECONDS * sampleRate);
	bool pauseCleanup = options->pauseCleanupDb > 0;
	reducer->pauseGain = pauseCleanup ? db_to_amplitude(-(float)options->pauseCleanupDb) : 1.0f;
	reducer->eqEnabled = options->voiceEq > VOICE_EQ_OFF;
	voice_eq_curve(options->voiceEq, (float)sampleRate, reducer->eqGain);
	reducer->speechPresence = 1; // start open: nothing is lost if the take begins with speech
	reducer->holdFrames = (int)(PAUSE_HOLD_SECONDS * sampleRate / FFT_HOP);
	reducer->speechBinLow = (int)(SPEECH_LOW_HZ * FFT_SIZE / sampleRate);
	reducer->speechBinHigh = (int)(SPEECH_HIGH_HZ * FFT_SIZE / sampleRate);
}

// True when a logged button press is close enough to this frame for its click to reach it.
static bool frame_has_click(NoiseReducer *reducer)
{
	if (!reducer->eventCount) {
		return false;
	}
	long frameFirst = reducer->frameStart;
	long frameEnd = reducer->frameStart + FFT_SIZE;
	// Skip the presses whose click ends before this frame.
	while (reducer->nextEvent < reducer->eventCount) {
		long eventSample = (long)reducer->events[reducer->nextEvent];
		if (eventSample + reducer->clickAfter >= frameFirst) {
			break;
		}
		reducer->nextEvent++;
	}
	for (int i = reducer->nextEvent; i < reducer->eventCount; i++) {
		long eventSample = (long)reducer->events[i];
		if (eventSample - reducer->clickBefore >= frameEnd) {
			break;
		}
		if (eventSample + reducer->clickAfter >= frameFirst) {
			return true;
		}
	}
	return false;
}

// Fills the Wiener gain of every bin and keeps this frame's clean speech power for the next one.
// Returns how many bins of the speech band are loud.
static int wiener_gains(NoiseReducer *reducer, const float *power, float *gains)
{
	int loudBins = 0;
	for (int k = 0; k < FFT_BINS; k++) {
		float noise = reducer->noisePower[k] * reducer->overEstimate + NOISE_POWER_EPSILON;
		float posteriorSnr = power[k] / noise;
		bool inSpeechBand = k >= reducer->speechBinLow && k <= reducer->speechBinHigh;
		if (inSpeechBand && posteriorSnr > LOUD_BIN_SNR) {
			loudBins++;
		}
		if (posteriorSnr > MAX_POSTERIOR_SNR) {
			posteriorSnr = MAX_POSTERIOR_SNR;
		}
		float currentSnr = posteriorSnr > 1 ? posteriorSnr - 1 : 0;
		float priorSnr = PREVIOUS_FRAME_WEIGHT * reducer->previousSpeech[k] / noise +
		                 CURRENT_FRAME_WEIGHT * currentSnr;
		if (priorSnr < MIN_PRIOR_SNR) {
			priorSnr = MIN_PRIOR_SNR;
		}
		float gain = priorSnr / (1 + priorSnr);
		reducer->previousSpeech[k] = gain * gain * power[k];
		gains[k] = gain;
	}
	return loudBins;
}

// Updates the speech presence from this frame and returns the pause cleanup gain.
static float pause_gate_gain(NoiseReducer *reducer, int loudBins)
{
	int speechBandBins = reducer->speechBinHigh - reducer->speechBinLow + 1;
	bool speech = loudBins > speechBandBins * SPEECH_LOUD_BIN_PERCENT / 100;
	if (speech) {
		reducer->holdLeft = reducer->holdFrames;
	} else if (reducer->holdLeft > 0) {
		reducer->holdLeft--;
	}
	bool open = speech || reducer->holdLeft != 0;
	float target = open ? 1.0f : 0.0f;
	float rate = target > reducer->speechPresence ? GATE_OPEN_RATE : GATE_CLOSE_RATE;
	reducer->speechPresence += (target - reducer->speechPresence) * rate;
	return powf(reducer->pauseGain, 1.0f - reducer->speechPresence);
}

// A button press: no bin may grow beyond CLICK_MAX_RISE times its magnitude before the press.
// The click's burst of energy goes; speech that simply carries on keeps its level and passes.
// The gain may go well below the usual floor, because a click far above the noise has to end at the noise.
static float click_capped_gain(const NoiseReducer *reducer, int k, float binPower, float gain)
{
	float magnitude = sqrtf(binPower) + MAGNITUDE_EPSILON;
	float cap = CLICK_MAX_RISE * reducer->lastMagnitude[k] / magnitude;
	float usualFloor = reducer->enabled ? reducer->floorGain : 1.0f;
	float clickFloor = usualFloor * CLICK_FLOOR_FACTOR;
	if (cap < gain) {
		gain = cap < clickFloor ? clickFloor : cap;
	}
	return gain;
}

// Noise reduction, click repair, pause cleanup and EQ on one frame's spectrum.
static void apply_gains(NoiseReducer *reducer, bool click, const float *power, float *real, float *imaginary)
{
	float gains[FFT_BINS];
	int loudBins = 0;
	if (reducer->enabled) {
		loudBins = wiener_gains(reducer, power, gains);
	} else {
		for (int k = 0; k < FFT_BINS; k++) {
			gains[k] = 1;
		}
	}
	float pauseGain = 1;
	// A button press is not speech, so the gate keeps its state through it.
	if (reducer->enabled && reducer->pauseGain < 1 && !click) {
		pauseGain = pause_gate_gain(reducer, loudBins);
	}
	for (int k = 0; k < FFT_BINS; k++) {
		float gain = fft_smoothed_bin(gains, k);
		if (gain < reducer->floorGain && reducer->enabled) {
			gain = reducer->floorGain;
		}
		if (click) {
			gain = click_capped_gain(reducer, k, power[k], gain);
		} else {
			if (!reducer->enabled) {
				gain = 1; // EQ only
			}
			gain *= pauseGain;
			reducer->lastMagnitude[k] = sqrtf(power[k]) * gain;
		}
		gain *= reducer->eqGain[k];
		real[k] *= gain;
		imaginary[k] *= gain;
	}
}

void noise_reducer_hop(NoiseReducer *reducer, const float *input, float *output)
{
	memmove(reducer->input, reducer->input + FFT_HOP, FFT_OVERLAP * sizeof(float));
	memcpy(reducer->input + FFT_OVERLAP, input, FFT_HOP * sizeof(float));
	reducer->frameStart += FFT_HOP;

	float real[FFT_BINS];
	float imaginary[FFT_BINS];
	float power[FFT_BINS];
	fft_frame_power(reducer->input, real, imaginary, power);
	bool click = frame_has_click(reducer);
	if (reducer->enabled || click || reducer->eqEnabled) {
		apply_gains(reducer, click, power, real, imaginary);
	} else {
		for (int k = 0; k < FFT_BINS; k++) {
			reducer->lastMagnitude[k] = sqrtf(power[k]);
		}
	}

	float frame[FFT_SIZE];
	fft_inverse_real(real, imaginary, frame);
	const float *window = fft_sqrt_hann();
	for (int n = 0; n < FFT_SIZE; n++) {
		reducer->overlapAdd[n] += frame[n] * window[n] * OVERLAP_ADD_SCALE;
	}
	memcpy(output, reducer->overlapAdd, FFT_HOP * sizeof(float));
	memmove(reducer->overlapAdd, reducer->overlapAdd + FFT_HOP, FFT_OVERLAP * sizeof(float));
	memset(reducer->overlapAdd + FFT_OVERLAP, 0, FFT_HOP * sizeof(float));
}
