/**
 * @file ftest_imp_reinforce_oracle.h
 * @brief In-tick imp-reinforce oracle for the keeper-rx port (keeper-rx design/imp-jobs.md Slice 3, item 6).
 *
 * Spawns one imp on the synthetic M-reinforce fixture (level 9007): a solid Player0-claimed room around the
 * dungeon heart with a single reinforceable EARTH wall jutting off its east edge (no dig designation). Past
 * the 128-turn digger-stack gate the imp goes idle, the heart-connected flood-fill stages that border earth
 * wall for reinforcing (add_to_reinforce_stack_if_need_to), the imp self-assigns it, walks to the floor tile
 * beside it and drives CrInst_REINFORCE until the 27th fire fortifies the wall into a pretty (torch) wall.
 * Per game tick it dumps the imp's mappos, active_state, reinforce instance + timer, consecutive_reinforcements
 * (0->26 then reset), and the wall slab's kind and owner. The keeper-rx reinforce loop diffs these
 * tick-by-tick (keeper-rx ADR-0002 parity gate).
 *
 * Pure observation: the ftest drives the normal simulation the way it would run (spawn a creature) and logs
 * existing state; it changes no engine behaviour.
 */

#pragma once

#include "../../globals.h"

#ifdef FUNCTESTING

#ifdef __cplusplus
extern "C" {
#endif

TbBool ftest_imp_reinforce_oracle_init();

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
