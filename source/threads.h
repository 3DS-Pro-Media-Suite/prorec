// Thread priorities and cores for threadCreate (libctru). A lower priority number runs first.
#pragma once
#include <3ds.h>

#define THREAD_PRIORITY_FALLBACK 0x30 // the main thread's usual priority, if asking for it fails
#define THREAD_PRIORITY_LOWEST 0x3F
#define THREAD_CORE_DEFAULT (-2) // the application's default core
#define NANOSECONDS_PER_MS 1000000LL // svcSleepThread counts in nanoseconds

// A priority relative to the calling (main) thread. A negative offset runs before it, a positive one after.
static inline s32 threads_priority_from_main(int offset)
{
	s32 mainPriority = THREAD_PRIORITY_FALLBACK;
	svcGetThreadPriority(&mainPriority, CUR_THREAD_HANDLE);
	s32 priority = mainPriority + offset;
	if (priority > THREAD_PRIORITY_LOWEST) {
		return THREAD_PRIORITY_LOWEST;
	}
	return priority;
}
