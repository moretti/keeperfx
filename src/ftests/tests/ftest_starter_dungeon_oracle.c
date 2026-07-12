#include "ftest_starter_dungeon_oracle.h"

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
#include "../../thing_objects.h"
#include "../../map_blocks.h"
#include "../../map_data.h"
#include "../../slab_data.h"
#include "../../light_data.h"
#include "../../player_data.h"
#include "../../player_instances.h"
#include "../../ver_defs.h"

#include "../../post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif

// Oracle dump format version — bump on any layout/meaning change (keeper-rx/docs/oracle/dump-format.md).
#define STARTER_DUNGEON_ORACLE_VERSION 1

// The starter-dungeon fixture (tests/.../Authoring/StarterDungeon.cs, emitted to reference/pathfinding-oracle).
#define STARTER_DUNGEON_ORACLE_LEVEL 9008

// Give the map loader a few turns to build the .tng things (the heart + its designer torches) before we
// census — nothing here waits on AI, so a short settle is all we need.
#define SETTLE_TURNS 4

// The east "new room" is a 3x3 Player0-claimed pad at slabs 14..16 x 14..16 (StarterDungeon.cs). Its four
// mid-edge earth neighbours lie on the %5 torch grid facing that claimed floor, so reinforcing each becomes a
// WallTorch bearing one slab-object torch — one per orientation, the multi-facing set Flag A needs.
#define EAST_WALL_N_X 15
#define EAST_WALL_N_Y 13
#define EAST_WALL_S_X 15
#define EAST_WALL_S_Y 17
#define EAST_WALL_W_X 13
#define EAST_WALL_W_Y 15
#define EAST_WALL_E_X 17
#define EAST_WALL_E_Y 15

// A pristine designer .tng torch wall: the heart room's north mid-edge wall (heart at slab 10,10; its 5x5
// claimed border is walled at 7..13, so the north torch wall is (10,7)).
#define HEART_TORCH_WALL_X 10
#define HEART_TORCH_WALL_Y 7

// The east pad's west-of-(17,15) claimed floor: re-claiming it re-runs place_slab_type_on_map(SlbT_CLAIMED),
// whose FortifiedGround category promotes the plain-earth wall (17,15) — on the %5 grid facing this floor —
// to SlbT_TORCHDIRT (an unreinforced dirt wall that carries a torch), so the torch appears on CLAIM, before
// any reinforcement. (17,15)'s West-facing torch lands in this floor slab (16,15). map_blocks.c#L1729-L1745.
#define CLAIM_FLOOR_X 16
#define CLAIM_FLOOR_Y 15

// Resolve the dump directory: $KEEPERFX_ORACLE_OUT, else CWD (mirrors the other oracle ftests).
static const char* starter_dungeon_out_dir(void)
{
    const char* dir = getenv("KEEPERFX_ORACLE_OUT");
    if (dir == NULL || dir[0] == '\0')
        return ".";
    mkdir(dir, 0755); // best-effort; ignore EEXIST
    return dir;
}

// Census every torch object (TCls_Object model ObjMdl_Torch) currently alive, tagged with this phase, as one
// jsonl line each; a "phase" marker line precedes them so the keeper-rx diff can group by phase. Fields are
// read straight from the C so the diff is auditable:
//   slb_x/slb_y            the slab the torch sits in    (subtile_slab(mappos >> 8))
//   pos_x/pos_y/pos_z      thing->mappos.{x,y,z}.val     (fixed-point, 256/subtile)
//   owner                  thing->owner
//   parent_idx             thing->parent_idx             (slab-object torch: == get_slab_number(owning wall); .tng: own idx)
//   parent_slb_x/y         slb_num_decode(parent_idx)    (a slab-object torch is parented to its WALL slab, which
//                                                          differs from the slab it sits in when an offset pushes
//                                                          the torch into the neighbour; a .tng torch decodes to a
//                                                          bogus slab, since its parent is its own thing index)
//   light_id               thing->light_id               (0 == none)
//   intensity/range/radius/lflags   the attached Light's fields (only when light_id is an allocated light)
// Positions/attrs are the identity the keeper-rx diff compares — never DK's light/thing index numbering.
static void starter_dungeon_census(FILE* f, const char* phase, const char* event)
{
    fprintf(f, "{\"v\":%d,\"type\":\"phase\",\"phase\":\"%s\",\"event\":\"%s\"}\n",
        STARTER_DUNGEON_ORACLE_VERSION, phase, event);

    struct StructureList* slist = get_list_for_thing_class(TCls_Object);
    if (slist == NULL)
        return;
    long i = slist->index;
    unsigned long guard = 0;
    while (i != 0)
    {
        struct Thing* thing = thing_get(i);
        if (thing_is_invalid(thing))
            break;
        i = thing->next_of_class;
        if (++guard > slist->count + 1)
            break;
        if (thing->model != ObjMdl_Torch)
            continue;

        const MapSubtlCoord stl_x = thing->mappos.x.val >> 8;
        const MapSubtlCoord stl_y = thing->mappos.y.val >> 8;
        const MapSlabCoord slb_x = subtile_slab(stl_x);
        const MapSlabCoord slb_y = subtile_slab(stl_y);
        const MapSlabCoord parent_slb_x = slb_num_decode_x(thing->parent_idx);
        const MapSlabCoord parent_slb_y = slb_num_decode_y(thing->parent_idx);

        int intensity = -1, range = -1, radius = -1, lflags = -1;
        if (thing->light_id != 0)
        {
            struct Light* lgt = &game.lish.lights[thing->light_id];
            if ((lgt->flags & LgtF_Allocated) != 0)
            {
                intensity = (int)lgt->intensity;
                range = (int)lgt->range;
                radius = (int)lgt->radius;
                lflags = (int)lgt->flags;
            }
        }

        fprintf(f,
            "{\"v\":%d,\"type\":\"torch\",\"phase\":\"%s\","
            "\"slb_x\":%d,\"slb_y\":%d,\"pos_x\":%ld,\"pos_y\":%ld,\"pos_z\":%ld,"
            "\"owner\":%d,\"parent_idx\":%d,\"parent_slb_x\":%d,\"parent_slb_y\":%d,"
            "\"light_id\":%d,\"intensity\":%d,\"range\":%d,\"radius\":%d,\"lflags\":%d}\n",
            STARTER_DUNGEON_ORACLE_VERSION, phase,
            (int)slb_x, (int)slb_y,
            (long)thing->mappos.x.val, (long)thing->mappos.y.val, (long)thing->mappos.z.val,
            (int)thing->owner, (int)thing->parent_idx, (int)parent_slb_x, (int)parent_slb_y,
            (int)thing->light_id, intensity, range, radius, lflags);
    }
}

// Reinforce one earth wall to its pretty (torch) type the way place_and_process_pretty_wall_slab does, minus
// the fill_in_reinforced_corners phase keeper-rx defers: choose the pretty type, then place it — which runs
// place_single_slab_type_on_map (delete-attached-things + place_slab_objects), spawning the torch.
static void starter_dungeon_reinforce(MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    const SlabKind pretty = choose_pretty_type(PLAYER0, slb_x, slb_y);
    place_slab_type_on_map(pretty, slab_subtile_center(slb_x), slab_subtile_center(slb_y), PLAYER0, 0);
}

// forward declaration
FTestActionResult ftest_starter_dungeon_oracle_action__run(struct FTestActionArgs* const args);

TbBool ftest_starter_dungeon_oracle_init()
{
    // One deterministic action, delayed a few turns so the map's .tng things exist before we census.
    ftest_append_action(ftest_starter_dungeon_oracle_action__run, SETTLE_TURNS, NULL);
    return true;
}

FTestActionResult ftest_starter_dungeon_oracle_action__run(struct FTestActionArgs* const args)
{
    const long level = (long)get_loaded_level_number();
    if (level != STARTER_DUNGEON_ORACLE_LEVEL)
    {
        FTEST_FAIL_TEST("starter dungeon oracle: expected level %d (M-start), got %ld",
            STARTER_DUNGEON_ORACLE_LEVEL, level);
        return FTRs_Go_To_Next_Action;
    }

    // Reveal so the reinforce/dig primitives and their flood-fill neighbours see the whole map (mirrors the
    // other oracles); it does not affect the terrain-object mechanism itself.
    ftest_util_reveal_map(PLAYER0);

    char path[512];
    snprintf(path, sizeof(path), "%s/oracle_starter_dungeon_torch.jsonl", starter_dungeon_out_dir());
    FILE* f = fopen(path, "w");
    if (f == NULL)
    {
        FTEST_FRAMEWORK_ABORT("starter dungeon oracle: failed to open '%s'", path);
        return FTRs_Go_To_Next_Action;
    }
    FTESTLOG("starter dungeon oracle: writing '%s'", path);

    const char* prov_build = getenv("KEEPERFX_ORACLE_BUILD");
    fprintf(f,
        "{\"v\":%d,\"type\":\"meta\",\"probe\":\"starter_dungeon_torch\",\"level\":%d,\"campaign\":\"classic\","
        "\"engine\":\"keeperfx-oracle-dumps\",\"version\":\"%s\",\"build\":\"%s\"}\n",
        STARTER_DUNGEON_ORACLE_VERSION, STARTER_DUNGEON_ORACLE_LEVEL, VER_STRING,
        prov_build ? prov_build : "");

    // Phase "load": the designer-placed .tng torches present at map load (the heart room's four grid walls).
    starter_dungeon_census(f, "load", "designer .tng torches at map load");

    // Phase "claim": re-claim the east pad floor (16,15) next to the still-plain-earth wall (17,15). The
    // FortifiedGround neighbour re-kind promotes (17,15) EARTH->TORCHDIRT (torch_flags_for_slab != 0), and the
    // neighbour re-bake places its torch — the torch appears on CLAIM, with no reinforcement. Runs before
    // "reinforce" so (17,15) is still earth; reinforce later re-places it as WallTorch (same torch).
    place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(CLAIM_FLOOR_X), slab_subtile_center(CLAIM_FLOOR_Y), PLAYER0, 0);
    starter_dungeon_census(f, "claim", "claimed floor (16,15); earth wall (17,15) promoted to TorchDirt, torch appears");

    // Phase "reinforce": wall the east room's four mid-edge earth neighbours — one fresh slab-object torch per
    // orientation (N/S/E/W), for the Flag-A per-facing world-position diff.
    starter_dungeon_reinforce(EAST_WALL_N_X, EAST_WALL_N_Y);
    starter_dungeon_reinforce(EAST_WALL_S_X, EAST_WALL_S_Y);
    starter_dungeon_reinforce(EAST_WALL_W_X, EAST_WALL_W_Y);
    starter_dungeon_reinforce(EAST_WALL_E_X, EAST_WALL_E_Y);
    starter_dungeon_census(f, "reinforce", "four east-room walls reinforced to WallTorch, one torch per facing");

    // Phase "dig_slabobj": dig the one freshly-reinforced WallTorch that actually bears a torch — the east wall
    // (17,15), whose West-facing torch sits in neighbour slab (16,15) but is parented to (17,15). Its
    // slab-object torch (+ light) must be removed with the wall (symptom 1 for slab-object torches). The
    // parity rule leaves (15,13)/(13,15) torchless, so digging one of those would remove nothing.
    dig_out_block(slab_subtile_center(EAST_WALL_E_X), slab_subtile_center(EAST_WALL_E_Y), PLAYER0);
    starter_dungeon_census(f, "dig_slabobj", "dug the east reinforced WallTorch (17,15); its torch sat at (16,15)");

    // Phase "dig_tng": dig a pristine .tng heart torch wall — does DK remove a designer torch? (Step 0: a .tng
    // torch's parent_idx is its own thing index, so delete_attached_things should leave it floating.)
    dig_out_block(slab_subtile_center(HEART_TORCH_WALL_X), slab_subtile_center(HEART_TORCH_WALL_Y), PLAYER0);
    starter_dungeon_census(f, "dig_tng", "dug the heart north .tng torch wall (10,7)");

    fclose(f);
    FTESTLOG("starter dungeon oracle: dump complete");
    return FTRs_Go_To_Next_Action;
}

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
