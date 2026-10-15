// What the quick menu and the setup menu share: the header with the develop progress and the RECORDINGS
// button, the record button, and the colours of a setting's text.
#pragma once
#include <3ds.h>
#include <stdbool.h>

void menu_frame_draw_header(const char *badge, const char *title);
// True when a press hit the RECORDINGS button in the header.
bool menu_frame_recordings_tapped(u32 down, touchPosition touch);
// A square while recording, a dot otherwise. The dot blinks while the take is paused.
void menu_frame_draw_record_button(void);
bool menu_frame_record_button_tapped(u32 down, touchPosition touch);
// Faint when the setting cannot be used, brighter when it is selected.
u32 menu_frame_setting_color(bool disabled, bool selected, u32 selectedColor, u32 normalColor);
