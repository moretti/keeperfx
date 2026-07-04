/**
 * @file ftest_parity_screenshot.h
 * @brief Parity screenshot oracle — tick-accurate KeeperFX frames + metadata for keeper-rx to compare.
 *
 * Loads a level (map00302 in the "classic" pack by default; override with KEEPERFX_FTEST_LEVEL /
 * KEEPERFX_FTEST_CAMPAIGN), runs deterministically, and at a fixed set of game turns writes the exact
 * rendered frame (PNG via take_screenshot) plus a metadata json sidecar (engine version, map, tick,
 * camera, resolution) into $KEEPERFX_PARITY_OUT (falling back to $KEEPERFX_ORACLE_OUT, then CWD). The
 * map in the json is read live (get_loaded_level_number), never assumed. keeper-rx reproduces each shot
 * from the json and scores the pair.
 *
 * Rationale + json schema: keeper-rx/docs/design/parity-snapshot-harness.md.
 */

#pragma once

#include "../../globals.h"

#ifdef FUNCTESTING

#ifdef __cplusplus
extern "C" {
#endif

TbBool ftest_parity_screenshot_init(void);

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
