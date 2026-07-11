#include "ftest_creature_state_slap.h"

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
#include "../../thing_creature.h"
#include "../../thing_navigate.h"
#include "../../creature_control.h"
#include "../../config_creature.h"
#include "../../creature_states.h"
#include "../../magic_powers.h"
#include "../../player_data.h"
#include "../../player_instances.h"
#include "../../map_data.h"
#include "../../slab_data.h"
#include "../../ver_defs.h"

#include "../../post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif

// Oracle dump format version — bump on any layout/meaning change (keeper-rx/docs/oracle/dump-format.md).
#define CREATURE_STATE_SLAP_ORACLE_VERSION 1

// The imp stands on a claimed, flat, open-ground pad we lay down ourselves, so the scene is independent of
// whatever the loaded map holds under the spawn tile — a cowering creature never moves, so the pad only has
// to be a valid claimed floor to spawn on. The spawn slab sits inside the pad.
#define PAD_SLB_MIN 20
#define PAD_SLB_MAX 28
#define SPAWN_SLB 24

// The whole cower runs 18 turns (cowers_from_slap_turns = 18) then restores on the tick the timer hits 0,
// so 18 integration steps reach the restore (records dumped = 18 + 1 = 19, record 0 being the just-slapped
// state). https://github.com/dkfans/keeperfx/blob/v1.4.0/src/magic_powers.c#L640
#define SLAP_DUMP_TURNS 18

static FILE* slap_jsonl = NULL;
static struct Thing* slap_imp = NULL;

// forward declarations
FTestActionResult ftest_creature_state_slap_action__pad_setup(struct FTestActionArgs* const args);
FTestActionResult ftest_creature_state_slap_action__run(struct FTestActionArgs* const args);

TbBool ftest_creature_state_slap_init()
{
    ftest_append_action(ftest_creature_state_slap_action__pad_setup, 8, NULL);
    ftest_append_action(ftest_creature_state_slap_action__run, 0, NULL);
    return true;
}

// Resolve the dump directory: $KEEPERFX_ORACLE_OUT, else the CWD (mirrors the movement oracle so both share
// the capture-script convention).
static const char* slap_out_dir(void)
{
    const char* dir = getenv("KEEPERFX_ORACLE_OUT");
    if (dir == NULL || dir[0] == '\0')
        return ".";
    mkdir(dir, 0755); // best-effort; ignore EEXIST
    return dir;
}

static FILE* slap_open(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/oracle_creature_state_slap.jsonl", slap_out_dir());
    FILE* f = fopen(path, "w");
    if (f == NULL)
        FTESTLOG("slap oracle: failed to open '%s'", path);
    else
        FTESTLOG("slap oracle: writing '%s'", path);
    return f;
}

// One per-turn ground-truth record: only the two fields the keeper-rx slice reproduces — active_state and
// the cower timer — so the slap's deferred damage/anger/speed side-effects never pollute the diff. Written
// BEFORE update_thing runs, so record 0 is the just-slapped state and each later record is the state left by
// one update.
static void slap_dump(FILE* f, struct Thing* imp)
{
    struct CreatureControl* cctrl = creature_control_get_from_thing(imp);
    fprintf(f,
        "{\"v\":%d,\"type\":\"creature_state\",\"tick\":%ld,"
        "\"active_state\":%d,\"cowers_from_slap_turns\":%d}\n",
        CREATURE_STATE_SLAP_ORACLE_VERSION, (long)get_gameturn(),
        (int)imp->active_state, (int)cctrl->cowers_from_slap_turns);
}

// One-time scene: reveal the map and stamp a claimed, flat pad the imp stands on.
FTestActionResult ftest_creature_state_slap_action__pad_setup(struct FTestActionArgs* const args)
{
    (void)args;
    ftest_util_reveal_map(PLAYER0);
    if (!ftest_util_replace_slabs(PAD_SLB_MIN, PAD_SLB_MIN, PAD_SLB_MAX, PAD_SLB_MAX, SlbT_CLAIMED, PLAYER0))
    {
        FTEST_FAIL_TEST("slap oracle: failed to lay the claimed open-ground pad");
        return FTRs_Go_To_Next_Action;
    }
    return FTRs_Go_To_Next_Action;
}

// Driver: on the first turn spawn the imp, slap it once (the real slap_creature), and dump the just-slapped
// state; every turn dump; after SLAP_DUMP_TURNS updates (through the restore) close the file and remove the
// imp.
FTestActionResult ftest_creature_state_slap_action__run(struct FTestActionArgs* const args)
{
    if (args->times_executed == 0)
    {
        MapCoord x = subtile_coord_center(slab_subtile_center(SPAWN_SLB));
        MapCoord y = subtile_coord_center(slab_subtile_center(SPAWN_SLB));
        slap_imp = ftest_util_create_creature(x, y, PLAYER0, 1, get_players_special_digger_model(PLAYER0));
        if (thing_is_invalid(slap_imp))
        {
            FTEST_FAIL_TEST("slap oracle: could not spawn the imp on the pad");
            return FTRs_Go_To_Next_Action;
        }
        // Settle the floor under the spawn tile, then apply the real slap — active_state becomes
        // CrSt_CreatureSlapCowers, the state pair is backed up, and cowers_from_slap_turns is set to 18.
        move_thing_in_map(slap_imp, &slap_imp->mappos);
        slap_creature(get_player(PLAYER0), slap_imp);

        slap_jsonl = slap_open();
        if (slap_jsonl == NULL)
        {
            FTEST_FRAMEWORK_ABORT("slap oracle: could not open the JSONL output");
            return FTRs_Go_To_Next_Action;
        }
        const char* prov_build  = getenv("KEEPERFX_ORACLE_BUILD");
        fprintf(slap_jsonl,
            "{\"v\":%d,\"type\":\"meta\",\"probe\":\"creature_state_slap\",\"cower_turns\":%d,"
            "\"engine\":\"keeperfx-oracle-dumps\",\"version\":\"%s\",\"build\":\"%s\"}\n",
            CREATURE_STATE_SLAP_ORACLE_VERSION, SLAP_DUMP_TURNS, VER_STRING,
            prov_build ? prov_build : "");
    }

    slap_dump(slap_jsonl, slap_imp);

    if (args->times_executed < SLAP_DUMP_TURNS)
        return FTRs_Repeat_Current_Action;

    fclose(slap_jsonl);
    slap_jsonl = NULL;
    FTESTLOG("slap oracle: dump complete (%d records)", SLAP_DUMP_TURNS + 1);
    delete_thing_structure(slap_imp, 0);
    slap_imp = NULL;
    return FTRs_Go_To_Next_Action;
}

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
