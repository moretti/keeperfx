/**
 * @file ftest_starter_dungeon_oracle.h
 * @brief Wall-torch slab-object oracle for the keeper-rx port (keeper-rx design/wall-torch-slab-objects.md,
 *        Steps 0 + 7 — the oracle gate).
 *
 * Loads the compact starter-dungeon fixture (level 9008: a walled Player0 heart room with %5-grid torch
 * walls, and a pre-claimed 3x3 "new room" to the east whose four mid-edge earth neighbours sit on the torch
 * grid). Then, via the same terrain-mutation primitives the imp reinforce/dig instances drive
 * (place_slab_type_on_map / dig_out_block), it forces the two terrain changes the port must match, dumping a
 * full census of every torch object + its attached light after each:
 *
 *   load        - the designer-placed .tng heart torches present at map load (baseline).
 *   reinforce   - reinforce each east-room mid-edge earth wall to WallTorch: one fresh slab-object torch per
 *                 orientation (N/S/E/W), so keeper-rx can diff the world position of every facing (Flag A).
 *   dig_slabobj - dig one freshly-reinforced WallTorch: its slab-object torch (+ light) must vanish (symptom 1
 *                 for slab-object torches: delete_attached_things matches parent_idx == slab number).
 *   dig_tng     - dig a pristine .tng heart torch wall: does DK remove the designer torch? (Step 0 — a .tng
 *                 torch carries parent_idx == own thing index, so the pure mechanism should leave it floating).
 *
 * Deterministic and near-instant: it drives the map primitives directly (no imp pathfinding, no 128-turn
 * digger-stack gate), so it needs no game-speed knob. Pure observation of DK's own place/remove mechanism —
 * it changes no engine behaviour. keeper-rx diffs the torch/light census by identity, never by DK's index
 * numbering (keeper-rx ADR-0021).
 */

#pragma once

#include "../../globals.h"

#ifdef FUNCTESTING

#ifdef __cplusplus
extern "C" {
#endif

TbBool ftest_starter_dungeon_oracle_init();

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
