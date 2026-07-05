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
#include "../../ariadne_tringls.h"
#include "../../ariadne_points.h"
#include "../../ariadne_regions.h"
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
FTestActionResult ftest_ariadne_oracle_action__dump_mesh(struct FTestActionArgs* const args);
FTestActionResult ftest_ariadne_oracle_action__dump_regions(struct FTestActionArgs* const args);

TbBool ftest_ariadne_oracle_init()
{
    // A small delay lets the level finish loading and init_navigation build the raster + mesh before we read.
    // The mesh dump runs before the regions dump, because computing the region partition lazily mutates the
    // triangles' region ids (regions_connected labels components on demand).
    ftest_append_action(ftest_ariadne_oracle_action__dump_navcolour, 8, NULL);
    ftest_append_action(ftest_ariadne_oracle_action__dump_mesh, 0, NULL);
    ftest_append_action(ftest_ariadne_oracle_action__dump_regions, 0, NULL);
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

static void fput_u32_le(FILE* f, unsigned int v)
{
    fput_u16_le(f, v & 0xFFFF);
    fput_u16_le(f, (v >> 16) & 0xFFFF);
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

// D1 — the static navigation mesh (the triangle set the fringe/Delaunay build produces). One binary
// file per level; the keeper-rx L1 gate diffs it by geometric identity (winding-ordered corner coords +
// per-slot neighbour identity + colour), never by DK's private triangle numbering (keeper-rx ADR-0021).
//   magic "KFXNAVM\0" (8) | version u16 | level u16 | tri_count u32
//   then tri_count records, each (all little-endian):
//     tree_alt u16 | navigation_flags u8 | region_and_edgelen u16
//     points[3]: for each corner, x i16, y i16  (the ari_Points[] coords; 0,0 for an unused triangle)
//     tags[3]:   for each edge slot, neighbour triangle id i32 (-1 = hull / no neighbour)
// tri_count is ix_Triangles (the high-water mark); a slot is "used" iff tree_alt != NAV_COL_UNSET, and
// only used triangles are compared. The neighbour ids index this same array so the reader can resolve
// each tag to the neighbour's coords without trusting the id value itself.
FTestActionResult ftest_ariadne_oracle_action__dump_mesh(struct FTestActionArgs* const args)
{
    (void)args;
    const long level = (long)get_loaded_level_number();

    char path[512];
    snprintf(path, sizeof(path), "%s/oracle_ariadne_mesh_%05ld.bin", ariadne_out_dir(), level);
    FILE* f = fopen(path, "wb");
    if (f == NULL)
    {
        FTEST_FRAMEWORK_ABORT("ariadne oracle: failed to open '%s'", path);
        return FTRs_Go_To_Next_Action;
    }

    const unsigned int tri_count = (unsigned int)ix_Triangles;
    fwrite("KFXNAVM\0", 1, 8, f);
    fput_u16_le(f, ARIADNE_ORACLE_VERSION);
    fput_u16_le(f, (unsigned int)(level & 0xFFFF));
    fput_u32_le(f, tri_count);

    unsigned int used = 0;
    for (unsigned int i = 0; i < tri_count; i++)
    {
        const struct Triangle* tri = &Triangles[i];
        const int is_used = (tri->tree_alt != NAV_COL_UNSET);
        if (is_used)
            used++;
        fput_u16_le(f, (unsigned int)tri->tree_alt);
        fputc((int)(tri->navigation_flags & 0xFF), f);
        fput_u16_le(f, (unsigned int)tri->region_and_edgelen);
        for (int c = 0; c < 3; c++)
        {
            short px = 0, py = 0;
            if (is_used)
            {
                const struct Point* pt = &ari_Points[tri->points[c]];
                px = pt->x; py = pt->y;
            }
            fput_u16_le(f, (unsigned int)(unsigned short)px);
            fput_u16_le(f, (unsigned int)(unsigned short)py);
        }
        for (int c = 0; c < 3; c++)
            fput_u32_le(f, (unsigned int)tri->tags[c]);
    }

    fclose(f);
    FTESTLOG("ariadne oracle: dumped mesh (%u/%u triangles used) for level %ld -> %s", used, tri_count, level, path);
    return FTRs_Go_To_Next_Action;
}

// D1r — the region partition (which triangles are mutually reachable). regions_connected() is lazy — it
// labels a connected component on demand — so we materialise the whole partition as a per-triangle
// "representative": rep[i] = the lowest triangle id j <= i with regions_connected(i, j), else i. Two
// triangles share a component iff they share a representative; a wall triangle (regions_connected is
// always false for it) is its own singleton. The keeper-rx D1r gate diffs this partition by geometric
// identity (rep resolved to the representative's corner coords via the D1 mesh dump), so DK's private
// triangle/region numbering never leaks (keeper-rx ADR-0021, decision #9).
//   magic "KFXNAVR\0" (8) | version u16 | level u16 | tri_count u32
//   then tri_count records: used u8 | rep i32   (rep indexes the same triangle array as the D1 mesh dump)
FTestActionResult ftest_ariadne_oracle_action__dump_regions(struct FTestActionArgs* const args)
{
    (void)args;
    const long level = (long)get_loaded_level_number();

    char path[512];
    snprintf(path, sizeof(path), "%s/oracle_ariadne_regions_%05ld.bin", ariadne_out_dir(), level);
    FILE* f = fopen(path, "wb");
    if (f == NULL)
    {
        FTEST_FRAMEWORK_ABORT("ariadne oracle: failed to open '%s'", path);
        return FTRs_Go_To_Next_Action;
    }

    const unsigned int tri_count = (unsigned int)ix_Triangles;
    fwrite("KFXNAVR\0", 1, 8, f);
    fput_u16_le(f, ARIADNE_ORACLE_VERSION);
    fput_u16_le(f, (unsigned int)(level & 0xFFFF));
    fput_u32_le(f, tri_count);

    for (unsigned int i = 0; i < tri_count; i++)
    {
        const int is_used = (Triangles[i].tree_alt != NAV_COL_UNSET);
        long rep = (long)i;
        if (is_used)
        {
            for (unsigned int j = 0; j < i; j++)
            {
                if (Triangles[j].tree_alt == NAV_COL_UNSET)
                    continue;
                if (regions_connected((long)i, (long)j))
                {
                    rep = (long)j;
                    break;
                }
            }
        }
        fputc(is_used ? 1 : 0, f);
        fput_u32_le(f, (unsigned int)rep);
    }

    fclose(f);
    FTESTLOG("ariadne oracle: dumped region partition (%u triangles) for level %ld -> %s", tri_count, level, path);
    return FTRs_Go_To_Next_Action;
}

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
