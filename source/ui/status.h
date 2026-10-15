// Status: the battery, free space on the card and the recording time it has left.
#pragma once
#include <3ds.h>
#include <stdbool.h>

// Reads the battery every few seconds and the free space about once a minute. force: now.
void status_refresh(bool force);
s64 status_card_free_bytes(void); // -1 while not known yet
double status_card_seconds_left(void); // -1 while not known yet
void status_draw_battery(float x, float y, float depth);
