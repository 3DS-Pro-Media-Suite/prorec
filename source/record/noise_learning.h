// Learning the noise profile: NOISE_LEARNING_SECONDS of quiet at the current gain.
#pragma once
#include <3ds.h>
#include <stdbool.h>

#define NOISE_LEARNING_SECONDS 4

void noise_learning_start(void);
void noise_learning_poll(void);
void noise_learning_cancel(void);
bool noise_learning_is_active(void);
float noise_learning_progress(void); // 0 at the start, 1 when done
