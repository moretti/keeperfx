/**
 * @file ftest_imp_handoff_oracle.h
 * @brief In-tick imp dig->claim handoff oracle for the keeper-rx port (keeper-rx design/imp-jobs.md Slice 2, item 6 (dig->claim handoff)).
 *
 * Spawns one imp on the synthetic M-handoff fixture (level 9006): a solid Player0-claimed room around the
 * dungeon heart with a single diggable EARTH tile jutting off its east edge. Past the 128-turn digger-stack gate
 * the imp goes idle, floods the shared stack from the heart (add_pretty_and_convert_to_imp_stack),
 * self-assigns that one path as an ImproveDungeon task, walks onto it and drives CrInst_PRETTY_PATH until the
 * single fire flips it to SlbT_CLAIMED. No player designation is involved — the heart-connected flood-fill
 * designates the tile. Per game tick it dumps the imp's mappos, active_state, claim instance + timer, the
 * target slab's kind and owner, and the dungeon's total_area. The keeper-rx claim loop diffs these
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

TbBool ftest_imp_handoff_oracle_init();

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
