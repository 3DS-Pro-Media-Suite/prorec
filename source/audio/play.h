// Playback through the DSP (ndsp, needs sdmc:/3ds/dspfirm.cdc) and live monitoring of the mic on headphones.
// A player thread streams the WAV from the card. The DSP runs at the mic's own rate, so takes play bit-exact.
#pragma once
#include <3ds.h>
#include <stdbool.h>

#define PLAY_NO_DSP_MESSAGE "No sound: sdmc:/3ds/dspfirm.cdc is missing (dump it with DSP1)"

bool play_init(void); // false without the DSP firmware: no sound, everything else works
void play_exit(void);
bool play_available(void); // play_init found the DSP firmware

bool play_open(const char *path); // loads a take, paused at the start (stops whatever played)
void play_close(void);
bool play_loaded(void);
void play_set_playing(bool playing);
bool play_is_playing(void);
void play_seek(u64 frame);
u64 play_position(void); // the sample being heard
u64 play_length(void);
u32 play_sample_rate(void);
int play_cues(u32 *out, int max);
float play_level(void); // peak of what is playing now (0..1), for the meter

void play_set_monitor(bool on); // live mic to the headphones (silent when no headphones are plugged in)
