// The screens' backlight while recording: off after a while without a key (the screens-off setting) or while
// the lid is closed, on again at a key press or when the lid opens. The take goes on either way.
#pragma once
#include <3ds.h>
#include <stdbool.h>

#define SCREEN_POWER_IDLE_SECONDS 10 // the screens-off setting: this long without a key while recording

void screen_power_poll(u64 now); // once per frame: the lid and the idle time
void screen_power_note_activity(u64 now); // a key press or a new take: the idle time starts again
// Screens off: the first press only wakes them. True while that press must do nothing else.
bool screen_power_input(u32 down, u32 held, u64 now);
bool screen_power_is_off(void);
void screen_power_on(void);

// The app goes to the background and comes back: the system's menus need the backlight.
void screen_power_suspend(void);
void screen_power_restore(void);
