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

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
