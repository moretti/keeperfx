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

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
