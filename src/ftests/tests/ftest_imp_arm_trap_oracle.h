/**
 * @file ftest_imp_arm_trap_oracle.h
 * @brief In-tick imp-arms-trap oracle for the keeper-rx port (keeper-rx docs/design/imp-hauling.md Slice S6).
 *
 * Spawns one imp on the synthetic M-trap fixture (level 9017 — a Player0 claimed room with a 2-slab
 * Workshop), then injects the S6 scenario the way a player's own commands would produce it: manufactures one
 * Boulder trap crate already inside the Workshop (create_crate_in_workshop, bumping the room's item-storage
 * capacity and the owner's trap_amount_stored/trap_amount_placeable counters together) and places one
 * Boulder trap on owned ground (player_place_trap_at, consuming the placeable slot the crate manufacture just
 * created and leaving the trap unarmed — num_shots stays 0, since every shipped DK1 trap kind's
 * InstantPlacement config is false, S6 §0). The unmodified game loop then runs: the imp self-assigns the
 * empty-trap job off the shared digger stack (seam #5, add_empty_traps_to_imp_stack), walks to the crate,
 * picks it up (creature_picks_up_trap_object — decrementing the workshop's item accounting since the crate
 * is room-resident, then attaching it via the generic creature_drag_object carry), walks it BACKWARDS to the
 * trap (creature_arms_trap), arms it (rearm_trap sets num_shots and flips the trap's transparency render
 * bits), destroys the consumed crate, and bumps dungeon->lvstats.traps_armed. Per tick it dumps the imp's
 * mappos/active_state/continue_state, whether its own pickup_object_id/arming_thing_id/dragtng_idx fields
 * point at the tracked crate/trap (booleans, never a raw thing index — ADR-0021), the crate's and trap's own
 * existence/position/owner/drag-flag/num_shots/rendering_flags, the workshop room's capacity fields, the
 * owner's per-kind trap_amount_stored/trap_amount_placeable, dungeon->lvstats.traps_armed, and the shared
 * digger stack's occupancy at seam #5 (DigTsk_PicksUpCrateToArm) and seam #10
 * (DigTsk_PicksUpCrateForWorkshop — must stay empty every tick: every fixture crate is room-resident from
 * tick 0, so the "already in a matching room" exclusion the crate-to-workshop job's own eligibility test
 * applies fires for it the whole time, S6 §1) — for the keeper-rx trap-arm haul chain (staging, dispatch,
 * the two-state pickup/arm chain, the shared drag mechanism, workshop accounting) to diff tick-by-tick.
 *
 * Deliberately excludes door placement: S6 §0/§5 derives that door placement is a single synchronous
 * player-command transaction with zero digger-stack/imp involvement (no CrSt_* state, no DigTsk_* task), and
 * no RX door system (Dungeon.TotalDoors, door_amount_stored/placeable) is landed yet to gate a door
 * observable against — adding one here would be gating on a missing system, not a divergence in a landed
 * mechanism.
 *
 * Pure observation: the ftest drives the normal simulation the way a player would (spawn a creature, place a
 * trap, manufacture a crate) and logs existing state; it changes no engine behaviour.
 */

#pragma once

#include "../../globals.h"

#ifdef FUNCTESTING

#ifdef __cplusplus
extern "C" {
#endif

TbBool ftest_imp_arm_trap_oracle_init();

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
