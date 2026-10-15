// The player of the recordings: which file of a take plays (raw or clean), its place, the markers, seeking
// and the scrub bar.
#pragma once
#include <3ds.h>
#include <stdbool.h>
#include "takes.h"

// Loads a take: the clean file when there is one and it is wanted. keepPosition keeps the place of the same
// take or of a parked player. A NULL take closes the player.
void gallery_player_load(const Take *take, bool keepPosition);
void gallery_player_close(void);
// Closes the player but keeps its place for gallery_player_load(take, true).
void gallery_player_park(void);
// Asks for the overview of the file that plays.
void gallery_player_request_overview(void);
bool gallery_player_plays_clean(int takeId);
// Plays or pauses the take, loading it first when needed.
void gallery_player_toggle(const Take *take);
// Playing with the volume slider at zero sounds like a fault.
void gallery_player_warn_if_muted(void);
// Switches between the raw take and the clean file when the take has both and no develop is busy with it.
void gallery_player_switch_raw_or_clean(const Take *take);
// Seeks to the next marker (direction > 0) or the one before; without one, to the end or the start.
void gallery_player_jump_to_marker(int direction);
// The D-pad or the circle pad held to one side seeks.
void gallery_player_seek_input(u32 held);
// Moves the player to where the scrub bar is touched. True while the touch is on the bar.
bool gallery_player_scrub_input(u32 held, touchPosition touch);
// Tap or drag to move; the ticks are the markers.
void gallery_player_draw_scrub_bar(void);
