/**
 * @file ftest_dig_shuffle_oracle.h
 * @brief Unattached-designer-object dig oracle for the keeper-rx port (shuffle_unattached_things_on_slab
 *        parity gate).
 *
 * Loads the compact dig-shuffle fixture (level 9010: keeper-rx's synthetic DigShuffleScene, which places
 * three unattached designer .tng objects — a Torch (Persistence=Persist), a GoldChest (Persistence=Move)
 * and a Barrel (Persistence=Vanish) — inside a single earth slab at (40,42), away from the dungeon heart).
 * Digging that slab runs place_slab_type_on_map(SlbT_PATH) -> shuffle_unattached_things_on_slab
 * (https://github.com/dkfans/keeperfx/blob/v1.4.0/src/map_blocks.c#L1397), which sweeps the slab's own 3x3
 * subtiles for things not "attached" to it (parent_idx != this slab's number) and, per the object's
 * Persistence category, either leaves it (Persist), relocates it to the nearest free position (Move), or
 * deletes it (Vanish). The oracle census's identity per phase lets keeper-rx diff which of the three
 * designer objects survived the dig, and where.
 *
 *   load - the three designer .tng objects present at map load (baseline, before any dig).
 *   dig  - after digging earth slab (40,42): what's left (Torch persists, GoldChest may have moved,
 *          Barrel is gone).
 *
 * Deterministic and near-instant: it drives dig_out_block directly (no imp pathfinding, no digger-stack
 * gate), so it needs no game-speed knob. Pure observation of DK's own shuffle mechanism — it changes no
 * engine behaviour. keeper-rx diffs the census by identity, never by DK's index numbering (keeper-rx
 * ADR-0021).
 *
 * This is a fork-only oracle ftest (not part of upstream KeeperFX v1.4.0) — see ftest_dig_shuffle_oracle.c
 * for the KeeperFX v1.4.0 permalinks backing the engine functions/values it exercises.
 */

#pragma once

#include "../../globals.h"

#ifdef FUNCTESTING

#ifdef __cplusplus
extern "C" {
#endif

TbBool ftest_dig_shuffle_oracle_init();

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
