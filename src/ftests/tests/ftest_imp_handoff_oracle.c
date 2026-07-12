#include "ftest_imp_handoff_oracle.h"

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
#define IMP_HANDOFF_ORACLE_VERSION 1

// The scenario is pinned to the M-claim synthetic fixture: a Player0-claimed room (slabs 2..20 x 2..20) with
// the dungeon heart at slab (2,2) and one free PATH tile on the east edge at slab (21,10) — the single claim
// target the heart-connected flood-fill designates.
#define IMP_HANDOFF_ORACLE_LEVEL 9006
#define TILE_SLB_X 21
#define TILE_SLB_Y 10

// The imp spawns inside the claimed room (slab (10,10), centre subtile (31,31)) and must route east to the
// path tile — a short, deterministic walk over owned land.
#define SPAWN_STL_X 31
#define SPAWN_STL_Y 31

// Spawn the imp only AFTER game turn 128, for the same reason as the dig oracle: the shared digger stack
// rebuilds at most once per 128 turns and dungeon->digger_stack_update_turn is 0 at load, so an imp spawned
// earlier would RANDOM-WANDER (the RNG toke/wander fallback keeper-rx defers) until the gate opens. Spawning
// past turn 128 means the imp's first idle tick floods the stack and it self-assigns the claim immediately.
#define SPAWN_DELAY_TURNS 130

// Stop on the very tick the tile is claimed (its slab flips to SlbT_CLAIMED). One tick later the imp has no
// claim left and begins to RANDOM-WANDER (the deferred RNG fallback), so dumping past the claim would capture
// a non-reproducible tail. The claim loop's terminal observable is exactly this claimed record.
#define POST_HANDOFF_TICKS 0
// Safety cap: the imp waits out the 128-turn gate, then walks (~10 turns) and dances PRETTY_PATH once
// (~4 turns to the action fire). A few hundred turns is a generous ceiling a stuck scenario cannot exceed.
#define HANDOFF_MAX_TICKS 600

static FILE* imp_handoff_jsonl = NULL;
static struct Thing* imp_handoff_imp = NULL;
static int imp_handoff_countdown = -1; // -1 until the tile is claimed; then counts POST_HANDOFF_TICKS down to 0.

// forward declaration
FTestActionResult ftest_imp_handoff_oracle_action__run(struct FTestActionArgs* const args);

TbBool ftest_imp_handoff_oracle_init()
{
    // One action, delayed past the 128-turn digger-stack gate (see SPAWN_DELAY_TURNS) so the imp self-assigns
    // on its first idle tick with no random-wander window; then the per-tick driver spawns/dumps.
    ftest_append_action(ftest_imp_handoff_oracle_action__run, SPAWN_DELAY_TURNS, NULL);
    return true;
}

// Resolve the dump directory: $KEEPERFX_ORACLE_OUT, else the CWD (mirrors the other oracle ftests so they
// share the capture-script convention).
static const char* imp_handoff_out_dir(void)
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
//   instance_id          cctrl->instance_id     (CrInst_* id; 31 == CrInst_PRETTY_PATH while claiming)
//   inst_turn/action     cctrl->inst_turn / inst_action_turns  (the claim fires when they meet)
//   block_kind           slb->kind              (SlbT_PATH=10 until claimed, SlbT_CLAIMED once claimed)
//   block_owner          slabmap_owner(slb)     (PLAYER_NEUTRAL until claimed, PLAYER0 once claimed)
//   total_area           dungeon->total_area    (owned-slab counter; bumps +1 on the claim)
static void imp_handoff_dump(FILE* f, struct Thing* imp)
{
    struct CreatureControl* cctrl = creature_control_get_from_thing(imp);
    struct SlabMap* slb = get_slabmap_block(TILE_SLB_X, TILE_SLB_Y);
    struct Dungeon* dungeon = get_dungeon(PLAYER0);
    fprintf(f,
        "{\"v\":%d,\"type\":\"imp_handoff\",\"tick\":%ld,"
        "\"pos_x\":%ld,\"pos_y\":%ld,\"active_state\":%d,"
        "\"instance_id\":%d,\"inst_turn\":%d,\"inst_action_turns\":%d,"
        "\"block_kind\":%d,\"block_owner\":%d,\"total_area\":%ld}\n",
        IMP_HANDOFF_ORACLE_VERSION, (long)get_gameturn(),
        (long)imp->mappos.x.val, (long)imp->mappos.y.val, (int)imp->active_state,
        (int)cctrl->instance_id, (int)cctrl->inst_turn, (int)cctrl->inst_action_turns,
        (int)slb->kind, (int)slabmap_owner(slb), (long)dungeon->total_area);
}

// Driver: on the first turn reveal the map and spawn the imp; every turn dump; stop once the tile is claimed
// (plus a short tail) or the safety cap trips.
FTestActionResult ftest_imp_handoff_oracle_action__run(struct FTestActionArgs* const args)
{
    if (args->times_executed == 0)
    {
        const long level = (long)get_loaded_level_number();
        if (level != IMP_HANDOFF_ORACLE_LEVEL)
        {
            FTEST_FAIL_TEST("imp handoff oracle: expected level %d (M-claim), got %ld — no claim tile",
                IMP_HANDOFF_ORACLE_LEVEL, level);
            return FTRs_Go_To_Next_Action;
        }

        // Reveal so the flood-fill / check_place_to_pretty see the tile (mirrors the other oracles).
        ftest_util_reveal_map(PLAYER0);

        const MapCoord sx = subtile_coord_center(SPAWN_STL_X);
        const MapCoord sy = subtile_coord_center(SPAWN_STL_Y);
        imp_handoff_imp = ftest_util_create_creature(sx, sy, PLAYER0, 1, get_players_special_digger_model(PLAYER0));
        if (thing_is_invalid(imp_handoff_imp))
        {
            FTEST_FAIL_TEST("imp handoff oracle: could not spawn the imp at subtile (%d,%d)", SPAWN_STL_X, SPAWN_STL_Y);
            return FTRs_Go_To_Next_Action;
        }
        // Settle the floor under the spawn tile before the game loop takes over (mirrors the movement oracle).
        move_thing_in_map(imp_handoff_imp, &imp_handoff_imp->mappos);

        // Guard: the target must load as a free path tile (the flood-fill's claim target), else the fixture is
        // wrong and the dump would be meaningless.
        struct SlabMap* target = get_slabmap_block(TILE_SLB_X, TILE_SLB_Y);
        if (target->kind != SlbT_EARTH)
        {
            FTEST_FAIL_TEST("imp handoff oracle: target slab (%d,%d) is kind %d, expected SlbT_EARTH",
                TILE_SLB_X, TILE_SLB_Y, (int)target->kind);
            delete_thing_structure(imp_handoff_imp, 0);
            imp_handoff_imp = NULL;
            return FTRs_Go_To_Next_Action;
        }

        // Tag the earth tile for digging with the real player action, so the imp digs it first; after the
        // dig-out it continues into a claim of the revealed path (check_out_imp_last_did continue-nearby).
        game_action(PLAYER0, GA_MarkDig, 0,
            slab_subtile_center(TILE_SLB_X), slab_subtile_center(TILE_SLB_Y), 1, 1);

        char path[512];
        snprintf(path, sizeof(path), "%s/oracle_imp_handoff.jsonl", imp_handoff_out_dir());
        imp_handoff_jsonl = fopen(path, "w");
        if (imp_handoff_jsonl == NULL)
        {
            FTEST_FRAMEWORK_ABORT("imp handoff oracle: failed to open '%s'", path);
            delete_thing_structure(imp_handoff_imp, 0);
            imp_handoff_imp = NULL;
            return FTRs_Go_To_Next_Action;
        }
        FTESTLOG("imp handoff oracle: writing '%s'", path);

        const char* prov_binary = getenv("KEEPERFX_ORACLE_BINARY");
        const char* prov_build  = getenv("KEEPERFX_ORACLE_BUILD");
        fprintf(imp_handoff_jsonl,
            "{\"v\":%d,\"type\":\"meta\",\"probe\":\"imp_handoff\",\"level\":%d,\"campaign\":\"classic\","
            "\"tile_slb_x\":%d,\"tile_slb_y\":%d,\"spawn_stl_x\":%d,\"spawn_stl_y\":%d,"
            "\"engine\":\"keeperfx-oracle-dumps\",\"version\":\"%s\",\"binary\":\"%s\",\"build\":\"%s\"}\n",
            IMP_HANDOFF_ORACLE_VERSION, IMP_HANDOFF_ORACLE_LEVEL, TILE_SLB_X, TILE_SLB_Y,
            SPAWN_STL_X, SPAWN_STL_Y, VER_STRING,
            prov_binary ? prov_binary : "", prov_build ? prov_build : "");
    }

    imp_handoff_dump(imp_handoff_jsonl, imp_handoff_imp);

    // Stop once the tile is claimed (its slab flips to SlbT_CLAIMED) plus a short tail, or the safety cap.
    struct SlabMap* slb = get_slabmap_block(TILE_SLB_X, TILE_SLB_Y);
    if (slb->kind == SlbT_CLAIMED && imp_handoff_countdown < 0)
        imp_handoff_countdown = POST_HANDOFF_TICKS;

    if (imp_handoff_countdown == 0 || (long)args->times_executed >= HANDOFF_MAX_TICKS)
    {
        FTESTLOG("imp handoff oracle: dump complete (%ld records, final block kind %d)",
            (long)args->times_executed + 1, (int)slb->kind);
        fclose(imp_handoff_jsonl);
        imp_handoff_jsonl = NULL;
        delete_thing_structure(imp_handoff_imp, 0);
        imp_handoff_imp = NULL;
        return FTRs_Go_To_Next_Action;
    }
    if (imp_handoff_countdown > 0)
        imp_handoff_countdown--;
    return FTRs_Repeat_Current_Action;
}

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
