// The top screen of the recordings: the selected take's header, its overview with the markers and the play
// head, the play time, the develop state and the level meter.
#pragma once
#include "takes.h"

// take is NULL when there are no recordings; meta is the take's TXT.
void gallery_view_draw(const Take *take, const TakeMeta *meta);
