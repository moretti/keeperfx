/**
 * @file ftest_imp_convert_oracle.h
 * @brief In-tick imp-convert oracle for the keeper-rx port (keeper-rx design/imp-jobs.md Slice 2, the
 * "standing coverage gap" note — plan slice S1 in keeper-rx docs/roadmap.md).
 *
 * Spawns one Player0 imp on the synthetic M-convert fixture (level 9012): a solid Player0-claimed room
 * around the Player0 heart with a single Player1-CLAIMED tile (and a Player1 heart on it) jutting off its
 * east edge. Past the 128-turn digger-stack gate the imp goes idle, floods the shared stack from its heart
 * (add_pretty_and_convert_to_imp_stack), self-assigns the enemy tile as a ConvertDungeon task, walks onto it
 * and drives CrInst_DESTROY_AREA until the slab's health runs out and neutralise_enemy_block hands it back
 * to the neutral player — then, still standing on the now-free path, falls through to CrInst_PRETTY_PATH and
 * claims it for Player0, exactly as imp_converts_dungeon does. No player designation is involved — the
 * heart-connected flood-fill designates the tile.
 *
 * Per game tick it dumps the imp's mappos, active_state, convert/claim instance + timer, the target slab's
 * kind/owner/health, the shared digger stack's occupancy, and both dungeons' total_area (Player1's dips on
 * the neutralise, Player0's rises on the follow-on claim). The keeper-rx convert loop
 * (ImpTaskAcquisition/ImpClaimStates/CreatureInstances.Destroy+PrettyPath/BlockDigOut) diffs these
 * tick-by-tick (keeper-rx ADR-0002 parity gate) — closing the one job slice 2 shipped with no independent
 * KeeperFX ground truth.
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

TbBool ftest_imp_convert_oracle_init();

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
