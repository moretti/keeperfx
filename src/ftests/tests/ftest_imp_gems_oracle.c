#include "ftest_imp_gems_oracle.h"

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
#define IMP_GEMS_ORACLE_VERSION 1

// The scenario is pinned to the M-gems synthetic fixture (SyntheticMaps.MGems, level 9014): M-mine's shape
// with the corridor's one designation re-kinded to a Player0-owned GEMS seam, plus a second GEMS tile inside
// the room that is never actually worked — it only pads the shared digger stack to length 2, the anti-camp
// escape's own precondition (is_digging_indestructible_place's caller requires digger_stack_length > 1,
// https://github.com/dkfans/keeperfx/blob/v1.4.0/src/spdigger_stack.c#L2780).
#define IMP_GEMS_ORACLE_LEVEL 9014
#define WORKED_SLB_X 21
#define WORKED_SLB_Y 42
#define PAD_SLB_X 10
#define PAD_SLB_Y 10

// Both imps spawn at the same subtile — the worked seam's corridor entrance, identical to the imp-mine
// oracle's spawn point — a few subtiles west of the block, routing east to a dig spot beside it.
#define SPAWN_STL_X 60
#define SPAWN_STL_Y 127

// Spawn the FIRST imp only after game turn 128 (see ftest_imp_mine_oracle.c's identical rationale): the
// shared digger stack rebuilds at most once per 128 turns, so spawning past the gate means it self-assigns
// on its very first idle tick with no RNG-driven wander to reproduce.
//
// Frozen-seed derive-and-verify (ADR-0028, docs/design/imp-jobs.md Slice S3): this exact value is also what
// makes imp 1's anti-camp escape OBSERVABLE. A probe capture at the "natural" 130 landed imp 1 at
// thing_index 2 (random_seed 28323, 28323%20=3 — a miss) and imp 2 (SECOND_IMP_SPAWN_DELAY_TURNS later) at
// thing_index 3 (random_seed 37790, %20=10 — also a miss); neither ever takes the escape, so the branch
// would go unobserved. Since random_seed = index*9377 + 9439 + creation_turn and index is fixed by
// allocation order (deterministic for this fixture, unaffected by which turn we spawn on), solving
// (17*2 + 19 + turn) % 20 == 1 for thing_index 2 gives turn % 20 == 8 — 148 is the smallest value clearing
// the 128-turn gate that satisfies it (index 2, turn 148 -> random_seed 28341, 28341%20 == 1: the escape
// fires). Re-solving for imp 2 at its now-shifted creation_turn (238, thing_index 3 unchanged by this
// change) confirms it lands on %20 == 8 — still a miss — so this ONE fixture, unmodified otherwise,
// exercises BOTH branches: imp 1 escapes, imp 2 never does.
#define SPAWN_DELAY_TURNS 148

// The SECOND imp spawns this many turns after the first — kept well under the 128-turn rebuild window so
// the shared stack (already built for imp 1) is never rebuilt a second time, and past imp 1's own
// fresh-idle guard so imp 1 is already working the block by the time imp 2 goes looking for it. This delta
// (not imp 2's absolute spawn turn) is what gives imp 2 a different creation_turn — and so a different
// frozen THING_RANDOM seed (index*9377 + 9439 + creation_turn, ADR-0028) — from imp 1, independent of
// whichever thing_index each actually lands on. See the .h file's derive-and-verify note.
#define SECOND_IMP_SPAWN_DELAY_TURNS 90

// Own-land damage (2, not neutral's 1) doubles GEMS's per-hit yield (17 vs. 8 gold/hit — design/imp-jobs.md
// item 3's worked example), which is what keeps this fixture's 6-round grind inside a few thousand ticks
// instead of tens of thousands — indestructibility already makes the damage value irrelevant to whether the
// block survives, so this only speeds up the derivation, it does not change which branches are exercised.
// The exact stop point is measured, not guessed: a first capture (unbounded to 5500 ticks) showed imp 1
// reaching its first multiple-of-5 task_repeats checkpoint (docs/design/imp-jobs.md Slice S3's "at least 6
// continuations" fixture requirement) at tick 3780 and imp 2 (spawned SECOND_IMP_SPAWN_DELAY_TURNS later)
// at tick 3880 — this value is the later of the two plus a short grace window past it, long enough to dump
// the immediate aftermath (the imp settling back into idle/re-dispatch) without paying for the next full
// round neither imp needs to complete for this fixture's purpose.
#define GEMS_MAX_TICKS 3900

static FILE* imp_gems_jsonl = NULL;
static struct Thing* imp_gems_1 = NULL;
static struct Thing* imp_gems_2 = NULL;

// forward declaration
FTestActionResult ftest_imp_gems_oracle_action__run(struct FTestActionArgs* const args);

// Delete every creature on the map except the two tracked imps, so only they ever touch the shared digger
// stack — mirrors ftest_starter_dungeon_jobs_oracle.c's jobs_clear_creatures. Called every tick, not just at
// setup: the dungeon heart keeps generating replacement imps (left below the free-imp minimum by design),
// and an untracked imp going idle would call imp_stack_update on its own — rebuilding the shared stack
// (resetting dungeon->digger_stack_update_turn) and so silently invalidating both tracked imps' cached
// digger.stack_update_turn and, critically, the anti-camp escape's "sync the cursor to the CURRENT rebuild
// turn" trick this whole fixture exists to observe. skip2 is NULL until imp 2 spawns, which is fine —
// nothing else is ever skipped by a null pointer.
static void gems_clear_creatures(struct Thing* skip1, struct Thing* skip2)
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
        if (thing != skip1 && thing != skip2)
            delete_thing_structure(thing, 0);
        i = next;
    }
}

TbBool ftest_imp_gems_oracle_init()
{
    ftest_append_action(ftest_imp_gems_oracle_action__run, SPAWN_DELAY_TURNS, NULL);
    return true;
}

// Resolve the dump directory: $KEEPERFX_ORACLE_OUT, else the CWD (mirrors the other oracle ftests).
static const char* imp_gems_out_dir(void)
{
    const char* dir = getenv("KEEPERFX_ORACLE_OUT");
    if (dir == NULL || dir[0] == '\0')
        return ".";
    mkdir(dir, 0755); // best-effort; ignore EEXIST
    return dir;
}

// One per-tick, per-imp ground-truth record — self-contained (both gems tiles' state duplicated on every
// line) so the keeper-rx reader never has to join across imps. Written BEFORE this turn's update_thing runs,
// so the first record for each imp is its posed initial state. Fields, read straight from the C:
//   imp_slot               1 or 2 — which spawned imp this record is for
//   pos_x/pos_y             imp->mappos.{x,y}.val (fixed-point, 256/subtile)
//   active_state            imp->active_state
//   instance_id             cctrl->instance_id (30 == CrInst_DIG while swinging)
//   inst_turn/action        cctrl->inst_turn / inst_action_turns (the swing fires when they meet)
//   gold_carried            imp->creature.gold_carried
//   task_repeats            cctrl->digger.task_repeats (the anti-camp fatigue counter)
//   last_did_job            cctrl->digger.last_did_job (SDLstJob_DigOrMine=1, SDLstJob_None=0 on escape)
//   task_stack_pos          cctrl->digger.task_stack_pos (the escape jumps this to a random slot)
//   thing_index             imp->index (drives the frozen seed formula)
//   creation_turn           imp->creation_turn (drives the frozen seed formula)
//   random_seed             imp->random_seed (index*9377 + 9439 + creation_turn, frozen — ADR-0028)
//   pile_exists/gold_stored/sprite_size/owner   the loose ObjMdl_Goldl pile at THIS imp's own stand subtile
//   worked_health/kind      the worked GEMS tile's health/kind (pinned at seed health, kind never changes)
//   pad_health/kind         the pad GEMS tile's health/kind (never touched — sanity that it stays inert)
//   task_count              dungeon->task_count (both designations stay tagged forever — gems never mined out)
//   digger_stack_length     dungeon->digger_stack_length (fixed at 2 for this whole run — see the .h note)
static void imp_gems_dump(FILE* f, struct Thing* imp, int imp_slot)
{
    struct CreatureControl* cctrl = creature_control_get_from_thing(imp);
    struct SlabMap* worked_slb = get_slabmap_block(WORKED_SLB_X, WORKED_SLB_Y);
    struct SlabMap* pad_slb = get_slabmap_block(PAD_SLB_X, PAD_SLB_Y);
    struct Dungeon* dungeon = get_dungeon(PLAYER0);

    struct Thing* pile = smallest_gold_pile_at_xy(imp->mappos.x.stl.num, imp->mappos.y.stl.num);
    const int pile_exists = !thing_is_invalid(pile);
    const long pile_gold_stored = pile_exists ? pile->valuable.gold_stored : 0;
    const int pile_sprite_size = pile_exists ? pile->sprite_size : 0;
    const int pile_owner = pile_exists ? (int)pile->owner : -1;

    fprintf(f,
        "{\"v\":%d,\"type\":\"imp_gems\",\"tick\":%ld,\"imp_slot\":%d,"
        "\"pos_x\":%ld,\"pos_y\":%ld,\"active_state\":%d,"
        "\"instance_id\":%d,\"inst_turn\":%d,\"inst_action_turns\":%d,"
        "\"gold_carried\":%ld,"
        "\"task_repeats\":%d,\"last_did_job\":%d,\"task_stack_pos\":%d,"
        "\"thing_index\":%d,\"creation_turn\":%ld,\"random_seed\":%u,"
        "\"pile_exists\":%d,\"pile_gold_stored\":%ld,\"pile_sprite_size\":%d,\"pile_owner\":%d,"
        "\"worked_health\":%d,\"worked_kind\":%d,\"pad_health\":%d,\"pad_kind\":%d,"
        "\"task_count\":%d,\"digger_stack_length\":%lu}\n",
        IMP_GEMS_ORACLE_VERSION, (long)get_gameturn(), imp_slot,
        (long)imp->mappos.x.val, (long)imp->mappos.y.val, (int)imp->active_state,
        (int)cctrl->instance_id, (int)cctrl->inst_turn, (int)cctrl->inst_action_turns,
        (long)imp->creature.gold_carried,
        (int)cctrl->digger.task_repeats, (int)cctrl->digger.last_did_job, (int)cctrl->digger.task_stack_pos,
        (int)imp->index, (long)imp->creation_turn, (unsigned)imp->random_seed,
        pile_exists, pile_gold_stored, pile_sprite_size, pile_owner,
        (int)worked_slb->health, (int)worked_slb->kind, (int)pad_slb->health, (int)pad_slb->kind,
        (int)dungeon->task_count, (unsigned long)dungeon->digger_stack_length);
}

// Driver: on the first turn reveal the map, spawn imp 1, tag both gems tiles with the real player action,
// and open the JSONL; SECOND_IMP_SPAWN_DELAY_TURNS turns later spawn imp 2; every turn dump both spawned
// imps; stop once the safety cap trips.
FTestActionResult ftest_imp_gems_oracle_action__run(struct FTestActionArgs* const args)
{
    if (args->times_executed == 0)
    {
        const long level = (long)get_loaded_level_number();
        if (level != IMP_GEMS_ORACLE_LEVEL)
        {
            FTEST_FAIL_TEST("imp gems oracle: expected level %d (M-gems), got %ld — no gems seam to mine",
                IMP_GEMS_ORACLE_LEVEL, level);
            return FTRs_Go_To_Next_Action;
        }

        ftest_util_reveal_map(PLAYER0);
        gems_clear_creatures(NULL, NULL); // clear the map's starting imps first, as jobs_clear_creatures(NULL) does

        const MapCoord sx = subtile_coord_center(SPAWN_STL_X);
        const MapCoord sy = subtile_coord_center(SPAWN_STL_Y);
        imp_gems_1 = ftest_util_create_creature(sx, sy, PLAYER0, 1, get_players_special_digger_model(PLAYER0));
        if (thing_is_invalid(imp_gems_1))
        {
            FTEST_FAIL_TEST("imp gems oracle: could not spawn imp 1 at subtile (%d,%d)", SPAWN_STL_X, SPAWN_STL_Y);
            return FTRs_Go_To_Next_Action;
        }
        move_thing_in_map(imp_gems_1, &imp_gems_1->mappos);

        // Tag both GEMS tiles for mining with the real player action — the same GA_MarkDig the mine oracle
        // uses; tag_blocks_for_digging_in_area picks SDDigTask_MineGold for either because both carry
        // SlbAtFlg_Valuable (design/imp-jobs.md Correction 4). The WORKED tile is tagged FIRST so it lands
        // at shared-stack slot 0 (add_gems_to_imp_stack scans the task list in tag order) — the slot both
        // imps' very first check_out_imp_stack dispatch actually claims.
        game_action(PLAYER0, GA_MarkDig, 0,
            slab_subtile_center(WORKED_SLB_X), slab_subtile_center(WORKED_SLB_Y), 1, 1);
        game_action(PLAYER0, GA_MarkDig, 0,
            slab_subtile_center(PAD_SLB_X), slab_subtile_center(PAD_SLB_Y), 1, 1);
        struct Dungeon* dungeon = get_dungeon(PLAYER0);
        if (dungeon->task_count < 2)
        {
            FTEST_FAIL_TEST("imp gems oracle: GA_MarkDig did not register both gems tasks (task_count=%d)",
                (int)dungeon->task_count);
            delete_thing_structure(imp_gems_1, 0);
            imp_gems_1 = NULL;
            return FTRs_Go_To_Next_Action;
        }

        char path[512];
        snprintf(path, sizeof(path), "%s/oracle_imp_gems.jsonl", imp_gems_out_dir());
        imp_gems_jsonl = fopen(path, "w");
        if (imp_gems_jsonl == NULL)
        {
            FTEST_FRAMEWORK_ABORT("imp gems oracle: failed to open '%s'", path);
            delete_thing_structure(imp_gems_1, 0);
            imp_gems_1 = NULL;
            return FTRs_Go_To_Next_Action;
        }
        FTESTLOG("imp gems oracle: writing '%s'", path);

        const char* prov_build  = getenv("KEEPERFX_ORACLE_BUILD");
        fprintf(imp_gems_jsonl,
            "{\"v\":%d,\"type\":\"meta\",\"probe\":\"imp_gems\",\"level\":%d,\"campaign\":\"classic\","
            "\"worked_slb_x\":%d,\"worked_slb_y\":%d,\"pad_slb_x\":%d,\"pad_slb_y\":%d,"
            "\"spawn_stl_x\":%d,\"spawn_stl_y\":%d,\"second_imp_spawn_delay_turns\":%d,"
            "\"engine\":\"keeperfx-oracle-dumps\",\"version\":\"%s\",\"build\":\"%s\"}\n",
            IMP_GEMS_ORACLE_VERSION, IMP_GEMS_ORACLE_LEVEL, WORKED_SLB_X, WORKED_SLB_Y, PAD_SLB_X, PAD_SLB_Y,
            SPAWN_STL_X, SPAWN_STL_Y, SECOND_IMP_SPAWN_DELAY_TURNS, VER_STRING,
            prov_build ? prov_build : "");
    }

    // Cull any heart-generated imps every tick — before either spawning imp 2 or dumping — so only the
    // tracked imp(s) ever touch the shared digger stack (see gems_clear_creatures's own comment).
    gems_clear_creatures(imp_gems_1, imp_gems_2);

    if (imp_gems_2 == NULL && (long)args->times_executed == SECOND_IMP_SPAWN_DELAY_TURNS)
    {
        const MapCoord sx = subtile_coord_center(SPAWN_STL_X);
        const MapCoord sy = subtile_coord_center(SPAWN_STL_Y);
        imp_gems_2 = ftest_util_create_creature(sx, sy, PLAYER0, 1, get_players_special_digger_model(PLAYER0));
        if (thing_is_invalid(imp_gems_2))
        {
            FTEST_FAIL_TEST("imp gems oracle: could not spawn imp 2 at subtile (%d,%d)", SPAWN_STL_X, SPAWN_STL_Y);
            return FTRs_Go_To_Next_Action;
        }
        move_thing_in_map(imp_gems_2, &imp_gems_2->mappos);
    }

    imp_gems_dump(imp_gems_jsonl, imp_gems_1, 1);
    if (imp_gems_2 != NULL)
        imp_gems_dump(imp_gems_jsonl, imp_gems_2, 2);

    if ((long)args->times_executed >= GEMS_MAX_TICKS)
    {
        FTESTLOG("imp gems oracle: dump complete (%ld ticks)", (long)args->times_executed + 1);
        fclose(imp_gems_jsonl);
        imp_gems_jsonl = NULL;
        delete_thing_structure(imp_gems_1, 0);
        imp_gems_1 = NULL;
        if (imp_gems_2 != NULL)
        {
            delete_thing_structure(imp_gems_2, 0);
            imp_gems_2 = NULL;
        }
        return FTRs_Go_To_Next_Action;
    }
    return FTRs_Repeat_Current_Action;
}

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
