#include "develop.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "auto_noise.h"
#include "decibels.h"
#include "fft.h"
#include "filters.h"
#include "levels.h"
#include "noise_profile.h"
#include "noise_reducer.h"
#include "wav.h"

#define BLOCK_SIZE 4096 // samples per read and write
#define SCRATCH_BUFFER_SIZE (128 * 1024) // stdio buffer of the scratch file
// The float scratch file has to fit in FAT32's 4 GiB.
#define FAT32_MAX_FILE_SIZE 0xFFFFFFFFull
#define SCRATCH_FILE_SLACK 4096
#define MAX_FRAMES ((FAT32_MAX_FILE_SIZE - SCRATCH_FILE_SLACK) / sizeof(float))

// Each pass's share of the progress bar.
#define WEIGHT_NOISE_ESTIMATE 15
#define WEIGHT_NOISE_REDUCTION 60
#define WEIGHT_LEVELER 10
#define WEIGHT_OUTPUT 15

#define NO_NOISE_ESTIMATE (-1.0f) // a negative bin power: that source has no estimate

#define MAX_PRE_GAIN_DB 42 // a near-silent take is not blown up into pure noise
#define MAX_MAKE_UP_GAIN_DB 12
#define LIMITER_CEILING_DB (-1.0f)
#define PEAK_FLOOR 1e-9f // keeps the logarithm finite on silence

// One develop: the input, the buffers and the state the passes share.
typedef struct {
	const DevelopParams *params;
	DevelopState *state;
	const char *scratchPath;
	WavReader reader;
	double sampleRate;
	long totalFrames;
	float *block; // samples read
	float *output; // samples to write
	NoiseReducer *reducer;
	PreFilters preFilters;
	AutoNoise autoNoise;
	LoudnessMeter loudness;
	FILE *scratchFile;
	char *scratchBuffer;
	float progressStart; // the weight of the passes done
	float progressTotal; // the weight of all passes
} DevelopRun;

// The gains of the level stages, worked out from the loudness after noise reduction.
typedef struct {
	float targetLufs;
	float preGainDb; // brings the take to the target, where the leveler works
	float makeUpGainDb; // puts the leveler's result back on the target
	bool useLeveler;
} LevelPlan;

// The noise reducer's output on its way to the scratch file.
typedef struct {
	long emitted;
	long latencyLeft; // the STFT's latency: its first outputs are dropped
	int outputCount;
} ReducedOutput;

// The last stages of the output pass.
typedef struct {
	Compressor leveler;
	Limiter *limiter;
	float preGain;
	float makeUpGain;
	bool useLeveler;
	int latencyLeft; // the limiter's first outputs are its delay and are dropped
	long written;
	float peak;
} OutputChain;

static void set_message(DevelopState *state, const char *format, const char *argument)
{
	snprintf(state->message, sizeof state->message, format, argument ? argument : "");
}

// True when the develop is to stop; the message says so.
static bool cancel_requested(DevelopRun *run)
{
	if (!run->state->cancel) {
		return false;
	}
	set_message(run->state, "Cancelled", NULL);
	return true;
}

static void report_progress(DevelopRun *run, float passWeight, long framesDone)
{
	float passShare = passWeight * framesDone / run->totalFrames;
	run->state->progress = (int)((run->progressStart + passShare) * 100 / run->progressTotal);
}

// The scratch buffer is optional: without it the scratch file uses the default stdio buffer.
static void buffer_scratch_file(DevelopRun *run)
{
	if (run->scratchBuffer) {
		setvbuf(run->scratchFile, run->scratchBuffer, _IOFBF, SCRATCH_BUFFER_SIZE);
	}
}

static bool open_scratch_for_reading(DevelopRun *run)
{
	run->scratchFile = fopen(run->scratchPath, "rb");
	if (!run->scratchFile) {
		set_message(run->state, "Cannot read %s", run->scratchPath);
		return false;
	}
	buffer_scratch_file(run);
	return true;
}

// Pass 0: the noise estimate from the take itself.
static bool estimate_noise(DevelopRun *run)
{
	filters_init(&run->preFilters, run->sampleRate, run->params);
	if (!auto_noise_begin(&run->autoNoise)) {
		set_message(run->state, "Out of memory", NULL);
		return false;
	}
	long framesDone = 0;
	int count;
	while ((count = wav_read_f32(&run->reader, run->block, BLOCK_SIZE)) > 0) {
		for (int i = 0; i < count; i++) {
			run->block[i] = filters_run(&run->preFilters, run->block[i]);
		}
		auto_noise_feed(&run->autoNoise, run->block, count);
		framesDone += count;
		report_progress(run, WEIGHT_NOISE_ESTIMATE, framesDone);
		if (cancel_requested(run)) {
			return false;
		}
	}
	run->progressStart += WEIGHT_NOISE_ESTIMATE;
	wav_seek(&run->reader, 0);
	return true;
}

// One bin's noise power from the chosen source; the other source stands in when the chosen one has none.
static float pick_noise_power(int source, float profilePower, float autoPower)
{
	if (source == NOISE_SOURCE_PROFILE) {
		return profilePower >= 0 ? profilePower : autoPower;
	}
	if (source == NOISE_SOURCE_AUTO) {
		return autoPower >= 0 ? autoPower : profilePower;
	}
	return profilePower > autoPower ? profilePower : autoPower;
}

// The noise each bin is reduced against. False when there is nothing to go on (a very short take).
static bool find_noise_power(DevelopRun *run, const NoiseProfile *profile, bool wantAuto, float *noisePower)
{
	float autoNoise[FFT_BINS];
	bool haveAuto = wantAuto && auto_noise_result(&run->autoNoise, autoNoise);
	// The learned profile comes from the unfiltered mic, so it goes through the pre-filters' response.
	filters_init(&run->preFilters, run->sampleRate, run->params);
	for (int k = 0; k < FFT_BINS; k++) {
		float profilePower = NO_NOISE_ESTIMATE;
		if (profile) {
			double binHz = run->sampleRate * k / FFT_SIZE;
			float filterGain = (float)filters_power_gain(&run->preFilters, run->sampleRate, binHz);
			profilePower = profile->binPower[k] * filterGain;
		}
		float autoPower = haveAuto ? autoNoise[k] : NO_NOISE_ESTIMATE;
		float power = pick_noise_power(run->params->noiseSource, profilePower, autoPower);
		noisePower[k] = power > 0 ? power : 0;
	}
	return profile || haveAuto;
}

static void init_reducer(DevelopRun *run, const NoiseProfile *profile, bool wantAuto, const uint32_t *events,
                         int eventCount)
{
	const DevelopParams *params = run->params;
	float noisePower[FFT_BINS];
	bool reduceNoise = params->noiseReductionDb > 0 && find_noise_power(run, profile, wantAuto, noisePower);
	NoiseReducerOptions options = {
	        .sampleRate = run->sampleRate,
	        .reduceNoise = reduceNoise,
	        .noisePower = reduceNoise ? noisePower : NULL,
	        .reductionDb = params->noiseReductionDb,
	        .voiceEq = params->voiceEq,
	        .events = params->removeClicks ? events : NULL,
	        .eventCount = params->removeClicks ? eventCount : 0,
	        .pauseCleanupDb = params->pauseCleanupDb,
	};
	noise_reducer_init(run->reducer, &options);
}

// Reads the next block through the pre-filters. After the end it gives zeros, which push the last samples out
// of the noise reducer.
static int read_filtered_block(DevelopRun *run, bool *atEnd)
{
	int count = *atEnd ? 0 : wav_read_f32(&run->reader, run->block, BLOCK_SIZE);
	if (count <= 0) {
		*atEnd = true;
		memset(run->block, 0, BLOCK_SIZE * sizeof(float));
		return BLOCK_SIZE;
	}
	for (int i = 0; i < count; i++) {
		run->block[i] = filters_run(&run->preFilters, run->block[i]);
	}
	return count;
}

// Measures the loudness of the output samples and appends them to the scratch file.
static bool write_scratch(DevelopRun *run, int count)
{
	levels_loudness_feed(&run->loudness, run->output, count);
	if (fwrite(run->output, sizeof(float), count, run->scratchFile) == (size_t)count) {
		return true;
	}
	set_message(run->state, "Card full? %s", run->scratchPath);
	return false;
}

// Keeps one hop of the noise reducer's output once the latency is over. Full blocks go to the scratch file.
static bool keep_hop(DevelopRun *run, ReducedOutput *reduced, const float *hop)
{
	for (int j = 0; j < FFT_HOP && reduced->emitted < run->totalFrames; j++) {
		if (reduced->latencyLeft > 0) {
			reduced->latencyLeft--;
			continue;
		}
		run->output[reduced->outputCount] = hop[j];
		reduced->outputCount++;
		reduced->emitted++;
		if (reduced->outputCount < BLOCK_SIZE) {
			continue;
		}
		if (!write_scratch(run, reduced->outputCount)) {
			return false;
		}
		reduced->outputCount = 0;
	}
	return true;
}

// Pass 1: pre-filters, noise reduction and clarity EQ into the scratch file (floats), and the loudness before
// levelling.
static bool reduce_noise(DevelopRun *run)
{
	filters_init(&run->preFilters, run->sampleRate, run->params);
	levels_loudness_init(&run->loudness, run->sampleRate);
	run->scratchFile = fopen(run->scratchPath, "wb");
	if (!run->scratchFile) {
		set_message(run->state, "Cannot write %s", run->scratchPath);
		return false;
	}
	run->scratchBuffer = malloc(SCRATCH_BUFFER_SIZE);
	buffer_scratch_file(run);
	float hopInput[FFT_HOP];
	float hopOutput[FFT_HOP];
	int hopFill = 0;
	ReducedOutput reduced = {.emitted = 0, .latencyLeft = FFT_OVERLAP, .outputCount = 0};
	bool atEnd = false;
	while (reduced.emitted < run->totalFrames) {
		int count = read_filtered_block(run, &atEnd);
		reduced.outputCount = 0;
		for (int i = 0; i < count && reduced.emitted < run->totalFrames; i++) {
			hopInput[hopFill] = run->block[i];
			hopFill++;
			if (hopFill < FFT_HOP) {
				continue;
			}
			hopFill = 0;
			noise_reducer_hop(run->reducer, hopInput, hopOutput);
			if (!keep_hop(run, &reduced, hopOutput)) {
				return false;
			}
		}
		if (reduced.outputCount && !write_scratch(run, reduced.outputCount)) {
			return false;
		}
		report_progress(run, WEIGHT_NOISE_REDUCTION, reduced.emitted);
		if (cancel_requested(run)) {
			return false;
		}
	}
	run->progressStart += WEIGHT_NOISE_REDUCTION;
	fclose(run->scratchFile);
	run->scratchFile = NULL;
	return true;
}

static LevelPlan plan_levels(const DevelopParams *params, float inputLufs)
{
	LevelPlan plan;
	plan.targetLufs = params->targetLufs ? (float)params->targetLufs : inputLufs;
	bool measured = inputLufs > LOUDNESS_SILENCE_LUFS;
	plan.preGainDb = measured ? plan.targetLufs - inputLufs : 0;
	if (plan.preGainDb > MAX_PRE_GAIN_DB) {
		plan.preGainDb = MAX_PRE_GAIN_DB;
	}
	plan.makeUpGainDb = 0;
	plan.useLeveler = params->leveler && measured;
	return plan;
}

// Pass 2: runs the leveler over the scratch file to find its make-up gain (what it took off).
static bool measure_leveler(DevelopRun *run, LevelPlan *plan)
{
	if (!open_scratch_for_reading(run)) {
		return false;
	}
	Compressor leveler;
	levels_compressor_init(&leveler, run->sampleRate, plan->targetLufs);
	levels_loudness_init(&run->loudness, run->sampleRate);
	float preGain = db_to_amplitude(plan->preGainDb);
	long framesDone = 0;
	size_t count;
	while ((count = fread(run->block, sizeof(float), BLOCK_SIZE, run->scratchFile)) > 0) {
		for (size_t i = 0; i < count; i++) {
			run->block[i] = levels_compressor_run(&leveler, run->block[i] * preGain);
		}
		levels_loudness_feed(&run->loudness, run->block, (int)count);
		framesDone += (long)count;
		report_progress(run, WEIGHT_LEVELER, framesDone);
		if (cancel_requested(run)) {
			return false;
		}
	}
	fclose(run->scratchFile);
	run->scratchFile = NULL;
	float leveledLufs = levels_loudness_result(&run->loudness);
	if (leveledLufs > LOUDNESS_SILENCE_LUFS) {
		plan->makeUpGainDb = plan->targetLufs - leveledLufs;
	}
	if (plan->makeUpGainDb > MAX_MAKE_UP_GAIN_DB) {
		plan->makeUpGainDb = MAX_MAKE_UP_GAIN_DB;
	}
	return true;
}

static float output_chain_run(OutputChain *chain, float sample)
{
	float leveled = sample * chain->preGain;
	if (chain->useLeveler) {
		leveled = levels_compressor_run(&chain->leveler, leveled);
	}
	leveled *= chain->makeUpGain;
	return levels_limiter_run(chain->limiter, leveled);
}

// Runs one block through the chain into run->output. Returns how many samples came out.
static int run_output_block(DevelopRun *run, OutputChain *chain, size_t count)
{
	int outputCount = 0;
	for (size_t i = 0; i < count && chain->written < run->totalFrames; i++) {
		float sample = output_chain_run(chain, run->block[i]);
		if (chain->latencyLeft > 0) {
			chain->latencyLeft--;
			continue;
		}
		run->output[outputCount] = sample;
		outputCount++;
		chain->written++;
		float magnitude = fabsf(sample);
		if (magnitude > chain->peak) {
			chain->peak = magnitude;
		}
	}
	return outputCount;
}

// Feeds the scratch file through the chain into the writer. False on a write error or a cancel.
static bool run_output_chain(DevelopRun *run, OutputChain *chain, WavWriter *writer)
{
	bool failed = false;
	while (chain->written < run->totalFrames && !failed) {
		size_t count = fread(run->block, sizeof(float), BLOCK_SIZE, run->scratchFile);
		if (count == 0) { // the tail: zeros push the limiter's last samples out
			count = BLOCK_SIZE;
			memset(run->block, 0, BLOCK_SIZE * sizeof(float));
		}
		int outputCount = run_output_block(run, chain, count);
		levels_loudness_feed(&run->loudness, run->output, outputCount);
		if (!wav_write_f32(writer, run->output, outputCount)) {
			failed = true;
		}
		report_progress(run, WEIGHT_OUTPUT, chain->written);
		if (cancel_requested(run)) {
			failed = true;
		}
	}
	return !failed;
}

// Pass 3: gains, leveler and limiter from the scratch file into the developed file.
static bool write_output(DevelopRun *run, const LevelPlan *plan, const char *outPath)
{
	if (!open_scratch_for_reading(run)) {
		return false;
	}
	WavWriter writer;
	if (!wav_create(&writer, outPath, run->params->outputFormat, run->reader.rate)) {
		set_message(run->state, "Cannot write %s", outPath);
		return false;
	}
	OutputChain chain;
	levels_compressor_init(&chain.leveler, run->sampleRate, plan->targetLufs);
	chain.limiter = malloc(sizeof(Limiter));
	if (!chain.limiter) {
		wav_close_writer(&writer, NULL, 0);
		set_message(run->state, "Out of memory", NULL);
		return false;
	}
	levels_limiter_init(chain.limiter, run->sampleRate, LIMITER_CEILING_DB);
	levels_loudness_init(&run->loudness, run->sampleRate);
	chain.preGain = db_to_amplitude(plan->preGainDb);
	chain.makeUpGain = db_to_amplitude(plan->makeUpGainDb);
	chain.useLeveler = plan->useLeveler;
	chain.latencyLeft = levels_limiter_latency(chain.limiter);
	chain.written = 0;
	chain.peak = 0;
	bool complete = run_output_chain(run, &chain, &writer);
	free(chain.limiter);
	bool closed = wav_close_writer(&writer, run->reader.cues, run->reader.cueCount);
	if (!complete || !closed) {
		if (!run->state->message[0]) {
			set_message(run->state, "Card full? %s", outPath);
		}
		remove(outPath);
		return false;
	}
	run->state->outLufs = levels_loudness_result(&run->loudness);
	run->state->peakDb = amplitude_to_db(chain.peak + PEAK_FLOOR);
	return true;
}

static bool allocate_buffers(DevelopRun *run)
{
	run->block = malloc(BLOCK_SIZE * sizeof(float));
	run->output = malloc(BLOCK_SIZE * sizeof(float));
	run->reducer = calloc(1, sizeof(NoiseReducer));
	if (run->block && run->output && run->reducer) {
		return true;
	}
	set_message(run->state, "Out of memory", NULL);
	return false;
}

static bool run_passes(DevelopRun *run, const char *outPath, const NoiseProfile *profile,
                       const uint32_t *events, int eventCount)
{
	const DevelopParams *params = run->params;
	bool wantReduction = params->noiseReductionDb > 0;
	bool wantAuto = wantReduction && (params->noiseSource != NOISE_SOURCE_PROFILE || !profile);
	float estimateWeight = wantAuto ? WEIGHT_NOISE_ESTIMATE : 0;
	run->progressTotal = estimateWeight + WEIGHT_NOISE_REDUCTION + WEIGHT_LEVELER + WEIGHT_OUTPUT;
	run->progressStart = 0;

	if (wantAuto && !estimate_noise(run)) {
		return false;
	}
	init_reducer(run, profile, wantAuto, events, eventCount);
	if (!reduce_noise(run)) {
		return false;
	}

	float inputLufs = levels_loudness_result(&run->loudness);
	run->state->inLufs = inputLufs;
	LevelPlan plan = plan_levels(params, inputLufs);
	if (plan.useLeveler && !measure_leveler(run, &plan)) {
		return false;
	}
	run->progressStart += WEIGHT_LEVELER;
	if (!write_output(run, &plan, outPath)) {
		return false;
	}

	DevelopState *state = run->state;
	state->progress = 100;
	snprintf(state->message, sizeof state->message, "%.1f LUFS -> %.1f LUFS, peak %.1f dBFS", state->inLufs,
	         state->outLufs, state->peakDb);
	return true;
}

static void free_run(DevelopRun *run)
{
	if (run->scratchFile) {
		fclose(run->scratchFile);
	}
	free(run->scratchBuffer);
	remove(run->scratchPath);
	auto_noise_free(&run->autoNoise);
	levels_loudness_free(&run->loudness);
	free(run->reducer);
	free(run->block);
	free(run->output);
	wav_close_reader(&run->reader);
}

bool develop_run(const char *inPath, const char *outPath, const char *scratchPath,
                 const DevelopParams *params, const NoiseProfile *profile, const uint32_t *events,
                 int eventCount, DevelopState *state)
{
	state->progress = 0;
	state->message[0] = 0;
	state->peakDb = 0;
	state->outLufs = 0;
	state->inLufs = 0;
	DevelopRun run = {0};
	run.params = params;
	run.state = state;
	run.scratchPath = scratchPath;
	if (!wav_open(&run.reader, inPath)) {
		set_message(state, "Cannot read %s", inPath);
		return false;
	}
	run.sampleRate = run.reader.rate;
	run.totalFrames = (long)run.reader.frames;
	if (run.totalFrames < DEVELOP_MIN_FRAMES) {
		wav_close_reader(&run.reader);
		set_message(state, "Too short to develop", NULL);
		return false;
	}
	if (run.reader.frames > MAX_FRAMES) {
		wav_close_reader(&run.reader);
		set_message(state, "Too long to develop (over 9 hours)", NULL);
		return false;
	}
	bool ok = allocate_buffers(&run) && run_passes(&run, outPath, profile, events, eventCount);
	free_run(&run);
	return ok;
}
