// Functional tests for input_read + the immediate input-handling pipeline.
// We don't call input_read in isolation (it's an asm subroutine); instead,
// we drive each button via simavr's IOPORT IRQ and assert the observable
// state change after one main_loop iteration.

#include <stdio.h>
#include <stdlib.h>

#include "sim_helpers.h"

static sim_t *S;

static void scenario_reset(int sprite_y, int scroll, int facing) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, sprite_y, scroll, facing);
    // Release all buttons (active-low → 1)
    sim_btn_release(S->btn_up);
    sim_btn_release(S->btn_down);
    sim_btn_release(S->btn_left);
    sim_btn_release(S->btn_right);
    sim_btn_release(S->btn_a);
    sim_btn_release(S->btn_b);
}

static void quiet_run(void) {
    sim_mem_w(S, S->sym_frame_counter, 0);
    sim_run_frame(S);
}

// ---- Idle: no buttons pressed → no state change ----
static void test_idle_no_change(void) {
    scenario_reset(28, 100, 0);
    quiet_run();
    SIM_CHECK(sim_mem_r(S, S->sym_sprite_y) == 28);
    SIM_CHECK(sim_mem_r(S, S->sym_scroll_offset) == 100);
    SIM_CHECK(sim_mem_r(S, S->sym_ship_facing) == 0);
}

// ---- UP: sprite_y decrements by 1; clamps at 8 (radar floor) ----
static void test_up_normal(void) {
    scenario_reset(28, 0, 0);
    sim_btn_press(S->btn_up);
    quiet_run();
    SIM_CHECK_MSG(sim_mem_r(S, S->sym_sprite_y) == 27,
                  "got sprite_y=%d", sim_mem_r(S, S->sym_sprite_y));
}

static void test_up_step_to_clamp(void) {
    // From y=9, pressing UP yields y=8 (the clamped minimum).
    scenario_reset(9, 0, 0);
    sim_btn_press(S->btn_up);
    quiet_run();
    SIM_CHECK_MSG(sim_mem_r(S, S->sym_sprite_y) == 8,
                  "got %d", sim_mem_r(S, S->sym_sprite_y));
}

static void test_up_clamps_at_radar(void) {
    // From y=8, UP must not go further (would overlap radar in page 0).
    scenario_reset(8, 0, 0);
    sim_btn_press(S->btn_up);
    quiet_run();
    SIM_CHECK_MSG(sim_mem_r(S, S->sym_sprite_y) == 8,
                  "expected clamp at 8, got %d", sim_mem_r(S, S->sym_sprite_y));
}

// ---- DOWN: sprite_y increments by 1; clamps at 47 (mountain floor) ----
static void test_down_normal(void) {
    scenario_reset(28, 0, 0);
    sim_btn_press(S->btn_down);
    quiet_run();
    SIM_CHECK_MSG(sim_mem_r(S, S->sym_sprite_y) == 29,
                  "got sprite_y=%d", sim_mem_r(S, S->sym_sprite_y));
}

static void test_down_clamps_above_mountains(void) {
    scenario_reset(47, 0, 0);
    sim_btn_press(S->btn_down);
    quiet_run();
    SIM_CHECK_MSG(sim_mem_r(S, S->sym_sprite_y) == 47,
                  "expected clamp at 47, got %d", sim_mem_r(S, S->sym_sprite_y));
}

// ---- LEFT: ship_facing := 1, scroll_offset -= 1 ----
static void test_left_sets_facing_and_scroll(void) {
    scenario_reset(28, 100, 0);
    sim_btn_press(S->btn_left);
    quiet_run();
    SIM_CHECK_MSG(sim_mem_r(S, S->sym_ship_facing) == 1,
                  "facing=%d", sim_mem_r(S, S->sym_ship_facing));
    SIM_CHECK_MSG(sim_mem_r(S, S->sym_scroll_offset) == 99,
                  "scroll=%d", sim_mem_r(S, S->sym_scroll_offset));
}

// scroll wraparound: 0 - 1 = 255 (uint8)
static void test_left_scroll_wraps(void) {
    scenario_reset(28, 0, 0);
    sim_btn_press(S->btn_left);
    quiet_run();
    SIM_CHECK_MSG(sim_mem_r(S, S->sym_scroll_offset) == 255,
                  "scroll=%d", sim_mem_r(S, S->sym_scroll_offset));
}

// ---- RIGHT: ship_facing := 0, scroll_offset += 1 ----
static void test_right_sets_facing_and_scroll(void) {
    scenario_reset(28, 100, 1);                     // start facing left
    sim_btn_press(S->btn_right);
    quiet_run();
    SIM_CHECK_MSG(sim_mem_r(S, S->sym_ship_facing) == 0,
                  "facing=%d", sim_mem_r(S, S->sym_ship_facing));
    SIM_CHECK_MSG(sim_mem_r(S, S->sym_scroll_offset) == 101,
                  "scroll=%d", sim_mem_r(S, S->sym_scroll_offset));
}

// scroll wraparound: 255 + 1 = 0
static void test_right_scroll_wraps(void) {
    scenario_reset(28, 255, 0);
    sim_btn_press(S->btn_right);
    quiet_run();
    SIM_CHECK_MSG(sim_mem_r(S, S->sym_scroll_offset) == 0,
                  "scroll=%d", sim_mem_r(S, S->sym_scroll_offset));
}

// ---- B: should result in a beam in slot 0 (we use this here only to
//        verify the bit gets through input_read; deeper spawn tests are
//        in test_proj_spawn).
static void test_b_spawns_beam(void) {
    scenario_reset(28, 0, 0);
    sim_btn_press(S->btn_b);
    quiet_run();
    SIM_CHECK_MSG(sim_beam_active(S, 0),
                  "expected beam in slot 0; byte0=0x%02X",
                  sim_mem_r(S, S->sym_projectiles));
}

// ---- A: no observable side effect today (placeholder for smart bomb).
//        Verify pressing A doesn't accidentally change other state.
static void test_a_no_state_change(void) {
    scenario_reset(28, 100, 0);
    sim_btn_press(S->btn_a);
    quiet_run();
    SIM_CHECK(sim_mem_r(S, S->sym_sprite_y) == 28);
    SIM_CHECK(sim_mem_r(S, S->sym_scroll_offset) == 100);
    SIM_CHECK(sim_mem_r(S, S->sym_ship_facing) == 0);
    SIM_CHECK(!sim_beam_active(S, 0));
}

// ---- Multi-button: UP+RIGHT both register the same frame ----
static void test_up_and_right_simultaneous(void) {
    scenario_reset(28, 100, 1);
    sim_btn_press(S->btn_up);
    sim_btn_press(S->btn_right);
    quiet_run();
    SIM_CHECK_MSG(sim_mem_r(S, S->sym_sprite_y) == 27,
                  "sprite_y=%d", sim_mem_r(S, S->sym_sprite_y));
    SIM_CHECK_MSG(sim_mem_r(S, S->sym_scroll_offset) == 101,
                  "scroll=%d", sim_mem_r(S, S->sym_scroll_offset));
    SIM_CHECK(sim_mem_r(S, S->sym_ship_facing) == 0);
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <defender.elf>\n", argv[0]); return 2; }
    S = sim_boot(argv[1]);

    test_idle_no_change();
    test_up_normal();
    test_up_step_to_clamp();
    test_up_clamps_at_radar();
    test_down_normal();
    test_down_clamps_above_mountains();
    test_left_sets_facing_and_scroll();
    test_left_scroll_wraps();
    test_right_sets_facing_and_scroll();
    test_right_scroll_wraps();
    test_b_spawns_beam();
    test_a_no_state_change();
    test_up_and_right_simultaneous();

    sim_coverage_save_for(argv[0]);
    sim_print_summary("input");
    if (sim_fail_count) { printf("FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
