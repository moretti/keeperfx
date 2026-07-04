#include "ftest_ariadne_oracle.h"

#ifdef FUNCTESTING

#include "../../pre_inc.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "../ftest.h"

#include "../../keeperfx.hpp"
#include "../../game_legacy.h"
#include "../../game_merge.h"
#include "../../ariadne.h"
#include "../../ver_defs.h"

#include "../../post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif

// Oracle dump format version — bump on any layout/meaning change (keeper-rx/docs/oracle/dump-format.md).
#define ARIADNE_ORACLE_VERSION 1

// The subtile square dumped: a DEFAULT_MAP_SIZE (85) slab map is STL_PER_SLB*85 subtiles per axis. We
// dump [0,SUBTILES) on both axes (the playable subtiles; the border slabs are solid rock) and keeper-rx
// diffs the same range. STL_PER_SLB/DEFAULT_MAP_SIZE are KeeperFX's own (map_data.h).
#define ARIADNE_ORACLE_SUBTILES (STL_PER_SLB * DEFAULT_MAP_SIZE)

// forward declaration
FTestActionResult ftest_ariadne_oracle_action__dump_navcolour(struct FTestActionArgs* const args);

TbBool ftest_ariadne_oracle_init()
{
    // A small delay lets the level finish loading and init_navigation build the raster before we read it.
    ftest_append_action(ftest_ariadne_oracle_action__dump_navcolour, 8, NULL);
    return true;
}

// Resolve the directory the dump files are written to: $KEEPERFX_ORACLE_OUT, else the CWD (shared with
// the movement/spike oracles so the capture-script convention is identical).
static const char* ariadne_out_dir(void)
{
    const char* dir = getenv("KEEPERFX_ORACLE_OUT");
    if (dir == NULL || dir[0] == '\0')
        return ".";
    mkdir(dir, 0755); // best-effort; ignore EEXIST
    return dir;
}

static void fput_u16_le(FILE* f, unsigned int v)
{
    fputc((int)(v & 0xFF), f);
    fputc((int)((v >> 8) & 0xFF), f);
}

// D0 — the per-subtile NavColour raster. One binary file per level:
//   magic "KFXNAVC\0" (8) | version u16 | level u16 | width u16 | height u16 | width*height NavColour u16
// values row-major (y outer, x inner). NavColour is the terrain-only value the navmesh triangulates from
// (get_navigation_colour, https://github.com/dkfans/keeperfx/blob/v1.4.0/src/ariadne.c#L4743).
FTestActionResult ftest_ariadne_oracle_action__dump_navcolour(struct FTestActionArgs* const args)
{
    (void)args;
    const long level = (long)get_loaded_level_number();

    char path[512];
    snprintf(path, sizeof(path), "%s/oracle_ariadne_navcolour_%05ld.bin", ariadne_out_dir(), level);
    FILE* f = fopen(path, "wb");
    if (f == NULL)
    {
        FTEST_FRAMEWORK_ABORT("ariadne oracle: failed to open '%s'", path);
        return FTRs_Go_To_Next_Action;
    }

    const unsigned int width = ARIADNE_ORACLE_SUBTILES;
    const unsigned int height = ARIADNE_ORACLE_SUBTILES;
    fwrite("KFXNAVC\0", 1, 8, f);
    fput_u16_le(f, ARIADNE_ORACLE_VERSION);
    fput_u16_le(f, (unsigned int)(level & 0xFFFF));
    fput_u16_le(f, width);
    fput_u16_le(f, height);

    for (unsigned int y = 0; y < height; y++)
        for (unsigned int x = 0; x < width; x++)
            fput_u16_le(f, (unsigned int)get_navigation_colour((long)x, (long)y));

    fclose(f);
    FTESTLOG("ariadne oracle: dumped %ux%u NavColour raster for level %ld -> %s", width, height, level, path);
    return FTRs_Go_To_Next_Action;
}

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
