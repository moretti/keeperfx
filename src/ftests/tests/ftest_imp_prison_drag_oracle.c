#include "ftest_imp_prison_drag_oracle.h"

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
#include "../../creature_control.h"
#include "../../config_creature.h"
#include "../../creature_states.h"
#include "../../map_data.h"
#include "../../slab_data.h"
#include "../../dungeon_data.h"
#include "../../spdigger_stack.h"
#include "../../room_data.h"
#include "../../config_terrain.h"
#include "../../player_data.h"
#include "../../player_instances.h"
#include "../../ver_defs.h"

#include "../../post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif

// Oracle dump format version — bump on any layout/meaning change (keeper-rx/docs/oracle/dump-format.md).
#define IMP_PRISON_DRAG_ORACLE_VERSION 1

// The scenario is pinned to the M-prison synthetic fixture (SyntheticMaps.MPrison): a Player0 claimed room
// (slabs (2,2)-(20,20)) with a 2-slab Prison inside it (slabs (10,10)-(11,10)), plus an isolated Player1
// heart on the room's east edge (slab (21,10)) so Player1 is a real, registered dungeon. Neither the
// imprison tendency nor the enemy's unconsciousness is baked into the fixture — both are injected below
// the same way a player's own command flow (the imprison GUI button) and combat's own knockout would have
// produced them.
#define IMP_PRISON_DRAG_ORACLE_LEVEL 9018
#define PRISON_SLB_X0 10
#define PRISON_SLB_X1 11
#define PRISON_SLB_Y  10

// The enemy's spawn site — inside the Player0 claimed room (reachable, walkable ground; ownership of the
// SLAB the enemy stands on is irrelevant to its own thing ownership), well clear of the Prison and the
// imp's own spawn.
#define ENEMY_SLB_X 15
#define ENEMY_SLB_Y 15

// The imp spawns inside the claimed room, away from the Prison and the enemy.
#define SPAWN_SLB_X 5
#define SPAWN_SLB_Y 5

// Spawn the imp only AFTER game turn 128 (see ftest_imp_arm_trap_oracle.c's identical rationale): the
// shared digger stack rebuilds at most once per 128 turns, so spawning past the gate means the imp
// self-assigns off seam #2 (the unclaimed-unconscious-body scan) on its very first idle tick with no
// RNG-driven wander to reproduce.
#define SPAWN_DELAY_TURNS 130

// Stop dumping the INSTANT the enemy's active_state first reads CrSt_CreatureInPrison — zero extra ticks.
// A first read of crstates.cfg suggests CrSt_CreatureInPrison's only observable effect
// (process_prison_visuals) is gated 200 turns out, but its real MoveCheckFunction
// (process_prison_function, creature_states_prisn.c#L463) runs UNCONDITIONALLY every tick and starts a
// real, unported starvation/feeding/positioning chain (process_creature_hunger/process_prison_food) —
// confirmed empirically, not assumed: an earlier capture with a nonzero tail showed the prisoner driven
// into CrSt_MoveToPosition the very next tick after settling. So the golden's last useful record is the
// tick that shows the clean CrSt_CreatureInPrison transition itself, BEFORE that state's own body has run
// even once (docs/design/imp-hauling.md S7 §4's own "minimal faithful body" scope — RX's stub does
// nothing, so any tick where the real C's process_prison_function has already run would be an unfair
// mismatch, not a real divergence to fix).
#define POST_SETTLE_TICKS 0
// Safety cap: the imp waits out the 128-turn gate, walks to the enemy (~60 turns), picks it up, drags it
// BACKWARDS to the prison (slower than a forward walk), the two-tick arrive/settle hand-off, then the
// short tail. Comfortably under this bound.
#define PRISON_DRAG_MAX_TICKS 500

static FILE* imp_prison_drag_jsonl = NULL;
static struct Thing* imp_prison_drag_imp = NULL;
static struct Thing* imp_prison_drag_enemy = NULL;
// Captured once, right after creation — see ftest_imp_arm_trap_oracle.c's identical rationale for why
// identity must be checked against these captured index values, not a live thing's own (potentially
// stale, post-destruction) ->index field. Neither thing here is ever destroyed, but the convention is kept
// for consistency and because cctrl->pickup_creature_id/dragtng_idx are left stale by design past use.
static ThingIndex imp_prison_drag_imp_index = 0;
static ThingIndex imp_prison_drag_enemy_index = 0;
static ThingModel imp_prison_drag_enemy_model = 0; // the Dwarf creature kind, resolved by name at t0
static int imp_prison_drag_countdown = -1; // -1 until the enemy settles into CrSt_CreatureInPrison; then
                                            // counts POST_SETTLE_TICKS down to 0.

// forward declaration
FTestActionResult ftest_imp_prison_drag_oracle_action__run(struct FTestActionArgs* const args);

TbBool ftest_imp_prison_drag_oracle_init()
{
    ftest_append_action(ftest_imp_prison_drag_oracle_action__run, SPAWN_DELAY_TURNS, NULL);
    return true;
}

// Resolve the dump directory: $KEEPERFX_ORACLE_OUT, else the CWD (mirrors the other oracle ftests).
static const char* imp_prison_drag_out_dir(void)
{
    const char* dir = getenv("KEEPERFX_ORACLE_OUT");
    if (dir == NULL || dir[0] == '\0')
        return ".";
    mkdir(dir, 0755); // best-effort; ignore EEXIST
    return dir;
}

// One per-tick ground-truth record. Written BEFORE this turn's update_thing runs, so record 0 is the posed
// initial state (imp at spawn, enemy unconscious at its own spawn, seam #2 not yet rebuilt) and each later
// record is the state left by one game update. Fields:
//   imp_pos_x/y, imp_move_angle_xy, imp_active_state/continue_state    the imp's own mappos/facing/state
//   enemy_pos_x/y, enemy_move_angle_xy, enemy_active_state/continue_state   the enemy's own mappos/facing/state
//   enemy_conscious_back_turns, enemy_control_flags, enemy_owner       the enemy's own unconscious/captivity
//                                                                      bookkeeping and its (never-changing) owner
//   pickup_is_our_enemy       imp cctrl->pickup_creature_id == our captured enemy index
//   imp_dragging_is_enemy     imp cctrl->dragtng_idx == our captured enemy index
//   enemy_dragtng_is_imp      enemy cctrl->dragtng_idx == our captured imp index
//   enemy_next_in_room_is_none/enemy_prev_in_room_is_none   the enemy's own room-list linkage — booleans
//                                                            (raw index 0 == "none"), never a raw index (ADR-0021)
//   prison_*                  the Prison room's identity/capacity fields
//   seam1_count/seam2_count   live digger-stack entries at DigTsk_SaveUnconscious (must stay 0 every tick)
//                             / DigTsk_PickUpUnconscious
//   imp_random_seed/imp_index/imp_creation_turn   ADR-0028's diagnosis-pinning triple: the imp's own
//     frozen per-thing RNG seed (the source of the two THING_RANDOM draws in find_random_valid_position_
//     for_thing_in_room, imp-hauling.md S7 §5) plus the index/creation_turn that derive it
//     (random_seed = index*9377 + 9439 + creation_turn), so a keeper-rx replay can verify it derived the
//     IDENTICAL seed rather than merely asserting the resulting position by coincidence.
static void imp_prison_drag_dump(FILE* f, struct Thing* imp, struct Thing* enemy)
{
    struct CreatureControl* impctrl = creature_control_get_from_thing(imp);
    struct CreatureControl* enemyctrl = creature_control_get_from_thing(enemy);
    struct Dungeon* dungeon = get_dungeon(PLAYER0);

    const TbBool pickup_is_our_enemy = (impctrl->pickup_creature_id == imp_prison_drag_enemy_index);
    const TbBool imp_dragging_is_enemy = (impctrl->dragtng_idx == imp_prison_drag_enemy_index);
    const TbBool enemy_dragtng_is_imp = (enemyctrl->dragtng_idx == imp_prison_drag_imp_index);
    const TbBool enemy_next_in_room_is_none = (enemyctrl->next_in_room == 0);
    const TbBool enemy_prev_in_room_is_none = (enemyctrl->prev_in_room == 0);

    struct Room* room = subtile_room_get(slab_subtile_center(PRISON_SLB_X0), slab_subtile_center(PRISON_SLB_Y));
    const TbBool room_exists_flag = !room_is_invalid(room);
    const int room_owner = room_exists_flag ? (int)room->owner : -1;
    const int room_kind = room_exists_flag ? (int)room->kind : 0;
    const int room_slabs_count = room_exists_flag ? (int)room->slabs_count : 0;
    const int room_total_capacity = room_exists_flag ? (int)room->total_capacity : 0;
    const int room_used_capacity = room_exists_flag ? (int)room->used_capacity : 0;

    int seam1_count = 0, seam2_count = 0;
    for (unsigned long i = 0; i < dungeon->digger_stack_length; i++)
    {
        if (dungeon->digger_stack[i].task_type == DigTsk_SaveUnconscious)
            seam1_count++;
        else if (dungeon->digger_stack[i].task_type == DigTsk_PickUpUnconscious)
            seam2_count++;
    }

    fprintf(f,
        "{\"v\":%d,\"type\":\"imp_prison_drag\",\"tick\":%ld,"
        "\"imp_pos_x\":%ld,\"imp_pos_y\":%ld,\"imp_move_angle_xy\":%d,"
        "\"imp_active_state\":%d,\"imp_continue_state\":%d,"
        "\"enemy_pos_x\":%ld,\"enemy_pos_y\":%ld,\"enemy_move_angle_xy\":%d,"
        "\"enemy_active_state\":%d,\"enemy_continue_state\":%d,"
        "\"enemy_conscious_back_turns\":%ld,\"enemy_control_flags\":%d,\"enemy_owner\":%d,"
        "\"pickup_is_our_enemy\":%d,\"imp_dragging_is_enemy\":%d,\"enemy_dragtng_is_imp\":%d,"
        "\"enemy_next_in_room_is_none\":%d,\"enemy_prev_in_room_is_none\":%d,"
        "\"prison_exists\":%d,\"prison_owner\":%d,\"prison_kind\":%d,\"prison_slabs_count\":%d,"
        "\"prison_total_capacity\":%d,\"prison_used_capacity\":%d,"
        "\"seam1_count\":%d,\"seam2_count\":%d,"
        "\"imp_random_seed\":%lu,\"imp_index\":%d,\"imp_creation_turn\":%ld}\n",
        IMP_PRISON_DRAG_ORACLE_VERSION, (long)get_gameturn(),
        (long)imp->mappos.x.val, (long)imp->mappos.y.val, (int)imp->move_angle_xy,
        (int)imp->active_state, (int)imp->continue_state,
        (long)enemy->mappos.x.val, (long)enemy->mappos.y.val, (int)enemy->move_angle_xy,
        (int)enemy->active_state, (int)enemy->continue_state,
        (long)enemyctrl->conscious_back_turns, (int)enemyctrl->creature_control_flags, (int)enemy->owner,
        pickup_is_our_enemy, imp_dragging_is_enemy, enemy_dragtng_is_imp,
        enemy_next_in_room_is_none, enemy_prev_in_room_is_none,
        room_exists_flag, room_owner, room_kind, room_slabs_count,
        room_total_capacity, room_used_capacity,
        seam1_count, seam2_count,
        (unsigned long)imp->random_seed, (int)imp->index, (long)imp->creation_turn);
}

// Driver: on the first turn, spawn the imp and the enemy, flip the imprison tendency, knock the enemy out,
// and open the JSONL; every turn dump; stop once the enemy settles into CrSt_CreatureInPrison AND the
// post-settle tail completes, or the safety cap trips.
FTestActionResult ftest_imp_prison_drag_oracle_action__run(struct FTestActionArgs* const args)
{
    if (args->times_executed == 0)
    {
        const long level = (long)get_loaded_level_number();
        if (level != IMP_PRISON_DRAG_ORACLE_LEVEL)
        {
            FTEST_FAIL_TEST("imp prison drag oracle: expected level %d (M-prison), got %ld — no prison to drag a body into",
                IMP_PRISON_DRAG_ORACLE_LEVEL, level);
            return FTRs_Go_To_Next_Action;
        }

        ftest_util_reveal_map(PLAYER0);

        struct Room* room = subtile_room_get(slab_subtile_center(PRISON_SLB_X0), slab_subtile_center(PRISON_SLB_Y));
        if (room_is_invalid(room) || (room->owner != PLAYER0) || !room_role_matches(room->kind, RoRoF_Prison))
        {
            FTEST_FAIL_TEST("imp prison drag oracle: no Player0 Prison-role room found at the fixture's prison slabs");
            return FTRs_Go_To_Next_Action;
        }

        // Enable the Player0 imprison tendency (bypasses the GUI button, same convention the fork's own
        // ftest_bug_imp_tp_job_attack_door.c uses for this exact rule) — IMPRISON_BUTTON_DEFAULT ships
        // false, so without this call seam #2 never stages anything (imp-hauling.md S7 §2).
        if (!set_creature_tendencies(get_player(PLAYER0), CrTend_Imprison, true))
        {
            FTEST_FAIL_TEST("imp prison drag oracle: set_creature_tendencies(CrTend_Imprison, true) failed for player 0");
            return FTRs_Go_To_Next_Action;
        }

        // Mark Player1 as an existing (allocated) player — players_are_enemies requires BOTH sides to
        // player_exists() (https://github.com/dkfans/keeperfx/blob/v1.4.0/src/player_data.c#L145-L164), and
        // a synthetic single-player-campaign-booted map otherwise never allocates a second player (unlike a
        // real skirmish/multiplayer level, whose own boot flow — setup_computer_players,
        // https://github.com/dkfans/keeperfx/blob/v1.4.0/src/thing_list.c#L1260-L1270 — detects every
        // heart-bearing player and calls script_support_setup_player_as_computer_keeper for it; MConvert's
        // own doc comment already established the mirror-image fact, that leaving Player1 unallocated is
        // what makes players_are_mutual_allies false for its own convert check). The minimal, surgical fix:
        // set only the PlaF_Allocated bit player_exists() actually reads
        // (https://github.com/dkfans/keeperfx/blob/v1.4.0/src/player_data.c#L101-L106) — not the full
        // computer-keeper bootstrap (AI decisions, map exploration), which this fixture's single unarmed
        // enemy creature never needs.
        struct PlayerInfo* enemy_player = get_player(PLAYER1);
        enemy_player->allocflags |= PlaF_Allocated;
        enemy_player->id_number = PLAYER1;

        const MapCoord sx = subtile_coord_center(slab_subtile_center(SPAWN_SLB_X));
        const MapCoord sy = subtile_coord_center(slab_subtile_center(SPAWN_SLB_Y));
        imp_prison_drag_imp = ftest_util_create_creature(sx, sy, PLAYER0, 1, get_players_special_digger_model(PLAYER0));
        if (thing_is_invalid(imp_prison_drag_imp))
        {
            FTEST_FAIL_TEST("imp prison drag oracle: could not spawn the imp at slab (%d,%d)", SPAWN_SLB_X, SPAWN_SLB_Y);
            return FTRs_Go_To_Next_Action;
        }
        move_thing_in_map(imp_prison_drag_imp, &imp_prison_drag_imp->mappos);
        imp_prison_drag_imp_index = imp_prison_drag_imp->index;

        // The enemy: a Player1 Dwarf, resolved by trapdoor-cfg-style name lookup (not a hardcoded model id),
        // spawned on the Player0 claimed room's own walkable floor (its own thing ownership, not the slab's,
        // is what matters — players_are_enemies(PLAYER0, PLAYER1) needs no further setup, imp-hauling.md S7
        // §"Fixture requirements").
        imp_prison_drag_enemy_model = (ThingModel)creature_model_id("DWARFA");
        if ((long)imp_prison_drag_enemy_model <= 0)
        {
            FTEST_FAIL_TEST("imp prison drag oracle: creature.cfg has no DWARFA creature kind");
            delete_thing_structure(imp_prison_drag_imp, 0);
            imp_prison_drag_imp = NULL;
            return FTRs_Go_To_Next_Action;
        }
        const MapCoord ex = subtile_coord_center(slab_subtile_center(ENEMY_SLB_X));
        const MapCoord ey = subtile_coord_center(slab_subtile_center(ENEMY_SLB_Y));
        imp_prison_drag_enemy = ftest_util_create_creature(ex, ey, PLAYER1, 1, imp_prison_drag_enemy_model);
        if (thing_is_invalid(imp_prison_drag_enemy))
        {
            FTEST_FAIL_TEST("imp prison drag oracle: could not spawn the enemy at slab (%d,%d)", ENEMY_SLB_X, ENEMY_SLB_Y);
            delete_thing_structure(imp_prison_drag_imp, 0);
            imp_prison_drag_imp = NULL;
            return FTRs_Go_To_Next_Action;
        }
        move_thing_in_map(imp_prison_drag_enemy, &imp_prison_drag_enemy->mappos);
        imp_prison_drag_enemy_index = imp_prison_drag_enemy->index;

        // Knock the enemy out directly — the real, exported v1.4.0 function combat's own knockout decision
        // (creature_can_be_set_unconscious, RNG-gated and unported) would otherwise call, reaching the
        // identical field state with zero combat code exercised (imp-hauling.md S7 §1).
        make_creature_unconscious(imp_prison_drag_enemy);

        char path[512];
        snprintf(path, sizeof(path), "%s/oracle_imp_prison_drag.jsonl", imp_prison_drag_out_dir());
        imp_prison_drag_jsonl = fopen(path, "w");
        if (imp_prison_drag_jsonl == NULL)
        {
            FTEST_FRAMEWORK_ABORT("imp prison drag oracle: failed to open '%s'", path);
            delete_thing_structure(imp_prison_drag_imp, 0);
            imp_prison_drag_imp = NULL;
            delete_thing_structure(imp_prison_drag_enemy, 0);
            imp_prison_drag_enemy = NULL;
            return FTRs_Go_To_Next_Action;
        }
        FTESTLOG("imp prison drag oracle: writing '%s'", path);

        const char* prov_build = getenv("KEEPERFX_ORACLE_BUILD");
        fprintf(imp_prison_drag_jsonl,
            "{\"v\":%d,\"type\":\"meta\",\"probe\":\"imp_prison_drag\",\"level\":%d,\"campaign\":\"classic\","
            "\"enemy_model\":%d,"
            "\"prison_slb_x0\":%d,\"prison_slb_x1\":%d,\"prison_slb_y\":%d,"
            "\"spawn_stl_x\":%d,\"spawn_stl_y\":%d,\"enemy_stl_x\":%d,\"enemy_stl_y\":%d,"
            "\"engine\":\"keeperfx-oracle-dumps\",\"version\":\"%s\",\"build\":\"%s\"}\n",
            IMP_PRISON_DRAG_ORACLE_VERSION, IMP_PRISON_DRAG_ORACLE_LEVEL, (int)imp_prison_drag_enemy_model,
            PRISON_SLB_X0, PRISON_SLB_X1, PRISON_SLB_Y,
            (int)sx, (int)sy, (int)ex, (int)ey,
            VER_STRING, prov_build ? prov_build : "");
    }

    imp_prison_drag_dump(imp_prison_drag_jsonl, imp_prison_drag_imp, imp_prison_drag_enemy);

    // Stop once the enemy settles into CrSt_CreatureInPrison plus a bounded settle tail, or the safety cap.
    const TbBool settled = thing_exists(imp_prison_drag_enemy)
        && (imp_prison_drag_enemy->active_state == CrSt_CreatureInPrison);
    if (settled && imp_prison_drag_countdown < 0)
        imp_prison_drag_countdown = POST_SETTLE_TICKS;

    if (imp_prison_drag_countdown == 0 || (long)args->times_executed >= PRISON_DRAG_MAX_TICKS)
    {
        FTESTLOG("imp prison drag oracle: dump complete (%ld records, settled = %d)",
            (long)args->times_executed + 1, (int)settled);
        fclose(imp_prison_drag_jsonl);
        imp_prison_drag_jsonl = NULL;
        delete_thing_structure(imp_prison_drag_imp, 0);
        imp_prison_drag_imp = NULL;
        delete_thing_structure(imp_prison_drag_enemy, 0);
        imp_prison_drag_enemy = NULL;
        return FTRs_Go_To_Next_Action;
    }
    if (imp_prison_drag_countdown > 0)
        imp_prison_drag_countdown--;
    return FTRs_Repeat_Current_Action;
}

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
