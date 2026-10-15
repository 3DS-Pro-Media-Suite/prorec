// The toast: a short message shown for a few seconds (warnings in amber).
#pragma once
#include <stdbool.h>

void toast_show(bool isWarning, const char *format, ...);
#define toast(...) toast_show(false, __VA_ARGS__)
#define toast_warning(...) toast_show(true, __VA_ARGS__)
bool toast_is_showing(void);
void toast_draw(float y); // in a box, centred on the top screen
void toast_draw_line(float x, float y); // as one line of text, centred on x
