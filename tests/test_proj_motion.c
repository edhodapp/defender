// Functional tests for the projectile motion layer (proj_update_loop).
// Layer contract:
//   1. Active beams advance proj_x by ±4 each frame based on dx_sign.
//   2. Once proj_x's MSB becomes set (signed off-screen), the slot
//      deactivates (active bit cleared).
//   3. Inactive slots are left alone.
//   4. y in byte 0 (bits 0-5) is invariant during motion.

#include <stdio.h>
#include <stdlib.h>

#include "sim_helpers.h"

static sim_t *S;

// Suppress periodic spawn during a test by zeroing frame_counter each frame.
// Each call advances exactly one main_loop iteration.
static void run_quiet_frames(int n) {
    for (int i = 0; i < n; i++) {
        sim_mem_w(S, S->sym_frame_counter, 0);
        sim_run_frame(S);
    }
}

// Park the firmware at main_loop entry, then clear state. Subsequent
// place_*() writes are observed by the firmware on its next iteration.
static void scenario_reset(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, 28, 0, 0);
}

// 1. Right-moving beam advances exactly +4 per frame.
static void test_right_motion_step(void) {
    scenario_reset();
    sim_place_beam(S, 0, /*x*/68, /*y*/32, /*dx_sign*/0);
    run_quiet_frames(1);
    SIM_CHECK_MSG(sim_beam_x(S, 0) == 72, "got %d", sim_beam_x(S, 0));
    run_quiet_frames(1);
    SIM_CHECK_MSG(sim_beam_x(S, 0) == 76, "got %d", sim_beam_x(S, 0));
    run_quiet_frames(1);
    SIM_CHECK_MSG(sim_beam_x(S, 0) == 80, "got %d", sim_beam_x(S, 0));
}

// 2. Left-moving beam advances exactly -4 per frame.
static void test_left_motion_step(void) {
    scenario_reset();
    sim_place_beam(S, 0, /*x*/59, /*y*/32, /*dx_sign*/1);
    run_quiet_frames(1);
    SIM_CHECK_MSG(sim_beam_x(S, 0) == 55, "got %d", sim_beam_x(S, 0));
    run_quiet_frames(1);
    SIM_CHECK_MSG(sim_beam_x(S, 0) == 51, "got %d", sim_beam_x(S, 0));
}

// 3. Beam y is invariant during motion.
static void test_y_invariant(void) {
    scenario_reset();
    int spawn_y = 51;
    sim_place_beam(S, 0, 68, spawn_y, 0);
    for (int f = 0; f < 5; f++) {
        run_quiet_frames(1);
        if (!sim_beam_active(S, 0)) break;
        SIM_CHECK_MSG(sim_beam_y(S, 0) == spawn_y,
                      "y changed at frame %d to %d", f, sim_beam_y(S, 0));
    }
}

// 4. Right-moving beam at proj_x=124 deactivates next frame
//    (proj_x = 128 → MSB set).
static void test_right_decay_at_edge(void) {
    scenario_reset();
    sim_place_beam(S, 0, 124, 32, 0);
    SIM_CHECK(sim_beam_active(S, 0));
    run_quiet_frames(1);
    SIM_CHECK_MSG(!sim_beam_active(S, 0),
                  "beam still active after move past edge (x=%d)",
                  sim_beam_x(S, 0));
}

// 5. Left-moving beam at proj_x=3 deactivates next frame
//    (proj_x = -1 → MSB set).
static void test_left_decay_at_edge(void) {
    scenario_reset();
    sim_place_beam(S, 0, 3, 32, 1);
    SIM_CHECK(sim_beam_active(S, 0));
    run_quiet_frames(1);
    SIM_CHECK(!sim_beam_active(S, 0));
}

// 6. Right-moving beam at proj_x=120 stays active (next x=124, MSB clear);
//    one more frame deactivates it.
static void test_right_decay_two_frames(void) {
    scenario_reset();
    sim_place_beam(S, 0, 120, 32, 0);
    run_quiet_frames(1);
    SIM_CHECK_MSG(sim_beam_active(S, 0) && sim_beam_x(S, 0) == 124,
                  "expected active at 124, got active=%d x=%d",
                  sim_beam_active(S, 0), sim_beam_x(S, 0));
    run_quiet_frames(1);
    SIM_CHECK(!sim_beam_active(S, 0));
}

// 7. Inactive slots are not modified by proj_update.
//    Set a known byte pattern in inactive slots and verify it's untouched.
static void test_inactive_slot_untouched(void) {
    scenario_reset();
    // Slot 0 inactive (active bit cleared): byte0 = 0x00.
    // We can't easily put "junk" in a known-inactive slot via place_beam
    // (that sets active=1), but we can verify byte 0 stays 0 across frames.
    sim_mem_w(S, S->sym_projectiles + 0, 0);
    sim_mem_w(S, S->sym_projectiles + 1, 0xAB);     // arbitrary junk in x
    run_quiet_frames(3);
    SIM_CHECK_MSG(sim_mem_r(S, S->sym_projectiles + 0) == 0,
                  "inactive byte0 changed: 0x%02x",
                  sim_mem_r(S, S->sym_projectiles + 0));
    SIM_CHECK_MSG(sim_mem_r(S, S->sym_projectiles + 1) == 0xAB,
                  "inactive x changed: 0x%02x",
                  sim_mem_r(S, S->sym_projectiles + 1));
}

// 8. Multiple beams move independently.
static void test_multiple_beams(void) {
    scenario_reset();
    sim_place_beam(S, 0, 68, 32, 0);   // right
    sim_place_beam(S, 1, 60, 16, 1);   // left
    sim_place_beam(S, 2, 80, 40, 0);   // right
    run_quiet_frames(2);
    SIM_CHECK_MSG(sim_beam_x(S, 0) == 76, "slot 0 x=%d", sim_beam_x(S, 0));
    SIM_CHECK_MSG(sim_beam_x(S, 1) == 52, "slot 1 x=%d", sim_beam_x(S, 1));
    SIM_CHECK_MSG(sim_beam_x(S, 2) == 88, "slot 2 x=%d", sim_beam_x(S, 2));
    SIM_CHECK(sim_beam_y(S, 0) == 32);
    SIM_CHECK(sim_beam_y(S, 1) == 16);
    SIM_CHECK(sim_beam_y(S, 2) == 40);
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <defender.elf>\n", argv[0]); return 2; }
    S = sim_boot(argv[1]);

    test_right_motion_step();
    test_left_motion_step();
    test_y_invariant();
    test_right_decay_at_edge();
    test_left_decay_at_edge();
    test_right_decay_two_frames();
    test_inactive_slot_untouched();
    test_multiple_beams();

    sim_coverage_save_for(argv[0]);
    sim_print_summary("proj_motion");
    if (sim_fail_count) { printf("FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
