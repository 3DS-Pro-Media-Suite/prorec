// The develop queue: one worker thread on a spare core develops the queued takes one after another.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "develop.h"

void develop_queue_init(void);
// A develop in progress stops; its take can be developed again from the recordings.
void develop_queue_exit(void);
bool develop_queue_add(int id, const DevelopParams *params);
bool develop_queue_active(void);
bool develop_queue_has(int id);
int develop_queue_current(void); // the take being developed, 0 = none
int develop_queue_progress(void); // percent done of the current take

// Picks up a develop that ended, with what it reported: false when none did.
bool develop_queue_take_done(int *id, bool *ok, char *message, size_t messageSize);
