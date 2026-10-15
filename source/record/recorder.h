// Recording a take: the self-timer, the pre-record, pause, markers and stop. A take goes on while the app
// is in the background.
#pragma once
#include <3ds.h>
#include <stdbool.h>
#include "takes.h"

void recorder_init(void);
void recorder_exit(void); // a running take is closed
void recorder_poll(void); // once per frame: stops that are due
// A button press or tap at that mic sample. Develop repairs its click in the take or in a later pre-record.
void recorder_note_press(u64 sample);
void recorder_record_button(void); // starts the self-timer or a take, or stops the take
void recorder_toggle_pause(void);
void recorder_add_marker(void);
bool recorder_is_saving(void); // a stop is pending: "Saving" is on screen
int recorder_take_id(void); // the take being recorded
const TakeMeta *recorder_take_meta(void);

bool recorder_timer_running(void);
bool recorder_timer_due(u64 now);
u64 recorder_timer_started_at(void);
u64 recorder_timer_ends_at(void);
void recorder_cancel_timer(void);
void recorder_start_after_timer(void);
