#include "ftest_dig_shuffle_oracle.h"

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
#define DIG_SHUFFLE_ORACLE_VERSION 1

// The dig-shuffle fixture (keeper-rx tests/.../Authoring/DigShuffleScene.cs, emitted to reference/pathfinding-oracle).
#define DIG_SHUFFLE_ORACLE_LEVEL 9010

// Give the map loader a few turns to build the .tng things (the three designer objects) before we census —
// nothing here waits on AI, so a short settle is all we need.
#define SETTLE_TURNS 4

// The earth slab holding the three unattached designer objects (DigShuffleScene.cs), away from the dungeon
// heart so digging it can't touch anything else.
#define DIG_SLAB_X 40
#define DIG_SLAB_Y 42

// Object models census-eligible for this oracle: the three designer objects DigShuffleScene places on
// DIG_SLAB_X/Y, one per Persistence category (config/fxdata/objects.cfg). ObjMdl_Torch and ObjMdl_GoldChest
// are declared in thing_objects.h's ObjectModels enum; ObjMdl_Barrel is not (that enum only lists models
// referenced elsewhere in the C — see its "avoid using this enum for new stuff" comment), so it is added
// here from the [object1] Name=BARREL row of the same config file.
#define ObjMdl_Barrel 1

// Resolve the dump directory: $KEEPERFX_ORACLE_OUT, else CWD (mirrors the other oracle ftests).
static const char* dig_shuffle_out_dir(void)
{
    const char* dir = getenv("KEEPERFX_ORACLE_OUT");
    if (dir == NULL || dir[0] == '\0')
        return ".";
    mkdir(dir, 0755); // best-effort; ignore EEXIST
    return dir;
}

// Census every TCls_Object thing whose model is one of the three DigShuffleScene designer objects (Barrel,
// Torch, GoldChest) currently alive, tagged with this phase, as one jsonl line each; a "phase" marker line
// precedes them so the keeper-rx diff can group by phase. Other object models (the far-away dungeon heart,
// dig rubble, ...) are skipped. Fields are read straight from the C so the diff is auditable:
//   model                  thing->model                  (1 Barrel / 2 Torch / 3 GoldChest)
//   slb_x/slb_y            the slab the object sits in    (subtile_slab(mappos >> 8))
//   pos_x/pos_y/pos_z      thing->mappos.{x,y,z}.val      (fixed-point, 256/subtile)
//   owner                  thing->owner
//   light_id               thing->light_id                (0 == none)
//   intensity/range/radius/lflags   the attached Light's fields (only when light_id is an allocated light)
// Positions/attrs are the identity the keeper-rx diff compares — never DK's light/thing index numbering.
static void dig_shuffle_census(FILE* f, const char* phase, const char* event)
{
    fprintf(f, "{\"v\":%d,\"type\":\"phase\",\"phase\":\"%s\",\"event\":\"%s\"}\n",
        DIG_SHUFFLE_ORACLE_VERSION, phase, event);

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
        if (thing->model != ObjMdl_Barrel && thing->model != ObjMdl_Torch && thing->model != ObjMdl_GoldChest)
            continue;

        const MapSubtlCoord stl_x = thing->mappos.x.val >> 8;
        const MapSubtlCoord stl_y = thing->mappos.y.val >> 8;
        const MapSlabCoord slb_x = subtile_slab(stl_x);
        const MapSlabCoord slb_y = subtile_slab(stl_y);

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
            "{\"v\":%d,\"type\":\"object\",\"phase\":\"%s\",\"model\":%d,"
            "\"slb_x\":%d,\"slb_y\":%d,\"pos_x\":%ld,\"pos_y\":%ld,\"pos_z\":%ld,"
            "\"owner\":%d,\"light_id\":%d,\"intensity\":%d,\"range\":%d,\"radius\":%d,\"lflags\":%d}\n",
            DIG_SHUFFLE_ORACLE_VERSION, phase, (int)thing->model,
            (int)slb_x, (int)slb_y,
            (long)thing->mappos.x.val, (long)thing->mappos.y.val, (long)thing->mappos.z.val,
            (int)thing->owner, (int)thing->light_id, intensity, range, radius, lflags);
    }
}

// forward declaration
FTestActionResult ftest_dig_shuffle_oracle_action__run(struct FTestActionArgs* const args);

TbBool ftest_dig_shuffle_oracle_init()
{
    // One deterministic action, delayed a few turns so the map's .tng things exist before we census.
    ftest_append_action(ftest_dig_shuffle_oracle_action__run, SETTLE_TURNS, NULL);
    return true;
}

FTestActionResult ftest_dig_shuffle_oracle_action__run(struct FTestActionArgs* const args)
{
    const long level = (long)get_loaded_level_number();
    if (level != DIG_SHUFFLE_ORACLE_LEVEL)
    {
        FTEST_FAIL_TEST("dig shuffle oracle: expected level %d (DigShuffleScene), got %ld",
            DIG_SHUFFLE_ORACLE_LEVEL, level);
        return FTRs_Go_To_Next_Action;
    }

    // Reveal so the dig primitive and its flood-fill neighbours see the whole map (mirrors the other
    // oracles); it does not affect the shuffle mechanism itself.
    ftest_util_reveal_map(PLAYER0);

    char path[512];
    snprintf(path, sizeof(path), "%s/oracle_dig_shuffle.jsonl", dig_shuffle_out_dir());
    FILE* f = fopen(path, "w");
    if (f == NULL)
    {
        FTEST_FRAMEWORK_ABORT("dig shuffle oracle: failed to open '%s'", path);
        return FTRs_Go_To_Next_Action;
    }
    FTESTLOG("dig shuffle oracle: writing '%s'", path);

    const char* prov_build = getenv("KEEPERFX_ORACLE_BUILD");
    fprintf(f,
        "{\"v\":%d,\"type\":\"meta\",\"probe\":\"dig_shuffle\",\"level\":%d,\"campaign\":\"classic\","
        "\"engine\":\"keeperfx-oracle-dumps\",\"version\":\"%s\",\"build\":\"%s\"}\n",
        DIG_SHUFFLE_ORACLE_VERSION, DIG_SHUFFLE_ORACLE_LEVEL, VER_STRING,
        prov_build ? prov_build : "");

    // Phase "load": the three designer .tng objects present at map load, before any dig.
    dig_shuffle_census(f, "load", "designer .tng objects at map load");

    // Phase "dig": dig the earth block holding all three objects. dig_out_block ->
    // replace_map_slab_when_destroyed -> place_slab_type_on_map(SlbT_PATH) -> shuffle_unattached_things_on_slab,
    // which sweeps this slab's own 3x3 subtiles for unattached things and applies each object's Persistence
    // category.
    dig_out_block(slab_subtile_center(DIG_SLAB_X), slab_subtile_center(DIG_SLAB_Y), PLAYER0);
    dig_shuffle_census(f, "dig", "dug earth slab (40,42); shuffle_unattached_things_on_slab applied");

    fclose(f);
    FTESTLOG("dig shuffle oracle: dump complete");
    return FTRs_Go_To_Next_Action;
}

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
