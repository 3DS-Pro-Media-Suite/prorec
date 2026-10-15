// The setup menu: the pages of settings and actions that START opens, with a section tab for each page.
#pragma once
#include <3ds.h>
#include <stdbool.h>

bool setup_menu_is_open(void);
void setup_menu_open(void);
void setup_menu_close(void);
void setup_menu_draw(void);
void setup_menu_input(u32 down, u32 held, touchPosition touch, bool *quit);
