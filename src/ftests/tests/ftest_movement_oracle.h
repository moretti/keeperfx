/**
 * @file ftest_movement_oracle.h
 * @brief Movement velocity-integration oracle for the keeper-rx port (keeper-rx ADR-0016, movement.md).
 *
 * Spawns one frozen imp on flat, claimed open ground and dumps its mappos + the four velocity fields
 * per turn across three isolated, nav-free / AI-free / collision-free runs (fall / coast / walk). The
 * keeper-rx MovementSystem diffs these turn-by-turn (ADR-0002 parity gate).
 */

#pragma once

#include "../../globals.h"

#ifdef FUNCTESTING

#ifdef __cplusplus
extern "C" {
#endif

TbBool ftest_movement_oracle_init();

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
