#include "ftest_imp_convert_oracle.h"

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
#define IMP_CONVERT_ORACLE_VERSION 1

// The scenario is pinned to the M-convert synthetic fixture: a Player0-claimed room (slabs 2..20 x 2..20)
// with the Player0 heart at slab (2,2), and one Player1-CLAIMED tile (with a Player1 heart on it) jutting
// off the east edge at slab (21,10) — the single convert target the heart-connected flood-fill designates.
#define IMP_CONVERT_ORACLE_LEVEL 9012
#define TILE_SLB_X 21
#define TILE_SLB_Y 10

// The imp spawns inside the Player0 room (slab (10,10), centre subtile (31,31)) and must route east to the
// enemy tile — a short, deterministic walk over owned land.
#define SPAWN_STL_X 31
#define SPAWN_STL_Y 31

// Spawn the imp only AFTER game turn 128, for the same reason as the dig/claim oracles: the shared digger
// stack rebuilds at most once per 128 turns and dungeon->digger_stack_update_turn is 0 at load, so an imp
// spawned earlier would RANDOM-WANDER (the RNG toke/wander fallback keeper-rx defers) until the gate opens.
// Spawning past turn 128 means the imp's first idle tick floods the stack and it self-assigns the convert
// immediately.
#define SPAWN_DELAY_TURNS 130

// Stop once the tile is claimed BACK for Player0 — the convert job's own neutralise
// (CrInst_DESTROY_AREA -> neutralise_enemy_block, SlbT_CLAIMED/PLAYER1 -> SlbT_PATH/neutral) falls straight
// through into claiming the freed path (CrInst_PRETTY_PATH, exactly as imp_converts_dungeon does), so this
// terminal observable captures BOTH halves in one connected, fully-deterministic run: no RNG is read once
// the imp is already standing on its one and only target, so there is no random-wander window to avoid by
// stopping earlier (unlike the single-hop claim/dig oracles, which stop the instant their one job finishes).
// One tick later the imp truly has no work left (the flood-fill has nothing else to stage) and begins to
// RANDOM-WANDER, so dumping past this record would capture a non-reproducible tail.
#define POST_CONVERT_TICKS 0
// Safety cap: the imp waits out the 128-turn gate, walks (~10 turns), wears the CLAIMED slab's 5 hit points
// down with DESTROY_AREA (5 fires x ~8 turns) and then claims the freed path with one PRETTY_PATH fire
// (~8 turns). A few hundred turns is a generous ceiling a stuck scenario cannot exceed.
#define CONVERT_MAX_TICKS 600

static FILE* imp_convert_jsonl = NULL;
static struct Thing* imp_convert_imp = NULL;
static int imp_convert_countdown = -1; // -1 until the tile is reclaimed; then counts POST_CONVERT_TICKS down.

// forward declaration
FTestActionResult ftest_imp_convert_oracle_action__run(struct FTestActionArgs* const args);

TbBool ftest_imp_convert_oracle_init()
{
    // One action, delayed past the 128-turn digger-stack gate (see SPAWN_DELAY_TURNS) so the imp self-assigns
    // on its first idle tick with no random-wander window; then the per-tick driver spawns/dumps.
    ftest_append_action(ftest_imp_convert_oracle_action__run, SPAWN_DELAY_TURNS, NULL);
    return true;
}

// Resolve the dump directory: $KEEPERFX_ORACLE_OUT, else the CWD (mirrors the other oracle ftests so they
// share the capture-script convention).
static const char* imp_convert_out_dir(void)
{
    const char* dir = getenv("KEEPERFX_ORACLE_OUT");
    if (dir == NULL || dir[0] == '\0')
        return ".";
    mkdir(dir, 0755); // best-effort; ignore EEXIST
    return dir;
}

// One per-tick ground-truth record. Written BEFORE this turn's update_thing runs, so record 0 is the posed
// initial state (imp at spawn, no work claimed yet) and each later record is the state left by one game
// update. Fields are read straight from the C so the keeper-rx diff is auditable:
//   pos_x/pos_y          imp->mappos.{x,y}.val  (fixed-point, 256/subtile)
//   active_state         imp->active_state      (CrSt_* id; keeper-rx CreatureStateId equals it)
//   instance_id          cctrl->instance_id     (CrInst_* id; 39 == CrInst_DESTROY_AREA, 31 == CrInst_PRETTY_PATH)
//   inst_turn/action     cctrl->inst_turn / inst_action_turns  (each fire lands when they meet)
//   block_kind           slb->kind              (SlbT_CLAIMED=11 enemy-owned, then SlbT_PATH=10 neutral, then
//                                                 SlbT_CLAIMED=11 Player0-owned once reclaimed)
//   block_owner          slabmap_owner(slb)     (PLAYER1 -> PLAYER_NEUTRAL -> PLAYER0)
//   block_health         slb->health            (the enemy slab's remaining hit-points, worn down by DESTROY_AREA)
//   digger_stack_length   dungeon->digger_stack_length  (the shared claim/convert task-list occupancy)
//   total_area_p0/p1      get_dungeon(PLAYER0/1)->total_area  (P0's rises on the follow-on claim; P1's dips
//                                                              on the neutralise, decrease_dungeon_area)
static void imp_convert_dump(FILE* f, struct Thing* imp)
{
    struct CreatureControl* cctrl = creature_control_get_from_thing(imp);
    struct SlabMap* slb = get_slabmap_block(TILE_SLB_X, TILE_SLB_Y);
    struct Dungeon* dungeon0 = get_dungeon(PLAYER0);
    struct Dungeon* dungeon1 = get_dungeon(PLAYER1);
    fprintf(f,
        "{\"v\":%d,\"type\":\"imp_convert\",\"tick\":%ld,"
        "\"pos_x\":%ld,\"pos_y\":%ld,\"active_state\":%d,"
        "\"instance_id\":%d,\"inst_turn\":%d,\"inst_action_turns\":%d,"
        "\"block_kind\":%d,\"block_owner\":%d,\"block_health\":%d,"
        "\"digger_stack_length\":%lu,\"total_area_p0\":%ld,\"total_area_p1\":%ld}\n",
        IMP_CONVERT_ORACLE_VERSION, (long)get_gameturn(),
        (long)imp->mappos.x.val, (long)imp->mappos.y.val, (int)imp->active_state,
        (int)cctrl->instance_id, (int)cctrl->inst_turn, (int)cctrl->inst_action_turns,
        (int)slb->kind, (int)slabmap_owner(slb), (int)slb->health,
        (unsigned long)dungeon0->digger_stack_length, (long)dungeon0->total_area, (long)dungeon1->total_area);
}

// Driver: on the first turn reveal the map and spawn the imp; every turn dump; stop once the tile is
// reclaimed for Player0 (plus a short tail) or the safety cap trips.
FTestActionResult ftest_imp_convert_oracle_action__run(struct FTestActionArgs* const args)
{
    if (args->times_executed == 0)
    {
        const long level = (long)get_loaded_level_number();
        if (level != IMP_CONVERT_ORACLE_LEVEL)
        {
            FTEST_FAIL_TEST("imp convert oracle: expected level %d (M-convert), got %ld — no convert tile",
                IMP_CONVERT_ORACLE_LEVEL, level);
            return FTRs_Go_To_Next_Action;
        }

        // Reveal so the flood-fill / check_place_to_convert see the tile (mirrors the other oracles).
        ftest_util_reveal_map(PLAYER0);

        const MapCoord sx = subtile_coord_center(SPAWN_STL_X);
        const MapCoord sy = subtile_coord_center(SPAWN_STL_Y);
        imp_convert_imp = ftest_util_create_creature(sx, sy, PLAYER0, 1, get_players_special_digger_model(PLAYER0));
        if (thing_is_invalid(imp_convert_imp))
        {
            FTEST_FAIL_TEST("imp convert oracle: could not spawn the imp at subtile (%d,%d)", SPAWN_STL_X, SPAWN_STL_Y);
            return FTRs_Go_To_Next_Action;
        }
        // Settle the floor under the spawn tile before the game loop takes over (mirrors the movement oracle).
        move_thing_in_map(imp_convert_imp, &imp_convert_imp->mappos);

        // Guard: the target must load as a Player1-CLAIMED tile (the flood-fill's convert target), else the
        // fixture is wrong and the dump would be meaningless.
        struct SlabMap* target = get_slabmap_block(TILE_SLB_X, TILE_SLB_Y);
        if ((target->kind != SlbT_CLAIMED) || (slabmap_owner(target) != PLAYER1))
        {
            FTEST_FAIL_TEST("imp convert oracle: target slab (%d,%d) is kind %d owner %d, expected SlbT_CLAIMED/PLAYER1",
                TILE_SLB_X, TILE_SLB_Y, (int)target->kind, (int)slabmap_owner(target));
            delete_thing_structure(imp_convert_imp, 0);
            imp_convert_imp = NULL;
            return FTRs_Go_To_Next_Action;
        }

        char path[512];
        snprintf(path, sizeof(path), "%s/oracle_imp_convert.jsonl", imp_convert_out_dir());
        imp_convert_jsonl = fopen(path, "w");
        if (imp_convert_jsonl == NULL)
        {
            FTEST_FRAMEWORK_ABORT("imp convert oracle: failed to open '%s'", path);
            delete_thing_structure(imp_convert_imp, 0);
            imp_convert_imp = NULL;
            return FTRs_Go_To_Next_Action;
        }
        FTESTLOG("imp convert oracle: writing '%s'", path);

        const char* prov_build  = getenv("KEEPERFX_ORACLE_BUILD");
        fprintf(imp_convert_jsonl,
            "{\"v\":%d,\"type\":\"meta\",\"probe\":\"imp_convert\",\"level\":%d,\"campaign\":\"classic\","
            "\"tile_slb_x\":%d,\"tile_slb_y\":%d,\"spawn_stl_x\":%d,\"spawn_stl_y\":%d,"
            "\"engine\":\"keeperfx-oracle-dumps\",\"version\":\"%s\",\"build\":\"%s\"}\n",
            IMP_CONVERT_ORACLE_VERSION, IMP_CONVERT_ORACLE_LEVEL, TILE_SLB_X, TILE_SLB_Y,
            SPAWN_STL_X, SPAWN_STL_Y, VER_STRING,
            prov_build ? prov_build : "");
    }

    imp_convert_dump(imp_convert_jsonl, imp_convert_imp);

    // Stop once the tile is reclaimed for Player0 (its slab is CLAIMED again, owned by PLAYER0) plus a short
    // tail, or the safety cap.
    struct SlabMap* slb = get_slabmap_block(TILE_SLB_X, TILE_SLB_Y);
    if ((slb->kind == SlbT_CLAIMED) && (slabmap_owner(slb) == PLAYER0) && (imp_convert_countdown < 0))
        imp_convert_countdown = POST_CONVERT_TICKS;

    if (imp_convert_countdown == 0 || (long)args->times_executed >= CONVERT_MAX_TICKS)
    {
        FTESTLOG("imp convert oracle: dump complete (%ld records, final block kind %d owner %d)",
            (long)args->times_executed + 1, (int)slb->kind, (int)slabmap_owner(slb));
        fclose(imp_convert_jsonl);
        imp_convert_jsonl = NULL;
        delete_thing_structure(imp_convert_imp, 0);
        imp_convert_imp = NULL;
        return FTRs_Go_To_Next_Action;
    }
    if (imp_convert_countdown > 0)
        imp_convert_countdown--;
    return FTRs_Repeat_Current_Action;
}

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
