/**
 * @file ftest_imp_prison_drag_oracle.h
 * @brief In-tick imp-drags-unconscious-enemy-to-prison oracle for the keeper-rx port (keeper-rx
 * docs/design/imp-hauling.md Slice S7).
 *
 * Spawns one Player0 imp and one Player1 enemy on the synthetic M-prison fixture (level 9018 — a Player0
 * claimed room with a 2-slab Prison, plus an isolated Player1 heart so Player1 is a real, registered
 * dungeon), then injects the S7 scenario deterministically the way combat's own knockout would have
 * produced it, with zero combat code exercised: enables the Player0 imprison tendency
 * (set_creature_tendencies, CrTend_Imprison — IMPRISON_BUTTON_DEFAULT ships false, so without this call
 * the whole chain stays inert) and knocks the enemy out directly (make_creature_unconscious — the real,
 * exported v1.4.0 function combat's own creature_can_be_set_unconscious would otherwise call). The
 * unmodified game loop then runs: the imp self-assigns the unclaimed-unconscious-body job off the shared
 * digger stack (seam #2, add_unclaimed_unconscious_bodies_to_imp_stack), walks to the body
 * (creature_pick_up_unconscious_body — drawing the two frozen RNG picks for a drop spot inside the
 * resolved prison, then attaching it via the bidirectional set_creature_being_dragged_by), drags it
 * BACKWARDS to the prison (creature_drop_body_in_prison), wakes it and hands it to
 * creature_arrived_at_prison (add_creature_to_work_room(Job_CAPTIVITY), room->used_capacity += 1,
 * threaded onto room->creatures_list), and settles into creature_in_prison. Per tick it dumps both
 * creatures' mappos/move_angle_xy/active_state/continue_state, the enemy's conscious_back_turns and
 * creature_control_flags, the bidirectional dragtng_idx linkage and pickup_creature_id (booleans against
 * captured indices, never a raw thing index — ADR-0021), the enemy's own room-list linkage
 * (next_in_room/prev_in_room, as "is this none" booleans), the Prison room's identity/capacity fields, and
 * the shared digger stack's occupancy at seam #2 (DigTsk_PickUpUnconscious) and seam #1
 * (DigTsk_SaveUnconscious — must stay 0 every tick: DRAGUNCONSCIOUSTOLAIR ships false, so the sibling
 * drag-own-unconscious-creature-to-lair job stays inert) — for the keeper-rx prison-drag chain (staging,
 * dispatch, the pickup/drag/drop chain, the bidirectional creature-drag mechanism, captivity capacity) to
 * diff tick-by-tick.
 *
 * Pure observation: the ftest drives the normal simulation the way a player's own command flow would
 * (flip a tendency, spawn a creature, knock it out) and logs existing state; it changes no engine
 * behaviour.
 */

#pragma once

#include "../../globals.h"

#ifdef FUNCTESTING

#ifdef __cplusplus
extern "C" {
#endif

TbBool ftest_imp_prison_drag_oracle_init();

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
