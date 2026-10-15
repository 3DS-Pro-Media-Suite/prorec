#include "draw.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include "f_big_bcfnt.h"
#include "f_body_bcfnt.h"
#include "f_small_bcfnt.h"

#define TEXT_BUFFER_GLYPHS 8192 // the text of one frame on both screens
#define TEXT_FORMAT_SIZE 128
#define TEXT_DEPTH 0.9f
#define FONT_CACHE_SIZE 3
#define FONT_REFERENCE_CELL_HEIGHT 30.0f // a font with this cell height draws at text scale 1
#define SYSTEM_FONT_SCALE 0.5f

#define ALPHA_SHIFT 24 // C2D colours are 0xAABBGGRR
#define COLOR_RGB_MASK 0x00FFFFFFu

#define HALF_TURN (DRAW_FULL_TURN / 2)
#define QUARTER_TURN 1.5707963f // not DRAW_FULL_TURN / 4: that is one float ulp higher

// The soft edge spans AA_FEATHER on each side of the true outline.
#define AA_FEATHER 0.5f
#define AA_MAX_POINTS 128
#define AA_SAME_POINT 1e-3f // closer points count as one
#define AA_MIN_BISECTOR 1e-4f // a shorter bisector means the two edges point opposite ways
#define AA_MIN_MITER_COSINE 0.35f // keeps the soft edge of a sharp corner from reaching far out
#define RING_COUNT 4 // soft outer edge, two rings around the solid band, soft inner edge

#define CURVE_EXTRA_SEGMENTS 8
#define CURVE_MAX_SEGMENTS 96
#define CIRCLE_SEGMENTS_PER_RADIUS 1.5f
#define CIRCLE_MIN_SEGMENTS 12
#define ARC_SEGMENTS_PER_LENGTH 0.5f // per pixel of arc length
#define ARC_FULL_TOLERANCE 1e-3f

#define TRIANGLE_CORNERS 3
#define RECT_CORNERS 4
#define RECT_MIN_RADIUS 0.25f // smaller radii draw square corners
#define RECT_FEATHER_SHARE 0.25f // the soft edge takes at most this share of the thinner side
#define CORNER_SEGMENTS_PER_RADIUS 0.9f
#define CORNER_EXTRA_SEGMENTS 2
#define CORNER_MAX_SEGMENTS 16

#define ARROW_BASE_SHARE 0.7f // the base lies this share of the size behind the centre

#define BADGE_Y 3
#define BADGE_SIZE 16
#define BADGE_RADIUS 3
#define BADGE_CENTER_X (PANEL_MARGIN + BADGE_SIZE / 2)

#define HAIRLINE_THICKNESS 1

// The highlight of a list's selected row: a tile and an accent bar at its left.
#define LIST_ROW_GAP 2
#define LIST_ROW_RADIUS 5
#define LIST_BAR_INSET 5
#define LIST_BAR_WIDTH 2
#define LIST_BAR_HEIGHT (LIST_ROW_HEIGHT - LIST_ROW_GAP - 2 * LIST_BAR_INSET)

#define BLINK_MS 500 // this long on, this long off

static C2D_TextBuf g_textBuffer;
C2D_Font g_fontSmall;
C2D_Font g_fontBody;
C2D_Font g_fontLarge;

void draw_init(void)
{
	g_textBuffer = C2D_TextBufNew(TEXT_BUFFER_GLYPHS);
	g_fontSmall = C2D_FontLoadFromMem(f_small_bcfnt, f_small_bcfnt_size);
	g_fontBody = C2D_FontLoadFromMem(f_body_bcfnt, f_body_bcfnt_size);
	g_fontLarge = C2D_FontLoadFromMem(f_big_bcfnt, f_big_bcfnt_size);
}

void draw_exit(void)
{
	C2D_FontFree(g_fontSmall);
	C2D_FontFree(g_fontBody);
	C2D_FontFree(g_fontLarge);
	C2D_TextBufDelete(g_textBuffer);
}

void draw_clear_text(void)
{
	C2D_TextBufClear(g_textBuffer);
}

// The factor that text scales of a font are multiplied by. A null font is the system font.
static float font_scale(C2D_Font font)
{
	static C2D_Font cachedFonts[FONT_CACHE_SIZE];
	static float cachedScales[FONT_CACHE_SIZE];
	for (int i = 0; i < FONT_CACHE_SIZE; i++) {
		if (cachedFonts[i] == font && cachedScales[i] > 0) {
			return cachedScales[i];
		}
	}
	if (!font) {
		return SYSTEM_FONT_SCALE;
	}
	float scale = C2D_FontGetInfo(font)->tglp->cellHeight / FONT_REFERENCE_CELL_HEIGHT;
	for (int i = 0; i < FONT_CACHE_SIZE; i++) {
		if (!cachedFonts[i]) {
			cachedFonts[i] = font;
			cachedScales[i] = scale;
			break;
		}
	}
	return scale;
}

float draw_text_fit(C2D_Font font, float x, float y, float scale, u32 color, int align, float maxWidth,
                    const char *text)
{
	scale *= font_scale(font);
	C2D_Text parsed;
	C2D_TextFontParse(&parsed, font, g_textBuffer, text);
	C2D_TextOptimize(&parsed);
	float width;
	float height;
	C2D_TextGetDimensions(&parsed, scale, scale, &width, &height);
	if (maxWidth > 0 && width > maxWidth) { // squeeze long values into their box instead of overflowing it
		scale *= maxWidth / width;
		width = maxWidth;
	}
	if (align == ALIGN_CENTER) {
		x -= width * 0.5f;
	} else if (align == ALIGN_RIGHT) {
		x -= width;
	}
	C2D_DrawText(&parsed, C2D_WithColor, x, y, TEXT_DEPTH, scale, scale, color);
	return width;
}

float draw_text_width(C2D_Font font, const char *text)
{
	C2D_Text parsed;
	C2D_TextFontParse(&parsed, font, g_textBuffer, text);
	float scale = font_scale(font);
	float width;
	float height;
	C2D_TextGetDimensions(&parsed, scale, scale, &width, &height);
	return width;
}

float draw_text(C2D_Font font, float x, float y, u32 color, int align, const char *format, ...)
{
	char text[TEXT_FORMAT_SIZE];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof text, format, args);
	va_end(args);
	return draw_text_fit(font, x, y, 1.0f, color, align, 0, text);
}

// Anti-aliased shapes. The GPU fills whole pixels with hard edges, so each shape is a solid core plus a
// soft ring whose alpha falls to 0 across the true edge. Nothing overlaps, so translucent colours work.

u32 draw_scale_alpha(u32 color, float factor)
{
	u32 alpha = (u32)((color >> ALPHA_SHIFT) * factor + 0.5f);
	if (alpha > 255) {
		alpha = 255;
	}
	return (color & COLOR_RGB_MASK) | alpha << ALPHA_SHIFT;
}

static bool is_same_point(float ax, float ay, float bx, float by)
{
	return fabsf(ax - bx) < AA_SAME_POINT && fabsf(ay - by) < AA_SAME_POINT;
}

// Copies the points without repeats, which have no edge direction. Returns how many are left.
static int copy_distinct_points(const float *pointsX, const float *pointsY, int pointCount, float *cornerX,
                                float *cornerY)
{
	int cornerCount = 0;
	for (int i = 0; i < pointCount && cornerCount < AA_MAX_POINTS; i++) {
		int last = cornerCount - 1;
		if (cornerCount && is_same_point(pointsX[i], pointsY[i], cornerX[last], cornerY[last])) {
			continue;
		}
		cornerX[cornerCount] = pointsX[i];
		cornerY[cornerCount] = pointsY[i];
		cornerCount++;
	}
	while (cornerCount > 2 &&
	       is_same_point(cornerX[cornerCount - 1], cornerY[cornerCount - 1], cornerX[0], cornerY[0])) {
		cornerCount--;
	}
	return cornerCount;
}

// The outward normal of the edge from corner i to the next one.
static void find_outward_normals(const float *cornerX, const float *cornerY, int cornerCount, float *normalX,
                                 float *normalY)
{
	float centerX = 0;
	float centerY = 0;
	for (int i = 0; i < cornerCount; i++) {
		centerX += cornerX[i];
		centerY += cornerY[i];
	}
	centerX /= cornerCount;
	centerY /= cornerCount;
	for (int i = 0; i < cornerCount; i++) {
		int next = (i + 1) % cornerCount;
		float edgeX = cornerX[next] - cornerX[i];
		float edgeY = cornerY[next] - cornerY[i];
		float edgeLength = sqrtf(edgeX * edgeX + edgeY * edgeY);
		normalX[i] = edgeY / edgeLength;
		normalY[i] = -edgeX / edgeLength;
		float middleX = (cornerX[i] + cornerX[next]) * 0.5f;
		float middleY = (cornerY[i] + cornerY[next]) * 0.5f;
		if (normalX[i] * (middleX - centerX) + normalY[i] * (middleY - centerY) < 0) {
			normalX[i] = -normalX[i];
			normalY[i] = -normalY[i];
		}
	}
}

// A convex polygon, points in either winding. feather is half the width of the soft edge.
static void aa_poly(const float *pointsX, const float *pointsY, int pointCount, u32 color, float depth,
                    float feather)
{
	float cornerX[AA_MAX_POINTS];
	float cornerY[AA_MAX_POINTS];
	int cornerCount = copy_distinct_points(pointsX, pointsY, pointCount, cornerX, cornerY);
	if (cornerCount < TRIANGLE_CORNERS) {
		return;
	}
	float normalX[AA_MAX_POINTS];
	float normalY[AA_MAX_POINTS];
	find_outward_normals(cornerX, cornerY, cornerCount, normalX, normalY);

	// Each corner moves along its bisector so that both of its edges shift by feather.
	float innerX[AA_MAX_POINTS];
	float innerY[AA_MAX_POINTS];
	float outerX[AA_MAX_POINTS];
	float outerY[AA_MAX_POINTS];
	for (int i = 0; i < cornerCount; i++) {
		int previous = (i + cornerCount - 1) % cornerCount;
		float bisectorX = normalX[previous] + normalX[i];
		float bisectorY = normalY[previous] + normalY[i];
		float bisectorLength = sqrtf(bisectorX * bisectorX + bisectorY * bisectorY);
		if (bisectorLength < AA_MIN_BISECTOR) {
			bisectorX = normalX[i];
			bisectorY = normalY[i];
		} else {
			bisectorX /= bisectorLength;
			bisectorY /= bisectorLength;
		}
		float cosine = bisectorX * normalX[i] + bisectorY * normalY[i];
		if (cosine < AA_MIN_MITER_COSINE) {
			cosine = AA_MIN_MITER_COSINE;
		}
		float shift = feather / cosine;
		innerX[i] = cornerX[i] - bisectorX * shift;
		innerY[i] = cornerY[i] - bisectorY * shift;
		outerX[i] = cornerX[i] + bisectorX * shift;
		outerY[i] = cornerY[i] + bisectorY * shift;
	}

	u32 transparent = draw_scale_alpha(color, 0);
	for (int i = 1; i + 1 < cornerCount; i++) {
		C2D_DrawTriangle(innerX[0], innerY[0], color, innerX[i], innerY[i], color, innerX[i + 1],
		                 innerY[i + 1], color, depth);
	}
	for (int i = 0; i < cornerCount; i++) {
		int next = (i + 1) % cornerCount;
		C2D_DrawTriangle(innerX[i], innerY[i], color, innerX[next], innerY[next], color, outerX[next],
		                 outerY[next], transparent, depth);
		C2D_DrawTriangle(innerX[i], innerY[i], color, outerX[next], outerY[next], transparent, outerX[i],
		                 outerY[i], transparent, depth);
	}
}

void draw_circle(float centerX, float centerY, float radius, u32 color, float depth)
{
	int segments = (int)(radius * CIRCLE_SEGMENTS_PER_RADIUS) + CURVE_EXTRA_SEGMENTS;
	if (segments < CIRCLE_MIN_SEGMENTS) {
		segments = CIRCLE_MIN_SEGMENTS;
	}
	if (segments > CURVE_MAX_SEGMENTS) {
		segments = CURVE_MAX_SEGMENTS;
	}
	float pointX[CURVE_MAX_SEGMENTS];
	float pointY[CURVE_MAX_SEGMENTS];
	for (int i = 0; i < segments; i++) {
		float angle = DRAW_FULL_TURN * i / segments;
		pointX[i] = centerX + radius * cosf(angle);
		pointY[i] = centerY + radius * sinf(angle);
	}
	float feather = radius < 1 ? radius * 0.5f : AA_FEATHER; // a dot under a pixel is all soft edge
	aa_poly(pointX, pointY, segments, color, depth, feather);
}

void draw_rounded_rect(float x, float y, float width, float height, float radius, u32 color, float depth)
{
	if (radius > width * 0.5f) {
		radius = width * 0.5f;
	}
	if (radius > height * 0.5f) {
		radius = height * 0.5f;
	}
	// Thin bars keep a solid core.
	float feather = width < height ? width * RECT_FEATHER_SHARE : height * RECT_FEATHER_SHARE;
	if (feather > AA_FEATHER) {
		feather = AA_FEATHER;
	}
	float pointX[AA_MAX_POINTS];
	float pointY[AA_MAX_POINTS];
	int pointCount = 0;
	if (radius < RECT_MIN_RADIUS) {
		pointX[0] = x;
		pointY[0] = y;
		pointX[1] = x + width;
		pointY[1] = y;
		pointX[2] = x + width;
		pointY[2] = y + height;
		pointX[3] = x;
		pointY[3] = y + height;
		pointCount = RECT_CORNERS;
	} else {
		int cornerSegments = (int)(radius * CORNER_SEGMENTS_PER_RADIUS) + CORNER_EXTRA_SEGMENTS;
		if (cornerSegments > CORNER_MAX_SEGMENTS) {
			cornerSegments = CORNER_MAX_SEGMENTS;
		}
		const float centerX[RECT_CORNERS] = {x + radius, x + width - radius, x + width - radius, x + radius};
		const float centerY[RECT_CORNERS] = {y + radius, y + radius, y + height - radius,
		                                     y + height - radius};
		for (int corner = 0; corner < RECT_CORNERS; corner++) { // clockwise from the top left
			for (int i = 0; i <= cornerSegments; i++) {
				float angle = HALF_TURN + QUARTER_TURN * (corner + (float)i / cornerSegments);
				pointX[pointCount] = centerX[corner] + radius * cosf(angle);
				pointY[pointCount] = centerY[corner] + radius * sinf(angle);
				pointCount++;
			}
		}
	}
	aa_poly(pointX, pointY, pointCount, color, depth, feather);
}

// Fills between two cross-sections of RING_COUNT points each: one quad per pair of neighbouring rings.
static void draw_ring_band(const float *fromX, const float *fromY, const float *toX, const float *toY,
                           const u32 *ringColor, float depth)
{
	for (int ring = 0; ring + 1 < RING_COUNT; ring++) {
		int nextRing = ring + 1;
		C2D_DrawTriangle(fromX[ring], fromY[ring], ringColor[ring], toX[ring], toY[ring], ringColor[ring],
		                 toX[nextRing], toY[nextRing], ringColor[nextRing], depth);
		C2D_DrawTriangle(fromX[ring], fromY[ring], ringColor[ring], toX[nextRing], toY[nextRing],
		                 ringColor[nextRing], fromX[nextRing], fromY[nextRing], ringColor[nextRing], depth);
	}
}

void draw_frame(float x, float y, float width, float height, float thickness, u32 color, float depth)
{
	if (thickness < 1) {
		color = draw_scale_alpha(color, thickness);
		thickness = 1;
	}
	u32 transparent = draw_scale_alpha(color, 0);
	// How far each ring lies outside the box edge.
	const float ringOffset[RING_COUNT] = {AA_FEATHER, -AA_FEATHER, AA_FEATHER - thickness,
	                                      -AA_FEATHER - thickness};
	const u32 ringColor[RING_COUNT] = {transparent, color, color, transparent};
	// The corners clockwise from the top left, each on every ring.
	float cornerX[RECT_CORNERS][RING_COUNT];
	float cornerY[RECT_CORNERS][RING_COUNT];
	for (int ring = 0; ring < RING_COUNT; ring++) {
		float left = x - ringOffset[ring];
		float top = y - ringOffset[ring];
		float right = x + width + ringOffset[ring];
		float bottom = y + height + ringOffset[ring];
		cornerX[0][ring] = left;
		cornerY[0][ring] = top;
		cornerX[1][ring] = right;
		cornerY[1][ring] = top;
		cornerX[2][ring] = right;
		cornerY[2][ring] = bottom;
		cornerX[3][ring] = left;
		cornerY[3][ring] = bottom;
	}
	for (int corner = 0; corner < RECT_CORNERS; corner++) {
		int next = (corner + 1) % RECT_CORNERS;
		draw_ring_band(cornerX[corner], cornerY[corner], cornerX[next], cornerY[next], ringColor, depth);
	}
}

void draw_triangle(float x0, float y0, float x1, float y1, float x2, float y2, u32 color, float depth)
{
	const float pointX[TRIANGLE_CORNERS] = {x0, x1, x2};
	const float pointY[TRIANGLE_CORNERS] = {y0, y1, y2};
	aa_poly(pointX, pointY, TRIANGLE_CORNERS, color, depth, AA_FEATHER);
}

void draw_arrow(float centerX, float centerY, float size, int direction, u32 color, float depth)
{
	if (direction == ARROW_UP) {
		float baseY = centerY + size * ARROW_BASE_SHARE;
		draw_triangle(centerX, centerY - size, centerX - size, baseY, centerX + size, baseY, color, depth);
	} else {
		float tipX = centerX + direction * size;
		float baseX = centerX - direction * size * ARROW_BASE_SHARE;
		draw_triangle(tipX, centerY, baseX, centerY - size, baseX, centerY + size, color, depth);
	}
}

void draw_arc(float centerX, float centerY, float radius, float fromAngle, float toAngle, float thickness,
              u32 color, float depth)
{
	float span = toAngle - fromAngle;
	bool isFullCircle = fabsf(span) >= DRAW_FULL_TURN - ARC_FULL_TOLERANCE;
	int segments = (int)(fabsf(span) * radius * ARC_SEGMENTS_PER_LENGTH) + CURVE_EXTRA_SEGMENTS;
	if (segments > CURVE_MAX_SEGMENTS) {
		segments = CURVE_MAX_SEGMENTS;
	}
	if (thickness < 1) {
		color = draw_scale_alpha(color, thickness);
		thickness = 1;
	}
	u32 transparent = draw_scale_alpha(color, 0);
	float innerEdge = radius - thickness * 0.5f;
	float outerEdge = radius + thickness * 0.5f;
	const float ringRadius[RING_COUNT] = {innerEdge - AA_FEATHER, innerEdge + AA_FEATHER,
	                                      outerEdge - AA_FEATHER, outerEdge + AA_FEATHER};
	const u32 ringColor[RING_COUNT] = {transparent, color, color, transparent};

	// The cross-section at the last step and at this one.
	float pointX[2][RING_COUNT];
	float pointY[2][RING_COUNT];
	for (int step = 0; step <= segments; step++) {
		float angle = fromAngle + span * step / segments;
		float sine = sinf(angle);
		float cosine = cosf(angle);
		int current = step % 2;
		for (int ring = 0; ring < RING_COUNT; ring++) {
			pointX[current][ring] = centerX + ringRadius[ring] * sine;
			pointY[current][ring] = centerY - ringRadius[ring] * cosine;
		}
		if (step == 0) {
			continue;
		}
		int last = (step - 1) % 2;
		draw_ring_band(pointX[last], pointY[last], pointX[current], pointY[current], ringColor, depth);
	}
	if (isFullCircle) {
		return;
	}

	// Open arcs fade out over half a pixel past each end.
	float spanSign = span < 0 ? -1.0f : 1.0f;
	for (int cap = 0; cap < 2; cap++) { // the cap at fromAngle, then the cap at toAngle
		float angle = cap ? toAngle : fromAngle;
		float capSign = cap ? 1.0f : -1.0f;
		float outward = capSign * spanSign;
		float sine = sinf(angle);
		float cosine = cosf(angle);
		float tangentX = cosine * AA_FEATHER * outward; // the clockwise tangent
		float tangentY = sine * AA_FEATHER * outward;
		for (int ring = 0; ring + 1 < RING_COUNT; ring++) {
			float x0 = centerX + ringRadius[ring] * sine;
			float y0 = centerY - ringRadius[ring] * cosine;
			float x1 = centerX + ringRadius[ring + 1] * sine;
			float y1 = centerY - ringRadius[ring + 1] * cosine;
			C2D_DrawTriangle(x0, y0, ringColor[ring], x1, y1, ringColor[ring + 1], x1 + tangentX,
			                 y1 + tangentY, transparent, depth);
			C2D_DrawTriangle(x0, y0, ringColor[ring], x1 + tangentX, y1 + tangentY, transparent,
			                 x0 + tangentX, y0 + tangentY, transparent, depth);
		}
	}
}

static void draw_badge(const char *label)
{
	draw_rounded_rect(PANEL_MARGIN, BADGE_Y, BADGE_SIZE, BADGE_SIZE, BADGE_RADIUS, COLOR_ACCENT, DEPTH_PANEL);
	draw_text(g_fontSmall, BADGE_CENTER_X, PANEL_TITLE_Y, COLOR_BLACK, ALIGN_CENTER, "%s", label);
}

void draw_hairline(float y)
{
	C2D_DrawRectSolid(0, y, DEPTH_PANEL, BOTTOM_SCREEN_WIDTH, HAIRLINE_THICKNESS, COLOR_LINE);
}

void draw_panel_header(const char *badge, const char *title)
{
	draw_badge(badge);
	draw_text(g_fontSmall, PANEL_TITLE_X, PANEL_TITLE_Y, COLOR_MUTED, ALIGN_LEFT, "%s", title);
	draw_hairline(PANEL_HEADER_LINE_Y);
}

void draw_list_row_highlight(float y)
{
	draw_rounded_rect(PANEL_MARGIN, y, PANEL_WIDTH, LIST_ROW_HEIGHT - LIST_ROW_GAP, LIST_ROW_RADIUS,
	                  COLOR_TILE_SELECTED, DEPTH_PANEL);
	C2D_DrawRectSolid(PANEL_MARGIN, y + LIST_BAR_INSET, DEPTH_PANEL_RAISED, LIST_BAR_WIDTH, LIST_BAR_HEIGHT,
	                  COLOR_ACCENT);
}

void draw_top_header_strip(void)
{
	C2D_DrawRectangle(0, 0, DEPTH_HEADER, TOP_SCREEN_WIDTH, TOP_HEADER_HEIGHT, COLOR_SHADE, COLOR_SHADE,
	                  COLOR_CLEAR, COLOR_CLEAR);
}

bool draw_blink_is_on(u64 now)
{
	return (now / BLINK_MS) % 2;
}
