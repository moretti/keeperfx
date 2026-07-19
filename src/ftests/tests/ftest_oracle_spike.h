/**
 * @file ftest_oracle_spike.h
 * @brief Oracle-dump spike — emits ground-truth numeric state for the keeper-rx port to diff.
 *
 * Loads map00302 ("Vassago", the `classic` free-play pack), runs deterministically, and each turn
 * appends the beating dungeon heart's state (Tier A) to a JSONL file; at the target turn it also
 * dumps the subtile-lightness and stat-light-map arrays (Tier B) as little-endian binary. The
 * keeper-rx side diffs these against DungeonHeartBeatSystem and StaticLightMap.Compute.
 *
 * Format + rationale: keeper-rx/docs/oracle/ (README.md, dump-format.md), keeper-rx ADR-0016.
 */

#pragma once

#include "../../globals.h"

#ifdef FUNCTESTING

#ifdef __cplusplus
extern "C" {
#endif

TbBool ftest_oracle_spike_init();

// The oracle dump, exposed so the parity-screenshot ftest can emit the same numeric ground truth in the
// SAME game launch (one boot, one frozen frame — no second run, no mouse-drift window between runs). The
// standalone spike below is a thin wrapper over these. Call order per run:
//   ftest_oracle_begin(target)  once, in setup — arms the iso-shade capture and opens the JSONL + meta.
//   ftest_oracle_write_heartbeat()  every turn up to the target — the Tier-A per-turn beat record.
//   ftest_oracle_write_dumps(tick)  once, at the target turn AFTER the frame is drawn — light inputs,
//                                   thing-shade, the Tier-B arrays, the randomisors, and the iso-shade.
//   ftest_oracle_close()  once, after the last write — flushes and closes the JSONL.
void ftest_oracle_begin(GameTurn target_tick);
void ftest_oracle_write_heartbeat(void);
void ftest_oracle_write_dumps(GameTurn tick);
void ftest_oracle_close(void);

// --- Creature-shadow oracle (keeper-rx docs/design/creature-shadows.md slice 3) ---------------------
// A SEPARATE dump file (oracle_creature_shadow.jsonl) grounding find_closest_lights / create_shadows. It is
// armed for exactly ONE rendered frame: the caller (the parity ftest) calls ftest_oracle_shadow_begin()
// right before the target frame's keeper_screen_redraw() and ftest_oracle_shadow_close() right after, so the
// two writers below — called in place from engine_render.c's shadow path — emit one frame's rows and no
// more. Both writers are no-ops until begin() opens the file, so they are safe to leave compiled into the
// render path unconditionally (behind FUNCTESTING). Struct params are forward-declared here (pointers only);
// the .c pulls in their full definitions.
struct Thing;
struct Coord3d;
struct EngineCoord;

void ftest_oracle_shadow_begin(void);
void ftest_oracle_shadow_close(void);

// One row per creature that reaches the shadow cast, from find_closest_lights: the ordered candidate light
// list the walk visits (static list then dynamic list, in next_in_list order) and the kept nearest lights in
// slot order. keeper-rx's ShadowLights.NearestN(creature, candidates, n) must reproduce `kept` exactly.
//   kept  — the nlgt.coord[] array (Coord3d), the first `count` of which were kept.
//   n     — settings.video_shadows (the keep-N the pick used).
void ftest_oracle_write_shadow_selection(
    const struct Thing* thing, const struct Coord3d* kept, int count, int n);

// One row per (creature, kept light) from create_shadows, captured BEFORE rotpers (so the corner
// displacements are the raw FROM_FIXED offsets, not the camera-projected view coords). keeper-rx's
// CreatureShadow.Cast(creature, light, dims…) must reproduce sh_angle/sprite_angle/dist_sq and the four
// corner displacements. c1..c4 are the four EngineCoord corners; base_x/base_z are ecor->x/ecor->z, so the
// dumped displacement is (cK->x - base_x, cK->z - base_z).
void ftest_oracle_write_shadow_geometry(
    const struct Thing* thing, const struct Coord3d* light,
    int sh_angle, int sprite_angle, long dist_sq,
    int dim_ow, int dim_oh, int dim_tw, int dim_th, int animation_sprite, int current_frame,
    int base_x, int base_z,
    const struct EngineCoord* c1, const struct EngineCoord* c2,
    const struct EngineCoord* c3, const struct EngineCoord* c4);

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
