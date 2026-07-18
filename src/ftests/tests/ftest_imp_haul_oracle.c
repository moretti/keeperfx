#include "ftest_imp_haul_oracle.h"

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
#include "../../creature_instances.h"
#include "../../map_data.h"
#include "../../slab_data.h"
#include "../../dungeon_data.h"
#include "../../room_data.h"
#include "../../room_treasure.h"
#include "../../config_terrain.h"
#include "../../player_data.h"
#include "../../player_instances.h"
#include "../../player_computer.h"
#include "../../ver_defs.h"

#include "../../post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif

// Oracle dump format version — bump on any layout/meaning change (keeper-rx/docs/oracle/dump-format.md).
#define IMP_HAUL_ORACLE_VERSION 1

// The scenario is pinned to the M-haul synthetic fixture: M-mine's shape (SyntheticMaps.MHaul, slab
// (21, corridorY=Size/2=42)) plus a 2-slab Player0 Treasury inside the claimed room (slabs (10,10)-(11,10)).
#define IMP_HAUL_ORACLE_LEVEL 9016
#define TILE_SLB_X 21
#define TILE_SLB_Y 42
#define TREASURY_SLB_X0 10
#define TREASURY_SLB_X1 11
#define TREASURY_SLB_Y 10

// The imp spawns a few subtiles west of the seam, inside the left claimed room, and must route east to a
// dig spot beside it — identical to the imp-mine oracle's spawn point (the same fixture shape).
#define SPAWN_STL_X 60
#define SPAWN_STL_Y 127

// Spawn the imp only AFTER game turn 128 (see ftest_imp_mine_oracle.c's identical rationale): the shared
// digger stack rebuilds at most once per 128 turns, so spawning past the gate means the imp self-assigns on
// its very first idle tick with no RNG-driven wander to reproduce.
#define SPAWN_DELAY_TURNS 130

// Keep dumping a long DETERMINISTIC tail past the mine-out tick: unlike the plain mine-gold oracle, S5's
// treasury changes what happens after mine-out — once the seam is exhausted the imp still needs to bank
// whatever it is carrying AND sweep up any loose overflow pile(s) it left behind earlier via the shared-stack
// gold-pickup seam (#8/#13), both of which take further ticks to walk/dispatch/settle. Generous but bounded.
#define POST_MINE_TICKS 300
// Safety cap: the imp waits out the 128-turn stack-rebuild gate, walks (~10-20 turns), swings twenty times on
// the neutral GOLD pool (health 20, damage 1 => 20 hits, ~9 turns each, ~180 turns), diverting to bank at
// least once along the way, then sweeps the tail — comfortably under 1200.
#define HAUL_MAX_TICKS 1200

static FILE* imp_haul_jsonl = NULL;
static struct Thing* imp_haul_imp = NULL;
static int imp_haul_countdown = -1; // -1 until the seam is mined; then counts POST_MINE_TICKS down to 0.

// forward declaration
FTestActionResult ftest_imp_haul_oracle_action__run(struct FTestActionArgs* const args);

TbBool ftest_imp_haul_oracle_init()
{
    // One action, delayed past the 128-turn digger-stack gate (see SPAWN_DELAY_TURNS) so the imp
    // self-assigns on its first idle tick with no random-wander window; then the per-tick driver
    // spawns/tags/dumps.
    ftest_append_action(ftest_imp_haul_oracle_action__run, SPAWN_DELAY_TURNS, NULL);
    return true;
}

// Resolve the dump directory: $KEEPERFX_ORACLE_OUT, else the CWD (mirrors the other oracle ftests).
static const char* imp_haul_out_dir(void)
{
    const char* dir = getenv("KEEPERFX_ORACLE_OUT");
    if (dir == NULL || dir[0] == '\0')
        return ".";
    mkdir(dir, 0755); // best-effort; ignore EEXIST
    return dir;
}

// Appends one gold-hoard object's JSON, comma-prefixed unless it is the array's first entry, for every
// treasury slab that currently holds a hoard (find_gold_hoarde_at — room_treasure.c's spelling, the same one
// count_gold_hoardes_in_room uses). Identity is (owner, position), never a raw thing index (ADR-0021).
static void imp_haul_dump_hoards(FILE* f)
{
    fprintf(f, "\"hoards\":[");
    TbBool first = true;
    for (MapSlabCoord slb_x = TREASURY_SLB_X0; slb_x <= TREASURY_SLB_X1; slb_x++)
    {
        MapSubtlCoord stl_x = slab_subtile_center(slb_x);
        MapSubtlCoord stl_y = slab_subtile_center(TREASURY_SLB_Y);
        struct Thing* gldtng = find_gold_hoarde_at(stl_x, stl_y);
        if (thing_is_invalid(gldtng))
            continue;
        fprintf(f,
            "%s{\"owner\":%d,\"stl_x\":%d,\"stl_y\":%d,\"gold_stored\":%ld,\"model\":%d}",
            first ? "" : ",",
            (int)gldtng->owner, (int)stl_x, (int)stl_y, (long)gldtng->valuable.gold_stored, (int)gldtng->model);
        first = false;
    }
    fprintf(f, "]");
}

// One per-tick ground-truth record. Written BEFORE this turn's update_thing runs, so record 0 is the posed
// initial state and each later record is the state left by one game update. Fields:
//   pos_x/pos_y                  imp->mappos.{x,y}.val
//   active_state/continue_state  imp->active_state / imp->continue_state (CrSt_* ids)
//   instance_id/inst_turn/inst_action_turns   cctrl fields (the dig-swing timer)
//   block_health/block_kind      the GOLD seam's slb->health / slb->kind
//   task_count                   dungeon->task_count
//   gold_carried                 imp->creature.gold_carried
//   random_seed                  imp->random_seed (ADR-0028's frozen draw, setup_head_for_empty_treasure_space)
//   pile_*                       the loose ObjMdl_Goldl pile at the imp's own stand subtile, if any
//   room_*                       the Treasury room's identity/capacity fields
//   total_money_owned            dungeon->total_money_owned
//   hoards                       every gold-hoard object sitting on one of the Treasury's own slabs
static void imp_haul_dump(FILE* f, struct Thing* imp)
{
    struct CreatureControl* cctrl = creature_control_get_from_thing(imp);
    struct SlabMap* slb = get_slabmap_block(TILE_SLB_X, TILE_SLB_Y);
    struct Dungeon* dungeon = get_dungeon(PLAYER0);

    struct Thing* pile = smallest_gold_pile_at_xy(imp->mappos.x.stl.num, imp->mappos.y.stl.num);
    const int pile_exists = !thing_is_invalid(pile);
    const long pile_gold_stored = pile_exists ? pile->valuable.gold_stored : 0;
    const int pile_sprite_size = pile_exists ? pile->sprite_size : 0;
    const int pile_owner = pile_exists ? (int)pile->owner : -1;

    struct Room* room = subtile_room_get(slab_subtile_center(TREASURY_SLB_X0), slab_subtile_center(TREASURY_SLB_Y));
    const TbBool room_exists_flag = !room_is_invalid(room);
    const int room_owner = room_exists_flag ? (int)room->owner : -1;
    const int room_kind = room_exists_flag ? (int)room->kind : 0;
    const int room_slabs_count = room_exists_flag ? (int)room->slabs_count : 0;
    const int room_total_capacity = room_exists_flag ? (int)room->total_capacity : 0;
    const int room_used_capacity = room_exists_flag ? (int)room->used_capacity : 0;
    const long room_capacity_used_for_storage = room_exists_flag ? (long)room->capacity_used_for_storage : 0;

    fprintf(f,
        "{\"v\":%d,\"type\":\"imp_haul\",\"tick\":%ld,"
        "\"pos_x\":%ld,\"pos_y\":%ld,\"active_state\":%d,\"continue_state\":%d,"
        "\"instance_id\":%d,\"inst_turn\":%d,\"inst_action_turns\":%d,"
        "\"block_health\":%d,\"block_kind\":%d,\"task_count\":%d,"
        "\"gold_carried\":%ld,\"random_seed\":%lu,"
        "\"pile_exists\":%d,\"pile_gold_stored\":%ld,\"pile_sprite_size\":%d,\"pile_owner\":%d,"
        "\"room_exists\":%d,\"room_owner\":%d,\"room_kind\":%d,\"room_slabs_count\":%d,"
        "\"room_total_capacity\":%d,\"room_used_capacity\":%d,\"room_capacity_used_for_storage\":%ld,"
        "\"total_money_owned\":%ld,",
        IMP_HAUL_ORACLE_VERSION, (long)get_gameturn(),
        (long)imp->mappos.x.val, (long)imp->mappos.y.val, (int)imp->active_state, (int)imp->continue_state,
        (int)cctrl->instance_id, (int)cctrl->inst_turn, (int)cctrl->inst_action_turns,
        (int)slb->health, (int)slb->kind, (int)dungeon->task_count,
        (long)imp->creature.gold_carried, (unsigned long)imp->random_seed,
        pile_exists, pile_gold_stored, pile_sprite_size, pile_owner,
        room_exists_flag, room_owner, room_kind, room_slabs_count,
        room_total_capacity, room_used_capacity, room_capacity_used_for_storage,
        (long)dungeon->total_money_owned);
    imp_haul_dump_hoards(f);
    fprintf(f, "}\n");
}

// Driver: on the first turn reveal the map, spawn the imp, tag the gold seam with the real player action,
// and open the JSONL; every turn dump; stop once the seam is mined AND the post-mine sweep/bank tail
// completes, or the safety cap trips.
FTestActionResult ftest_imp_haul_oracle_action__run(struct FTestActionArgs* const args)
{
    if (args->times_executed == 0)
    {
        const long level = (long)get_loaded_level_number();
        if (level != IMP_HAUL_ORACLE_LEVEL)
        {
            FTEST_FAIL_TEST("imp haul oracle: expected level %d (M-haul), got %ld — no gold seam/treasury to haul between",
                IMP_HAUL_ORACLE_LEVEL, level);
            return FTRs_Go_To_Next_Action;
        }

        // Reveal so the imp's valid_dig_position / the designation see the tile (mirrors the other oracles).
        ftest_util_reveal_map(PLAYER0);

        const MapCoord sx = subtile_coord_center(SPAWN_STL_X);
        const MapCoord sy = subtile_coord_center(SPAWN_STL_Y);
        imp_haul_imp = ftest_util_create_creature(sx, sy, PLAYER0, 1, get_players_special_digger_model(PLAYER0));
        if (thing_is_invalid(imp_haul_imp))
        {
            FTEST_FAIL_TEST("imp haul oracle: could not spawn the imp at subtile (%d,%d)", SPAWN_STL_X, SPAWN_STL_Y);
            return FTRs_Go_To_Next_Action;
        }
        // Settle the floor under the spawn tile before the game loop takes over (mirrors the movement oracle).
        move_thing_in_map(imp_haul_imp, &imp_haul_imp->mappos);

        // Tag the gold seam for mining with the real player action (what the dig cursor issues) — the same
        // GA_MarkDig the mine oracle uses: tag_blocks_for_digging_in_area picks SDDigTask_MineGold on its own
        // because the slab carries SlbAtFlg_Valuable, no separate "mark mine" action exists.
        game_action(PLAYER0, GA_MarkDig, 0,
            slab_subtile_center(TILE_SLB_X), slab_subtile_center(TILE_SLB_Y), 1, 1);
        struct Dungeon* dungeon = get_dungeon(PLAYER0);
        if (dungeon->task_count < 1)
        {
            FTEST_FAIL_TEST("imp haul oracle: GA_MarkDig did not register a mine task at slab (%d,%d)",
                TILE_SLB_X, TILE_SLB_Y);
            delete_thing_structure(imp_haul_imp, 0);
            imp_haul_imp = NULL;
            return FTRs_Go_To_Next_Action;
        }

        // The Treasury room is baked into the fixture's own slab kinds (M-haul's .slb), assembled at level
        // load by the unmodified initialise_map_rooms/reinitialise_map_rooms — no room-placement action is
        // needed here, exactly like the room-state oracle's M-rooms fixture.
        struct Room* room = subtile_room_get(slab_subtile_center(TREASURY_SLB_X0), slab_subtile_center(TREASURY_SLB_Y));
        if (room_is_invalid(room) || (room->owner != PLAYER0) || !room_role_matches(room->kind, RoRoF_GoldStorage))
        {
            FTEST_FAIL_TEST("imp haul oracle: no Player0 GoldStorage-role room found at the fixture's treasury slabs");
            delete_thing_structure(imp_haul_imp, 0);
            imp_haul_imp = NULL;
            return FTRs_Go_To_Next_Action;
        }

        char path[512];
        snprintf(path, sizeof(path), "%s/oracle_imp_haul.jsonl", imp_haul_out_dir());
        imp_haul_jsonl = fopen(path, "w");
        if (imp_haul_jsonl == NULL)
        {
            FTEST_FRAMEWORK_ABORT("imp haul oracle: failed to open '%s'", path);
            delete_thing_structure(imp_haul_imp, 0);
            imp_haul_imp = NULL;
            return FTRs_Go_To_Next_Action;
        }
        FTESTLOG("imp haul oracle: writing '%s'", path);

        const char* prov_build  = getenv("KEEPERFX_ORACLE_BUILD");
        fprintf(imp_haul_jsonl,
            "{\"v\":%d,\"type\":\"meta\",\"probe\":\"imp_haul\",\"level\":%d,\"campaign\":\"classic\","
            "\"tile_slb_x\":%d,\"tile_slb_y\":%d,\"spawn_stl_x\":%d,\"spawn_stl_y\":%d,"
            "\"treasury_slb_x0\":%d,\"treasury_slb_x1\":%d,\"treasury_slb_y\":%d,"
            "\"engine\":\"keeperfx-oracle-dumps\",\"version\":\"%s\",\"build\":\"%s\"}\n",
            IMP_HAUL_ORACLE_VERSION, IMP_HAUL_ORACLE_LEVEL, TILE_SLB_X, TILE_SLB_Y,
            SPAWN_STL_X, SPAWN_STL_Y, TREASURY_SLB_X0, TREASURY_SLB_X1, TREASURY_SLB_Y,
            VER_STRING, prov_build ? prov_build : "");
    }

    imp_haul_dump(imp_haul_jsonl, imp_haul_imp);

    // Stop once the seam is mined out (its slab flips off SlbT_GOLD) plus a long deterministic tail (the
    // bank-and-sweep chain), or the safety cap.
    struct SlabMap* slb = get_slabmap_block(TILE_SLB_X, TILE_SLB_Y);
    if (slb->kind != SlbT_GOLD && imp_haul_countdown < 0)
        imp_haul_countdown = POST_MINE_TICKS;

    if (imp_haul_countdown == 0 || (long)args->times_executed >= HAUL_MAX_TICKS)
    {
        FTESTLOG("imp haul oracle: dump complete (%ld records, final block kind %d)",
            (long)args->times_executed + 1, (int)slb->kind);
        fclose(imp_haul_jsonl);
        imp_haul_jsonl = NULL;
        delete_thing_structure(imp_haul_imp, 0);
        imp_haul_imp = NULL;
        return FTRs_Go_To_Next_Action;
    }
    if (imp_haul_countdown > 0)
        imp_haul_countdown--;
    return FTRs_Repeat_Current_Action;
}

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
