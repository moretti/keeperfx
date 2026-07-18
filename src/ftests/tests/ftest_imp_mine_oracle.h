/**
 * @file ftest_imp_mine_oracle.h
 * @brief In-tick imp-mine-gold oracle for the keeper-rx port (keeper-rx design/imp-jobs.md Slice S2).
 *
 * Spawns one imp on the synthetic M-mine fixture (level 9013 — M-full's shape with a neutral-owned GOLD
 * seam in place of the earth tile), tags the seam for mining with the real player action at turn 0, and
 * then lets the unmodified game loop run: the imp self-assigns off the shared digger stack
 * (imp_doing_nothing), walks to a dig spot, and swings CrInst_DIG until the seam is mined out. Per game
 * tick it dumps the imp-dig oracle's baseline fields plus gold_carried, the mined slab's kind + health, and
 * the loose gold pile the carry-cap overflow drops at the imp's stand subtile (existence, gold_stored,
 * sprite_size, owner) — through the mine-out and a short deterministic tail. The keeper-rx mine-gold loop
 * (the same acquisition/walk/swing chain as dig, plus instf_dig's gold branch, imp_digs_mines's overflow
 * drop, and BlockDigOut.Mine) diffs these tick-by-tick.
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

TbBool ftest_imp_mine_oracle_init();

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
