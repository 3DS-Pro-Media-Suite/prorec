// Routes each frame: which screen draws each display, and which one gets the keys and touch.
#pragma once
#include <stdbool.h>

void ui_draw_top(void);
void ui_draw_bottom(void);
void ui_handle_input(bool *quit);
