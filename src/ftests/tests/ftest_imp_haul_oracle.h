/**
 * @file ftest_imp_haul_oracle.h
 * @brief In-tick imp-haul-gold oracle for the keeper-rx port (keeper-rx docs/design/imp-hauling.md Slice S5).
 *
 * Spawns one imp on the synthetic M-haul fixture (level 9016 — M-mine's shape, a neutral GOLD seam at the
 * corridor's one diggable tile, PLUS a 2-slab Player0 Treasury inside the claimed room), tags the seam for
 * mining with the real player action at turn 0, and lets the unmodified game loop run: the imp self-assigns
 * off the shared digger stack, mines the seam, and — once carrying more than gold_hold and the 128-turn
 * money-for-treasury throttle window is open — diverts mid-mine to bank instead of dropping another
 * overflow pile, growing/creating a gold hoard on the treasury, then resumes mining, and (once the seam is
 * exhausted) sweeps up any loose overflow pile it left behind via the shared-stack gold-pickup seam. Per
 * tick it dumps the imp-mine oracle's baseline fields (mappos/state/instance/block/task-count/gold_carried/
 * loose-pile) plus continue_state, the imp's random_seed (the frozen ADR-0028 THING_RANDOM draw), the
 * Treasury room's identity/capacity fields, every gold-hoard object on the treasury's own slabs, and
 * dungeon->total_money_owned — for the keeper-rx imp-haul chain (staging/dispatch/pickup/deposit/hoard
 * arithmetic/the five-call-site throttle) to diff tick-by-tick.
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

TbBool ftest_imp_haul_oracle_init();

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
