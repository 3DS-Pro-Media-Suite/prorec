// Drawing for both screens: fonts and text, the colours, anti-aliased shapes and the parts the screens share
// (panel headers, hairlines, list highlights, the top header strip).
// The look: black, off-white type, rounded tiles, hairlines and one red accent. Warnings are amber so they
// never read as the accent.
#pragma once
#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>

enum {
	ALIGN_LEFT,
	ALIGN_CENTER,
	ALIGN_RIGHT
};

// Directions for draw_arrow.
enum {
	ARROW_LEFT = -1,
	ARROW_UP = 0,
	ARROW_RIGHT = 1
};

#define COLOR_RGBA(r, g, b, a) C2D_Color32(r, g, b, a)
#define COLOR_BLACK COLOR_RGBA(0, 0, 0, 255)
#define COLOR_INK COLOR_RGBA(238, 235, 228, 255)
#define COLOR_SOFT COLOR_RGBA(210, 207, 200, 255)
#define COLOR_MUTED COLOR_RGBA(136, 133, 128, 255)
#define COLOR_FAINT COLOR_RGBA(74, 73, 72, 255)
#define COLOR_LINE COLOR_RGBA(40, 40, 42, 255)
#define COLOR_TILE COLOR_RGBA(22, 22, 25, 255)
#define COLOR_TILE_SELECTED COLOR_RGBA(54, 20, 20, 255)
#define COLOR_ACCENT COLOR_RGBA(246, 64, 58, 255)
#define COLOR_ACCENT_DIM COLOR_RGBA(246, 64, 58, 110)
#define COLOR_WARNING COLOR_RGBA(255, 196, 72, 255)
#define COLOR_SHADE COLOR_RGBA(0, 0, 0, 200)
#define COLOR_CLEAR COLOR_RGBA(0, 0, 0, 0)
#define COLOR_WAVE_IDLE COLOR_RGBA(110, 108, 104, 255) // waveform not recorded, or not played yet

#define TOP_SCREEN_WIDTH 400
#define BOTTOM_SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define TOP_CENTER_X (TOP_SCREEN_WIDTH / 2)
#define BOTTOM_CENTER_X (BOTTOM_SCREEN_WIDTH / 2)
#define TOP_MARGIN 12 // left and right of the top screen's content
#define TOP_HEADER_HEIGHT 30 // the shaded strip behind the top screen's header

// The bottom-screen panels: a badge and a title, a hairline under them, then rows, tiles and lines of text.
#define PANEL_MARGIN 6
#define PANEL_WIDTH (BOTTOM_SCREEN_WIDTH - 2 * PANEL_MARGIN)
#define PANEL_RIGHT_X (BOTTOM_SCREEN_WIDTH - PANEL_MARGIN)
#define PANEL_TITLE_X 28
#define PANEL_TITLE_Y 5
#define PANEL_HEADER_LINE_Y 21
#define PANEL_CONTENT_Y 28
#define PANEL_TEXT_X 10
#define PANEL_TEXT_WIDTH (BOTTOM_SCREEN_WIDTH - 2 * PANEL_TEXT_X)
#define PANEL_HINT_Y 214 // the two lines of key hints at the bottom
#define PANEL_HINT_SECOND_Y 227
#define LIST_ROW_HEIGHT 25 // setup menu rows and the recordings list
#define LIST_TEXT_X 16
#define LIST_VALUE_RIGHT_X 306
#define TILE_RADIUS 6 // quick-menu and picker tiles
#define TILE_SELECT_RADIUS 7
#define TILE_SELECT_GROW 1.5f // the selection ring stands this far outside the tile

// Depth (z) of the bottom-screen panels: a higher layer covers a lower one.
#define DEPTH_PANEL 0.2f // hairlines, highlights, selection rings
#define DEPTH_PANEL_RAISED 0.21f // tiles and bars over the highlights
#define DEPTH_CONTROL 0.3f // arrows, dots, buttons, the scrub bar
#define DEPTH_CONTROL_RAISED 0.31f
#define DEPTH_CONTROL_TOP 0.32f

// Depth (z) on the top screen.
#define DEPTH_WAVE 0.4f // the base line of the waveform and the spectrum
#define DEPTH_HEADER 0.6f // the shaded header strip
#define DEPTH_HEADER_RAISED 0.7f // badges and icons on the header strip

#define DRAW_FULL_TURN 6.2831853f // radians

extern C2D_Font g_fontSmall; // small (~10 px)
extern C2D_Font g_fontBody; // body (~15 px)
extern C2D_Font g_fontLarge; // large (~26 px)

void draw_init(void); // loads the fonts and makes the text buffer
void draw_exit(void);
void draw_clear_text(void); // call before each frame: it drops the text of the last one
float draw_text_width(C2D_Font font, const char *text); // at the font's own size
// Text squeezed to maxWidth when it is wider (0: no limit). Returns the width drawn.
float draw_text_fit(C2D_Font font, float x, float y, float scale, u32 color, int align, float maxWidth,
                    const char *text);
// printf-style text at the font's own size. Returns the width drawn.
float draw_text(C2D_Font font, float x, float y, u32 color, int align, const char *format, ...);
u32 draw_scale_alpha(u32 color, float factor);
void draw_circle(float centerX, float centerY, float radius, u32 color, float depth);
void draw_rounded_rect(float x, float y, float width, float height, float radius, u32 color, float depth);
// An outline inside the box, with mitred corners. Thinner than 1 px draws 1 px with less alpha.
void draw_frame(float x, float y, float width, float height, float thickness, u32 color, float depth);
void draw_triangle(float x0, float y0, float x1, float y1, float x2, float y2, u32 color, float depth);
void draw_arrow(float centerX, float centerY, float size, int direction, u32 color, float depth);
// A ring segment. Angles are in radians, clockwise from 12 o'clock. Thinner than 1 px draws 1 px with less
// alpha.
void draw_arc(float centerX, float centerY, float radius, float fromAngle, float toAngle, float thickness,
              u32 color, float depth);

// A bottom-screen panel's header: the badge, the title and the hairline under them.
void draw_panel_header(const char *badge, const char *title);
void draw_hairline(float y); // across the bottom screen
void draw_list_row_highlight(float y); // the selected row of a list
void draw_top_header_strip(void); // the shaded strip behind the top screen's header
bool draw_blink_is_on(u64 now); // paused things blink
