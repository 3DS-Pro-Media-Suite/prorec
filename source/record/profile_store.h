// The learned noise profiles: one file per mic gain on the card, and their copies in memory.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "noise_profile.h"

void profile_store_path(int gain, char *path, size_t pathSize);
void profile_store_load_all(void);
void profile_store_set(int gain, const NoiseProfile *profile); // a profile just learned and saved
bool profile_store_has(int gain);
int profile_store_version(void); // changes whenever a profile is learned, loaded or forgotten
int profile_store_count(void);
int profile_store_nearest(int gain); // the learned gain nearest to gain, -1 when none is learned

// The exact profile for gain, else the nearest learned one shifted by the gain difference. It is built in
// storage. NULL when no profile is learned.
const NoiseProfile *profile_store_for_gain(int gain, NoiseProfile *storage);
void profile_store_forget_all(void);
