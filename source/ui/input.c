#include "input.h"

#define REPEAT_DELAY_FRAMES 18 // a held key starts repeating after this

static int g_upFrames;
static int g_downFrames;
static int g_leftFrames;
static int g_rightFrames;

bool input_repeat(bool active, int *heldFrames, int framesBetween)
{
	if (!active) {
		*heldFrames = 0;
		return false;
	}
	(*heldFrames)++;
	bool isFirstFrame = *heldFrames == 1;
	bool isRepeatFrame = *heldFrames > REPEAT_DELAY_FRAMES && *heldFrames % framesBetween == 0;
	return isFirstFrame || isRepeatFrame;
}

bool input_confirm(u64 *armedUntil, u64 now)
{
	if (now < *armedUntil) {
		*armedUntil = 0;
		return true;
	}
	*armedUntil = now + INPUT_CONFIRM_MS;
	return false;
}

static int *dpad_held_frames(u32 key)
{
	switch (key) {
	case KEY_DUP:
		return &g_upFrames;
	case KEY_DDOWN:
		return &g_downFrames;
	case KEY_DLEFT:
		return &g_leftFrames;
	default:
		return &g_rightFrames;
	}
}

bool input_dpad_repeat(u32 held, u32 key, int framesBetween)
{
	return input_repeat(held & key, dpad_held_frames(key), framesBetween);
}

bool input_in_rect(touchPosition touch, float x, float y, float width, float height)
{
	return touch.px >= x && touch.px < x + width && touch.py >= y && touch.py < y + height;
}
