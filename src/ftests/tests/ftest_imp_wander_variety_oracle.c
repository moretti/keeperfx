#include "ftest_imp_wander_variety_oracle.h"

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
#include "../../player_data.h"
#include "../../player_instances.h"
#include "../../ver_defs.h"

#include "../../post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif

// Oracle dump format version — bump on any layout/meaning change (keeper-rx/docs/oracle/dump-format.md).
#define IMP_WANDER_VARIETY_ORACLE_VERSION 1

// Pinned to the M-wander synthetic fixture (SyntheticMaps.MWander, level 9019): a fully Player0-claimed open
// floor field (the same footprint as M0, slabs (2,2)-(82,82)) with a Dungeon Heart and NOTHING ELSE — no
// earth, no rock walls to reinforce, no gold/gems seam, no room but the heart's own registration. Every idle
// imp therefore finds check_out_imp_last_did and check_out_available_imp_tasks both empty on its very first
// idle tick and falls straight to creature_choose_random_destination_on_valid_adjacent_slab every cycle —
// this fixture exists ONLY to exercise that one wander call, across imps whose different (thing index,
// creation turn) pairs give it a different frozen per-thing seed (ADR-0028), so the captured golden covers
// more than the single, accidentally-axis-aligned leg the starter-dungeon-jobs oracle (9008) happens to show.
#define IMP_WANDER_VARIETY_ORACLE_LEVEL 9019
#define WANDER_IMP_COUNT 6

// Six spawn points, a 3x2 grid straddling the open arena's centre (slab (42,42) of the (2,2)-(82,82)
// field): 16 slabs apart from their row/column neighbours (an empirically-verified-safe pairwise gap — an
// initial capture at a wider 20-slab spacing never brought any two tracked imps within 40 subtiles of each
// other) and >=24 slabs clear of the arena's own claimed-floor edge in every direction. The clearance
// matters: a first capture with only ~20 slabs of edge clearance let one imp's fixed wander direction walk
// it all the way to the rock border inside 600 ticks, where a still-open near-wall route/follower behaviour
// (a separate, not-yet-in-scope feature from the wander destination pick this fixture exists to gate)
// produced a real divergence unrelated to CreatureWander itself. WANDER_MAX_TICKS is capped well under what
// this clearance affords, so every tracked imp's whole walk stays on open, wall-clear floor for the run.
static const int WANDER_SPAWN_SLB_X[WANDER_IMP_COUNT] = { 26, 42, 58, 26, 42, 58 };
static const int WANDER_SPAWN_SLB_Y[WANDER_IMP_COUNT] = { 34, 34, 34, 50, 50, 50 };

// Spawn all six imps together, past the 128-turn digger-stack rebuild gate the other imp-job oracles pin to
// (see e.g. ftest_imp_prison_drag_oracle.c's identical rationale) — this fixture has no digger-stack work
// of its own to gate on, but keeping the same pin avoids relying on any other first-turn-load edge case
// those fixtures already steer clear of.
#define SPAWN_DELAY_TURNS 130

// Long enough for many wander legs per imp, short enough that (measured, not guessed) no imp's own repeating
// walk reaches anywhere near the arena edge. Two earlier captures — 600 ticks, then 450 with more edge
// clearance — both eventually let some imp's fixed wander direction march it all the way to the rock border
// (a march, not a random walk: the direction is a lifelong constant, so nothing brings it back). This
// fixture's whole job is the wander DESTINATION pick (CreatureWander), not near-wall route/follower
// behaviour — a separate, not-yet-in-scope feature — so 170 ticks is chosen from the SAME captured run's own
// measurements: every imp keeps >=15 subtiles (5 slabs) of clearance through tick 170, while both diversity
// requirements (differing directions, a genuine diagonal leg) are already satisfied by tick 140 — 30 ticks
// after the imps spawn.
#define WANDER_MAX_TICKS 170

static FILE* imp_wander_jsonl = NULL;
static struct Thing* imp_wander_imps[WANDER_IMP_COUNT] = { NULL, NULL, NULL, NULL, NULL, NULL };

// forward declaration
FTestActionResult ftest_imp_wander_variety_oracle_action__run(struct FTestActionArgs* const args);

// Delete every creature on the map except the tracked imps — mirrors ftest_imp_gems_oracle.c's
// gems_clear_creatures. A fresh Keeper Player's starting-imp gift would otherwise wander this same arena
// untracked, and could physically jostle a tracked imp off its own path — this fixture's whole point is
// each tracked imp's OWN frozen-seed wander, undisturbed by any other creature.
static void wander_clear_creatures(void)
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
        TbBool tracked = false;
        for (int k = 0; k < WANDER_IMP_COUNT; k++)
        {
            if (thing == imp_wander_imps[k])
            {
                tracked = true;
                break;
            }
        }
        if (!tracked)
            delete_thing_structure(thing, 0);
        i = next;
    }
}

// Resolve the dump directory: $KEEPERFX_ORACLE_OUT, else the CWD (mirrors the other oracle ftests).
static const char* imp_wander_out_dir(void)
{
    const char* dir = getenv("KEEPERFX_ORACLE_OUT");
    if (dir == NULL || dir[0] == '\0')
        return ".";
    mkdir(dir, 0755); // best-effort; ignore EEXIST
    return dir;
}

// One per-tick, per-imp ground-truth record — self-contained (imp_slot picks it out), mirroring
// ftest_imp_gems_oracle.c's multi-imp shape. Written BEFORE this turn's update_thing runs, so each imp's
// first record is its posed spawn state. Fields:
//   imp_slot                  1..WANDER_IMP_COUNT — which spawned imp this record is for
//   pos_x/pos_y                imp->mappos.{x,y}.val (fixed-point, 256/subtile) — the walk geometry itself;
//                               a genuine diagonal leg shows as BOTH changing between consecutive ticks
//   active_state                imp->active_state (CrSt_ImpDoingNothing=1 idle, CrSt_MoveToPosition=14 walking)
//   move_angle_xy               imp->move_angle_xy — the facing the walk is driving toward
//   thing_index/creation_turn/random_seed   ADR-0028's diagnosis-pinning triple: random_seed =
//     index*9377 + 9439 + creation_turn is the frozen per-thing seed the wander's two THING_RANDOM draws
//     (start_stl = seed%9, m = seed%4) come from — dumped every tick (constant per imp, by construction of
//     FUNCTESTING's frozen regime) so a keeper-rx replay can verify it derived the IDENTICAL seed, not
//     merely a coincidentally-matching position.
static void imp_wander_dump(FILE* f, struct Thing* imp, int imp_slot)
{
    fprintf(f,
        "{\"v\":%d,\"type\":\"imp_wander\",\"tick\":%ld,\"imp_slot\":%d,"
        "\"pos_x\":%ld,\"pos_y\":%ld,\"active_state\":%d,\"move_angle_xy\":%d,"
        "\"thing_index\":%d,\"creation_turn\":%ld,\"random_seed\":%u}\n",
        IMP_WANDER_VARIETY_ORACLE_VERSION, (long)get_gameturn(), imp_slot,
        (long)imp->mappos.x.val, (long)imp->mappos.y.val, (int)imp->active_state, (int)imp->move_angle_xy,
        (int)imp->index, (long)imp->creation_turn, (unsigned)imp->random_seed);
}

TbBool ftest_imp_wander_variety_oracle_init()
{
    ftest_append_action(ftest_imp_wander_variety_oracle_action__run, SPAWN_DELAY_TURNS, NULL);
    return true;
}

// Driver: on the first turn reveal the map, clear any starting imps, spawn all WANDER_IMP_COUNT tracked
// imps and open the JSONL; every turn cull any newcomer and dump every tracked imp; stop at the safety cap.
FTestActionResult ftest_imp_wander_variety_oracle_action__run(struct FTestActionArgs* const args)
{
    if (args->times_executed == 0)
    {
        const long level = (long)get_loaded_level_number();
        if (level != IMP_WANDER_VARIETY_ORACLE_LEVEL)
        {
            FTEST_FAIL_TEST("imp wander variety oracle: expected level %d (M-wander), got %ld — no open arena to wander",
                IMP_WANDER_VARIETY_ORACLE_LEVEL, level);
            return FTRs_Go_To_Next_Action;
        }

        ftest_util_reveal_map(PLAYER0);
        wander_clear_creatures(); // clear the map's starting imps first, as ftest_imp_gems_oracle.c does

        struct Dungeon* dungeon = get_dungeon(PLAYER0);
        if (dungeon->task_count != 0)
        {
            FTEST_FAIL_TEST("imp wander variety oracle: expected an empty task list (no diggable/claimable work), task_count=%d",
                (int)dungeon->task_count);
            return FTRs_Go_To_Next_Action;
        }

        for (int k = 0; k < WANDER_IMP_COUNT; k++)
        {
            const MapCoord sx = subtile_coord_center(slab_subtile_center(WANDER_SPAWN_SLB_X[k]));
            const MapCoord sy = subtile_coord_center(slab_subtile_center(WANDER_SPAWN_SLB_Y[k]));
            imp_wander_imps[k] = ftest_util_create_creature(sx, sy, PLAYER0, 1, get_players_special_digger_model(PLAYER0));
            if (thing_is_invalid(imp_wander_imps[k]))
            {
                FTEST_FAIL_TEST("imp wander variety oracle: could not spawn imp %d at slab (%d,%d)",
                    k + 1, WANDER_SPAWN_SLB_X[k], WANDER_SPAWN_SLB_Y[k]);
                return FTRs_Go_To_Next_Action;
            }
            move_thing_in_map(imp_wander_imps[k], &imp_wander_imps[k]->mappos);
        }

        char path[512];
        snprintf(path, sizeof(path), "%s/oracle_imp_wander_variety.jsonl", imp_wander_out_dir());
        imp_wander_jsonl = fopen(path, "w");
        if (imp_wander_jsonl == NULL)
        {
            FTEST_FRAMEWORK_ABORT("imp wander variety oracle: failed to open '%s'", path);
            return FTRs_Go_To_Next_Action;
        }
        FTESTLOG("imp wander variety oracle: writing '%s'", path);

        const char* prov_build = getenv("KEEPERFX_ORACLE_BUILD");
        fprintf(imp_wander_jsonl,
            "{\"v\":%d,\"type\":\"meta\",\"probe\":\"imp_wander_variety\",\"level\":%d,\"campaign\":\"classic\","
            "\"imp_count\":%d,"
            "\"engine\":\"keeperfx-oracle-dumps\",\"version\":\"%s\",\"build\":\"%s\"}\n",
            IMP_WANDER_VARIETY_ORACLE_VERSION, IMP_WANDER_VARIETY_ORACLE_LEVEL, WANDER_IMP_COUNT,
            VER_STRING, prov_build ? prov_build : "");
    }

    // Cull any heart-generated newcomer every tick — before dumping — so only the tracked imps ever occupy
    // this arena (see wander_clear_creatures's own comment).
    wander_clear_creatures();

    for (int k = 0; k < WANDER_IMP_COUNT; k++)
        imp_wander_dump(imp_wander_jsonl, imp_wander_imps[k], k + 1);

    if ((long)args->times_executed >= WANDER_MAX_TICKS)
    {
        FTESTLOG("imp wander variety oracle: dump complete (%ld ticks)", (long)args->times_executed + 1);
        fclose(imp_wander_jsonl);
        imp_wander_jsonl = NULL;
        for (int k = 0; k < WANDER_IMP_COUNT; k++)
        {
            delete_thing_structure(imp_wander_imps[k], 0);
            imp_wander_imps[k] = NULL;
        }
        return FTRs_Go_To_Next_Action;
    }
    return FTRs_Repeat_Current_Action;
}

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
