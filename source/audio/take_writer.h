// The take writer: a thread moves mic samples from the ring into a raw 16-bit WAV, untouched. The header
// is patched every 2 s, so a crash or a flat battery loses only the last moments. Pause and resume keep
// writing the same file; each resume point becomes a marker.
#pragma once
#include <3ds.h>
#include <stdbool.h>

#define TAKE_WRITER_MAX_EVENTS 512
#define TAKE_WRITER_MAX_MARKERS 64

bool take_writer_start(const char *path, u64 fromSample); // fromSample may lie in the past (pre-record)
void take_writer_pause(u64 atSample);
bool take_writer_resume(u64 atSample); // false until the writer has reached the last pause: try again
// Writes up to atSample, adds the markers and closes the file; false when the file had trouble.
bool take_writer_stop(u64 atSample);
bool take_writer_is_active(void);
bool take_writer_is_paused(void);
u64 take_writer_frames(void); // samples in the file so far (written and still to write)
u32 take_writer_lost_samples(void); // samples lost because the writer fell more than the ring behind
bool take_writer_failed(void); // a write failed (card full or removed)
u32 take_writer_backlog(void); // samples captured but not yet written

void take_writer_add_marker(u64 sample); // a marker at that moment
void take_writer_add_event(u64 sample); // a button press or tap, so its click can be silenced later
int take_writer_markers(u32 *out, int max);
int take_writer_events(u32 *out, int max);
