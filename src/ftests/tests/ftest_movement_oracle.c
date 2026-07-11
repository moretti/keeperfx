#include "ftest_movement_oracle.h"

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
#include "../../player_instances.h"
#include "../../map_data.h"
#include "../../slab_data.h"
#include "../../ver_defs.h"

#include "../../post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif

// Oracle dump format version — bump on any layout/meaning change (keeper-rx/docs/oracle/dump-format.md).
#define MOVEMENT_ORACLE_VERSION 1

// The imps stand on a claimed, flat, open-ground pad we lay down ourselves, so the scene is a physics
// vacuum independent of whatever the loaded map happens to hold under the spawn tiles: no walls to slide
// on, no lava/water to nudge the creature, a single uniform floor height for the whole run. The pad spans
// [PAD_SLB_MIN..PAD_SLB_MAX] in both axes — wide enough that the walk never leaves it.
#define PAD_SLB_MIN 20
#define PAD_SLB_MAX 44

// The imp is driven at its base speed (imp.cfg BaseSpeed=96) — coord-units/tick, and the exact walk-step
// magnitude. https://github.com/dkfans/keeperfx/blob/v1.4.0/src/thing_stats.c#L744-L767
#define IMP_MOVE_SPEED 96

// Freeze the imp's AI without touching the physics: a large stopped_for_hand_turns makes update_creature
// skip process_creature_state (the nav/wander that would otherwise set move_speed), while leaving
// stateblock_flags == 0 so update_creature_movements still builds the walk accel from the move_speed we
// set. https://github.com/dkfans/keeperfx/blob/v1.4.0/src/thing_creature.c#L6389-L6398  https://github.com/dkfans/keeperfx/blob/v1.4.0/src/thing_creature.c#L5982-L6010
#define FREEZE_HAND_TURNS 30000

// One nav-free/AI-free/collision-free run: a fresh imp posed with a single initial condition, then dumped
// per turn while it is driven by a constant move_speed. The three below isolate gravity, an impulse, and a
// driven walk respectively (movement.md "Oracle strategy").
struct movement_scenario
{
    const char* name;           // scenario tag, also the JSONL basename suffix
    MapSlabCoord slb_x, slb_y;   // spawn slab (its centre subtile), inside the pad
    int lift_subtiles;          // fall: start this many subtiles above the floor (0 = grounded)
    long push_once_x;           // coast: one-shot veloc_push_once.x (0 = none)
    short move_speed;           // walk: constant driven speed each turn (0 = undriven)
    short move_angle_xy;        // walk: facing the speed is projected along
    GameTurn dump_turns;        // integration steps to observe (records dumped = dump_turns + 1)
};

static const struct movement_scenario movement_scenarios[] = {
    // Fall: lifted, undriven — gravity (fall_acceleration=32), air friction (inertia_air=8), the per-axis
    // clamp and the floor-clamp landing.
    { .name = "fall",  .slb_x = 24, .slb_y = 24, .lift_subtiles = 5, .push_once_x = 0,
      .move_speed = 0,             .move_angle_xy = ANGLE_NORTH,     .dump_turns = 14 },
    // Coast: a one-shot impulse past the ±256 clamp, undriven — the clamp caps the first step and floor
    // friction (inertia_floor=32) shows the impulse never persists (it folds into velocity, not veloc_base).
    { .name = "coast", .slb_x = 32, .slb_y = 32, .lift_subtiles = 0, .push_once_x = 300,
      .move_speed = 0,             .move_angle_xy = ANGLE_NORTH,     .dump_turns = 6 },
    // Walk: driven at base speed along a diagonal so both the sin and cos of the accel vector are exercised
    // (a straight line re-derived fresh each tick).
    { .name = "walk",  .slb_x = 36, .slb_y = 36, .lift_subtiles = 0, .push_once_x = 0,
      .move_speed = IMP_MOVE_SPEED, .move_angle_xy = ANGLE_NORTHEAST, .dump_turns = 8 },
};

#define MOVEMENT_SCENARIO_COUNT ((int)(sizeof(movement_scenarios)/sizeof(movement_scenarios[0])))

// The currently-open scenario dump and the imp being dumped, at module scope so the generic dump action
// (reused across all three scenarios via its data pointer) can carry state between its per-turn executions.
static FILE* movement_jsonl = NULL;
static struct Thing* movement_imp = NULL;

// forward declarations
FTestActionResult ftest_movement_oracle_action__pad_setup(struct FTestActionArgs* const args);
FTestActionResult ftest_movement_oracle_action__run_scenario(struct FTestActionArgs* const args);

TbBool ftest_movement_oracle_init()
{
    ftest_append_action(ftest_movement_oracle_action__pad_setup, 8, NULL);
    for (int i = 0; i < MOVEMENT_SCENARIO_COUNT; i++)
        ftest_append_action(ftest_movement_oracle_action__run_scenario, 0, (void*)&movement_scenarios[i]);
    return true;
}

// Resolve the directory the dump files are written to: $KEEPERFX_ORACLE_OUT, else the CWD (mirrors
// ftest_oracle_spike.c so both oracles share the capture-script convention).
static const char* movement_out_dir(void)
{
    const char* dir = getenv("KEEPERFX_ORACLE_OUT");
    if (dir == NULL || dir[0] == '\0')
        return ".";
    mkdir(dir, 0755); // best-effort; ignore EEXIST
    return dir;
}

static FILE* movement_open(const char* scenario_name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/oracle_movement_%s.jsonl", movement_out_dir(), scenario_name);
    FILE* f = fopen(path, "w");
    if (f == NULL)
        FTESTLOG("movement oracle: failed to open '%s'", path);
    else
        FTESTLOG("movement oracle: writing '%s'", path);
    return f;
}

// Hold the imp inert for one turn without disturbing the physics primitive: skip the AI, keep it mobile
// and grounded-flagged exactly as a freshly spawned imp (create_creature sets only TMvF_ZeroVerticalVelocity
// for a non-flyer), and apply this scenario's constant drive. Re-applied every turn because update_creature
// zeroes a non-controlled creature's move_speed at the end of each tick.
// https://github.com/dkfans/keeperfx/blob/v1.4.0/src/thing_creature.c#L6455-L6459
static void movement_apply_drive(struct Thing* imp, const struct movement_scenario* scn)
{
    struct CreatureControl* cctrl = creature_control_get_from_thing(imp);
    cctrl->stopped_for_hand_turns = FREEZE_HAND_TURNS; // suppress process_creature_state (nav/wander)
    cctrl->stateblock_flags = 0;                       // keep the driven-walk accel path live
    cctrl->creature_state_flags &= ~TF2_CreatureIsMoving;
    imp->movement_flags = TMvF_ZeroVerticalVelocity;   // not flying, not immobile, no magic-fall bounce
    cctrl->move_speed = scn->move_speed;
    imp->move_angle_xy = scn->move_angle_xy;
}

// One per-turn ground-truth record: mappos + the four velocity fields + the floor and the drive that will
// integrate this turn. Written BEFORE update_thing runs, so a record holds the state left by the previous
// turn's integration (record 0 is the posed initial state); the keeper-rx diff seeds record 0 and replays.
static void movement_dump(FILE* f, const struct movement_scenario* scn, struct Thing* imp)
{
    struct CreatureControl* cctrl = creature_control_get_from_thing(imp);
    fprintf(f,
        "{\"v\":%d,\"type\":\"movement\",\"scenario\":\"%s\",\"tick\":%ld,"
        "\"pos_x\":%d,\"pos_y\":%d,\"pos_z\":%d,\"floor_height\":%d,"
        "\"veloc_base_x\":%d,\"veloc_base_y\":%d,\"veloc_base_z\":%d,"
        "\"veloc_push_add_x\":%d,\"veloc_push_add_y\":%d,\"veloc_push_add_z\":%d,"
        "\"veloc_push_once_x\":%d,\"veloc_push_once_y\":%d,\"veloc_push_once_z\":%d,"
        "\"velocity_x\":%d,\"velocity_y\":%d,\"velocity_z\":%d,"
        "\"move_speed\":%d,\"move_angle_xy\":%d,\"state_flags\":%d,\"movement_flags\":%d,"
        "\"fall_acceleration\":%d,\"inertia_floor\":%d,\"inertia_air\":%d}\n",
        MOVEMENT_ORACLE_VERSION, scn->name, (long)get_gameturn(),
        (int)imp->mappos.x.val, (int)imp->mappos.y.val, (int)imp->mappos.z.val, (int)imp->floor_height,
        (int)imp->veloc_base.x.val, (int)imp->veloc_base.y.val, (int)imp->veloc_base.z.val,
        (int)imp->veloc_push_add.x.val, (int)imp->veloc_push_add.y.val, (int)imp->veloc_push_add.z.val,
        (int)imp->veloc_push_once.x.val, (int)imp->veloc_push_once.y.val, (int)imp->veloc_push_once.z.val,
        (int)imp->velocity.x.val, (int)imp->velocity.y.val, (int)imp->velocity.z.val,
        (int)cctrl->move_speed, (int)imp->move_angle_xy, (int)imp->state_flags, (int)imp->movement_flags,
        (int)imp->fall_acceleration, (int)imp->inertia_floor, (int)imp->inertia_air);
}

// One-time scene: reveal the map and stamp a wide claimed, flat pad the imps run on.
FTestActionResult ftest_movement_oracle_action__pad_setup(struct FTestActionArgs* const args)
{
    (void)args;
    ftest_util_reveal_map(PLAYER0);
    if (!ftest_util_replace_slabs(PAD_SLB_MIN, PAD_SLB_MIN, PAD_SLB_MAX, PAD_SLB_MAX, SlbT_CLAIMED, PLAYER0))
    {
        FTEST_FAIL_TEST("movement oracle: failed to lay the claimed open-ground pad");
        return FTRs_Go_To_Next_Action;
    }
    return FTRs_Go_To_Next_Action;
}

// Generic per-scenario driver: on its first turn spawn+pose the imp and dump the initial state; every turn
// re-apply the drive and dump; after dump_turns integration steps close the file and remove the imp so the
// next scenario runs in isolation. The scenario is passed by pointer via the action's data slot.
FTestActionResult ftest_movement_oracle_action__run_scenario(struct FTestActionArgs* const args)
{
    const struct movement_scenario* scn = (const struct movement_scenario*)args->data;

    if (args->times_executed == 0)
    {
        MapCoord x = subtile_coord_center(slab_subtile_center(scn->slb_x));
        MapCoord y = subtile_coord_center(slab_subtile_center(scn->slb_y));
        movement_imp = ftest_util_create_creature(x, y, PLAYER0, 1, get_players_special_digger_model(PLAYER0));
        if (thing_is_invalid(movement_imp))
        {
            FTEST_FAIL_TEST("movement oracle[%s]: could not spawn the imp on the pad", scn->name);
            return FTRs_Go_To_Next_Action;
        }
        // Settle the floor under the spawn tile, then pose the single initial condition for this scenario.
        move_thing_in_map(movement_imp, &movement_imp->mappos);
        movement_apply_drive(movement_imp, scn);
        movement_imp->veloc_base.x.val = movement_imp->veloc_base.y.val = movement_imp->veloc_base.z.val = 0;
        movement_imp->veloc_push_add.x.val = movement_imp->veloc_push_add.y.val = movement_imp->veloc_push_add.z.val = 0;
        movement_imp->veloc_push_once.x.val = movement_imp->veloc_push_once.y.val = movement_imp->veloc_push_once.z.val = 0;
        movement_imp->velocity.x.val = movement_imp->velocity.y.val = movement_imp->velocity.z.val = 0;
        movement_imp->state_flags &= ~(TF1_PushAdd | TF1_PushOnce);
        if (scn->lift_subtiles > 0)
            movement_imp->mappos.z.val = movement_imp->floor_height + scn->lift_subtiles * COORD_PER_STL;
        if (scn->push_once_x != 0)
        {
            movement_imp->veloc_push_once.x.val = scn->push_once_x;
            set_flag(movement_imp->state_flags, TF1_PushOnce);
        }

        movement_jsonl = movement_open(scn->name);
        if (movement_jsonl == NULL)
        {
            FTEST_FRAMEWORK_ABORT("movement oracle[%s]: could not open the JSONL output", scn->name);
            return FTRs_Go_To_Next_Action;
        }
        // Provenance echo so a stray dump is regeneratable from its meta line alone (matches oracle_spike).
        const char* prov_build  = getenv("KEEPERFX_ORACLE_BUILD");
        fprintf(movement_jsonl,
            "{\"v\":%d,\"type\":\"meta\",\"probe\":\"movement\",\"scenario\":\"%s\",\"move_speed\":%d,"
            "\"move_angle_xy\":%d,\"engine\":\"keeperfx-oracle-dumps\",\"version\":\"%s\","
            "\"build\":\"%s\"}\n",
            MOVEMENT_ORACLE_VERSION, scn->name, (int)scn->move_speed, (int)scn->move_angle_xy, VER_STRING,
            prov_build ? prov_build : "");
    }
    else
    {
        movement_apply_drive(movement_imp, scn);
    }

    movement_dump(movement_jsonl, scn, movement_imp);

    if (args->times_executed < scn->dump_turns)
        return FTRs_Repeat_Current_Action;

    // Scenario complete: close its dump and delete the imp so the next scenario is fully isolated.
    fclose(movement_jsonl);
    movement_jsonl = NULL;
    FTESTLOG("movement oracle[%s]: dump complete (%ld records)", scn->name, (long)scn->dump_turns + 1);
    delete_thing_structure(movement_imp, 0);
    movement_imp = NULL;
    return FTRs_Go_To_Next_Action;
}

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
