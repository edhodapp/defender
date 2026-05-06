// Functional tests for the Lander drift layer (update_lander).
// Layer contract:
//   - y increments by 1 only on frames where (frame_counter & 0x03) == 0
//   - y clamps at 47 (one row above the mountains)
//   - state2 bits (byte 2 bits 6-7) and other slot bytes are preserved
//   - inactive entity slots are not modified

#include <stdio.h>
#include <stdlib.h>

#include "sim_helpers.h"

static sim_t *S;

// Park firmware, clear state, set frame_counter to a starting value.
// Then place a Lander at the given y. Use this when we want predictable
// drift timing — the next main_loop iteration will see fc = start_fc+1.
static void scenario_with_fc(int start_fc, int slot, int world_x, int y) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, 28, 0, 0);                  // ship out of any lander's path
    sim_mem_w(S, S->sym_frame_counter, start_fc);
    sim_place_lander(S, slot, world_x, y);
}

static void run_n_frames(int n) {
    for (int i = 0; i < n; i++) sim_run_frame(S);
}

// 1. After 4 frames starting from fc=0, fc reaches 4 → exactly one drift.
static void test_one_drift_in_four_frames(void) {
    scenario_with_fc(0, /*slot*/0, /*x*/100, /*y*/10);
    run_n_frames(4);
    SIM_CHECK_MSG(sim_lander_y(S, 0) == 11,
                  "expected y=11 after 4 frames; got %d", sim_lander_y(S, 0));
}

// 2. After 8 frames, two drift events → y advances by 2.
static void test_two_drifts_in_eight_frames(void) {
    scenario_with_fc(0, 0, 100, 10);
    run_n_frames(8);
    SIM_CHECK_MSG(sim_lander_y(S, 0) == 12,
                  "expected y=12 after 8 frames; got %d", sim_lander_y(S, 0));
}

// 3. fc=1, 2, 3 are non-drift frames; lander stays at y=10.
static void test_no_drift_in_non_aligned_frames(void) {
    scenario_with_fc(0, 0, 100, 10);
    run_n_frames(3);
    SIM_CHECK_MSG(sim_lander_y(S, 0) == 10,
                  "expected y=10 after 3 non-drift frames; got %d",
                  sim_lander_y(S, 0));
}

// 4. y clamps at 47: place at y=47, run a drift frame, verify still 47.
static void test_clamp_at_bottom(void) {
    scenario_with_fc(0, 0, 100, 47);
    run_n_frames(4);                            // fc → 4 → drift attempted
    SIM_CHECK_MSG(sim_lander_y(S, 0) == 47,
                  "expected clamp at 47; got %d", sim_lander_y(S, 0));
}

// 5. y=46 → drift to 47 → clamp on next drift.
static void test_clamp_takes_effect_after_drift(void) {
    scenario_with_fc(0, 0, 100, 46);
    run_n_frames(4);
    SIM_CHECK_MSG(sim_lander_y(S, 0) == 47, "first drift y=%d", sim_lander_y(S, 0));
    run_n_frames(4);
    SIM_CHECK_MSG(sim_lander_y(S, 0) == 47, "clamp y=%d", sim_lander_y(S, 0));
}

// 6. Multiple landers drift in lockstep (they share frame_counter).
static void test_multiple_landers_drift_together(void) {
    scenario_with_fc(0, 0, 100, 10);
    sim_place_lander(S, 1, 50, 20);
    sim_place_lander(S, 2, 200, 30);
    run_n_frames(4);
    SIM_CHECK_MSG(sim_lander_y(S, 0) == 11, "slot 0 y=%d", sim_lander_y(S, 0));
    SIM_CHECK_MSG(sim_lander_y(S, 1) == 21, "slot 1 y=%d", sim_lander_y(S, 1));
    SIM_CHECK_MSG(sim_lander_y(S, 2) == 31, "slot 2 y=%d", sim_lander_y(S, 2));
}

// 7. world_x is invariant during drift.
static void test_world_x_invariant(void) {
    scenario_with_fc(0, 0, 173, 10);
    run_n_frames(8);
    SIM_CHECK_MSG(sim_lander_world_x(S, 0) == 173,
                  "world_x changed: got %d", sim_lander_world_x(S, 0));
}

// 8. Inactive slots are untouched. (Slot 0 active; slots 1-3 inactive.
//    Run several frames; slots 1-3 should still read inactive.)
static void test_inactive_slots_untouched(void) {
    scenario_with_fc(0, 0, 100, 10);
    SIM_CHECK(!sim_lander_active(S, 1));
    SIM_CHECK(!sim_lander_active(S, 2));
    SIM_CHECK(!sim_lander_active(S, 3));
    run_n_frames(8);
    SIM_CHECK(!sim_lander_active(S, 1));
    SIM_CHECK(!sim_lander_active(S, 2));
    SIM_CHECK(!sim_lander_active(S, 3));
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <defender.elf>\n", argv[0]); return 2; }
    S = sim_boot(argv[1]);

    test_one_drift_in_four_frames();
    test_two_drifts_in_eight_frames();
    test_no_drift_in_non_aligned_frames();
    test_clamp_at_bottom();
    test_clamp_takes_effect_after_drift();
    test_multiple_landers_drift_together();
    test_world_x_invariant();
    test_inactive_slots_untouched();

    sim_coverage_save_for(argv[0]);
    sim_print_summary("lander_drift");
    if (sim_fail_count) { printf("FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
