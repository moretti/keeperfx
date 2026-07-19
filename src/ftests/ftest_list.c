#include "../globals.h"

#ifdef FUNCTESTING

#include "../pre_inc.h"

#include "ftest.h"

/**
 * Add the header files for all tests below here
 */
#include "tests/ftest_oracle_spike.h"
#include "tests/ftest_movement_oracle.h"
#include "tests/ftest_creature_state_slap.h"
#include "tests/ftest_parity_screenshot.h"
#include "tests/ftest_ariadne_oracle.h"
#include "tests/ftest_imp_dig_oracle.h"
#include "tests/ftest_imp_convert_oracle.h"
#include "tests/ftest_imp_mine_oracle.h"
#include "tests/ftest_imp_gems_oracle.h"
#include "tests/ftest_starter_dungeon_oracle.h"
#include "tests/ftest_starter_dungeon_jobs_oracle.h"
#include "tests/ftest_dig_shuffle_oracle.h"
#include "tests/ftest_room_state_oracle.h"
#include "tests/ftest_imp_haul_oracle.h"
#include "tests/ftest_imp_arm_trap_oracle.h"
#include "tests/ftest_imp_prison_drag_oracle.h"
#include "tests/ftest_imp_wander_variety_oracle.h"
#if !defined(__APPLE__) // the legacy ftests below reference since-changed APIs (magic.h, the rules-config
// layout, get_slab_attrs); the macOS oracle build (keeper-rx ADR-0016) compiles only the framework +
// oracle_spike, so they are excluded here and from macos.mk's FTEST_C_SOURCES.
#include "tests/ftest_template.h"
#include "tests/ftest_bug_imp_tp_job_attack_door.h"
#include "tests/ftest_bug_pathing_pillar_circling.h"
#include "tests/ftest_bug_imp_goldseam_dig.h"
#include "tests/ftest_bug_invisible_units_cant_select.h"
#include "tests/ftest_bug_pathing_stair_treasury.h"
#include "tests/ftest_bug_ai_bridge.h"
#endif
// append your test include here, eg: #include "tests/ftest_your_test_header.h"

#include "../post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Append the name/init function of your test here so it can be found/executed.
 */
struct ftest_onlyappendtests__config ftest_onlyappendtests__conf = {

    // place regular tests in this list
    .tests_list = {
         // Oracle-dump spike: dumps heart beat + lightness for keeper-rx to diff (ADR-0016). The level below is
         // the DEFAULT only (map00302, "Vassago", classic free-play pack) — override at runtime with
         // KEEPERFX_FTEST_LEVEL / KEEPERFX_FTEST_CAMPAIGN; the meta line records whichever level actually loaded.
         { .test_name="oracle_spike",                       .init_func=ftest_oracle_spike_init,                     .level_file="classic",  .level=302, .frame_skip=0 },
         // Movement velocity-integration oracle: a frozen imp on a claimed flat pad, dumped per turn across
         // fall/coast/walk for the keeper-rx MovementSystem to diff (keeper-rx ADR-0016, movement.md).
         { .test_name="movement_oracle",                    .init_func=ftest_movement_oracle_init,                  .level_file="classic",  .level=302, .frame_skip=0 },
         // Creature-state slap oracle: one imp slapped on a claimed flat pad, dumps active_state + cower timer
         // per turn across the 18-turn cower and the restore, for the keeper-rx state machine to diff (creature-states.md).
         { .test_name="creature_state_slap",                .init_func=ftest_creature_state_slap_init,              .level_file="classic",  .level=302, .frame_skip=0 },
         // Parity screenshot oracle: tick-accurate frames + metadata json for keeper-rx's visual parity harness.
         // The level below is the DEFAULT only — override the map/campaign at runtime with KEEPERFX_FTEST_LEVEL / KEEPERFX_FTEST_CAMPAIGN.
         { .test_name="parity_screenshot",                  .init_func=ftest_parity_screenshot_init,                .level_file="classic",  .level=302, .frame_skip=0 },
         // Ariadne navigation oracle: loads a synthetic keeper-rx level (mapNNNNN.* in levels/classic) and
         // dumps the NavColour raster (D0) for the keeper-rx pathfinding port to diff (ADR-0024, pathfinding.md).
         // Level below is the DEFAULT (M-full) — pick the fixture at runtime with KEEPERFX_FTEST_LEVEL.
         { .test_name="ariadne_oracle",                     .init_func=ftest_ariadne_oracle_init,                   .level_file="classic",  .level=9003, .frame_skip=0 },
         // In-tick imp-dig oracle: one imp self-assigns and digs the M-full earth tile (slab 21,42); dumps
         // mappos/state/instance/block-health/task-count per turn for the keeper-rx dig loop to diff (imp-jobs.md).
         { .test_name="imp_dig_oracle",                     .init_func=ftest_imp_dig_oracle_init,                   .level_file="classic",  .level=9003, .frame_skip=0 },
         // In-tick imp-convert oracle: one Player0 imp self-assigns and neutralises the M-convert fixture's
         // one Player1-CLAIMED tile (slab 21,10), then claims the freed path for Player0; dumps
         // mappos/state/instance/block-kind-owner-health/digger-stack/both-dungeons'-total_area per turn for
         // the keeper-rx convert loop to diff (imp-jobs.md Slice 2's standing coverage-gap note).
         { .test_name="imp_convert_oracle",                 .init_func=ftest_imp_convert_oracle_init,               .level_file="classic",  .level=9012, .frame_skip=0 },
         // In-tick imp-mine-gold oracle: one imp self-assigns and mines the M-mine fixture's neutral GOLD seam
         // (slab 21,42) to completion; dumps the imp-dig oracle's baseline fields plus gold_carried and the
         // carry-cap overflow's dropped pile (existence/gold_stored/sprite_size/owner) per turn, through the
         // mine-out and a short deterministic tail, for the keeper-rx mine-gold loop to diff (imp-jobs.md
         // Slice S2).
         { .test_name="imp_mine_oracle",                    .init_func=ftest_imp_mine_oracle_init,                  .level_file="classic",  .level=9013, .frame_skip=0 },
         // In-tick imp-mine-gems oracle: two Player0 imps (staggered creation turns, so different frozen
         // THING_RANDOM seeds) mine the M-gems fixture's Player0-owned, indestructible GEMS seam (slab
         // 21,42) forever, each parked by imp_digs_mines's too-much-gold-lying-around gate once its own
         // overflow pile floods and either relocated or (a 1-in-20 roll every 5th continuation) escaped onto
         // a random shared-stack slot; dumps both imps' mappos/state/instance/gold/digger-stack fields plus
         // thing_index/creation_turn/random_seed per turn, for the keeper-rx mine-gems loop to diff
         // (imp-jobs.md Slice S3).
         { .test_name="imp_gems_oracle",                    .init_func=ftest_imp_gems_oracle_init,                  .level_file="classic",  .level=9014, .frame_skip=0 },
         // Wall-torch slab-object oracle: on the starter dungeon (9008), reinforce the east room's walls and
         // dig them back out, dumping every torch object + its light per phase for the keeper-rx torch mesh /
         // invalidation gate (wall-torch-slab-objects.md Steps 0+7). Direct primitives -> deterministic + fast.
         { .test_name="starter_dungeon_oracle",             .init_func=ftest_starter_dungeon_oracle_init,           .level_file="classic",  .level=9008, .frame_skip=0 },
         // Realistic imp job oracle: one isolated imp digs → hands off to claim → reinforces on the starter
         // dungeon (9008), dumping the dig/claim/reinforce loop per turn — consolidates the retired 9005/6/7.
         { .test_name="starter_dungeon_jobs_oracle",        .init_func=ftest_starter_dungeon_jobs_oracle_init,      .level_file="classic",  .level=9008, .frame_skip=0 },
         // Unattached-designer-object dig oracle: on the dig-shuffle fixture (9010), dig the earth slab
         // holding three designer .tng objects (Barrel/Torch/GoldChest, one per Persistence category) and
         // census which survive, for the keeper-rx shuffle_unattached_things_on_slab parity gate.
         { .test_name="dig_shuffle_oracle",                 .init_func=ftest_dig_shuffle_oracle_init,               .level_file="classic",  .level=9010, .frame_skip=0 },
         // Load-time room-state oracle: on the M-rooms fixture (9015: a Dungeon Heart room, a 2x2 Treasury
         // and a 2-slab Training room, all Player0-owned), dumps every room's identity/content and the
         // per-(player,kind) Dungeon aggregates for a few ticks — no designation/creature/action, since
         // room assembly is entirely a load-time effect (keeper-rx docs/design/rooms-read-model.md, S4).
         { .test_name="room_state_oracle",                  .init_func=ftest_room_state_oracle_init,                .level_file="classic",  .level=9015, .frame_skip=0 },
         // In-tick imp-haul-gold oracle: on the M-haul fixture (9016: M-mine's shape plus a 2-slab Player0
         // Treasury), one imp mines the neutral GOLD seam (slab 21,42), diverting mid-mine to bank once the
         // 128-turn money-for-treasury throttle window opens with a reachable treasury, then resumes mining
         // and sweeps up any loose overflow pile it left behind; dumps the imp-mine oracle's baseline fields
         // plus continue_state/random_seed, the Treasury's identity/capacity, every gold-hoard on its slabs,
         // and total_money_owned, through mine-out and a long deterministic bank/sweep tail (keeper-rx
         // docs/design/imp-hauling.md Slice S5).
         { .test_name="imp_haul_oracle",                    .init_func=ftest_imp_haul_oracle_init,                  .level_file="classic",  .level=9016, .frame_skip=0 },
         // In-tick imp-arms-trap oracle: on the M-trap fixture (9017: a Player0 claimed room with a 2-slab
         // Workshop), a manufactured Boulder crate is placed in the Workshop and one Boulder trap is placed
         // unarmed on owned ground (both pre-t0 scenario transactions), then the unmodified game loop runs:
         // the imp self-assigns the empty-trap job off the shared digger stack (seam #5), walks to the
         // crate, picks it up (decrementing the workshop's item accounting), drags it BACKWARDS to the trap,
         // arms it (num_shots 0 -> shots, the transparency render flags flip), destroys the crate and bumps
         // traps_armed; dumps the imp's state/pos, the crate's and trap's own identity/flags, the workshop's
         // capacity, the owner's per-kind trap counters, and the shared digger stack's seam #5/#10 occupancy
         // through arm completion and a bounded settle tail (keeper-rx docs/design/imp-hauling.md Slice S6).
         { .test_name="imp_arm_trap_oracle",                .init_func=ftest_imp_arm_trap_oracle_init,              .level_file="classic",  .level=9017, .frame_skip=0 },
         { .test_name="imp_prison_drag_oracle",             .init_func=ftest_imp_prison_drag_oracle_init,           .level_file="classic",  .level=9018, .frame_skip=0 },
         // In-tick idle-imp-wander-diversity oracle: on the M-wander fixture (9019: a fully Player0-claimed
         // open floor field with a Dungeon Heart and no work of any kind anywhere), six imps at spread-out
         // spawn slabs go idle on their first tick and, finding nothing to do, ever, all fall to
         // creature_choose_random_destination_on_valid_adjacent_slab — each imp's own frozen (thing index,
         // creation turn) pair gives it a different (start_stl, m) pair, so the captured golden covers many
         // wander directions and, unlike every other oracle's single accidentally-axis-aligned leg, genuine
         // off-axis diagonal legs too (keeper-rx docs/rx-internals/87-special-digger-jobs.md).
         { .test_name="imp_wander_variety_oracle",          .init_func=ftest_imp_wander_variety_oracle_init,        .level_file="classic",  .level=9019, .frame_skip=0 },
#if !defined(__APPLE__) // legacy ftests excluded from the macOS oracle build (see the include guard above)
         { .test_name="example_template_test",              .init_func=ftest_template_init,                         .level_file="keeporig", .level=8,  .frame_skip=8 },
         { .test_name="bug_imp_tp_attack_door__claim",      .init_func=ftest_bug_imp_tp_attack_door__claim_init,    .level_file="deepdngn", .level=80, .frame_skip=8 },
         { .test_name="bug_imp_tp_attack_door__prisoner",   .init_func=ftest_bug_imp_tp_attack_door__prisoner_init, .level_file="deepdngn", .level=80, .frame_skip=8 },
         { .test_name="bug_imp_tp_attack_door__deadbody",   .init_func=ftest_bug_imp_tp_attack_door__deadbody_init, .level_file="deepdngn", .level=80, .frame_skip=8 },
         { .test_name="bug_imp_goldseam_dig",               .init_func=ftest_bug_imp_goldseam_dig_init,             .level_file="keeporig", .level=1,  .frame_skip=8 },
         { .test_name="bug_pathing_stair_treasury",         .init_func=ftest_bug_pathing_stair_treasury_init,       .level_file="keeporig", .level=1,  .frame_skip=8 },
         { .test_name="bug_invisible_units_cant_select",    .init_func=ftest_bug_invisible_units_cant_select_init,  .level_file="keeporig", .level=1,  .frame_skip=0 },
#endif
         // WIP TEST { .test_name="bug_pathing_pillar_circling",        .init_func=ftest_bug_pathing_pillar_circling_init,      .level_file="keeporig", .level=1, .frame_skip=0 },
         // append your test to tests_list here, eg: { .test_name="your_test_name",    .init_func=ftest_your_test_name_init, .level_file="lostlvls", .level=103 },
    }
#if !defined(__APPLE__)
    ,
    // place long-running tests in this list, to include them use the -includelongtests flag
    .long_running_tests_list = {
        { .test_name="bug_ai_bridge",                      .init_func=ftest_bug_ai_bridge_init,                    .level_file="keeporig", .level=15, .frame_skip=128, .seed=1, .repeat_n_times=100 },
    }
#endif
};


#ifdef __cplusplus
}
#endif

#endif

