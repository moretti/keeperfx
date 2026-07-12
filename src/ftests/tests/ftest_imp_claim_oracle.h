/**
 * @file ftest_imp_claim_oracle.h
 * @brief In-tick imp-claim oracle for the keeper-rx port (keeper-rx design/imp-jobs.md Slice 2, item 6).
 *
 * Spawns one imp on the synthetic M-claim fixture (level 9005): a solid Player0-claimed room around the
 * dungeon heart with a single free PATH tile jutting off its east edge. Past the 128-turn digger-stack gate
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

TbBool ftest_imp_claim_oracle_init();

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
