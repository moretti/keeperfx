/**
 * @file ftest_starter_dungeon_jobs_oracle.h
 * @brief Realistic in-tick imp job oracle on the starter dungeon (level 9008), consolidating the three
 *        single-action job oracles (imp-claim 9005, imp dig→claim handoff 9006, imp-reinforce 9007) onto one
 *        real-looking map.
 *
 * One controlled imp runs the opening a keeper actually plays: from the heart room it digs a designated east
 * earth tile, hands off into claiming the revealed floor (check_out_imp_last_did), then reinforces the %5-grid
 * wall the fresh claimed floor exposes into a fortified (torch) wall. Per game tick it dumps the imp's mappos,
 * active_state, the driving instance + its timer, consecutive_reinforcements, the slab it is working
 * (working_stl) with that slab's kind + owner, and the dungeon's total_area — the union of what the three
 * retired oracles dumped, so the keeper-rx dig/claim/reinforce loops diff tick-by-tick against it.
 *
 * Determinism: the ftest isolates a single imp — it deletes the map's starting imps and resets the
 * pre-claimed east room (used by the torch oracle) back to earth, so only the one designated dig, its claim
 * and the one wall it exposes are on the shared digger stack. Spawns past the 128-turn stack gate (as the
 * retired oracles do) so the imp self-assigns with no random-wander window. Pure observation; changes no
 * engine behaviour.
 */

#pragma once

#include "../../globals.h"

#ifdef FUNCTESTING

#ifdef __cplusplus
extern "C" {
#endif

TbBool ftest_starter_dungeon_jobs_oracle_init();

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
