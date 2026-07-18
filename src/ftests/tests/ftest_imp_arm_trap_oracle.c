#include "ftest_imp_arm_trap_oracle.h"

#ifdef FUNCTESTING

#include "../../pre_inc.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "../ftest.h"
#include "../ftest_util.h"

#include "../../game_legacy.h"
#include "../../game_merge.h"
#include "../../keeperfx.hpp"
#include "../../thing_data.h"
#include "../../thing_list.h"
#include "../../thing_creature.h"
#include "../../thing_navigate.h"
#include "../../thing_traps.h"
#include "../../creature_control.h"
#include "../../config_creature.h"
#include "../../config_trapdoor.h"
#include "../../creature_states.h"
#include "../../map_data.h"
#include "../../slab_data.h"
#include "../../dungeon_data.h"
#include "../../spdigger_stack.h"
#include "../../room_data.h"
#include "../../room_workshop.h"
#include "../../config_terrain.h"
#include "../../player_data.h"
#include "../../player_instances.h"
#include "../../ver_defs.h"

#include "../../post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif

// Oracle dump format version — bump on any layout/meaning change (keeper-rx/docs/oracle/dump-format.md).
#define IMP_ARM_TRAP_ORACLE_VERSION 1

// The scenario is pinned to the M-trap synthetic fixture (SyntheticMaps.MTrap): a Player0 claimed room
// (slabs (2,2)-(20,20)) with a 2-slab Workshop inside it (slabs (10,10)-(11,10)). Neither the crate nor the
// trap is baked into the fixture's own .tng — both are created below by the same scenario-injection
// transactions the keeper-rx side calls (WorkshopAccounting.CreateCrateInWorkshop, TrapPlacement.PlaceUnarmedTrap),
// mirroring create_crate_in_workshop/player_place_trap_at running AFTER room integration.
#define IMP_ARM_TRAP_ORACLE_LEVEL 9017
#define WORKSHOP_SLB_X0 10
#define WORKSHOP_SLB_X1 11
#define WORKSHOP_SLB_Y  10

// Where the manufactured crate sits inside the Workshop's own footprint.
#define CRATE_SLB_X 10
#define CRATE_SLB_Y 10

// The trap's own placement site — well clear of the heart (slab (2,2)), the Workshop and the imp's spawn,
// but still inside the Player0 claimed room (slabs (2,2)-(20,20)).
#define TRAP_SLB_X 15
#define TRAP_SLB_Y 15

// The imp spawns inside the claimed room, away from the Workshop and the trap site.
#define SPAWN_SLB_X 5
#define SPAWN_SLB_Y 5

// Spawn the imp only AFTER game turn 128 (see ftest_imp_haul_oracle.c's identical rationale): the shared
// digger stack rebuilds at most once per 128 turns, so spawning past the gate means the imp self-assigns off
// seam #5 (the empty-trap scan) on its very first idle tick with no RNG-driven wander to reproduce.
#define SPAWN_DELAY_TURNS 130

// Keep dumping one tick past the arm tick, so the golden also pins the imp settling back to idle
// (active_state returning to CrSt_ImpDoingNothing) — deliberately NOT a longer tail: once no staged work is
// left on the shared digger stack, the very next idle tick falls through to the RNG-driven random-wander
// fallback (mirrors ftest_imp_dig_oracle.c's identical dig-out rationale), which the keeper-rx port
// deliberately defers (no synced RNG for it yet), so dumping past this single settle tick would capture a
// non-reproducible record. Empirically the settle holds for exactly one extra tick before wander begins.
#define POST_ARM_TICKS 1
// Safety cap: the imp waits out the 128-turn gate, walks to the crate (~60 turns), picks it up, walks it
// BACKWARDS to the trap (slower than a forward walk), arms it, then the one-tick settle. Comfortably under
// the observed ~263-tick run.
#define ARM_TRAP_MAX_TICKS 500

static FILE* imp_arm_trap_jsonl = NULL;
static struct Thing* imp_arm_trap_imp = NULL;
static struct Thing* imp_arm_trap_crate = NULL;
static struct Thing* imp_arm_trap_trap = NULL;
// Captured once, right after creation: delete_thing_structure memsets the WHOLE freed Thing struct to zero
// (including ->index), so re-reading imp_arm_trap_crate->index after the crate is destroyed on a successful
// arm would wrongly read back 0 — cctrl->pickup_object_id itself is never cleared on that path (S6 §3/§4,
// the same "left stale" convention as arming_thing_id), so identity must be checked against these captured,
// never-mutated index values, not the live (and, for the crate, eventually-freed) thing pointer's own field.
static ThingIndex imp_arm_trap_crate_index = 0;
static ThingIndex imp_arm_trap_trap_index = 0;
static ThingModel imp_arm_trap_model = 0;      // the Boulder TrapKind, resolved by name at t0
static ThingModel imp_arm_trap_crate_model = 0; // the matching WRKBOX_BOULDER crate object model
static int imp_arm_trap_countdown = -1; // -1 until the trap is armed; then counts POST_ARM_TICKS down to 0.

// forward declaration
FTestActionResult ftest_imp_arm_trap_oracle_action__run(struct FTestActionArgs* const args);

TbBool ftest_imp_arm_trap_oracle_init()
{
    ftest_append_action(ftest_imp_arm_trap_oracle_action__run, SPAWN_DELAY_TURNS, NULL);
    return true;
}

// Resolve the dump directory: $KEEPERFX_ORACLE_OUT, else the CWD (mirrors the other oracle ftests).
static const char* imp_arm_trap_out_dir(void)
{
    const char* dir = getenv("KEEPERFX_ORACLE_OUT");
    if (dir == NULL || dir[0] == '\0')
        return ".";
    mkdir(dir, 0755); // best-effort; ignore EEXIST
    return dir;
}

// One per-tick ground-truth record. Written BEFORE this turn's update_thing runs, so record 0 is the posed
// initial state (imp at spawn, seam #5 not yet rebuilt, crate manufactured, trap placed unarmed) and each
// later record is the state left by one game update. Fields:
//   pos_x/pos_y                          imp->mappos.{x,y}.val
//   active_state/continue_state          imp->active_state / imp->continue_state (CrSt_* ids)
//   pickup_is_our_crate/arming_is_our_trap/dragging_is_our_trap   whether the imp's own cctrl pointer
//     fields target the tracked crate/trap — booleans, never a raw thing index (ADR-0021)
//   crate_*                              the tracked crate's own existence/position/owner/drag flag
//   trap_*                               the tracked trap's own existence/position/num_shots/rendering_flags
//   traps_armed                          dungeon->lvstats.traps_armed
//   trap_amount_stored/placeable         dungeon->mnfct_info.trap_amount_{stored,placeable}[imp_arm_trap_model]
//   room_*                               the Workshop room's identity/capacity fields
//   seam5_count/seam10_count             live digger-stack entries at DigTsk_PicksUpCrateToArm /
//                                         DigTsk_PicksUpCrateForWorkshop (the latter must stay 0 every tick)
static void imp_arm_trap_dump(FILE* f, struct Thing* imp)
{
    struct CreatureControl* cctrl = creature_control_get_from_thing(imp);
    struct Dungeon* dungeon = get_dungeon(PLAYER0);

    // thing_is_invalid only checks the pointer is in-range — it stays "valid" on a freed slot (destroy_thing
    // does not null it out), so whether OUR tracked crate is still alive must be thing_exists (checks
    // TAlF_Exists) instead: the crate is destroyed the instant the trap arms successfully (S6 §4).
    const TbBool crate_exists = thing_exists(imp_arm_trap_crate);
    const long crate_pos_x = crate_exists ? (long)imp_arm_trap_crate->mappos.x.val : 0;
    const long crate_pos_y = crate_exists ? (long)imp_arm_trap_crate->mappos.y.val : 0;
    const int crate_owner = crate_exists ? (int)imp_arm_trap_crate->owner : -1;
    const TbBool crate_is_dragged = crate_exists && ((imp_arm_trap_crate->state_flags & TF1_IsDragged1) != 0);

    const TbBool trap_exists = thing_exists(imp_arm_trap_trap);
    const long trap_pos_x = trap_exists ? (long)imp_arm_trap_trap->mappos.x.val : 0;
    const long trap_pos_y = trap_exists ? (long)imp_arm_trap_trap->mappos.y.val : 0;
    const int trap_num_shots = trap_exists ? (int)imp_arm_trap_trap->trap.num_shots : 0;
    const int trap_rendering_flags = trap_exists ? (int)imp_arm_trap_trap->rendering_flags : 0;

    // Compared against the captured indices (see the static declarations above), not the live pointers'
    // ->index fields, so these correctly stay true even once the crate is destroyed and cctrl's own field is
    // left stale (S6 §3/§4) rather than flipping false because the freed struct's own index was zeroed.
    const TbBool pickup_is_our_crate = (cctrl->pickup_object_id == imp_arm_trap_crate_index);
    const TbBool arming_is_our_trap = (cctrl->arming_thing_id == imp_arm_trap_trap_index);
    const TbBool dragging_is_our_crate = (cctrl->dragtng_idx == imp_arm_trap_crate_index);

    struct Room* room = subtile_room_get(slab_subtile_center(WORKSHOP_SLB_X0), slab_subtile_center(WORKSHOP_SLB_Y));
    const TbBool room_exists_flag = !room_is_invalid(room);
    const int room_owner = room_exists_flag ? (int)room->owner : -1;
    const int room_kind = room_exists_flag ? (int)room->kind : 0;
    const int room_slabs_count = room_exists_flag ? (int)room->slabs_count : 0;
    const int room_total_capacity = room_exists_flag ? (int)room->total_capacity : 0;
    const int room_used_capacity = room_exists_flag ? (int)room->used_capacity : 0;
    const long room_capacity_used_for_storage = room_exists_flag ? (long)room->capacity_used_for_storage : 0;

    int seam5_count = 0, seam10_count = 0;
    for (unsigned long i = 0; i < dungeon->digger_stack_length; i++)
    {
        if (dungeon->digger_stack[i].task_type == DigTsk_PicksUpCrateToArm)
            seam5_count++;
        else if (dungeon->digger_stack[i].task_type == DigTsk_PicksUpCrateForWorkshop)
            seam10_count++;
    }

    fprintf(f,
        "{\"v\":%d,\"type\":\"imp_arm_trap\",\"tick\":%ld,"
        "\"pos_x\":%ld,\"pos_y\":%ld,\"active_state\":%d,\"continue_state\":%d,"
        "\"pickup_is_our_crate\":%d,\"arming_is_our_trap\":%d,\"dragging_is_our_crate\":%d,"
        "\"crate_exists\":%d,\"crate_pos_x\":%ld,\"crate_pos_y\":%ld,\"crate_owner\":%d,\"crate_is_dragged\":%d,"
        "\"trap_exists\":%d,\"trap_pos_x\":%ld,\"trap_pos_y\":%ld,\"trap_num_shots\":%d,\"trap_rendering_flags\":%d,"
        "\"traps_armed\":%lu,"
        "\"trap_amount_stored\":%d,\"trap_amount_placeable\":%d,"
        "\"room_exists\":%d,\"room_owner\":%d,\"room_kind\":%d,\"room_slabs_count\":%d,"
        "\"room_total_capacity\":%d,\"room_used_capacity\":%d,\"room_capacity_used_for_storage\":%ld,"
        "\"seam5_count\":%d,\"seam10_count\":%d}\n",
        IMP_ARM_TRAP_ORACLE_VERSION, (long)get_gameturn(),
        (long)imp->mappos.x.val, (long)imp->mappos.y.val, (int)imp->active_state, (int)imp->continue_state,
        pickup_is_our_crate, arming_is_our_trap, dragging_is_our_crate,
        crate_exists, crate_pos_x, crate_pos_y, crate_owner, crate_is_dragged,
        trap_exists, trap_pos_x, trap_pos_y, trap_num_shots, trap_rendering_flags,
        (unsigned long)dungeon->lvstats.traps_armed,
        (int)dungeon->mnfct_info.trap_amount_stored[imp_arm_trap_model],
        (int)dungeon->mnfct_info.trap_amount_placeable[imp_arm_trap_model],
        room_exists_flag, room_owner, room_kind, room_slabs_count,
        room_total_capacity, room_used_capacity, room_capacity_used_for_storage,
        seam5_count, seam10_count);
}

// Driver: on the first turn, spawn the imp, manufacture the crate, place the trap, and open the JSONL; every
// turn dump; stop once the trap is armed AND the post-arm settle tail completes, or the safety cap trips.
FTestActionResult ftest_imp_arm_trap_oracle_action__run(struct FTestActionArgs* const args)
{
    if (args->times_executed == 0)
    {
        const long level = (long)get_loaded_level_number();
        if (level != IMP_ARM_TRAP_ORACLE_LEVEL)
        {
            FTEST_FAIL_TEST("imp arm trap oracle: expected level %d (M-trap), got %ld — no workshop/room to arm a trap in",
                IMP_ARM_TRAP_ORACLE_LEVEL, level);
            return FTRs_Go_To_Next_Action;
        }

        ftest_util_reveal_map(PLAYER0);

        // Resolve the Boulder trap kind by its trapdoor.cfg Name (not a hardcoded model id) — the same
        // trap S6's own crafted TDD tests exercise (TrapCrateHaulTests.cs), Shots = 1 in the shipped config
        // (https://github.com/dkfans/keeperfx/blob/v1.4.0/config/fxdata/trapdoor.cfg#L281-L317).
        imp_arm_trap_model = (ThingModel)trap_model_id("BOULDER");
        if ((long)imp_arm_trap_model <= 0)
        {
            FTEST_FAIL_TEST("imp arm trap oracle: trapdoor.cfg has no BOULDER trap kind");
            return FTRs_Go_To_Next_Action;
        }
        imp_arm_trap_crate_model = trap_crate_object_model(imp_arm_trap_model);

        const MapCoord sx = subtile_coord_center(slab_subtile_center(SPAWN_SLB_X));
        const MapCoord sy = subtile_coord_center(slab_subtile_center(SPAWN_SLB_Y));
        imp_arm_trap_imp = ftest_util_create_creature(sx, sy, PLAYER0, 1, get_players_special_digger_model(PLAYER0));
        if (thing_is_invalid(imp_arm_trap_imp))
        {
            FTEST_FAIL_TEST("imp arm trap oracle: could not spawn the imp at slab (%d,%d)", SPAWN_SLB_X, SPAWN_SLB_Y);
            return FTRs_Go_To_Next_Action;
        }
        move_thing_in_map(imp_arm_trap_imp, &imp_arm_trap_imp->mappos);

        // The Workshop room is baked into the fixture's own slab kinds (M-trap's .slb), assembled at level
        // load by the unmodified initialise_map_rooms — no room-placement action is needed here, mirroring
        // the room-state/imp-haul oracles' own treasury/workshop fixtures.
        struct Room* room = subtile_room_get(slab_subtile_center(WORKSHOP_SLB_X0), slab_subtile_center(WORKSHOP_SLB_Y));
        if (room_is_invalid(room) || (room->owner != PLAYER0) || !room_role_matches(room->kind, RoRoF_CratesStorage))
        {
            FTEST_FAIL_TEST("imp arm trap oracle: no Player0 CratesStorage-role room found at the fixture's workshop slabs");
            delete_thing_structure(imp_arm_trap_imp, 0);
            imp_arm_trap_imp = NULL;
            return FTRs_Go_To_Next_Action;
        }

        // Manufacture one Boulder crate already inside the Workshop — create_crate_in_workshop bumps the
        // room's item-storage capacity AND the owner's trap_amount_stored/trap_amount_placeable counters
        // together (add_workshop_item_to_amounts), the one moment a crate's physical presence and its
        // bookkeeping are created in lockstep (imp-hauling.md S6 §0/fixture requirements).
        imp_arm_trap_crate = create_crate_in_workshop(room, imp_arm_trap_crate_model,
            slab_subtile_center(CRATE_SLB_X), slab_subtile_center(CRATE_SLB_Y));
        if (thing_is_invalid(imp_arm_trap_crate))
        {
            FTEST_FAIL_TEST("imp arm trap oracle: create_crate_in_workshop failed");
            delete_thing_structure(imp_arm_trap_imp, 0);
            imp_arm_trap_imp = NULL;
            return FTRs_Go_To_Next_Action;
        }
        imp_arm_trap_crate_index = imp_arm_trap_crate->index;

        // Place one Boulder trap on owned ground via the real, CHECKED player command
        // (player_place_trap_at — refuses unless is_trap_placeable, exactly like the player's own UI):
        // consumes the placeable slot the crate manufacture just created and leaves the trap unarmed
        // (num_shots stays 0 — every shipped trap kind's InstantPlacement is false, S6 §0).
        const MapSubtlCoord trap_stl_x = slab_subtile_center(TRAP_SLB_X);
        const MapSubtlCoord trap_stl_y = slab_subtile_center(TRAP_SLB_Y);
        if (!player_place_trap_at(trap_stl_x, trap_stl_y, PLAYER0, imp_arm_trap_model))
        {
            FTEST_FAIL_TEST("imp arm trap oracle: player_place_trap_at refused (no placeable Boulder slot?)");
            delete_thing_structure(imp_arm_trap_imp, 0);
            imp_arm_trap_imp = NULL;
            return FTRs_Go_To_Next_Action;
        }
        imp_arm_trap_trap = get_trap_at_subtile_of_model_and_owned_by(trap_stl_x, trap_stl_y, imp_arm_trap_model, PLAYER0);
        if (thing_is_invalid(imp_arm_trap_trap))
        {
            FTEST_FAIL_TEST("imp arm trap oracle: player_place_trap_at succeeded but the trap thing can't be found");
            delete_thing_structure(imp_arm_trap_imp, 0);
            imp_arm_trap_imp = NULL;
            return FTRs_Go_To_Next_Action;
        }
        imp_arm_trap_trap_index = imp_arm_trap_trap->index;

        char path[512];
        snprintf(path, sizeof(path), "%s/oracle_imp_arm_trap.jsonl", imp_arm_trap_out_dir());
        imp_arm_trap_jsonl = fopen(path, "w");
        if (imp_arm_trap_jsonl == NULL)
        {
            FTEST_FRAMEWORK_ABORT("imp arm trap oracle: failed to open '%s'", path);
            delete_thing_structure(imp_arm_trap_imp, 0);
            imp_arm_trap_imp = NULL;
            return FTRs_Go_To_Next_Action;
        }
        FTESTLOG("imp arm trap oracle: writing '%s'", path);

        const char* prov_build = getenv("KEEPERFX_ORACLE_BUILD");
        fprintf(imp_arm_trap_jsonl,
            "{\"v\":%d,\"type\":\"meta\",\"probe\":\"imp_arm_trap\",\"level\":%d,\"campaign\":\"classic\","
            "\"trap_model\":%d,"
            "\"workshop_slb_x0\":%d,\"workshop_slb_x1\":%d,\"workshop_slb_y\":%d,"
            "\"crate_stl_x\":%d,\"crate_stl_y\":%d,\"trap_stl_x\":%d,\"trap_stl_y\":%d,"
            "\"spawn_stl_x\":%d,\"spawn_stl_y\":%d,"
            "\"engine\":\"keeperfx-oracle-dumps\",\"version\":\"%s\",\"build\":\"%s\"}\n",
            IMP_ARM_TRAP_ORACLE_VERSION, IMP_ARM_TRAP_ORACLE_LEVEL, (int)imp_arm_trap_model,
            WORKSHOP_SLB_X0, WORKSHOP_SLB_X1, WORKSHOP_SLB_Y,
            (int)slab_subtile_center(CRATE_SLB_X), (int)slab_subtile_center(CRATE_SLB_Y),
            (int)trap_stl_x, (int)trap_stl_y,
            (int)slab_subtile_center(SPAWN_SLB_X), (int)slab_subtile_center(SPAWN_SLB_Y),
            VER_STRING, prov_build ? prov_build : "");
    }

    imp_arm_trap_dump(imp_arm_trap_jsonl, imp_arm_trap_imp);

    // Stop once the trap is armed (num_shots flips off 0) plus a bounded settle tail, or the safety cap.
    const TbBool armed = thing_exists(imp_arm_trap_trap) && (imp_arm_trap_trap->trap.num_shots > 0);
    if (armed && imp_arm_trap_countdown < 0)
        imp_arm_trap_countdown = POST_ARM_TICKS;

    if (imp_arm_trap_countdown == 0 || (long)args->times_executed >= ARM_TRAP_MAX_TICKS)
    {
        FTESTLOG("imp arm trap oracle: dump complete (%ld records, trap armed = %d)",
            (long)args->times_executed + 1, (int)armed);
        fclose(imp_arm_trap_jsonl);
        imp_arm_trap_jsonl = NULL;
        delete_thing_structure(imp_arm_trap_imp, 0);
        imp_arm_trap_imp = NULL;
        return FTRs_Go_To_Next_Action;
    }
    if (imp_arm_trap_countdown > 0)
        imp_arm_trap_countdown--;
    return FTRs_Repeat_Current_Action;
}

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
