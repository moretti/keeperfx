/**
 * @file ftest_room_state_oracle.h
 * @brief Load-time room-state oracle for the keeper-rx port (keeper-rx docs/design/rooms-read-model.md, S4).
 *
 * Loads the synthetic M-rooms fixture (level 9015: a Dungeon Heart room, a 2x2 Treasury and a 2-slab
 * Training room, all Player0-owned) and dumps every Player0 room's identity/content — owner, kind,
 * creation_turn, decoded slab set, capacity pair, storage amount, efficiency — plus the per-(player,kind)
 * Dungeon aggregates, for a few ticks starting at the first tick after load (i.e. after
 * reinitialise_map_rooms has already run as part of the normal load sequence). Pure observation: no
 * designation, no creature, no player action — room assembly is entirely a load-time effect.
 */

#pragma once

#include "../../globals.h"

#ifdef FUNCTESTING

#ifdef __cplusplus
extern "C" {
#endif

TbBool ftest_room_state_oracle_init();

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
