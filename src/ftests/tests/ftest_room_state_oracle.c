#include "ftest_room_state_oracle.h"

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
#include "../../room_data.h"
#include "../../config_terrain.h"
#include "../../slab_data.h"
#include "../../dungeon_data.h"
#include "../../player_data.h"
#include "../../player_instances.h"
#include "../../ver_defs.h"

#include "../../post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif

// Oracle dump format version — bump on any layout/meaning change (keeper-rx/docs/oracle/dump-format.md).
#define ROOM_STATE_ORACLE_VERSION 1

#define ROOM_STATE_ORACLE_LEVEL 9015

// A handful of ticks starting at the first opportunity after load (reinitialise_map_rooms has already run
// as part of the normal load sequence by the time any ftest action executes) — enough to prove the
// assembled room state is stable across ticks with no designation/creature/player action ever touching it.
#define ROOM_STATE_DUMP_TICKS 5

static FILE* room_state_jsonl = NULL;

// Resolve the dump directory: $KEEPERFX_ORACLE_OUT, else the CWD (mirrors the other oracle ftests so they
// share the capture-script convention).
static const char* room_state_out_dir(void)
{
    const char* dir = getenv("KEEPERFX_ORACLE_OUT");
    if (dir == NULL || dir[0] == '\0')
        return ".";
    mkdir(dir, 0755); // best-effort; ignore EEXIST
    return dir;
}

// Dumps one room's identity/content — every field the keeper-rx read-model needs to diff (rooms-read-model.md
// "Oracle observables"): owner, kind (+ code name for readability), creation_turn (never Room.index, ADR-0021),
// its decoded slab set (content, not the C's slabs_list order), the capacity pair, the storage amount, and
// efficiency.
static void room_state_dump_room(FILE* f, long tick, const struct Room* room)
{
    fprintf(f,
        "{\"v\":%d,\"type\":\"room\",\"tick\":%ld,"
        "\"owner\":%d,\"kind\":%d,\"kind_name\":\"%s\",\"creation_turn\":%ld,"
        "\"slabs_count\":%d,\"total_capacity\":%d,\"used_capacity\":%d,"
        "\"capacity_used_for_storage\":%lu,\"efficiency\":%d,\"slabs\":[",
        ROOM_STATE_ORACLE_VERSION, tick,
        (int)room->owner, (int)room->kind, room_code_name(room->kind), (long)room->creation_turn,
        (int)room->slabs_count, (int)room->total_capacity, (int)room->used_capacity,
        (unsigned long)room->capacity_used_for_storage, (int)room->efficiency);

    unsigned long k = 0;
    long i = room->slabs_list;
    TbBool first = true;
    while (i != 0)
    {
        MapSlabCoord slb_x = slb_num_decode_x(i);
        MapSlabCoord slb_y = slb_num_decode_y(i);
        fprintf(f, "%s[%d,%d]", first ? "" : ",", (int)slb_x, (int)slb_y);
        first = false;
        i = get_next_slab_number_in_room(i);
        k++;
        if (k > room->slabs_count)
            break; // defensive — mirrors the C's own infinite-loop guards elsewhere in room_data.c
    }
    fprintf(f, "]}\n");
}

// Dumps the Dungeon aggregate for one (owner, kind) pair with at least one room — the summed
// total_capacity/used_capacity/capacity_used_for_storage plus the discrete room count Dungeon maintains
// alongside the per-kind list (rooms-read-model.md §3's get_room_kind_total_used_and_storage_capacity).
static void room_state_dump_kind_totals(FILE* f, long tick, PlayerNumber owner, RoomKind rkind)
{
    struct Dungeon* dungeon = get_dungeon(owner);
    int32_t total_cap, used_cap, storaged_cap;
    get_room_kind_total_used_and_storage_capacity(dungeon, rkind, &total_cap, &used_cap, &storaged_cap);
    fprintf(f,
        "{\"v\":%d,\"type\":\"room_kind_totals\",\"tick\":%ld,"
        "\"owner\":%d,\"kind\":%d,\"kind_name\":\"%s\",\"discrete_count\":%d,"
        "\"total_capacity\":%d,\"used_capacity\":%d,\"capacity_used_for_storage\":%d}\n",
        ROOM_STATE_ORACLE_VERSION, tick, (int)owner, (int)rkind, room_code_name(rkind),
        (int)dungeon->room_discrete_count[rkind], (int)total_cap, (int)used_cap, (int)storaged_cap);
}

// One dump pass: every Player0 room, then every (Player0, kind) aggregate that has at least one room.
static void room_state_dump_tick(FILE* f, long tick)
{
    struct Dungeon* dungeon = get_dungeon(PLAYER0);
    for (RoomKind rkind = 1; rkind < game.conf.slab_conf.room_types_count; rkind++)
    {
        if (dungeon->room_list_start[rkind] == 0)
            continue;

        long i = dungeon->room_list_start[rkind];
        unsigned long k = 0;
        while (i != 0)
        {
            struct Room* room = room_get(i);
            if (room_is_invalid(room))
                break;
            i = room->next_of_owner;
            room_state_dump_room(f, tick, room);
            k++;
            if (k > ROOMS_COUNT)
                break;
        }

        room_state_dump_kind_totals(f, tick, PLAYER0, rkind);
    }
}

// forward declaration
FTestActionResult ftest_room_state_oracle_action__run(struct FTestActionArgs* const args);

TbBool ftest_room_state_oracle_init()
{
    ftest_append_action(ftest_room_state_oracle_action__run, 0, NULL);
    return true;
}

FTestActionResult ftest_room_state_oracle_action__run(struct FTestActionArgs* const args)
{
    if (args->times_executed == 0)
    {
        const long level = (long)get_loaded_level_number();
        if (level != ROOM_STATE_ORACLE_LEVEL)
        {
            FTEST_FAIL_TEST("room state oracle: expected level %d (M-rooms), got %ld — no rooms to observe",
                ROOM_STATE_ORACLE_LEVEL, level);
            return FTRs_Go_To_Next_Action;
        }

        char path[512];
        snprintf(path, sizeof(path), "%s/oracle_room_state.jsonl", room_state_out_dir());
        room_state_jsonl = fopen(path, "w");
        if (room_state_jsonl == NULL)
        {
            FTEST_FRAMEWORK_ABORT("room state oracle: failed to open '%s'", path);
            return FTRs_Go_To_Next_Action;
        }
        FTESTLOG("room state oracle: writing '%s'", path);

        const char* prov_build = getenv("KEEPERFX_ORACLE_BUILD");
        fprintf(room_state_jsonl,
            "{\"v\":%d,\"type\":\"meta\",\"probe\":\"room_state\",\"level\":%d,\"campaign\":\"classic\","
            "\"engine\":\"keeperfx-oracle-dumps\",\"version\":\"%s\",\"build\":\"%s\"}\n",
            ROOM_STATE_ORACLE_VERSION, ROOM_STATE_ORACLE_LEVEL, VER_STRING,
            prov_build ? prov_build : "");
    }

    room_state_dump_tick(room_state_jsonl, (long)get_gameturn());

    if ((long)args->times_executed + 1 >= ROOM_STATE_DUMP_TICKS)
    {
        FTESTLOG("room state oracle: dump complete (%d ticks)", ROOM_STATE_DUMP_TICKS);
        fclose(room_state_jsonl);
        room_state_jsonl = NULL;
        return FTRs_Go_To_Next_Action;
    }
    return FTRs_Repeat_Current_Action;
}

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
