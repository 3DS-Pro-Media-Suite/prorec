// The information page: the mic, playback, the noise profiles, the takes, the card and the FTP server.
#pragma once
#include <3ds.h>
#include <stdbool.h>

bool info_page_is_open(void);
void info_page_open(void);
void info_page_draw(void);
void info_page_input(u32 down);
