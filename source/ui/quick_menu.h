// The quick menu: a grid of setting tiles, the value bar that shows and steps the selected one, and the
// record button.
#pragma once
#include <3ds.h>

void quick_menu_draw(void);
// Circle pad: up / down steps the gain, left / right turns the selected tile. D-pad: selects a tile.
void quick_menu_input(u32 down, u32 held, touchPosition touch);
