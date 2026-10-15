// Input helpers the screens share: key auto-repeat and touch hit tests.
#pragma once
#include <3ds.h>
#include <stdbool.h>

// Frames between repeats of a held key.
#define INPUT_REPEAT_FAST 4
#define INPUT_REPEAT_NORMAL 5
#define INPUT_REPEAT_SLOW 6
#define INPUT_REPEAT_SLOWEST 8

#define INPUT_STICK_THRESHOLD 60 // circle pad travel that counts as a push
#define INPUT_CONFIRM_MS 3000 // a second press within this confirms a delete
#define INPUT_STICK_KEYS                                                                              \
	(KEY_CPAD_UP | KEY_CPAD_DOWN | KEY_CPAD_LEFT | KEY_CPAD_RIGHT | KEY_CSTICK_UP | KEY_CSTICK_DOWN | \
	 KEY_CSTICK_LEFT | KEY_CSTICK_RIGHT)

// Key auto-repeat: true on the first frame, then every framesBetween frames after a short delay.
// heldFrames is the caller's count for this key.
bool input_repeat(bool active, int *heldFrames, int framesBetween);
// The same for one D-pad key. The count lives here, so a held key keeps its rhythm when one screen hands
// over to another.
bool input_dpad_repeat(u32 held, u32 key, int framesBetween);
// Press again to confirm: true for a press before armedUntil, else this press arms it for INPUT_CONFIRM_MS.
bool input_confirm(u64 *armedUntil, u64 now);
bool input_in_rect(touchPosition touch, float x, float y, float width, float height);
