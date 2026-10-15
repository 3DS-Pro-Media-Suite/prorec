#include "toast.h"
#include <stdarg.h>
#include <stdio.h>
#include "draw.h"

#define TOAST_TEXT_SIZE 112
#define TOAST_SHOW_MS 2800
#define TOAST_PADDING_X 12
#define TOAST_HEIGHT 22
#define TOAST_RADIUS (TOAST_HEIGHT / 2)
#define TOAST_TEXT_Y 4
#define TOAST_DEPTH 0.85f
#define COLOR_TOAST COLOR_RGBA(20, 20, 22, 255)

static char g_toastText[TOAST_TEXT_SIZE];
static u64 g_toastUntil;
static bool g_toastIsWarning;

void toast_show(bool isWarning, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	vsnprintf(g_toastText, sizeof g_toastText, format, args);
	va_end(args);
	g_toastUntil = osGetTime() + TOAST_SHOW_MS;
	g_toastIsWarning = isWarning;
}

bool toast_is_showing(void)
{
	return osGetTime() < g_toastUntil;
}

static u32 toast_color(void)
{
	return g_toastIsWarning ? COLOR_WARNING : COLOR_INK;
}

void toast_draw(float y)
{
	if (!toast_is_showing()) {
		return;
	}
	float textWidth = draw_text_width(g_fontSmall, g_toastText);
	float boxX = TOP_CENTER_X - textWidth / 2 - TOAST_PADDING_X;
	float boxWidth = textWidth + 2 * TOAST_PADDING_X;
	draw_rounded_rect(boxX, y, boxWidth, TOAST_HEIGHT, TOAST_RADIUS, COLOR_TOAST, TOAST_DEPTH);
	draw_text_fit(g_fontSmall, TOP_CENTER_X, y + TOAST_TEXT_Y, 1, toast_color(), ALIGN_CENTER, 0,
	              g_toastText);
}

void toast_draw_line(float x, float y)
{
	draw_text(g_fontSmall, x, y, toast_color(), ALIGN_CENTER, "%s", g_toastText);
}
