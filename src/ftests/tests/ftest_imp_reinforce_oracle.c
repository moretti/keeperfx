#include "ftest_imp_reinforce_oracle.h"

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
#define IMP_REINFORCE_ORACLE_VERSION 1

// The scenario is pinned to the M-reinforce synthetic fixture: a Player0-claimed room (slabs 2..20 x 2..20)
// with the dungeon heart at slab (2,2) and one reinforceable EARTH wall on the east edge at slab (21,10) — the
// single reinforce target the heart-connected flood-fill stages (no free path, no dig designation).
#define IMP_REINFORCE_ORACLE_LEVEL 9007
#define WALL_SLB_X 21
#define WALL_SLB_Y 10

// The imp spawns inside the claimed room (slab (10,10), centre subtile (31,31)) and must route east to the
// floor tile beside the earth wall — a short, deterministic walk over owned land.
#define SPAWN_STL_X 31
#define SPAWN_STL_Y 31

// Spawn the imp only AFTER game turn 128, for the same reason as the dig/claim oracles: the shared digger
// stack rebuilds at most once per 128 turns and dungeon->digger_stack_update_turn is 0 at load, so an imp
// spawned earlier would RANDOM-WANDER (the RNG toke/wander fallback keeper-rx defers) until the gate opens.
#define SPAWN_DELAY_TURNS 130

// Stop on the very tick the wall is fortified (its slab leaves SlbT_EARTH). One tick later the imp has no
// reinforce left and begins to RANDOM-WANDER (the deferred RNG fallback), so dumping past the fortify would
// capture a non-reproducible tail. The reinforce loop's terminal observable is exactly this fortified record.
#define POST_FORTIFY_TICKS 0
// Safety cap: the imp waits out the 128-turn gate, one throttle tick, walks (~10 turns), then dances REINFORCE
// 27 times (~9 turns each) to fortify. A few hundred turns is a generous ceiling a stuck scenario cannot
// exceed.
#define REINFORCE_MAX_TICKS 600

static FILE* imp_reinforce_jsonl = NULL;
static struct Thing* imp_reinforce_imp = NULL;
static int imp_reinforce_countdown = -1; // -1 until the wall is fortified; then counts POST_FORTIFY_TICKS to 0.

// forward declaration
FTestActionResult ftest_imp_reinforce_oracle_action__run(struct FTestActionArgs* const args);

TbBool ftest_imp_reinforce_oracle_init()
{
    // One action, delayed past the 128-turn digger-stack gate (see SPAWN_DELAY_TURNS) so the imp self-assigns
    // on its first idle tick with no random-wander window; then the per-tick driver spawns/dumps.
    ftest_append_action(ftest_imp_reinforce_oracle_action__run, SPAWN_DELAY_TURNS, NULL);
    return true;
}

// Resolve the dump directory: $KEEPERFX_ORACLE_OUT, else the CWD (mirrors the other oracle ftests so they
// share the capture-script convention).
static const char* imp_reinforce_out_dir(void)
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
//   pos_x/pos_y                imp->mappos.{x,y}.val  (fixed-point, 256/subtile)
//   active_state               imp->active_state      (CrSt_* id; keeper-rx CreatureStateId equals it)
//   instance_id                cctrl->instance_id     (CrInst_* id; 35 == CrInst_REINFORCE while reinforcing)
//   inst_turn/action           cctrl->inst_turn / inst_action_turns  (the dance fires when they meet)
//   consecutive_reinforcements cctrl->digger.consecutive_reinforcements  (0..26 charging, resets on fortify)
//   block_kind                 slb->kind              (SlbT_EARTH=2 until fortified, SlbT_WALLTORCH=5 after)
//   block_owner                slabmap_owner(slb)     (PLAYER_NEUTRAL until fortified, PLAYER0 after)
static void imp_reinforce_dump(FILE* f, struct Thing* imp)
{
    struct CreatureControl* cctrl = creature_control_get_from_thing(imp);
    struct SlabMap* slb = get_slabmap_block(WALL_SLB_X, WALL_SLB_Y);
    fprintf(f,
        "{\"v\":%d,\"type\":\"imp_reinforce\",\"tick\":%ld,"
        "\"pos_x\":%ld,\"pos_y\":%ld,\"active_state\":%d,"
        "\"instance_id\":%d,\"inst_turn\":%d,\"inst_action_turns\":%d,"
        "\"consecutive_reinforcements\":%d,\"block_kind\":%d,\"block_owner\":%d}\n",
        IMP_REINFORCE_ORACLE_VERSION, (long)get_gameturn(),
        (long)imp->mappos.x.val, (long)imp->mappos.y.val, (int)imp->active_state,
        (int)cctrl->instance_id, (int)cctrl->inst_turn, (int)cctrl->inst_action_turns,
        (int)cctrl->digger.consecutive_reinforcements, (int)slb->kind, (int)slabmap_owner(slb));
}

// Driver: on the first turn reveal the map and spawn the imp; every turn dump; stop once the wall is fortified
// (plus a short tail) or the safety cap trips.
FTestActionResult ftest_imp_reinforce_oracle_action__run(struct FTestActionArgs* const args)
{
    if (args->times_executed == 0)
    {
        const long level = (long)get_loaded_level_number();
        if (level != IMP_REINFORCE_ORACLE_LEVEL)
        {
            FTEST_FAIL_TEST("imp reinforce oracle: expected level %d (M-reinforce), got %ld — no reinforce wall",
                IMP_REINFORCE_ORACLE_LEVEL, level);
            return FTRs_Go_To_Next_Action;
        }

        // Reveal so the flood-fill / check_place_to_reinforce see the wall (mirrors the other oracles).
        ftest_util_reveal_map(PLAYER0);

        const MapCoord sx = subtile_coord_center(SPAWN_STL_X);
        const MapCoord sy = subtile_coord_center(SPAWN_STL_Y);
        imp_reinforce_imp = ftest_util_create_creature(sx, sy, PLAYER0, 1, get_players_special_digger_model(PLAYER0));
        if (thing_is_invalid(imp_reinforce_imp))
        {
            FTEST_FAIL_TEST("imp reinforce oracle: could not spawn the imp at subtile (%d,%d)", SPAWN_STL_X, SPAWN_STL_Y);
            return FTRs_Go_To_Next_Action;
        }
        // Settle the floor under the spawn tile before the game loop takes over (mirrors the movement oracle).
        move_thing_in_map(imp_reinforce_imp, &imp_reinforce_imp->mappos);

        // Guard: the target must load as a friable earth wall (the flood-fill's reinforce target), else the
        // fixture is wrong and the dump would be meaningless.
        struct SlabMap* target = get_slabmap_block(WALL_SLB_X, WALL_SLB_Y);
        if (target->kind != SlbT_EARTH)
        {
            FTEST_FAIL_TEST("imp reinforce oracle: target slab (%d,%d) is kind %d, expected SlbT_EARTH",
                WALL_SLB_X, WALL_SLB_Y, (int)target->kind);
            delete_thing_structure(imp_reinforce_imp, 0);
            imp_reinforce_imp = NULL;
            return FTRs_Go_To_Next_Action;
        }

        char path[512];
        snprintf(path, sizeof(path), "%s/oracle_imp_reinforce.jsonl", imp_reinforce_out_dir());
        imp_reinforce_jsonl = fopen(path, "w");
        if (imp_reinforce_jsonl == NULL)
        {
            FTEST_FRAMEWORK_ABORT("imp reinforce oracle: failed to open '%s'", path);
            delete_thing_structure(imp_reinforce_imp, 0);
            imp_reinforce_imp = NULL;
            return FTRs_Go_To_Next_Action;
        }
        FTESTLOG("imp reinforce oracle: writing '%s'", path);

        const char* prov_binary = getenv("KEEPERFX_ORACLE_BINARY");
        const char* prov_build  = getenv("KEEPERFX_ORACLE_BUILD");
        fprintf(imp_reinforce_jsonl,
            "{\"v\":%d,\"type\":\"meta\",\"probe\":\"imp_reinforce\",\"level\":%d,\"campaign\":\"classic\","
            "\"wall_slb_x\":%d,\"wall_slb_y\":%d,\"spawn_stl_x\":%d,\"spawn_stl_y\":%d,"
            "\"engine\":\"keeperfx-oracle-dumps\",\"version\":\"%s\",\"binary\":\"%s\",\"build\":\"%s\"}\n",
            IMP_REINFORCE_ORACLE_VERSION, IMP_REINFORCE_ORACLE_LEVEL, WALL_SLB_X, WALL_SLB_Y,
            SPAWN_STL_X, SPAWN_STL_Y, VER_STRING,
            prov_binary ? prov_binary : "", prov_build ? prov_build : "");
    }

    imp_reinforce_dump(imp_reinforce_jsonl, imp_reinforce_imp);

    // Stop once the wall is fortified (its slab leaves SlbT_EARTH) plus a short tail, or the safety cap.
    struct SlabMap* slb = get_slabmap_block(WALL_SLB_X, WALL_SLB_Y);
    if (slb->kind != SlbT_EARTH && imp_reinforce_countdown < 0)
        imp_reinforce_countdown = POST_FORTIFY_TICKS;

    if (imp_reinforce_countdown == 0 || (long)args->times_executed >= REINFORCE_MAX_TICKS)
    {
        FTESTLOG("imp reinforce oracle: dump complete (%ld records, final block kind %d)",
            (long)args->times_executed + 1, (int)slb->kind);
        fclose(imp_reinforce_jsonl);
        imp_reinforce_jsonl = NULL;
        delete_thing_structure(imp_reinforce_imp, 0);
        imp_reinforce_imp = NULL;
        return FTRs_Go_To_Next_Action;
    }
    if (imp_reinforce_countdown > 0)
        imp_reinforce_countdown--;
    return FTRs_Repeat_Current_Action;
}

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
