#include "ftest_imp_dig_oracle.h"

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
#define IMP_DIG_ORACLE_VERSION 2

// The scenario is pinned to the M-full synthetic fixture: it carries the one diggable earth tile the
// keeper-rx pathfinding/dig steps target (SyntheticMaps.MFull, slab (21, corridorY=Size/2=42)). The tile
// sits in a Player0-claimed corridor, so a Player0 imp can stand beside it and dig.
#define IMP_DIG_ORACLE_LEVEL 9003
#define TILE_SLB_X 21
#define TILE_SLB_Y 42

// The imp spawns a few subtiles west of the tile, inside the left claimed room (slabs x=2..20 at y=42), and
// must route east to a dig spot beside the earth block — a short, deterministic walk.
#define SPAWN_STL_X 60
#define SPAWN_STL_Y 127

// Spawn the imp only AFTER game turn 128. The shared digger stack rebuilds at most once per 128 turns and
// its dungeon->digger_stack_update_turn is 0 at level load, so an imp spawned before turn 128 would sit idle
// and RANDOM-WANDER (the RNG toke/wander fallback) until the gate opens — a non-deterministic window the
// keeper-rx port deliberately defers (no synced RNG yet). Spawning past turn 128 means the imp's very first
// idle tick rebuilds the stack and it self-assigns the dig immediately, with no wander to reproduce. The
// keeper-rx replay seeds its clock to record 0's tick, so the same "rebuild on the first idle tick" holds.
#define SPAWN_DELAY_TURNS 130

// Stop on the very tick the block is dug out (the record where its slab has flipped off SlbT_EARTH, the task
// list has cleared, and the imp is in ImpLastDidJob). One tick later the imp has no work left and begins to
// RANDOM-WANDER (the RNG toke/wander fallback the keeper-rx port defers), so dumping past the dig-out would
// capture a non-reproducible tail. The dig loop's terminal observable is exactly this dig-out record.
#define POST_DIG_TICKS 0
// Safety cap: the imp waits out the 128-turn stack-rebuild gate, then walks (~10 turns) and swings twice
// (~9 turns each). A few hundred turns is a generous ceiling that a stuck scenario cannot exceed silently.
#define DIG_MAX_TICKS 600

static FILE* imp_dig_jsonl = NULL;
static struct Thing* imp_dig_imp = NULL;
static int imp_dig_countdown = -1; // -1 until the block is dug; then counts POST_DIG_TICKS down to 0.

// forward declaration
FTestActionResult ftest_imp_dig_oracle_action__run(struct FTestActionArgs* const args);

TbBool ftest_imp_dig_oracle_init()
{
    // One action, delayed past the 128-turn digger-stack gate (see SPAWN_DELAY_TURNS) so the imp self-assigns
    // on its first idle tick with no random-wander window; then the per-tick driver spawns/tags/dumps.
    ftest_append_action(ftest_imp_dig_oracle_action__run, SPAWN_DELAY_TURNS, NULL);
    return true;
}

// Resolve the dump directory: $KEEPERFX_ORACLE_OUT, else the CWD (mirrors the other oracle ftests so they
// share the capture-script convention).
static const char* imp_dig_out_dir(void)
{
    const char* dir = getenv("KEEPERFX_ORACLE_OUT");
    if (dir == NULL || dir[0] == '\0')
        return ".";
    mkdir(dir, 0755); // best-effort; ignore EEXIST
    return dir;
}

// One per-tick ground-truth record. Written BEFORE this turn's update_thing runs, so record 0 is the posed
// initial state (imp at spawn, tile tagged, no work claimed yet) and each later record is the state left by
// one game update. Fields are read straight from the C so the keeper-rx diff is auditable:
//   pos_x/pos_y          imp->mappos.{x,y}.val  (fixed-point, 256/subtile)
//   active_state         imp->active_state      (CrSt_* id; keeper-rx CreatureStateId equals it)
//   instance_id          cctrl->instance_id     (CrInst_* id; 30 == CrInst_DIG while swinging)
//   inst_turn/action     cctrl->inst_turn / inst_action_turns  (the swing fires when they meet)
//   block_health         slb->health            (the earth block's remaining hit-points)
//   block_kind           slb->kind              (SlbT_EARTH=2 while standing, SlbT_PATH=10 once dug)
//   task_count           dungeon->task_count    (the designation list occupancy)
//   anim_sprite          imp->anim_sprite       (resolved top-down keepersprite id set by the selector)
//   anim_speed           imp->anim_speed        (the CGI speed the selector chose this tick)
//   max_frames           imp->max_frames        (keepersprite_frames of the current strip)
//   current_frame        imp->current_frame     (advanced by update_thing_animation after the selector)
//   distance_to_dest     cctrl->distance_to_destination      (signed 2-D distance moved toward the facing)
//   inst_anim_step_turns cctrl->instance_anim_step_turns     (the swing's stretched per-turn animation step)
static void imp_dig_dump(FILE* f, struct Thing* imp)
{
    struct CreatureControl* cctrl = creature_control_get_from_thing(imp);
    struct SlabMap* slb = get_slabmap_block(TILE_SLB_X, TILE_SLB_Y);
    struct Dungeon* dungeon = get_dungeon(PLAYER0);
    fprintf(f,
        "{\"v\":%d,\"type\":\"imp_dig\",\"tick\":%ld,"
        "\"pos_x\":%ld,\"pos_y\":%ld,\"active_state\":%d,"
        "\"instance_id\":%d,\"inst_turn\":%d,\"inst_action_turns\":%d,"
        "\"block_health\":%d,\"block_kind\":%d,\"task_count\":%d,"
        "\"anim_sprite\":%d,\"anim_speed\":%d,\"max_frames\":%d,\"current_frame\":%d,"
        "\"distance_to_destination\":%d,\"instance_anim_step_turns\":%d}\n",
        IMP_DIG_ORACLE_VERSION, (long)get_gameturn(),
        (long)imp->mappos.x.val, (long)imp->mappos.y.val, (int)imp->active_state,
        (int)cctrl->instance_id, (int)cctrl->inst_turn, (int)cctrl->inst_action_turns,
        (int)slb->health, (int)slb->kind, (int)dungeon->task_count,
        (int)imp->anim_sprite, (int)imp->anim_speed, (int)imp->max_frames, (int)imp->current_frame,
        (int)cctrl->distance_to_destination, (int)cctrl->instance_anim_step_turns);
}

// Driver: on the first turn reveal the map, spawn the imp, tag the earth tile with the real player action,
// and open the JSONL; every turn dump; stop once the block is dug (plus a short tail) or the safety cap trips.
FTestActionResult ftest_imp_dig_oracle_action__run(struct FTestActionArgs* const args)
{
    if (args->times_executed == 0)
    {
        const long level = (long)get_loaded_level_number();
        if (level != IMP_DIG_ORACLE_LEVEL)
        {
            FTEST_FAIL_TEST("imp dig oracle: expected level %d (M-full), got %ld — no earth tile to dig",
                IMP_DIG_ORACLE_LEVEL, level);
            return FTRs_Go_To_Next_Action;
        }

        // Reveal so the imp's valid_dig_position / the designation see the tile (mirrors the other oracles).
        ftest_util_reveal_map(PLAYER0);

        const MapCoord sx = subtile_coord_center(SPAWN_STL_X);
        const MapCoord sy = subtile_coord_center(SPAWN_STL_Y);
        imp_dig_imp = ftest_util_create_creature(sx, sy, PLAYER0, 1, get_players_special_digger_model(PLAYER0));
        if (thing_is_invalid(imp_dig_imp))
        {
            FTEST_FAIL_TEST("imp dig oracle: could not spawn the imp at subtile (%d,%d)", SPAWN_STL_X, SPAWN_STL_Y);
            return FTRs_Go_To_Next_Action;
        }
        // Settle the floor under the spawn tile before the game loop takes over (mirrors the movement oracle).
        move_thing_in_map(imp_dig_imp, &imp_dig_imp->mappos);

        // Tag the earth tile for digging with the real player action (what the dig cursor issues), so the imp
        // self-assigns off it. width/height = 1 slab. Done before the first dump so record 0 shows task_count=1.
        game_action(PLAYER0, GA_MarkDig, 0,
            slab_subtile_center(TILE_SLB_X), slab_subtile_center(TILE_SLB_Y), 1, 1);
        struct Dungeon* dungeon = get_dungeon(PLAYER0);
        if (dungeon->task_count < 1)
        {
            FTEST_FAIL_TEST("imp dig oracle: GA_MarkDig did not register a dig task at slab (%d,%d)",
                TILE_SLB_X, TILE_SLB_Y);
            delete_thing_structure(imp_dig_imp, 0);
            imp_dig_imp = NULL;
            return FTRs_Go_To_Next_Action;
        }

        char path[512];
        snprintf(path, sizeof(path), "%s/oracle_imp_dig.jsonl", imp_dig_out_dir());
        imp_dig_jsonl = fopen(path, "w");
        if (imp_dig_jsonl == NULL)
        {
            FTEST_FRAMEWORK_ABORT("imp dig oracle: failed to open '%s'", path);
            delete_thing_structure(imp_dig_imp, 0);
            imp_dig_imp = NULL;
            return FTRs_Go_To_Next_Action;
        }
        FTESTLOG("imp dig oracle: writing '%s'", path);

        const char* prov_build  = getenv("KEEPERFX_ORACLE_BUILD");
        fprintf(imp_dig_jsonl,
            "{\"v\":%d,\"type\":\"meta\",\"probe\":\"imp_dig\",\"level\":%d,\"campaign\":\"classic\","
            "\"tile_slb_x\":%d,\"tile_slb_y\":%d,\"spawn_stl_x\":%d,\"spawn_stl_y\":%d,"
            "\"engine\":\"keeperfx-oracle-dumps\",\"version\":\"%s\",\"build\":\"%s\"}\n",
            IMP_DIG_ORACLE_VERSION, IMP_DIG_ORACLE_LEVEL, TILE_SLB_X, TILE_SLB_Y,
            SPAWN_STL_X, SPAWN_STL_Y, VER_STRING,
            prov_build ? prov_build : "");
    }

    imp_dig_dump(imp_dig_jsonl, imp_dig_imp);

    // Stop once the tile is dug out (its slab flips off SlbT_EARTH) plus a short tail, or the safety cap.
    struct SlabMap* slb = get_slabmap_block(TILE_SLB_X, TILE_SLB_Y);
    if (slb->kind != SlbT_EARTH && imp_dig_countdown < 0)
        imp_dig_countdown = POST_DIG_TICKS;

    if (imp_dig_countdown == 0 || (long)args->times_executed >= DIG_MAX_TICKS)
    {
        FTESTLOG("imp dig oracle: dump complete (%ld records, final block kind %d)",
            (long)args->times_executed + 1, (int)slb->kind);
        fclose(imp_dig_jsonl);
        imp_dig_jsonl = NULL;
        delete_thing_structure(imp_dig_imp, 0);
        imp_dig_imp = NULL;
        return FTRs_Go_To_Next_Action;
    }
    if (imp_dig_countdown > 0)
        imp_dig_countdown--;
    return FTRs_Repeat_Current_Action;
}

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
