#include "ftest_starter_dungeon_jobs_oracle.h"

#ifdef FUNCTESTING

#include "../../pre_inc.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "../ftest.h"
#include "../ftest_util.h"

#include "../../game_legacy.h"
#include "../../keeperfx.hpp"
#include "../../thing_data.h"
#include "../../thing_list.h"
#include "../../thing_creature.h"
#include "../../thing_navigate.h"
#include "../../creature_control.h"
#include "../../creature_instances.h"
#include "../../map_blocks.h"
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

#define STARTER_JOBS_ORACLE_VERSION 1
#define STARTER_JOBS_ORACLE_LEVEL 9008

// Spawn past the 128-turn digger-stack gate (as the retired job oracles do), so the imp self-assigns on its
// first idle tick with no random-wander window.
#define SPAWN_DELAY_TURNS 130

// One imp, spawned on the heart room's claimed border by the east doorway (slab (12,11), a claimed floor tile).
#define SPAWN_STL_X 37   // slab 12 centre subtile (12*3+1)
#define SPAWN_STL_Y 34   // slab 11 centre subtile (11*3+1)

// The designated dig: one east earth tile just past the doorway (claimed corridor at (13,11)-(14,11)). The imp
// digs it, hands off into claiming the revealed floor, then reinforces the %5 wall the claim exposes.
#define DIG_SLB_X 15
#define DIG_SLB_Y 11

// The pre-claimed east room the torch oracle uses (StarterDungeon.cs) — reset to earth here so its walls are
// not stray reinforce targets competing with the one the claim exposes.
#define EAST_ROOM_X0 14
#define EAST_ROOM_Y0 14
#define EAST_ROOM_X1 16
#define EAST_ROOM_Y1 16

#define JOBS_MAX_TICKS 900

static FILE* jobs_jsonl = NULL;
static struct Thing* jobs_imp = NULL;
static int jobs_reinforced = 0; // set once the imp has driven a REINFORCE, so the dig-out is not mistaken for the terminal.

static const char* jobs_out_dir(void)
{
    const char* dir = getenv("KEEPERFX_ORACLE_OUT");
    if (dir == NULL || dir[0] == '\0')
        return ".";
    mkdir(dir, 0755);
    return dir;
}

// Delete every creature on the map except the one controlled imp (skip_imp), so only that imp drives the
// digger stack. Called every tick, not just at setup: the dungeon heart keeps generating replacement imps
// (the dungeon is left with a single imp, below the free-imp minimum), and a generated imp self-assigns to
// the one dig task, reaches it, and lands a stray dig hit — contaminating the single-imp isolation. Culling
// every tick keeps the capture to exactly the tracked imp. (At setup skip_imp is NULL, so all are cleared.)
static void jobs_clear_creatures(struct Thing* skip_imp)
{
    struct StructureList* slist = get_list_for_thing_class(TCls_Creature);
    if (slist == NULL)
        return;
    long i = slist->index;
    unsigned long guard = 0;
    while (i != 0)
    {
        struct Thing* thing = thing_get(i);
        if (thing_is_invalid(thing))
            break;
        long next = thing->next_of_class;
        if (++guard > slist->count + 1)
            break;
        if (thing != skip_imp)
            delete_thing_structure(thing, 0);
        i = next;
    }
}

// One per-tick job record — the union of what the retired claim/handoff/reinforce oracles dumped, keyed off
// the imp's *current* working slab so it tracks whichever loop (dig/claim/reinforce) is running:
//   pos_x/pos_y                imp->mappos.{x,y}.val
//   active_state               imp->active_state           (CrSt_* id)
//   instance_id                cctrl->instance_id          (30 DIG, 31 PRETTY_PATH claim, 35 REINFORCE)
//   inst_turn/inst_action      cctrl->inst_turn / inst_action_turns
//   consecutive_reinforcements cctrl->digger.consecutive_reinforcements
//   last_did_job               cctrl->digger.last_did_job  (the dig→claim handoff continue marker)
//   work_slb_x/y               subtile_slab(cctrl->digger.working_stl)  — the wall the imp reinforces (set
//                              only while reinforcing; (0,0) during dig/claim)
//   block_kind/block_owner     that reinforce slab's kind + owner
//   dig_kind/dig_owner         the fixed dig target (DIG_SLB): earth → path (dug) → claimed, so the dig/claim
//                              terrain transition is captured even though working_stl is not the dig target
//   total_area                 dungeon->total_area         (bumps on the claim)
//   thing_index/creation_turn/random_seed  the imp's index, creation gameturn, and per-thing RNG seed —
//                              dumped so keeper-rx can derive-and-verify the seed (ADR-0028): the seed is
//                              index*9377 + 9439 + creation_turn. Under FUNCTESTING the series is frozen
//                              (LbRandomSeries skips the advance), so THING_RANDOM(thing,R) == seed % R every
//                              draw and the seed never changes for the imp's life.
static void jobs_dump(FILE* f, struct Thing* imp)
{
    struct CreatureControl* cctrl = creature_control_get_from_thing(imp);
    struct Dungeon* dungeon = get_dungeon(imp->owner);
    MapSlabCoord wslb_x = subtile_slab(stl_num_decode_x(cctrl->digger.working_stl));
    MapSlabCoord wslb_y = subtile_slab(stl_num_decode_y(cctrl->digger.working_stl));
    struct SlabMap* slb = get_slabmap_block(wslb_x, wslb_y);
    struct SlabMap* dig = get_slabmap_block(DIG_SLB_X, DIG_SLB_Y);
    fprintf(f,
        "{\"v\":%d,\"type\":\"imp_jobs\",\"tick\":%ld,"
        "\"pos_x\":%ld,\"pos_y\":%ld,\"active_state\":%d,"
        "\"instance_id\":%d,\"inst_turn\":%d,\"inst_action_turns\":%d,"
        "\"consecutive_reinforcements\":%d,\"last_did_job\":%d,"
        "\"work_slb_x\":%d,\"work_slb_y\":%d,\"block_kind\":%d,\"block_owner\":%d,"
        "\"dig_kind\":%d,\"dig_owner\":%d,\"dig_health\":%d,\"total_area\":%ld,"
        "\"thing_index\":%d,\"creation_turn\":%ld,\"random_seed\":%u}\n",
        STARTER_JOBS_ORACLE_VERSION, (long)get_gameturn(),
        (long)imp->mappos.x.val, (long)imp->mappos.y.val, (int)imp->active_state,
        (int)cctrl->instance_id, (int)cctrl->inst_turn, (int)cctrl->inst_action_turns,
        (int)cctrl->digger.consecutive_reinforcements, (int)cctrl->digger.last_did_job,
        (int)wslb_x, (int)wslb_y, (int)slb->kind, (int)slabmap_owner(slb),
        (int)dig->kind, (int)slabmap_owner(dig), (int)dig->health, (long)dungeon->total_area,
        (int)imp->index, (long)imp->creation_turn, (unsigned)imp->random_seed);
}

FTestActionResult ftest_starter_dungeon_jobs_oracle_action__run(struct FTestActionArgs* const args);

TbBool ftest_starter_dungeon_jobs_oracle_init()
{
    ftest_append_action(ftest_starter_dungeon_jobs_oracle_action__run, SPAWN_DELAY_TURNS, NULL);
    return true;
}

FTestActionResult ftest_starter_dungeon_jobs_oracle_action__run(struct FTestActionArgs* const args)
{
    if (args->times_executed == 0)
    {
        const long level = (long)get_loaded_level_number();
        if (level != STARTER_JOBS_ORACLE_LEVEL)
        {
            FTEST_FAIL_TEST("starter jobs oracle: expected level %d (M-start), got %ld", STARTER_JOBS_ORACLE_LEVEL, level);
            return FTRs_Go_To_Next_Action;
        }

        ftest_util_reveal_map(PLAYER0);

        // Reset the torch oracle's pre-claimed east room back to neutral earth, so its walls are not competing
        // reinforce targets; then clear the map's starting imps so a single controlled imp drives the stack.
        for (MapSlabCoord y = EAST_ROOM_Y0; y <= EAST_ROOM_Y1; y++)
            for (MapSlabCoord x = EAST_ROOM_X0; x <= EAST_ROOM_X1; x++)
                place_slab_type_on_map(SlbT_EARTH, slab_subtile_center(x), slab_subtile_center(y), game.neutral_player_num, 0);
        jobs_clear_creatures(NULL);

        jobs_imp = ftest_util_create_creature(subtile_coord_center(SPAWN_STL_X), subtile_coord_center(SPAWN_STL_Y),
            PLAYER0, 1, get_players_special_digger_model(PLAYER0));
        if (thing_is_invalid(jobs_imp))
        {
            FTEST_FAIL_TEST("starter jobs oracle: could not spawn the imp at subtile (%d,%d)", SPAWN_STL_X, SPAWN_STL_Y);
            return FTRs_Go_To_Next_Action;
        }
        move_thing_in_map(jobs_imp, &jobs_imp->mappos);

        struct SlabMap* target = get_slabmap_block(DIG_SLB_X, DIG_SLB_Y);
        if (target->kind != SlbT_EARTH)
        {
            FTEST_FAIL_TEST("starter jobs oracle: dig target (%d,%d) is kind %d, expected SlbT_EARTH",
                DIG_SLB_X, DIG_SLB_Y, (int)target->kind);
            delete_thing_structure(jobs_imp, 0);
            jobs_imp = NULL;
            return FTRs_Go_To_Next_Action;
        }

        // Tag the earth tile for digging with the real player action; the imp digs it, hands off into a claim
        // of the revealed floor, then reinforces the %5 wall that claim exposes.
        game_action(PLAYER0, GA_MarkDig, 0, slab_subtile_center(DIG_SLB_X), slab_subtile_center(DIG_SLB_Y), 1, 1);

        char path[512];
        snprintf(path, sizeof(path), "%s/oracle_starter_dungeon_jobs.jsonl", jobs_out_dir());
        jobs_jsonl = fopen(path, "w");
        if (jobs_jsonl == NULL)
        {
            FTEST_FRAMEWORK_ABORT("starter jobs oracle: failed to open '%s'", path);
            delete_thing_structure(jobs_imp, 0);
            jobs_imp = NULL;
            return FTRs_Go_To_Next_Action;
        }
        FTESTLOG("starter jobs oracle: writing '%s'", path);

        const char* prov_build = getenv("KEEPERFX_ORACLE_BUILD");
        fprintf(jobs_jsonl,
            "{\"v\":%d,\"type\":\"meta\",\"probe\":\"starter_dungeon_jobs\",\"level\":%d,\"campaign\":\"classic\","
            "\"dig_slb_x\":%d,\"dig_slb_y\":%d,\"spawn_stl_x\":%d,\"spawn_stl_y\":%d,"
            "\"engine\":\"keeperfx-oracle-dumps\",\"version\":\"%s\",\"build\":\"%s\"}\n",
            STARTER_JOBS_ORACLE_VERSION, STARTER_JOBS_ORACLE_LEVEL, DIG_SLB_X, DIG_SLB_Y,
            SPAWN_STL_X, SPAWN_STL_Y, VER_STRING, prov_build ? prov_build : "");
    }

    // Cull any heart-generated imps every tick, so only jobs_imp ever touches the dig task.
    jobs_clear_creatures(jobs_imp);

    jobs_dump(jobs_jsonl, jobs_imp);

    // Terminal: the first reinforce fortify. Once the imp has driven a REINFORCE, stop the tick its working
    // wall leaves EARTH (fortified) — one tick later it random-wanders (the RNG fallback keeper-rx defers).
    struct CreatureControl* cctrl = creature_control_get_from_thing(jobs_imp);
    if (cctrl->instance_id == CrInst_REINFORCE)
        jobs_reinforced = 1;
    MapSlabCoord wslb_x = subtile_slab(stl_num_decode_x(cctrl->digger.working_stl));
    MapSlabCoord wslb_y = subtile_slab(stl_num_decode_y(cctrl->digger.working_stl));
    struct SlabMap* wslb = get_slabmap_block(wslb_x, wslb_y);
    int fortified = jobs_reinforced && wslb->kind != SlbT_EARTH;

    if (fortified || (long)args->times_executed >= JOBS_MAX_TICKS)
    {
        FTESTLOG("starter jobs oracle: dump complete (%ld records, reinforced=%d, final work-slab kind %d)",
            (long)args->times_executed + 1, jobs_reinforced, (int)wslb->kind);
        fclose(jobs_jsonl);
        jobs_jsonl = NULL;
        delete_thing_structure(jobs_imp, 0);
        jobs_imp = NULL;
        return FTRs_Go_To_Next_Action;
    }
    return FTRs_Repeat_Current_Action;
}

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
