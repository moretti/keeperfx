#include "ftest_imp_mine_oracle.h"

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
#include "../../player_data.h"
#include "../../player_instances.h"
#include "../../player_computer.h"
#include "../../ver_defs.h"

#include "../../post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif

// Oracle dump format version — bump on any layout/meaning change (keeper-rx/docs/oracle/dump-format.md).
#define IMP_MINE_ORACLE_VERSION 1

// The scenario is pinned to the M-mine synthetic fixture: M-full's shape with the one diggable tile
// re-kinded to a neutral-owned GOLD seam (SyntheticMaps.MMine, slab (21, corridorY=Size/2=42)) — same
// corridor, same spawn, as the imp-dig oracle's M-full.
#define IMP_MINE_ORACLE_LEVEL 9013
#define TILE_SLB_X 21
#define TILE_SLB_Y 42

// The imp spawns a few subtiles west of the seam, inside the left claimed room, and must route east to a
// dig spot beside it — identical to the imp-dig oracle's spawn point.
#define SPAWN_STL_X 60
#define SPAWN_STL_Y 127

// Spawn the imp only AFTER game turn 128 (see ftest_imp_dig_oracle.c's identical rationale): the shared
// digger stack rebuilds at most once per 128 turns, so spawning past the gate means the imp self-assigns on
// its very first idle tick with no RNG-driven wander to reproduce.
#define SPAWN_DELAY_TURNS 130

// Keep dumping a short DETERMINISTIC tail past the mine-out tick, to prove the destroying hit's gold is
// never overflow-dropped on its own tick and stays exactly where it landed (design/imp-jobs.md Correction
// 6): the tick after mine-out the imp's active_state flips to CrSt_ImpLastDidJob (already decided the same
// tick as the destroying hit), the tick after that imp_last_did_job finds no more MineGold work and falls
// to set_start_state (CrSt_ImpDoingNothing, re-seeding idle.start_gameturn), and the tick after THAT the
// fresh-idle guard (turn - idle.start_gameturn <= 1) holds it still — all fixed, no RNG draw reached yet.
// One more tick would risk reaching check_out_imp_tokes's THING_RANDOM roll, so this stops short of it.
#define POST_MINE_TICKS 3
// Safety cap: the imp waits out the 128-turn stack-rebuild gate, walks (~10-20 turns), then swings twenty
// times on the neutral GOLD pool (health 20, damage 1 => 20 hits, ~9 turns each) — comfortably under 700.
#define MINE_MAX_TICKS 700

static FILE* imp_mine_jsonl = NULL;
static struct Thing* imp_mine_imp = NULL;
static int imp_mine_countdown = -1; // -1 until the seam is mined; then counts POST_MINE_TICKS down to 0.

// forward declaration
FTestActionResult ftest_imp_mine_oracle_action__run(struct FTestActionArgs* const args);

TbBool ftest_imp_mine_oracle_init()
{
    // One action, delayed past the 128-turn digger-stack gate (see SPAWN_DELAY_TURNS) so the imp
    // self-assigns on its first idle tick with no random-wander window; then the per-tick driver
    // spawns/tags/dumps.
    ftest_append_action(ftest_imp_mine_oracle_action__run, SPAWN_DELAY_TURNS, NULL);
    return true;
}

// Resolve the dump directory: $KEEPERFX_ORACLE_OUT, else the CWD (mirrors the other oracle ftests).
static const char* imp_mine_out_dir(void)
{
    const char* dir = getenv("KEEPERFX_ORACLE_OUT");
    if (dir == NULL || dir[0] == '\0')
        return ".";
    mkdir(dir, 0755); // best-effort; ignore EEXIST
    return dir;
}

// One per-tick ground-truth record. Written BEFORE this turn's update_thing runs, so record 0 is the posed
// initial state (imp at spawn, seam tagged, no work claimed yet) and each later record is the state left by
// one game update. Fields are read straight from the C so the keeper-rx diff is auditable:
//   pos_x/pos_y          imp->mappos.{x,y}.val  (fixed-point, 256/subtile)
//   active_state         imp->active_state      (CrSt_* id; keeper-rx CreatureStateId equals it)
//   instance_id          cctrl->instance_id     (CrInst_* id; 30 == CrInst_DIG while swinging)
//   inst_turn/action     cctrl->inst_turn / inst_action_turns  (the swing fires when they meet)
//   block_health         slb->health            (the seam's remaining hit-points)
//   block_kind           slb->kind              (SlbT_GOLD=1 while standing, SlbT_PATH=10 once mined)
//   task_count           dungeon->task_count    (the designation list occupancy)
//   gold_carried         imp->creature.gold_carried  (the imp's running mined-gold total)
//   pile_exists          1 iff a loose ObjMdl_Goldl pile sits at the imp's own stand subtile
//   pile_gold_stored     that pile's valuable.gold_stored (0 if none)
//   pile_sprite_size     that pile's sprite_size (0 if none)
//   pile_owner           that pile's owner (game.neutral_player_num expected; -1 if none)
static void imp_mine_dump(FILE* f, struct Thing* imp)
{
    struct CreatureControl* cctrl = creature_control_get_from_thing(imp);
    struct SlabMap* slb = get_slabmap_block(TILE_SLB_X, TILE_SLB_Y);
    struct Dungeon* dungeon = get_dungeon(PLAYER0);

    struct Thing* pile = smallest_gold_pile_at_xy(imp->mappos.x.stl.num, imp->mappos.y.stl.num);
    const int pile_exists = !thing_is_invalid(pile);
    const long pile_gold_stored = pile_exists ? pile->valuable.gold_stored : 0;
    const int pile_sprite_size = pile_exists ? pile->sprite_size : 0;
    const int pile_owner = pile_exists ? (int)pile->owner : -1;

    fprintf(f,
        "{\"v\":%d,\"type\":\"imp_mine\",\"tick\":%ld,"
        "\"pos_x\":%ld,\"pos_y\":%ld,\"active_state\":%d,"
        "\"instance_id\":%d,\"inst_turn\":%d,\"inst_action_turns\":%d,"
        "\"block_health\":%d,\"block_kind\":%d,\"task_count\":%d,"
        "\"gold_carried\":%ld,"
        "\"pile_exists\":%d,\"pile_gold_stored\":%ld,\"pile_sprite_size\":%d,\"pile_owner\":%d}\n",
        IMP_MINE_ORACLE_VERSION, (long)get_gameturn(),
        (long)imp->mappos.x.val, (long)imp->mappos.y.val, (int)imp->active_state,
        (int)cctrl->instance_id, (int)cctrl->inst_turn, (int)cctrl->inst_action_turns,
        (int)slb->health, (int)slb->kind, (int)dungeon->task_count,
        (long)imp->creature.gold_carried,
        pile_exists, pile_gold_stored, pile_sprite_size, pile_owner);
}

// Driver: on the first turn reveal the map, spawn the imp, tag the gold seam with the real player action,
// and open the JSONL; every turn dump; stop once the seam is mined (plus a short deterministic tail) or the
// safety cap trips.
FTestActionResult ftest_imp_mine_oracle_action__run(struct FTestActionArgs* const args)
{
    if (args->times_executed == 0)
    {
        const long level = (long)get_loaded_level_number();
        if (level != IMP_MINE_ORACLE_LEVEL)
        {
            FTEST_FAIL_TEST("imp mine oracle: expected level %d (M-mine), got %ld — no gold seam to mine",
                IMP_MINE_ORACLE_LEVEL, level);
            return FTRs_Go_To_Next_Action;
        }

        // Reveal so the imp's valid_dig_position / the designation see the tile (mirrors the other oracles).
        ftest_util_reveal_map(PLAYER0);

        const MapCoord sx = subtile_coord_center(SPAWN_STL_X);
        const MapCoord sy = subtile_coord_center(SPAWN_STL_Y);
        imp_mine_imp = ftest_util_create_creature(sx, sy, PLAYER0, 1, get_players_special_digger_model(PLAYER0));
        if (thing_is_invalid(imp_mine_imp))
        {
            FTEST_FAIL_TEST("imp mine oracle: could not spawn the imp at subtile (%d,%d)", SPAWN_STL_X, SPAWN_STL_Y);
            return FTRs_Go_To_Next_Action;
        }
        // Settle the floor under the spawn tile before the game loop takes over (mirrors the movement oracle).
        move_thing_in_map(imp_mine_imp, &imp_mine_imp->mappos);

        // Tag the gold seam for mining with the real player action (what the dig cursor issues) — the same
        // GA_MarkDig the dig oracle uses: tag_blocks_for_digging_in_area picks SDDigTask_MineGold on its own
        // because the slab carries SlbAtFlg_Valuable, no separate "mark mine" action exists.
        game_action(PLAYER0, GA_MarkDig, 0,
            slab_subtile_center(TILE_SLB_X), slab_subtile_center(TILE_SLB_Y), 1, 1);
        struct Dungeon* dungeon = get_dungeon(PLAYER0);
        if (dungeon->task_count < 1)
        {
            FTEST_FAIL_TEST("imp mine oracle: GA_MarkDig did not register a mine task at slab (%d,%d)",
                TILE_SLB_X, TILE_SLB_Y);
            delete_thing_structure(imp_mine_imp, 0);
            imp_mine_imp = NULL;
            return FTRs_Go_To_Next_Action;
        }

        char path[512];
        snprintf(path, sizeof(path), "%s/oracle_imp_mine.jsonl", imp_mine_out_dir());
        imp_mine_jsonl = fopen(path, "w");
        if (imp_mine_jsonl == NULL)
        {
            FTEST_FRAMEWORK_ABORT("imp mine oracle: failed to open '%s'", path);
            delete_thing_structure(imp_mine_imp, 0);
            imp_mine_imp = NULL;
            return FTRs_Go_To_Next_Action;
        }
        FTESTLOG("imp mine oracle: writing '%s'", path);

        const char* prov_build  = getenv("KEEPERFX_ORACLE_BUILD");
        fprintf(imp_mine_jsonl,
            "{\"v\":%d,\"type\":\"meta\",\"probe\":\"imp_mine\",\"level\":%d,\"campaign\":\"classic\","
            "\"tile_slb_x\":%d,\"tile_slb_y\":%d,\"spawn_stl_x\":%d,\"spawn_stl_y\":%d,"
            "\"engine\":\"keeperfx-oracle-dumps\",\"version\":\"%s\",\"build\":\"%s\"}\n",
            IMP_MINE_ORACLE_VERSION, IMP_MINE_ORACLE_LEVEL, TILE_SLB_X, TILE_SLB_Y,
            SPAWN_STL_X, SPAWN_STL_Y, VER_STRING,
            prov_build ? prov_build : "");
    }

    imp_mine_dump(imp_mine_jsonl, imp_mine_imp);

    // Stop once the seam is mined out (its slab flips off SlbT_GOLD) plus a short deterministic tail, or the
    // safety cap.
    struct SlabMap* slb = get_slabmap_block(TILE_SLB_X, TILE_SLB_Y);
    if (slb->kind != SlbT_GOLD && imp_mine_countdown < 0)
        imp_mine_countdown = POST_MINE_TICKS;

    if (imp_mine_countdown == 0 || (long)args->times_executed >= MINE_MAX_TICKS)
    {
        FTESTLOG("imp mine oracle: dump complete (%ld records, final block kind %d)",
            (long)args->times_executed + 1, (int)slb->kind);
        fclose(imp_mine_jsonl);
        imp_mine_jsonl = NULL;
        delete_thing_structure(imp_mine_imp, 0);
        imp_mine_imp = NULL;
        return FTRs_Go_To_Next_Action;
    }
    if (imp_mine_countdown > 0)
        imp_mine_countdown--;
    return FTRs_Repeat_Current_Action;
}

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
