// Functional tests for ship_collision_check + death-flash decay.
// Layer contract:
//   - For each active Lander overlapping the ship's 8x8 box (cols 60..67,
//     rows sprite_y..sprite_y+7), reset sprite_y to 28 and set
//     death_flash to 8. Lander survives.
//   - X-overlap: lander_screen_x in [53, 67] (8x8 + 8x8).
//   - Y-overlap: |sprite_y - lander_y| <= 7.
//   - death_flash decrements by 1 per frame (in invert_if_dead) until 0.

#include <stdio.h>
#include <stdlib.h>

#include "sim_helpers.h"

static sim_t *S;

static void scenario(int sprite_y, int scroll) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, sprite_y, scroll, 0);
}

static void quiet_run(void) {
    sim_mem_w(S, S->sym_frame_counter, 0);
    sim_run_frame(S);
}

static int death_flash(void) {
    return sim_mem_r(S, S->sym_death_flash);
}

// ---- 1. No lander → no collision, death_flash stays 0. ----
static void test_no_lander_no_collision(void) {
    scenario(28, 0);
    quiet_run();
    SIM_CHECK(death_flash() == 0);
    SIM_CHECK(sim_mem_r(S, S->sym_sprite_y) == 28);
}

// ---- 2. Lander far away → no collision. ----
static void test_far_lander_no_collision(void) {
    scenario(28, 0);
    sim_place_lander(S, 0, 200, 10);
    quiet_run();
    SIM_CHECK(death_flash() == 0);
    SIM_CHECK(sim_lander_active(S, 0));         // lander untouched
}

// ---- 3. Direct overlap → collision: sprite_y reset, death_flash = 7
//        (set to 8 in ship_collision_check, decremented in invert_if_dead). ----
static void test_direct_overlap_triggers(void) {
    scenario(28, 0);
    sim_place_lander(S, 0, 60, 28);             // screen_x=60, y=28
    quiet_run();
    SIM_CHECK_MSG(death_flash() == 7,
                  "death_flash=%d after collision", death_flash());
    SIM_CHECK_MSG(sim_mem_r(S, S->sym_sprite_y) == 28,
                  "sprite_y=%d", sim_mem_r(S, S->sym_sprite_y));
    SIM_CHECK(sim_lander_active(S, 0));         // lander survives
}

// ---- 4. Reset effect: ship moved to y=15, lander at y=15 → collision
//        resets sprite_y to 28. ----
static void test_collision_resets_sprite_y(void) {
    scenario(15, 0);
    sim_place_lander(S, 0, 60, 15);
    quiet_run();
    SIM_CHECK_MSG(sim_mem_r(S, S->sym_sprite_y) == 28,
                  "expected 28; got %d", sim_mem_r(S, S->sym_sprite_y));
}

// ---- 5. X-overlap edges: lander_screen_x = 53 (just touching ship's left). ----
static void test_x_overlap_left_edge(void) {
    scenario(28, 0);
    sim_place_lander(S, 0, 53, 28);
    quiet_run();
    SIM_CHECK_MSG(death_flash() == 7, "no collision at screen_x=53");
}

// ---- 6. Just outside X (left): screen_x=52 → no collision. ----
static void test_just_outside_x_left(void) {
    scenario(28, 0);
    sim_place_lander(S, 0, 52, 28);
    quiet_run();
    SIM_CHECK_MSG(death_flash() == 0, "false collision at screen_x=52");
}

// ---- 7. X-overlap right edge: lander_screen_x = 67. ----
static void test_x_overlap_right_edge(void) {
    scenario(28, 0);
    sim_place_lander(S, 0, 67, 28);
    quiet_run();
    SIM_CHECK_MSG(death_flash() == 7, "no collision at screen_x=67");
}

// ---- 8. Just outside X (right): screen_x=68 → no collision. ----
static void test_just_outside_x_right(void) {
    scenario(28, 0);
    sim_place_lander(S, 0, 68, 28);
    quiet_run();
    SIM_CHECK_MSG(death_flash() == 0, "false collision at screen_x=68");
}

// ---- 9. Y-overlap edge: lander_y differs by 7. ----
static void test_y_overlap_edge_low(void) {
    scenario(28, 0);
    sim_place_lander(S, 0, 60, 21);             // delta = -7
    quiet_run();
    SIM_CHECK_MSG(death_flash() == 7, "no collision at lander_y=21");
}

static void test_y_overlap_edge_high(void) {
    scenario(28, 0);
    sim_place_lander(S, 0, 60, 35);             // delta = +7
    quiet_run();
    SIM_CHECK_MSG(death_flash() == 7, "no collision at lander_y=35");
}

// ---- 10. Just outside Y: |delta| == 8 → no collision. ----
static void test_just_outside_y(void) {
    scenario(28, 0);
    sim_place_lander(S, 0, 60, 20);
    quiet_run();
    SIM_CHECK_MSG(death_flash() == 0, "false collision at lander_y=20");
}

// ---- 11. Death-flash decays: after collision, deactivate lander manually,
//        then verify the flash counts down to 0 over 7 more frames. ----
static void test_death_flash_decays(void) {
    scenario(28, 0);
    sim_place_lander(S, 0, 60, 28);
    quiet_run();                                // collision frame
    SIM_CHECK_MSG(death_flash() == 7, "post-collision flash=%d", death_flash());
    // Remove lander so collision doesn't keep re-arming.
    sim_mem_w(S, S->sym_entities, 0);
    int expected = 7;
    for (int f = 0; f < 7; f++) {
        quiet_run();
        expected--;
        SIM_CHECK_MSG(death_flash() == expected,
                      "frame %d: expected %d, got %d",
                      f+1, expected, death_flash());
    }
    // After this, flash should be 0 and stay 0.
    quiet_run();
    SIM_CHECK_MSG(death_flash() == 0, "expected 0, got %d", death_flash());
}

// ---- 12. Multiple landers: only the first overlapping one needs to fire;
//        ship_collision_check returns early. State is identical either way. ----
static void test_multiple_landers_with_one_overlapping(void) {
    scenario(28, 0);
    sim_place_lander(S, 0, 200, 10);            // far, no overlap
    sim_place_lander(S, 1, 60, 28);             // overlap
    sim_place_lander(S, 2, 200, 30);            // far
    quiet_run();
    SIM_CHECK_MSG(death_flash() == 7, "expected collision; flash=%d", death_flash());
    // All landers should still be active.
    SIM_CHECK(sim_lander_active(S, 0));
    SIM_CHECK(sim_lander_active(S, 1));
    SIM_CHECK(sim_lander_active(S, 2));
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <defender.elf>\n", argv[0]); return 2; }
    S = sim_boot(argv[1]);

    test_no_lander_no_collision();
    test_far_lander_no_collision();
    test_direct_overlap_triggers();
    test_collision_resets_sprite_y();
    test_x_overlap_left_edge();
    test_just_outside_x_left();
    test_x_overlap_right_edge();
    test_just_outside_x_right();
    test_y_overlap_edge_low();
    test_y_overlap_edge_high();
    test_just_outside_y();
    test_death_flash_decays();
    test_multiple_landers_with_one_overlapping();

    sim_coverage_save_for(argv[0]);
    sim_print_summary("ship_collision");
    if (sim_fail_count) { printf("FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
