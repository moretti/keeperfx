/**
 * @file ftest_imp_wander_variety_oracle.h
 * @brief In-tick idle-imp-wander-diversity oracle for the keeper-rx port (keeper-rx
 * docs/rx-internals/87-special-digger-jobs.md, the CreatureWander slice).
 *
 * Every other imp-job oracle's imps eventually find real work, so the one idle-wander leg any of them
 * happens to take (the starter-dungeon-jobs oracle's post-reinforce drift) is a single data point — and
 * that one imp's frozen birth seed happens to produce a purely axis-aligned leg, so a port that can only
 * ever move along X or Y would still pass it. This ftest spawns WANDER_IMP_COUNT Player0 imps on the
 * synthetic M-wander fixture (level 9019 — a fully Player0-claimed open floor field with a Dungeon Heart
 * and nothing else diggable, claimable, mineable or reinforceable anywhere), so every one of them goes idle
 * on its first tick and, finding no directed job ever available, calls
 * creature_choose_random_destination_on_valid_adjacent_slab (creature_states.c) every idle cycle for the
 * whole run. Spawned at spread-out slabs so their frozen (thing index, creation turn) pairs differ, each
 * imp's own (start_stl, m) draw is a different, genuinely lifelong constant — covering more of the wander
 * function's behaviour (including off-axis, diagonal walk legs) than any single-imp fixture can.
 *
 * Per tick it dumps every tracked imp's mappos, active_state, move_angle_xy and its own frozen-seed
 * derivation triple (thing_index/creation_turn/random_seed, ADR-0028) — enough for a keeper-rx replay to
 * both reproduce the identical seed and diff the resulting walk tick-for-tick.
 *
 * Pure observation: the ftest drives the normal simulation the way spawning several idle keeper imps in an
 * empty room would, and logs existing state; it changes no engine behaviour.
 */

#pragma once

#include "../../globals.h"

#ifdef FUNCTESTING

#ifdef __cplusplus
extern "C" {
#endif

TbBool ftest_imp_wander_variety_oracle_init();

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
