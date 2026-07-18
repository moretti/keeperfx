/**
 * @file ftest_imp_gems_oracle.h
 * @brief In-tick imp-mine-gems oracle for the keeper-rx port (keeper-rx design/imp-jobs.md Slice S3).
 *
 * Spawns one imp on the synthetic M-gems fixture (level 9014 — M-mine's shape with its one designation
 * re-kinded to a Player0-owned, indestructible GEMS seam, plus a second, never-worked GEMS tile that only
 * pads the shared digger stack to length 2), tags both tiles for mining with the real player action at
 * turn 0, then lets the unmodified game loop run: the first imp self-assigns the worked seam off the shared
 * digger stack (add_gems_to_imp_stack, seam #9) and mines it forever — it can never be destroyed — banking
 * gold onto itself and, once its carry cap overflows, dropping a pile at its own stand subtile every hit. A
 * SECOND imp spawns a short, fixed delay later (a different creation_turn — and so a different frozen
 * THING_RANDOM seed, ADR-0028) and approaches the SAME worked seam from a different side, running the
 * identical loop independently. Once either imp's own overflow pile floods (rules.cfg's GoldPileMaximum),
 * imp_digs_mines's too-much-gold-lying-around gate aborts it back to check_out_imp_last_did, which either
 * relocates it to a fresh side of the block (task_repeats increments) or — on a 1-in-20 roll every 5th such
 * continuation — breaks it off onto a random shared-stack slot instead (the anti-camp escape,
 * is_digging_indestructible_place). Because the frozen build never advances a Thing's random_seed, whether
 * an imp EVER takes the escape is a fixed yes/no decided entirely by its birth seed — spawning a second imp
 * with a different creation_turn is what lets this one fixture observe both outcomes (docs/design/imp-jobs.md
 * Slice S3's derive-and-verify requirement). Per game tick it dumps, for each spawned imp, the imp-mine
 * oracle's baseline fields (mappos, active_state, instance timer, gold_carried, the dropped pile) plus
 * digger.task_repeats/last_did_job/task_stack_pos and the imp's own thing_index/creation_turn/random_seed —
 * alongside both gems tiles' kind/health (pinned, never moving) and the dungeon's task_count/
 * digger_stack_length.
 *
 * Pure observation: the ftest drives the normal simulation the way a player would (spawn creatures, tag
 * tiles) and logs existing state; it changes no engine behaviour.
 */

#pragma once

#include "../../globals.h"

#ifdef FUNCTESTING

#ifdef __cplusplus
extern "C" {
#endif

TbBool ftest_imp_gems_oracle_init();

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
