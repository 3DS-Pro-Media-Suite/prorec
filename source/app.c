#include "app.h"
#include <stdio.h>

#define TENTHS_PER_SECOND 10

bool g_isNew3ds;
bool g_micAvailable;

int app_clamp(int value, int low, int high)
{
	if (value < low) {
		return low;
	}
	if (value > high) {
		return high;
	}
	return value;
}

void app_format_time(char *out, size_t outSize, double totalSeconds, bool withTenths)
{
	if (totalSeconds < 0) {
		totalSeconds = 0;
	}
	int hours = (int)(totalSeconds / SECONDS_PER_HOUR);
	int minutes = (int)(totalSeconds / SECONDS_PER_MINUTE) % MINUTES_PER_HOUR;
	int seconds = (int)totalSeconds % SECONDS_PER_MINUTE;
	int tenths = (int)(totalSeconds * TENTHS_PER_SECOND) % TENTHS_PER_SECOND;
	if (hours && withTenths) {
		snprintf(out, outSize, "%d:%02d:%02d.%d", hours, minutes, seconds, tenths);
	} else if (hours) {
		snprintf(out, outSize, "%d:%02d:%02d", hours, minutes, seconds);
	} else if (withTenths) {
		snprintf(out, outSize, "%02d:%02d.%d", minutes, seconds, tenths);
	} else {
		snprintf(out, outSize, "%02d:%02d", minutes, seconds);
	}
}
