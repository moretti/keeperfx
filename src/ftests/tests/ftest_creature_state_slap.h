/**
 * @file ftest_creature_state_slap.h
 * @brief Creature-state slap/cower/restore oracle for the keeper-rx port (keeper-rx ADR-0016,
 * design/creature-states.md).
 *
 * Spawns one imp on a claimed, flat, open-ground pad, applies the real slap_creature to it, and dumps its
 * active_state + cowers_from_slap_turns per turn across the 18-turn cower and the restore to its start
 * state. The keeper-rx creature-state machine diffs these turn-by-turn (ADR-0002 parity gate).
 */

#pragma once

#include "../../globals.h"

#ifdef FUNCTESTING

#ifdef __cplusplus
extern "C" {
#endif

TbBool ftest_creature_state_slap_init();

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
