#include "ftest_oracle_spike.h"

#ifdef FUNCTESTING

#include "../../pre_inc.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "../ftest.h"
#include "../ftest_util.h"

#include "../../game_legacy.h"
#include "../../keeperfx.hpp"
#include "../../dungeon_data.h"
#include "../../light_data.h"
#include "../../game_lghtshdw.h"
#include "../../engine_render.h"
#include "../../engine_arrays.h"
#include "../../config_objects.h"
#include "../../thing_objects.h"
#include "../../map_data.h"
#include "../../player_instances.h"
#include "../../bflib_mouse.h"
#include "../../ver_defs.h"

#include "../../post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif

// Oracle dump format version — bump on any layout/meaning change (keeper-rx/docs/oracle/dump-format.md).
#define ORACLE_DUMP_VERSION 1

// The turn at which the array (Tier B) snapshot is taken and the test completes. The spike's target
// per keeper-rx ADR-0016; at the resting beat the heart tops out on turn 18 (128*18 = 2304 > 2303).
#define ORACLE_TARGET_TICK 18

// Subtile grid dumped, matching keeper-rx's MapConstants.SubtileArraySize (ushort[256,256]). map302
// (85 slabs) fills stl 0..254; the rest is border/ambient. get_subtile_number clamps the top edge.
#define ORACLE_DUMP_STRIDE 256

// Binary container magic (8 bytes incl. NUL). Tier B header, see dump-format.md.
#define ORACLE_BIN_MAGIC "KFXODMP"

// Probe ids in the binary header.
#define ORACLE_PROBE_SUBTILE_LIGHTNESS 1u
#define ORACLE_PROBE_STAT_LIGHT_MAP    2u
#define ORACLE_PROBE_ISO_SHADE         3u   // per-(subtile,height) shade_intensity, a 3D array (depth = heights)

// Oracle-dump state, at module scope so both the standalone spike action and the parity ftest (which
// calls the ftest_oracle_* functions to emit the same dump in its own launch) share one open JSONL.
static FILE* oracle_jsonl = NULL;         // Tier A: per-turn heart-beat records + the tick-target dumps
static GameTurn oracle_target_tick = ORACLE_TARGET_TICK;

// forward declarations
FTestActionResult ftest_oracle_spike_action001__dump(struct FTestActionArgs* const args);

// Resolve the directory the dump files are written to: $KEEPERFX_ORACLE_OUT, else the CWD.
static const char* oracle_out_dir(void)
{
    const char* dir = getenv("KEEPERFX_ORACLE_OUT");
    if (dir == NULL || dir[0] == '\0')
        return ".";
    mkdir(dir, 0755); // best-effort; ignore EEXIST
    return dir;
}

static FILE* oracle_open(const char* filename, const char* mode)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", oracle_out_dir(), filename);
    FILE* f = fopen(path, mode);
    if (f == NULL)
        FTESTLOG("oracle: failed to open '%s'", path);
    else
        FTESTLOG("oracle: writing '%s'", path);
    return f;
}

// Explicit little-endian writers so the on-disk format is host-endianness-independent ([[ADR-0005]]).
static void write_u16_le(FILE* f, unsigned short v)
{
    fputc(v & 0xFF, f);
    fputc((v >> 8) & 0xFF, f);
}

static void write_u32_le(FILE* f, unsigned int v)
{
    fputc(v & 0xFF, f);
    fputc((v >> 8) & 0xFF, f);
    fputc((v >> 16) & 0xFF, f);
    fputc((v >> 24) & 0xFF, f);
}

// Dump one lightness array (Tier B) in the documented row-major (stl_x-fastest) order. Writing through
// get_subtile_number decouples the file layout from the engine's internal MAX_SUBTILES_X stride.
static void oracle_dump_array(const char* filename, unsigned int probe_id, GameTurn tick,
                              const unsigned short* array)
{
    FILE* f = oracle_open(filename, "wb");
    if (f == NULL)
        return;

    fwrite(ORACLE_BIN_MAGIC, 1, 8, f); // 7 chars + NUL = 8 bytes
    write_u32_le(f, ORACLE_DUMP_VERSION);
    write_u32_le(f, probe_id);
    write_u32_le(f, (unsigned int)tick);
    write_u32_le(f, ORACLE_DUMP_STRIDE); // width
    write_u32_le(f, ORACLE_DUMP_STRIDE); // height
    write_u32_le(f, 2u);                 // elem_bytes (u16)

    for (MapSubtlCoord stl_y = 0; stl_y < ORACLE_DUMP_STRIDE; stl_y++)
        for (MapSubtlCoord stl_x = 0; stl_x < ORACLE_DUMP_STRIDE; stl_x++)
            write_u16_le(f, array[get_subtile_number(stl_x, stl_y)]);

    fclose(f);
}

// Dump the captured per-(subtile,height) shade_intensity (Tier B, 3D). The header adds a depth field
// (column height) after width/height; payload is plane-major (height slowest), then the same stl_x-fastest
// row-major order as the 2D arrays. 0xFFFF marks a (subtile,height) the frame didn't render (off-screen /
// above the column). Only the isometric fill writes this (fill_in_points_isometric), so it is the terrain
// shade *including* the randomisors dither — the ground truth the keeper-rx column-walk port diffs against.
#ifdef FUNCTESTING
static void oracle_dump_iso_shade(const char* filename, GameTurn tick)
{
    FILE* f = oracle_open(filename, "wb");
    if (f == NULL)
        return;

    fwrite(ORACLE_BIN_MAGIC, 1, 8, f);
    write_u32_le(f, ORACLE_DUMP_VERSION);
    write_u32_le(f, ORACLE_PROBE_ISO_SHADE);
    write_u32_le(f, (unsigned int)tick);
    write_u32_le(f, ORACLE_DUMP_STRIDE);          // width  (stl_x)
    write_u32_le(f, ORACLE_DUMP_STRIDE);          // height (stl_y)
    write_u32_le(f, ORACLE_ISO_SHADE_HEIGHTS);    // depth  (column height)
    write_u32_le(f, 2u);                          // elem_bytes (u16)

    for (int h = 0; h < ORACLE_ISO_SHADE_HEIGHTS; h++)
        for (MapSubtlCoord stl_y = 0; stl_y < ORACLE_DUMP_STRIDE; stl_y++)
            for (MapSubtlCoord stl_x = 0; stl_x < ORACLE_DUMP_STRIDE; stl_x++)
                write_u16_le(f, oracle_iso_shade[h * (MAX_SUBTILES_X * MAX_SUBTILES_Y)
                                                 + get_subtile_number(stl_x, stl_y)]);

    fclose(f);
}
#endif

// --- Reusable oracle dump (shared by this spike and the parity ftest) -------------------------------

// Open the JSONL, arm the iso-shade capture, and write the self-describing meta line. The caller has
// already frozen the heart's flicker and posed the camera; this only owns the dump files.
void ftest_oracle_begin(GameTurn target_tick)
{
    oracle_target_tick = target_tick;
    // Start capturing the isometric per-vertex shade_intensity so the target frame holds a full frame.
    oracle_iso_shade_reset();

    oracle_jsonl = oracle_open("oracle_heartbeat_t18.jsonl", "w");
    if (oracle_jsonl == NULL)
        return;
    // Provenance so a stray dump is reproducible from the meta line alone: which binary produced it
    // (absolute path) and the git commit it was built from. Both are passed by the capture script
    // (KEEPERFX_ORACLE_BINARY / KEEPERFX_ORACLE_BUILD); empty when run by hand. VER_STRING is the
    // compiled-in engine version. See keeper-rx/docs/oracle/dump-format.md.
    const char* prov_build  = getenv("KEEPERFX_ORACLE_BUILD");
    // Record the level that actually loaded (not the test's built-in default), so a dump from a
    // KEEPERFX_FTEST_LEVEL override is self-describing from its meta line alone.
    LevelNumber loaded_level = get_loaded_level_number();
    fprintf(oracle_jsonl,
        "{\"v\":%d,\"type\":\"meta\",\"probe\":\"heartbeat\",\"level\":%ld,\"campaign\":\"classic\","
        "\"target_tick\":%ld,\"heart_flicker_disabled\":true,\"engine\":\"keeperfx-oracle-dumps\","
        "\"version\":\"%s\",\"build\":\"%s\"}\n",
        ORACLE_DUMP_VERSION, (long)loaded_level, (long)oracle_target_tick, VER_STRING,
        prov_build ? prov_build : "");
}

// Tier A — the beating heart's deterministic per-turn state (thing_objects.c:1120 update). A no-op once
// the JSONL is closed, so the parity ftest can call it every turn without guarding on the target.
void ftest_oracle_write_heartbeat(void)
{
    if (oracle_jsonl == NULL)
        return;
    struct Thing* heartng = get_player_soul_container(PLAYER0);
    if (thing_is_invalid(heartng))
        return;
    struct ObjectConfigStats* objst = get_object_model_stats(heartng->model);
    unsigned char intensity = light_get_light_intensity(heartng->light_id);
    fprintf(oracle_jsonl,
        "{\"v\":%d,\"type\":\"heartbeat\",\"tick\":%ld,\"thing_idx\":%d,\"current_frame\":%d,"
        "\"anim_time\":%ld,\"beat_direction\":%d,\"light_id\":%d,\"light_intensity\":%d,"
        "\"health\":%ld,\"max_health\":%ld}\n",
        ORACLE_DUMP_VERSION, (long)get_gameturn(), (int)heartng->index,
        (int)heartng->current_frame, (long)heartng->anim_time,
        (int)(signed char)heartng->heart.beat_direction, (int)heartng->light_id, (int)intensity,
        (long)heartng->health, (long)objst->health);
}

// The target-turn dump: the light inputs, the heart's ground-truth shade, the two Tier-B lightness
// arrays, the randomisors, and the iso-shade array. Call once, AFTER the target frame has been drawn.
void ftest_oracle_write_dumps(GameTurn tick)
{
    if (oracle_jsonl == NULL)
        return;
    struct Thing* heartng = get_player_soul_container(PLAYER0);
    if (thing_is_invalid(heartng))
        return;
    struct Light* lgt = &game.lish.lights[heartng->light_id];
    unsigned char intensity = light_get_light_intensity(heartng->light_id);

    // Tier B — array snapshot + the ground-truth light inputs the keeper-rx diff feeds to Compute.
    fprintf(oracle_jsonl,
        "{\"v\":%d,\"type\":\"lightdump\",\"tick\":%ld,\"light_id\":%d,\"light_intensity\":%d,"
        "\"radius\":%d,\"range\":%d,\"pos_x\":%d,\"pos_y\":%d,\"pos_z\":%d,\"stl_x\":%d,\"stl_y\":%d,"
        "\"flags2\":%d,\"array_width\":%d,\"array_height\":%d}\n",
        ORACLE_DUMP_VERSION, (long)tick, (int)heartng->light_id, (int)intensity,
        (int)lgt->radius, (int)lgt->range,
        (int)lgt->mappos.x.val, (int)lgt->mappos.y.val, (int)lgt->mappos.z.val,
        (int)coord_subtile(lgt->mappos.x.val), (int)coord_subtile(lgt->mappos.y.val),
        (int)lgt->flags2, ORACLE_DUMP_STRIDE, ORACLE_DUMP_STRIDE);

    // Tier A — the heart sprite's ground-truth shade: get_thing_shade bilinearly samples the same
    // subtile_lightness array (dumped below) at the heart's four surrounding subtiles, floored at the
    // owner's thing_minimum_illumination and clamped, giving the 0..64 fade-table row the blit uses
    // (engine_render.c:7525, :4958). We dump its inputs and result so keeper-rx can diff its own
    // GetThingShade port bit-exact. lgh indexing matches the C's lgh[y][x].
    {
        MapSubtlCoord sx = heartng->mappos.x.stl.num;
        MapSubtlCoord sy = heartng->mappos.y.stl.num;
        long lgh00 = get_subtile_lightness(&game.lish, sx,     sy);
        long lgh01 = get_subtile_lightness(&game.lish, sx + 1, sy);
        long lgh10 = get_subtile_lightness(&game.lish, sx,     sy + 1);
        long lgh11 = get_subtile_lightness(&game.lish, sx + 1, sy + 1);
        unsigned short shade = get_thing_shade(heartng);
        fprintf(oracle_jsonl,
            "{\"v\":%d,\"type\":\"thing_shade\",\"tick\":%ld,\"thing_idx\":%d,\"model\":%d,"
            "\"rendering_flags\":%d,\"owner\":%d,\"min_illum\":%d,\"pos_x\":%d,\"pos_y\":%d,\"pos_z\":%d,"
            "\"stl_x\":%d,\"stl_y\":%d,\"fract_x\":%d,\"fract_y\":%d,"
            "\"lgh00\":%ld,\"lgh01\":%ld,\"lgh10\":%ld,\"lgh11\":%ld,\"shade\":%d,\"shade_row\":%d}\n",
            ORACLE_DUMP_VERSION, (long)tick, (int)heartng->index, (int)heartng->model,
            (int)heartng->rendering_flags, (int)heartng->owner,
            (int)game.conf.rules[heartng->owner].game.thing_minimum_illumination,
            (int)heartng->mappos.x.val, (int)heartng->mappos.y.val, (int)heartng->mappos.z.val,
            (int)sx, (int)sy, (int)heartng->mappos.x.stl.pos, (int)heartng->mappos.y.stl.pos,
            lgh00, lgh01, lgh10, lgh11, (int)shade, (int)(shade >> 8));
    }

    oracle_dump_array("oracle_subtile_lightness_t18.bin", ORACLE_PROBE_SUBTILE_LIGHTNESS,
                      tick, game.lish.subtile_lightness);
    oracle_dump_array("oracle_stat_light_map_t18.bin", ORACLE_PROBE_STAT_LIGHT_MAP,
                      tick, game.lish.stat_light_map);

    // Tier B — the deterministic mesh randomisors (setup_mesh_randomizers, seed 0x0f0f0f0f). Pure code
    // output (not copyrighted map data), so the keeper-rx port asserts its LbRandomSeries table against
    // these exactly. Emitted as one JSONL record (512 small ints ~2KB).
    fprintf(oracle_jsonl, "{\"v\":%d,\"type\":\"randomisors\",\"count\":%d,\"values\":[",
            ORACLE_DUMP_VERSION, RANDOMISORS_LEN);
    for (int i = 0; i < RANDOMISORS_LEN; i++)
        fprintf(oracle_jsonl, "%s%d", i ? "," : "", (int)randomisors[i]);
    fprintf(oracle_jsonl, "]}\n");

    // Tier B — the isometric per-(subtile,height) shade_intensity captured during rendering: DK's terrain
    // vertex shade *including* the randomisors dither the keeper-rx column walk must reproduce.
    oracle_dump_iso_shade("oracle_iso_shade_t18.bin", tick);
}

void ftest_oracle_close(void)
{
    if (oracle_jsonl != NULL)
    {
        fclose(oracle_jsonl);
        oracle_jsonl = NULL;
    }
}

// --- The standalone spike: a thin wrapper over the shared dump, for MODE=oracle -----------------------

TbBool ftest_oracle_spike_init()
{
    // Fire every turn from the start; the action self-terminates at the target tick.
    ftest_append_action(ftest_oracle_spike_action001__dump, 0, NULL);
    return true;
}

FTestActionResult ftest_oracle_spike_action001__dump(struct FTestActionArgs* const args)
{
    (void)args;
    static TbBool setup_done = false;

    struct Thing* heartng = get_player_soul_container(PLAYER0);
    if (thing_is_invalid(heartng))
    {
        FTEST_FAIL_TEST("oracle: PLAYER0 has no dungeon heart on map302 (is -campaign classic set?)");
        return FTRs_Go_To_Next_Action;
    }

    if (!setup_done)
    {
        // Isolate the deterministic light field: clear the heart light's flicker bits so
        // render_intensity = intensity<<8 (no UNSYNC_RANDOM term). See dump-format.md "Flicker".
        game.lish.lights[heartng->light_id].flags2 &= ~0xFE;
        // Camera on the heart (its dynamic light renders near the camera) — the documented default view.
        ftest_util_reveal_map(PLAYER0);
        ftest_util_move_camera_to_thing(heartng, PLAYER0);
        // Suspend the mouse so live pointer movement can't pan/zoom the camera and drift the frame the
        // iso-shade is captured from (the parity ftest does the same for its shots).
        LbMouseSuspend();
        ftest_oracle_begin(ORACLE_TARGET_TICK);
        if (oracle_jsonl == NULL)
        {
            FTEST_FRAMEWORK_ABORT("oracle: could not open JSONL output");
            return FTRs_Go_To_Next_Action;
        }
        setup_done = true;
    }

    ftest_oracle_write_heartbeat();

    if (get_gameturn() < oracle_target_tick)
        return FTRs_Repeat_Current_Action;

    ftest_oracle_write_dumps(get_gameturn());
    ftest_oracle_close();
    FTESTLOG("oracle: dump complete at tick %ld", (long)get_gameturn());
    return FTRs_Go_To_Next_Action; // completes the test -> game auto-exits
}

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
