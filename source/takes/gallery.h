// The recordings browser: the take list, its buttons, delete and develop again. gallery_player plays the
// selected take and gallery_view draws it on the top screen.
#pragma once
#include <3ds.h>
#include <stdbool.h>

bool gallery_is_open(void);
// Opens the recordings at the newest take. While a take records it warns and returns false.
bool gallery_open_newest_if_idle(void);
void gallery_take_developed(int id);
void gallery_draw_take(void);
void gallery_draw_panel(void);
void gallery_input(u32 down, u32 held, touchPosition touch);
