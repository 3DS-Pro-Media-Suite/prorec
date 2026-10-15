#include "status.h"
#include "develop_queue.h"
#include "draw.h"
#include "mic.h"
#include "settings.h"
#include "take_writer.h"
#include "threads.h"
#include "wav.h"

#define STATUS_REFRESH_MS 2000
#define FREE_SPACE_REFRESH_MS 60000
#define FREE_SPACE_STACK_SIZE (8 * 1024)
#define FREE_SPACE_PRIORITY_OFFSET 3 // runs after the main thread

#define BATTERY_LEVEL_FULL 5 // the system reports levels 0 to 5
#define BATTERY_LEVEL_LOW 1
#define BATTERY_WIDTH 20
#define BATTERY_HEIGHT 10
#define BATTERY_BORDER 1
#define BATTERY_TIP_WIDTH 2
#define BATTERY_TIP_HEIGHT 4
#define BATTERY_TIP_Y ((BATTERY_HEIGHT - BATTERY_TIP_HEIGHT) / 2)
#define BATTERY_BAR_X 3 // from the left edge of the battery
#define BATTERY_BAR_Y 2
#define BATTERY_BAR_WIDTH 2
#define BATTERY_BAR_HEIGHT (BATTERY_HEIGHT - 2 * BATTERY_BAR_Y)
#define BATTERY_BAR_SPACING 3
#define BATTERY_CHARGING_COLOR COLOR_RGBA(98, 166, 255, 255) // blue, since red means recording

static u8 g_batteryLevel = BATTERY_LEVEL_FULL;
static u8 g_charging;
static u64 g_refreshedAt;
static _Atomic s64 g_cardFreeBytes = -1;
static Thread g_freeSpaceThread;
static _Atomic bool g_freeSpaceBusy;
static u64 g_freeSpaceCheckedAt;
static bool g_freeSpaceNeverChecked = true;

static void free_space_thread(void *arg)
{
	(void)arg;
	FS_ArchiveResource resource;
	if (R_SUCCEEDED(FSUSER_GetSdmcArchiveResource(&resource))) {
		g_cardFreeBytes = (s64)resource.clusterSize * resource.freeClusters;
	}
	g_freeSpaceBusy = false;
}

static void start_free_space_thread(void)
{
	s32 priority = threads_priority_from_main(FREE_SPACE_PRIORITY_OFFSET);
	g_freeSpaceBusy = true;
	g_freeSpaceThread = threadCreate(free_space_thread, NULL, FREE_SPACE_STACK_SIZE, priority,
	                                 THREAD_CORE_DEFAULT, false);
	if (!g_freeSpaceThread) {
		g_freeSpaceBusy = false;
	}
}

static void refresh_battery(void)
{
	PTMU_GetBatteryLevel(&g_batteryLevel);
	PTMU_GetBatteryChargeState(&g_charging);
}

// Free space comes from a low-priority thread and never while recording or developing: after writes, FAT32
// rebuilds its free count by scanning the whole FAT, which can take seconds.
static void refresh_free_space(u64 now, bool force)
{
	if (g_freeSpaceThread && !g_freeSpaceBusy) {
		threadJoin(g_freeSpaceThread, U64_MAX);
		threadFree(g_freeSpaceThread);
		g_freeSpaceThread = NULL;
	}
	bool checkDue = g_freeSpaceNeverChecked || force || now - g_freeSpaceCheckedAt > FREE_SPACE_REFRESH_MS;
	if (g_freeSpaceThread || take_writer_is_active() || develop_queue_active() || !checkDue) {
		return;
	}
	g_freeSpaceNeverChecked = false;
	g_freeSpaceCheckedAt = now;
	start_free_space_thread();
}

void status_refresh(bool force)
{
	u64 now = osGetTime();
	if (!force && now - g_refreshedAt < STATUS_REFRESH_MS) {
		return;
	}
	g_refreshedAt = now;
	refresh_battery();
	refresh_free_space(now, force);
}

// The card fills with the raw take, and with the clean file too when takes develop on their own.
static double bytes_per_second(void)
{
	float rawBytes = MIC_RATE * wav_bytes_per_sample(WAV_S16);
	float cleanBytes = 0;
	if (g_settings.autoDevelop) {
		int cleanFormat = settings_develop_params(&g_settings).outputFormat;
		cleanBytes = MIC_RATE * wav_bytes_per_sample(cleanFormat);
	}
	return rawBytes + cleanBytes;
}

double status_card_seconds_left(void)
{
	if (g_cardFreeBytes < 0) {
		return -1;
	}
	// The free space is not read during a take, so the take so far comes off it.
	double freeBytes = (double)g_cardFreeBytes;
	if (take_writer_is_active()) {
		freeBytes -= take_writer_frames() * (double)wav_bytes_per_sample(WAV_S16);
	}
	return freeBytes > 0 ? freeBytes / bytes_per_second() : 0;
}

static u32 battery_color(void)
{
	if (g_charging) {
		return BATTERY_CHARGING_COLOR;
	}
	if (g_batteryLevel <= BATTERY_LEVEL_LOW) {
		return COLOR_WARNING;
	}
	return COLOR_INK;
}

void status_draw_battery(float x, float y, float depth)
{
	u32 color = battery_color();
	C2D_DrawRectSolid(x, y, depth, BATTERY_WIDTH, BATTERY_HEIGHT, color);
	C2D_DrawRectSolid(x + BATTERY_BORDER, y + BATTERY_BORDER, depth, BATTERY_WIDTH - 2 * BATTERY_BORDER,
	                  BATTERY_HEIGHT - 2 * BATTERY_BORDER, COLOR_BLACK);
	C2D_DrawRectSolid(x + BATTERY_WIDTH, y + BATTERY_TIP_Y, depth, BATTERY_TIP_WIDTH, BATTERY_TIP_HEIGHT,
	                  color);
	int bars = g_batteryLevel > BATTERY_LEVEL_FULL ? BATTERY_LEVEL_FULL : g_batteryLevel;
	for (int i = 0; i < bars; i++) {
		C2D_DrawRectSolid(x + BATTERY_BAR_X + i * BATTERY_BAR_SPACING, y + BATTERY_BAR_Y, depth,
		                  BATTERY_BAR_WIDTH, BATTERY_BAR_HEIGHT, color);
	}
}

s64 status_card_free_bytes(void)
{
	return g_cardFreeBytes;
}
