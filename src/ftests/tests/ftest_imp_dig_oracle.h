/**
 * @file ftest_imp_dig_oracle.h
 * @brief In-tick imp-dig oracle for the keeper-rx port (keeper-rx design/imp-jobs.md Slice 1, item 7).
 *
 * Spawns one imp on the synthetic M-full fixture (level 9003), tags the fixture's single earth tile for
 * digging with the real player action at turn 0, and then lets the unmodified game loop run: the imp
 * self-assigns off the shared digger stack (imp_doing_nothing), walks to a dig spot, and swings CrInst_DIG
 * until the block is destroyed. Per game tick it dumps the imp's mappos, active_state, dig instance + timer,
 * the target block's remaining health and slab kind, and the dungeon's task count. The keeper-rx dig loop
 * (acquisition -> walk -> swing -> BlockDigOut) diffs these tick-by-tick (keeper-rx ADR-0002 parity gate).
 *
 * Pure observation: the ftest drives the normal simulation the way a player would (spawn a creature, tag a
 * tile) and logs existing state; it changes no engine behaviour.
 */

#pragma once

#include "../../globals.h"

#ifdef FUNCTESTING

#ifdef __cplusplus
extern "C" {
#endif

TbBool ftest_imp_dig_oracle_init();

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
