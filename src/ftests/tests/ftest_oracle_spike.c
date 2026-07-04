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
#include "../../config_objects.h"
#include "../../thing_objects.h"
#include "../../map_data.h"
#include "../../player_instances.h"

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

struct ftest_oracle_spike__variables
{
    GameTurn target_tick;
    FILE* jsonl;      // Tier A: per-turn heart-beat records + the light-input record
    TbBool setup_done;
    TbBool dumped;
};

static struct ftest_oracle_spike__variables ftest_oracle_spike__vars = {
    .target_tick = ORACLE_TARGET_TICK,
    .jsonl = NULL,
    .setup_done = false,
    .dumped = false,
};

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

TbBool ftest_oracle_spike_init()
{
    // Fire every turn from the start; the action self-terminates at the target tick.
    ftest_append_action(ftest_oracle_spike_action001__dump, 0, &ftest_oracle_spike__vars);
    return true;
}

FTestActionResult ftest_oracle_spike_action001__dump(struct FTestActionArgs* const args)
{
    struct ftest_oracle_spike__variables* const vars = args->data;

    struct Thing* heartng = get_player_soul_container(PLAYER0);
    if (thing_is_invalid(heartng))
    {
        FTEST_FAIL_TEST("oracle: PLAYER0 has no dungeon heart on map302 (is -campaign classic set?)");
        return FTRs_Go_To_Next_Action;
    }

    struct Light* lgt = &game.lish.lights[heartng->light_id];

    if (!vars->setup_done)
    {
        // Isolate the deterministic light field: clear the heart light's flicker bits so
        // render_intensity = intensity<<8 (no UNSYNC_RANDOM term). See dump-format.md "Flicker".
        lgt->flags2 &= ~0xFE;
        // Camera on the heart (its dynamic light renders near the camera) — the documented default view.
        ftest_util_reveal_map(PLAYER0);
        ftest_util_move_camera_to_thing(heartng, PLAYER0);

        vars->jsonl = oracle_open("oracle_heartbeat_t18.jsonl", "w");
        if (vars->jsonl == NULL)
        {
            FTEST_FRAMEWORK_ABORT("oracle: could not open JSONL output");
            return FTRs_Go_To_Next_Action;
        }
        fprintf(vars->jsonl,
            "{\"v\":%d,\"type\":\"meta\",\"probe\":\"heartbeat\",\"level\":302,\"campaign\":\"classic\","
            "\"target_tick\":%ld,\"heart_flicker_disabled\":true,\"engine\":\"keeperfx-oracle-dumps\"}\n",
            ORACLE_DUMP_VERSION, (long)vars->target_tick);
        vars->setup_done = true;
    }

    // Tier A — the beating heart's deterministic per-turn state (thing_objects.c:1120 update).
    struct ObjectConfigStats* objst = get_object_model_stats(heartng->model);
    unsigned char intensity = light_get_light_intensity(heartng->light_id);
    fprintf(vars->jsonl,
        "{\"v\":%d,\"type\":\"heartbeat\",\"tick\":%ld,\"thing_idx\":%d,\"current_frame\":%d,"
        "\"anim_time\":%ld,\"beat_direction\":%d,\"light_id\":%d,\"light_intensity\":%d,"
        "\"health\":%ld,\"max_health\":%ld}\n",
        ORACLE_DUMP_VERSION, (long)get_gameturn(), (int)heartng->index,
        (int)heartng->current_frame, (long)heartng->anim_time,
        (int)(signed char)heartng->heart.beat_direction, (int)heartng->light_id, (int)intensity,
        (long)heartng->health, (long)objst->health);

    if (get_gameturn() < vars->target_tick)
        return FTRs_Repeat_Current_Action;

    // Tier B — array snapshot + the ground-truth light inputs the keeper-rx diff feeds to Compute.
    fprintf(vars->jsonl,
        "{\"v\":%d,\"type\":\"lightdump\",\"tick\":%ld,\"light_id\":%d,\"light_intensity\":%d,"
        "\"radius\":%d,\"range\":%d,\"pos_x\":%d,\"pos_y\":%d,\"pos_z\":%d,\"stl_x\":%d,\"stl_y\":%d,"
        "\"flags2\":%d,\"array_width\":%d,\"array_height\":%d}\n",
        ORACLE_DUMP_VERSION, (long)get_gameturn(), (int)heartng->light_id, (int)intensity,
        (int)lgt->radius, (int)lgt->range,
        (int)lgt->mappos.x.val, (int)lgt->mappos.y.val, (int)lgt->mappos.z.val,
        (int)coord_subtile(lgt->mappos.x.val), (int)coord_subtile(lgt->mappos.y.val),
        (int)lgt->flags2, ORACLE_DUMP_STRIDE, ORACLE_DUMP_STRIDE);

    oracle_dump_array("oracle_subtile_lightness_t18.bin", ORACLE_PROBE_SUBTILE_LIGHTNESS,
                      get_gameturn(), game.lish.subtile_lightness);
    oracle_dump_array("oracle_stat_light_map_t18.bin", ORACLE_PROBE_STAT_LIGHT_MAP,
                      get_gameturn(), game.lish.stat_light_map);

    if (vars->jsonl != NULL)
    {
        fclose(vars->jsonl);
        vars->jsonl = NULL;
    }
    vars->dumped = true;
    FTESTLOG("oracle: dump complete at tick %ld", (long)get_gameturn());
    return FTRs_Go_To_Next_Action; // completes the test -> game auto-exits
}

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
