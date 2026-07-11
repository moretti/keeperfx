#include "ftest_ariadne_oracle.h"

#ifdef FUNCTESTING

#include "../../pre_inc.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <string.h>

#include "../ftest.h"
#include "../ftest_util.h"

#include "../../keeperfx.hpp"
#include "../../game_legacy.h"
#include "../../game_merge.h"
#include "../../ariadne.h"
#include "../../ariadne_tringls.h"
#include "../../ariadne_points.h"
#include "../../ariadne_regions.h"
#include "../../thing_data.h"
#include "../../thing_list.h"
#include "../../thing_physics.h"
#include "../../thing_creature.h"
#include "../../thing_navigate.h"
#include "../../creature_control.h"
#include "../../creature_states.h"
#include "../../config_creature.h"
#include "../../player_instances.h"
#include "../../map_data.h"
#include "../../ver_defs.h"

// Defined (non-static) in ariadne.c but not declared in ariadne.h — the OnLine follower's block test.
long ariadne_creature_blocked_by_wall_at(struct Thing *thing, const struct Coord3d *pos);

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
FTestActionResult ftest_ariadne_oracle_action__dump_route(struct FTestActionArgs* const args);
FTestActionResult ftest_ariadne_oracle_action__dump_waypoints(struct FTestActionArgs* const args);
FTestActionResult ftest_ariadne_oracle_action__dump_collision(struct FTestActionArgs* const args);
FTestActionResult ftest_ariadne_oracle_action__dump_follow(struct FTestActionArgs* const args);

TbBool ftest_ariadne_oracle_init()
{
    // A small delay lets the level finish loading and init_navigation build the raster + mesh before we read.
    // The mesh dump runs before the regions dump, because computing the region partition lazily mutates the
    // triangles' region ids (regions_connected labels components on demand). The route dump runs last: it
    // re-runs regions_connected per query (idempotent), and its D2 records index the same triangle array as
    // the mesh dump — so it must see the fully-built mesh the earlier dumps captured.
    ftest_append_action(ftest_ariadne_oracle_action__dump_navcolour, 8, NULL);
    ftest_append_action(ftest_ariadne_oracle_action__dump_mesh, 0, NULL);
    ftest_append_action(ftest_ariadne_oracle_action__dump_regions, 0, NULL);
    ftest_append_action(ftest_ariadne_oracle_action__dump_route, 0, NULL);
    ftest_append_action(ftest_ariadne_oracle_action__dump_waypoints, 0, NULL);
    ftest_append_action(ftest_ariadne_oracle_action__dump_collision, 0, NULL);
    // The per-tick route follow (D3) runs last: it spawns a real imp and drives the live creature-state
    // machine over several ticks, mutating game state (creatures, nav scratch). Every earlier dump reads the
    // pristine post-load mesh/collision, so keep the follow after them.
    ftest_append_action(ftest_ariadne_oracle_action__dump_follow, 0, NULL);
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

// D2 — the triangle route (best-first search output), BEFORE the funnel/string-pull selects between the
// forward and backward searches (keeper-rx L2a isolates the search from the funnel). One binary per level;
// each record is one fixed query (start/dest subtile-centres, creature radius, lava-capability, owner) and
// the two raw triangle-id sequences ariadne_oracle_route_fwd_bak() copies out of triangle_route_do_fwd/bak.
// The ids index the same triangle array as the D1 mesh dump, so keeper-rx resolves each to its
// winding-ordered corner coords and diffs by geometric identity (ADR-0021) — DK's numbering never leaks.
//   magic "KFXNAVQ\0" (8) | version u16 | level u16 | query_count u32
//   then query_count records, each:
//     start_x i32 | start_y i32 | end_x i32 | end_y i32   (the <<8 coords the search used)
//     nav_size u8 | lava u8 | owner i8 | pad u8
//     status i32   (ariadne_oracle_route_fwd_bak return: 1 ran, 0 regions-gate rejected, -1 setup fail)
//     fwd_len i32 | (fwd_len+1) triangle-id i32   [the id block present only when fwd_len >= 0]
//     bak_len i32 | (bak_len+1) triangle-id i32   [likewise]
// A *_len of -1 with status 1 means "regions connected but no passable route" (e.g. an imp against a
// full-width lava strip); status 0 means the pair is in disconnected regions.

// The subtile centre coordinate for the search (subtile s spans [s<<8, (s+1)<<8); +128 is its middle).
#define ARIADNE_ORACLE_STL_CENTRE(s) (((long)(s) << 8) + 128)

struct AriadneOracleQuery
{
    int start_stl_x, start_stl_y, end_stl_x, end_stl_y; // subtile coords (converted to <<8 centres)
    unsigned char nav_size;   // creature_radius = nav_size + 1; 0 => radius 1 (an imp)
    unsigned char lava;       // nav_thing_can_travel_over_lava
    signed char owner;        // owner_player_navigating (-1 = none)
};

// Fixed per-level query sets. All at radius 1 (the imp): M0 open diagonal; M1 horizontal + vertical
// detours around the central wall; M2 a vertical line across the full-width lava strip, dry (blocked)
// then lava-capable; M-full the corridor route dry then lava-capable.
static const struct AriadneOracleQuery ariadne_oracle_queries_9000[] = {
    { 16, 16, 238, 238, 0, 0, -1 },
};
static const struct AriadneOracleQuery ariadne_oracle_queries_9001[] = {
    { 61, 127, 193, 127, 0, 0, -1 },
    { 127, 61, 127, 193, 0, 0, -1 },
    // A route whose destination is INSIDE the central rock block (subtile 127,127) — unreachable: the
    // regions gate rejects it (a wall triangle is disconnected from every floor component). keeper-rx's
    // FindRoute must return null here (Step 7 reachability), so both D2 (status 0) and D2b (0 waypoints)
    // capture the disconnected branch, distinct from M2's connected-but-no-passable-route imp-vs-lava case.
    { 61, 127, 127, 127, 0, 0, -1 },
};
static const struct AriadneOracleQuery ariadne_oracle_queries_9002[] = {
    { 61, 61, 61, 193, 0, 0, -1 },
    { 61, 61, 61, 193, 0, 1, -1 },
};
static const struct AriadneOracleQuery ariadne_oracle_queries_9003[] = {
    { 16, 127, 241, 127, 0, 0, -1 },
    { 16, 127, 241, 127, 0, 1, -1 },
};

static const struct AriadneOracleQuery* ariadne_oracle_queries_for(long level, unsigned int* count)
{
    switch (level)
    {
    case 9000: *count = sizeof(ariadne_oracle_queries_9000)/sizeof(ariadne_oracle_queries_9000[0]); return ariadne_oracle_queries_9000;
    case 9001: *count = sizeof(ariadne_oracle_queries_9001)/sizeof(ariadne_oracle_queries_9001[0]); return ariadne_oracle_queries_9001;
    case 9002: *count = sizeof(ariadne_oracle_queries_9002)/sizeof(ariadne_oracle_queries_9002[0]); return ariadne_oracle_queries_9002;
    case 9003: *count = sizeof(ariadne_oracle_queries_9003)/sizeof(ariadne_oracle_queries_9003[0]); return ariadne_oracle_queries_9003;
    default: *count = 0; return NULL;
    }
}

// Route buffers (ROUTE_LENGTH each); file-scope static to keep them off the ftest stack.
static int32_t ariadne_oracle_route_fwd_buf[ROUTE_LENGTH];
static int32_t ariadne_oracle_route_bak_buf[ROUTE_LENGTH];

static void fput_route(FILE* f, const int32_t* route, long len)
{
    fput_u32_le(f, (unsigned int)len);
    if (len >= 0)
        for (long i = 0; i <= len; i++)
            fput_u32_le(f, (unsigned int)route[i]);
}

FTestActionResult ftest_ariadne_oracle_action__dump_route(struct FTestActionArgs* const args)
{
    (void)args;
    const long level = (long)get_loaded_level_number();

    unsigned int qcount = 0;
    const struct AriadneOracleQuery* queries = ariadne_oracle_queries_for(level, &qcount);

    char path[512];
    snprintf(path, sizeof(path), "%s/oracle_ariadne_route_%05ld.bin", ariadne_out_dir(), level);
    FILE* f = fopen(path, "wb");
    if (f == NULL)
    {
        FTEST_FRAMEWORK_ABORT("ariadne oracle: failed to open '%s'", path);
        return FTRs_Go_To_Next_Action;
    }

    fwrite("KFXNAVQ\0", 1, 8, f);
    fput_u16_le(f, ARIADNE_ORACLE_VERSION);
    fput_u16_le(f, (unsigned int)(level & 0xFFFF));
    fput_u32_le(f, qcount);

    for (unsigned int q = 0; q < qcount; q++)
    {
        const struct AriadneOracleQuery* qq = &queries[q];
        const long sx = ARIADNE_ORACLE_STL_CENTRE(qq->start_stl_x);
        const long sy = ARIADNE_ORACLE_STL_CENTRE(qq->start_stl_y);
        const long ex = ARIADNE_ORACLE_STL_CENTRE(qq->end_stl_x);
        const long ey = ARIADNE_ORACLE_STL_CENTRE(qq->end_stl_y);
        long fwd_len = -1;
        long bak_len = -1;
        long status = ariadne_oracle_route_fwd_bak(sx, sy, ex, ey, qq->nav_size, qq->lava, qq->owner,
            ariadne_oracle_route_fwd_buf, ariadne_oracle_route_bak_buf, &fwd_len, &bak_len);

        fput_u32_le(f, (unsigned int)sx);
        fput_u32_le(f, (unsigned int)sy);
        fput_u32_le(f, (unsigned int)ex);
        fput_u32_le(f, (unsigned int)ey);
        fputc((int)qq->nav_size, f);
        fputc((int)qq->lava, f);
        fputc((int)(unsigned char)qq->owner, f);
        fputc(0, f); // pad
        fput_u32_le(f, (unsigned int)status);
        fput_route(f, ariadne_oracle_route_fwd_buf, fwd_len);
        fput_route(f, ariadne_oracle_route_bak_buf, bak_len);

        FTESTLOG("ariadne oracle: route q%u lvl %ld status=%ld fwd_len=%ld bak_len=%ld", q, level, status, fwd_len, bak_len);
    }

    fclose(f);
    FTESTLOG("ariadne oracle: dumped %u route quer%s for level %ld -> %s", qcount, qcount == 1 ? "y" : "ies", level, path);
    return FTRs_Go_To_Next_Action;
}

// D2b — the funnel/string-pull waypoints (route_to_path + path_out_a_bit), for the SAME query set as the
// D2 route dump. This is the path whose first ARID_WAYPOINTS_COUNT entries feed the Ariadne 10-waypoint
// window; keeper-rx's L2b gate diffs the whole waypoint list by exact integer coordinate (the coords are
// geometric — derived from the bit-exact mesh — so no numbering leaks). One binary per level:
//   magic "KFXNAVW\0" (8) | version u16 | level u16 | query_count u32
//   then query_count records, each:
//     start_x i32 | start_y i32 | end_x i32 | end_y i32   (the <<8 coords the funnel used)
//     nav_size u8 | lava u8 | owner i8 | pad u8
//     waypoints_num i32   (>= 0; 0 = no route — disconnected regions or no passable route)
//     waypoints_num * (x i32, y i32)   (the funnel waypoints, in <<8 coords)
static int32_t ariadne_oracle_wp_x[ARID_PATH_WAYPOINTS_COUNT];
static int32_t ariadne_oracle_wp_y[ARID_PATH_WAYPOINTS_COUNT];

FTestActionResult ftest_ariadne_oracle_action__dump_waypoints(struct FTestActionArgs* const args)
{
    (void)args;
    const long level = (long)get_loaded_level_number();

    unsigned int qcount = 0;
    const struct AriadneOracleQuery* queries = ariadne_oracle_queries_for(level, &qcount);

    char path[512];
    snprintf(path, sizeof(path), "%s/oracle_ariadne_waypoints_%05ld.bin", ariadne_out_dir(), level);
    FILE* f = fopen(path, "wb");
    if (f == NULL)
    {
        FTEST_FRAMEWORK_ABORT("ariadne oracle: failed to open '%s'", path);
        return FTRs_Go_To_Next_Action;
    }

    fwrite("KFXNAVW\0", 1, 8, f);
    fput_u16_le(f, ARIADNE_ORACLE_VERSION);
    fput_u16_le(f, (unsigned int)(level & 0xFFFF));
    fput_u32_le(f, qcount);

    for (unsigned int q = 0; q < qcount; q++)
    {
        const struct AriadneOracleQuery* qq = &queries[q];
        const long sx = ARIADNE_ORACLE_STL_CENTRE(qq->start_stl_x);
        const long sy = ARIADNE_ORACLE_STL_CENTRE(qq->start_stl_y);
        const long ex = ARIADNE_ORACLE_STL_CENTRE(qq->end_stl_x);
        const long ey = ARIADNE_ORACLE_STL_CENTRE(qq->end_stl_y);
        long n = ariadne_oracle_funnel(sx, sy, ex, ey, qq->nav_size, qq->lava, qq->owner,
            ariadne_oracle_wp_x, ariadne_oracle_wp_y, ARID_PATH_WAYPOINTS_COUNT);
        if (n < 0)
            n = 0;

        fput_u32_le(f, (unsigned int)sx);
        fput_u32_le(f, (unsigned int)sy);
        fput_u32_le(f, (unsigned int)ex);
        fput_u32_le(f, (unsigned int)ey);
        fputc((int)qq->nav_size, f);
        fputc((int)qq->lava, f);
        fputc((int)(unsigned char)qq->owner, f);
        fputc(0, f); // pad
        fput_u32_le(f, (unsigned int)n);
        for (long i = 0; i < n; i++)
        {
            fput_u32_le(f, (unsigned int)ariadne_oracle_wp_x[i]);
            fput_u32_le(f, (unsigned int)ariadne_oracle_wp_y[i]);
        }

        FTESTLOG("ariadne oracle: waypoints q%u lvl %ld waypoints_num=%ld", q, level, n);
    }

    fclose(f);
    FTESTLOG("ariadne oracle: dumped %u waypoint quer%s for level %ld -> %s", qcount, qcount == 1 ? "y" : "ies", level, path);
    return FTRs_Go_To_Next_Action;
}

// D4 — the wall/floor collision primitives, for a spawned imp (keeper-rx Step 8a). One binary per level:
//   magic "KFXNAVP\0" (8) | version u16 | level u16 | subtiles u16 | pad u16
//   Section A: subtiles*subtiles bytes, row-major (y outer, x inner) — for the imp standing at each subtile
//     centre: bits 0-3 = get_floor_height_under_thing_at (in subtiles), bit 4 = thing_in_wall_at.
//   Section B: source_x i32 | source_y i32 | ceil(subtiles*subtiles / 8) bytes, a bitmap (LSB-first,
//     row-major) of ariadne_creature_blocked_by_wall_at(imp@source, subtile-centre) — the OnLine block test
//     (creature_cannot_move_directly_to) from the level's first route-query start to every subtile.
FTestActionResult ftest_ariadne_oracle_action__dump_collision(struct FTestActionArgs* const args)
{
    (void)args;
    const long level = (long)get_loaded_level_number();
    const unsigned int n = ARIADNE_ORACLE_SUBTILES;

    // Use a stack-local imp rather than a spawned creature: the synthetic fixtures have no player dungeon,
    // so ftest_util_create_creature would deref a null dungeon. The collision primitives read only the
    // thing's clipbox size-xy/z and mappos. A stack pointer lies outside game.things_data[], so
    // thing_is_invalid() (hence thing_is_creature()) reads false — which makes the size functions take the
    // NON-creature branch (size = clipbox_size_xy) instead of thing_nav_sizexy. So we set clipbox_size_xy to
    // the imp's *nav* size 206 (= thing_nav_sizexy for the imp's config Size-xy of 200,
    // config/creatrs/imp.cfg via actual_sizexy_to_nav_sizexy_table), giving the same radius (103) a real imp
    // gets through the creature branch. The collision footprint — and thus the dump — is bit-identical to a
    // real imp's; keeper-rx's ThingPhysics uses nav size 206 / clipbox z 256 to match.
    unsigned int qcount = 0;
    const struct AriadneOracleQuery* queries = ariadne_oracle_queries_for(level, &qcount);
    const int spawn_stl_x = (qcount > 0) ? queries[0].start_stl_x : (int)(n / 2);
    const int spawn_stl_y = (qcount > 0) ? queries[0].start_stl_y : (int)(n / 2);
    struct Thing imp_storage;
    memset(&imp_storage, 0, sizeof(struct Thing));
    struct Thing* imp = &imp_storage;
    imp->class_id = TCls_Creature;
    imp->clipbox_size_xy = 206; // imp thing_nav_sizexy (see comment above)
    imp->clipbox_size_z = 256;  // imp config Size-z

    char path[512];
    snprintf(path, sizeof(path), "%s/oracle_ariadne_collision_%05ld.bin", ariadne_out_dir(), level);
    FILE* f = fopen(path, "wb");
    if (f == NULL)
    {
        FTEST_FRAMEWORK_ABORT("ariadne oracle: failed to open '%s'", path);
        return FTRs_Go_To_Next_Action;
    }

    fwrite("KFXNAVP\0", 1, 8, f);
    fput_u16_le(f, ARIADNE_ORACLE_VERSION);
    fput_u16_le(f, (unsigned int)(level & 0xFFFF));
    fput_u16_le(f, n);
    fput_u16_le(f, 0); // pad

    // Section A: per-subtile floor height + in-wall.
    for (unsigned int y = 0; y < n; y++)
    {
        for (unsigned int x = 0; x < n; x++)
        {
            struct Coord3d c;
            c.x.val = ARIADNE_ORACLE_STL_CENTRE(x);
            c.y.val = ARIADNE_ORACLE_STL_CENTRE(y);
            c.z.val = 0;
            long floor_z = get_floor_height_under_thing_at(imp, &c);
            c.z.val = floor_z;
            int in_wall = thing_in_wall_at(imp, &c) ? 1 : 0;
            int fh = (int)(floor_z >> 8) & 0x0F;
            fputc(fh | (in_wall ? 0x10 : 0), f);
        }
    }

    // Section B: cannot-move-directly bitmap from the first route query's start (= the spawn tile).
    long src_x = ARIADNE_ORACLE_STL_CENTRE(spawn_stl_x);
    long src_y = ARIADNE_ORACLE_STL_CENTRE(spawn_stl_y);
    imp->mappos.x.val = src_x;
    imp->mappos.y.val = src_y;
    imp->mappos.z.val = get_thing_height_at(imp, &imp->mappos);
    fput_u32_le(f, (unsigned int)src_x);
    fput_u32_le(f, (unsigned int)src_y);

    unsigned char bitbuf = 0;
    int bitcnt = 0;
    for (unsigned int y = 0; y < n; y++)
    {
        for (unsigned int x = 0; x < n; x++)
        {
            struct Coord3d t;
            t.x.val = ARIADNE_ORACLE_STL_CENTRE(x);
            t.y.val = ARIADNE_ORACLE_STL_CENTRE(y);
            t.z.val = 0;
            int cant = ariadne_creature_blocked_by_wall_at(imp, &t) ? 1 : 0;
            bitbuf |= (unsigned char)(cant << bitcnt);
            if (++bitcnt == 8)
            {
                fputc(bitbuf, f);
                bitbuf = 0;
                bitcnt = 0;
            }
        }
    }
    if (bitcnt > 0)
        fputc(bitbuf, f);

    fclose(f);
    FTESTLOG("ariadne oracle: dumped %ux%u collision map for level %ld -> %s", n, n, level, path);
    return FTRs_Go_To_Next_Action;
}

// D3 — the per-tick route follower (keeper-rx Step 8b). A real imp is spawned at the level's first
// route-query start, ordered to move to that query's end via the live creature-state machine
// (setup_person_move_to_position -> CrSt_MoveToPosition), and its state is dumped once per game tick as the
// follower walks it there. This is the ground truth for the per-tick position parity contract: keeper-rx
// seeds record 0 and replays its ported follower, diffing mappos + mode + waypoint each tick. One binary per
// level (streaming — no up-front tick count; the reader consumes fixed records to EOF):
//   header (22 bytes): magic "KFXNAVF\0" (8) | version u16 | level u16 |
//                      start_stl_x u16 | start_stl_y u16 | end_stl_x u16 | end_stl_y u16 | record_size u16
//   then N records (22 bytes each), one per tick:
//     tick u16 | pos_x i32 | pos_y i32   (thing->mappos.x/y.val)
//     mode u8 (arid.update_state: 0 Unset / 1 OnLine / 2 Wallhug / 3 Manoeuvre) |
//     current_waypoint u8 | stored_waypoints u8 | total_waypoints u8 (low byte) |
//     waypoint_x i32 | waypoint_y i32   (arid.waypoints[current_waypoint], or 0 if out of range)
// Record 0 is the posed initial state (imp at start, mode Unset — the follower has not run yet); records
// 1..N-1 are after each follow step. A level whose first query is unreachable by a plain (non-lava) imp
// (e.g. M2's lava strip fully separates the map) writes a header with zero records.
#define ARIADNE_FOLLOW_RECORD_SIZE 22u
// Safety cap so a mis-authored route that never arrives cannot spin the ftest forever. The clear fixtures
// arrive in well under this many ticks (a full 255-subtile diagonal at the imp's base speed of 96
// coord-units/tick is ~680 ticks).
#define ARIADNE_FOLLOW_MAX_TICKS 4000

// Dedicated per-tick follow routes (D3): short routes reachable by a plain (non-lava) imp and chosen to
// stay OnLine — no wallhug/manoeuvre — for the first follower gate. The corridor/tight-corner routes that
// trigger the fallback modes are the Step 9/10 fixtures, not this one. Subtile coords; the actual mode each
// tick is captured in the dump and verified numerically (never assumed).
struct AriadneFollowRoute { int start_stl_x, start_stl_y, end_stl_x, end_stl_y; TbBool present; };

static struct AriadneFollowRoute ariadne_follow_route_for(long level)
{
    struct AriadneFollowRoute r;
    r.present = true;
    switch (level)
    {
    case 9000: r.start_stl_x = 40;  r.start_stl_y = 40;  r.end_stl_x = 56;  r.end_stl_y = 56;  break; // open floor, straight
    case 9001: r.start_stl_x = 120; r.start_stl_y = 127; r.end_stl_x = 135; r.end_stl_y = 127; break; // bends around the central wall
    case 9002: r.start_stl_x = 30;  r.start_stl_y = 20;  r.end_stl_x = 60;  r.end_stl_y = 40;  break; // top half, clear of the lava strip
    case 9003: r.start_stl_x = 6;   r.start_stl_y = 127; r.end_stl_x = 18;  r.end_stl_y = 127; break; // inside the left room
    default:   r.present = false; r.start_stl_x = r.start_stl_y = r.end_stl_x = r.end_stl_y = 0; break;
    }
    return r;
}

static FILE* ariadne_follow_file = NULL;
static struct Thing* ariadne_follow_imp = NULL;

static void ariadne_follow_dump_record(FILE* f, long tick, struct Thing* imp)
{
    struct CreatureControl* cctrl = creature_control_get_from_thing(imp);
    const struct Ariadne* arid = &cctrl->arid;
    unsigned char cwp = arid->current_waypoint;
    long wx = 0, wy = 0;
    if (cwp < arid->stored_waypoints)
    {
        wx = arid->waypoints[cwp].x.val;
        wy = arid->waypoints[cwp].y.val;
    }
    fput_u16_le(f, (unsigned int)tick);
    fput_u32_le(f, (unsigned int)imp->mappos.x.val);
    fput_u32_le(f, (unsigned int)imp->mappos.y.val);
    fputc((int)arid->update_state, f);
    fputc((int)arid->current_waypoint, f);
    fputc((int)arid->stored_waypoints, f);
    fputc((int)(arid->total_waypoints & 0xFF), f);
    fput_u32_le(f, (unsigned int)wx);
    fput_u32_le(f, (unsigned int)wy);
}

FTestActionResult ftest_ariadne_oracle_action__dump_follow(struct FTestActionArgs* const args)
{
    const long level = (long)get_loaded_level_number();
    const struct AriadneFollowRoute route = ariadne_follow_route_for(level);

    if (args->times_executed == 0)
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/oracle_ariadne_follow_%05ld.bin", ariadne_out_dir(), level);
        ariadne_follow_file = fopen(path, "wb");
        if (ariadne_follow_file == NULL)
        {
            FTEST_FRAMEWORK_ABORT("ariadne oracle: failed to open '%s'", path);
            return FTRs_Go_To_Next_Action;
        }

        const int start_stl_x = route.start_stl_x;
        const int start_stl_y = route.start_stl_y;
        const int end_stl_x   = route.end_stl_x;
        const int end_stl_y   = route.end_stl_y;

        fwrite("KFXNAVF\0", 1, 8, ariadne_follow_file);
        fput_u16_le(ariadne_follow_file, ARIADNE_ORACLE_VERSION);
        fput_u16_le(ariadne_follow_file, (unsigned int)(level & 0xFFFF));
        fput_u16_le(ariadne_follow_file, (unsigned int)start_stl_x);
        fput_u16_le(ariadne_follow_file, (unsigned int)start_stl_y);
        fput_u16_le(ariadne_follow_file, (unsigned int)end_stl_x);
        fput_u16_le(ariadne_follow_file, (unsigned int)end_stl_y);
        fput_u16_le(ariadne_follow_file, ARIADNE_FOLLOW_RECORD_SIZE);

        if (!route.present)
        {
            fclose(ariadne_follow_file);
            ariadne_follow_file = NULL;
            FTESTLOG("ariadne oracle: no follow route for level %ld, follow dump is empty", level);
            return FTRs_Go_To_Next_Action;
        }

        const MapCoord sx = subtile_coord_center(start_stl_x);
        const MapCoord sy = subtile_coord_center(start_stl_y);
        ariadne_follow_imp = ftest_util_create_creature(sx, sy, PLAYER0, 1, get_players_special_digger_model(PLAYER0));
        if (thing_is_invalid(ariadne_follow_imp))
        {
            FTEST_FAIL_TEST("ariadne oracle: could not spawn the imp for the follow dump (level %ld)", level);
            fclose(ariadne_follow_file);
            ariadne_follow_file = NULL;
            return FTRs_Go_To_Next_Action;
        }
        // Settle the imp onto the floor under its spawn tile before ordering the move (mirrors the movement
        // oracle: create_creature places it, move_thing_in_map fixes floor_height/z for that subtile).
        move_thing_in_map(ariadne_follow_imp, &ariadne_follow_imp->mappos);

        if (!setup_person_move_to_position(ariadne_follow_imp, end_stl_x, end_stl_y, NavRtF_Default))
        {
            // Unreachable for a plain imp (e.g. M2's full-width lava strip). Header stays with zero records.
            FTESTLOG("ariadne oracle: route (%d,%d)->(%d,%d) unreachable for a plain imp on level %ld; empty follow dump",
                start_stl_x, start_stl_y, end_stl_x, end_stl_y, level);
            fclose(ariadne_follow_file);
            ariadne_follow_file = NULL;
            delete_thing_structure(ariadne_follow_imp, 0);
            ariadne_follow_imp = NULL;
            return FTRs_Go_To_Next_Action;
        }

        ariadne_follow_dump_record(ariadne_follow_file, args->times_executed, ariadne_follow_imp);
        return FTRs_Repeat_Current_Action;
    }

    ariadne_follow_dump_record(ariadne_follow_file, args->times_executed, ariadne_follow_imp);

    // Stop once the imp leaves MoveToPosition (arrived, or continue_state popped it) or the safety cap trips.
    if (ariadne_follow_imp->active_state != CrSt_MoveToPosition || args->times_executed >= ARIADNE_FOLLOW_MAX_TICKS)
    {
        FTESTLOG("ariadne oracle: follow dump for level %ld complete (%ld records, final state %d)",
            level, (long)args->times_executed + 1, (int)ariadne_follow_imp->active_state);
        fclose(ariadne_follow_file);
        ariadne_follow_file = NULL;
        delete_thing_structure(ariadne_follow_imp, 0);
        ariadne_follow_imp = NULL;
        return FTRs_Go_To_Next_Action;
    }
    return FTRs_Repeat_Current_Action;
}

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
