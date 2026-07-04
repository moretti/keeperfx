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
         // Oracle-dump spike: map00302 ("Vassago", classic free-play pack), dumps heart beat + lightness for keeper-rx to diff (ADR-0016).
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

