// Functional tests for projectile spawn (B button → find free slot → install beam).
// Layer contract:
//   - B pressed AND >=1 inactive slot → new beam at (68, sprite_y+4, dx_sign=0)
//     for facing right; (59, sprite_y+4, dx_sign=1) for facing left.
//   - B pressed AND all 4 slots active → no spawn (B press ignored).
//   - B not pressed → no spawn.
//   - Spawn picks the first inactive slot scanning slots 0..3.

#include <stdio.h>
#include <stdlib.h>

#include "sim_helpers.h"

static sim_t *S;

// Park, clear all state, set ship, release B.
static void scenario(int sprite_y, int facing) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, sprite_y, 0, facing);
    sim_btn_release(S->btn_b);
    sim_btn_release(S->btn_up);
    sim_btn_release(S->btn_down);
    sim_btn_release(S->btn_left);
    sim_btn_release(S->btn_right);
}

// Suppress periodic spawn during the test.
static void quiet_run(void) {
    sim_mem_w(S, S->sym_frame_counter, 0);
    sim_run_frame(S);
}

// ---- 1. No B press → no beams. ----
static void test_no_b_no_spawn(void) {
    scenario(28, 0);
    quiet_run();
    for (int i = 0; i < 4; i++) SIM_CHECK(!sim_beam_active(S, i));
}

// ---- 2. B press with empty slots, facing right → slot 0 has beam at
//        (68, sprite_y+4) with dx_sign=0. ----
static void test_b_facing_right(void) {
    scenario(28, 0);
    sim_btn_press(S->btn_b);
    quiet_run();
    SIM_CHECK(sim_beam_active(S, 0));
    SIM_CHECK_MSG(sim_beam_x(S, 0) == 68, "x=%d", sim_beam_x(S, 0));
    SIM_CHECK_MSG(sim_beam_y(S, 0) == 32, "y=%d", sim_beam_y(S, 0));
    // dx_sign is bit 6 of byte 0.
    SIM_CHECK_MSG((sim_mem_r(S, S->sym_projectiles) & 0x40) == 0,
                  "expected dx_sign=0 (facing right); byte0=0x%02X",
                  sim_mem_r(S, S->sym_projectiles));
    // Slots 1-3 should remain empty.
    for (int i = 1; i < 4; i++) SIM_CHECK(!sim_beam_active(S, i));
}

// ---- 3. B press with empty slots, facing left → slot 0 has beam at
//        (59, sprite_y+4) with dx_sign=1. ----
static void test_b_facing_left(void) {
    scenario(28, 1);
    sim_btn_press(S->btn_b);
    quiet_run();
    SIM_CHECK(sim_beam_active(S, 0));
    SIM_CHECK_MSG(sim_beam_x(S, 0) == 59, "x=%d", sim_beam_x(S, 0));
    SIM_CHECK_MSG(sim_beam_y(S, 0) == 32, "y=%d", sim_beam_y(S, 0));
    SIM_CHECK_MSG((sim_mem_r(S, S->sym_projectiles) & 0x40) != 0,
                  "expected dx_sign=1; byte0=0x%02X",
                  sim_mem_r(S, S->sym_projectiles));
}

// ---- 4. Auto-fire cooldown: B held for cooldown_period-1 frames after the
//        first shot fires only ONE beam. The cooldown gates further spawns. ----
//        FIRE_COOLDOWN_RELOAD = 5; fire period = 6 frames @ 60 Hz = 10 shots/sec.
static int count_active_beams(void) {
    int n = 0;
    for (int i = 0; i < 4; i++) if (sim_beam_active(S, i)) n++;
    return n;
}

static void test_cooldown_blocks_held_b(void) {
    scenario(28, 0);
    sim_btn_press(S->btn_b);
    // F1 fires (cooldown was 0). F2..F5 are blocked (cooldown 5..1).
    for (int i = 0; i < 5; i++) quiet_run();
    SIM_CHECK_MSG(count_active_beams() == 1,
                  "expected 1 beam after 5 frames of held B; got %d",
                  count_active_beams());
}

// ---- 5. After the cooldown elapses (frame 6), holding B fires the next shot. ----
static void test_cooldown_elapses_then_fires(void) {
    scenario(28, 0);
    sim_btn_press(S->btn_b);
    // F1 fires. F2..F6: cooldown 5,4,3,2,1,0. F7 fires. F8..F12 cooldown.
    for (int i = 0; i < 7; i++) quiet_run();
    SIM_CHECK_MSG(count_active_beams() == 2,
                  "expected 2 beams after 7 frames of held B; got %d",
                  count_active_beams());
}

// ---- 6. Releasing B clears the cooldown — the next press fires immediately. ----
static void test_release_resets_cooldown(void) {
    scenario(28, 0);
    sim_btn_press(S->btn_b);
    quiet_run();                       // F1 fires (1 beam, cooldown=5)
    sim_btn_release(S->btn_b);
    quiet_run();                       // F2 release path clears cooldown to 0
    SIM_CHECK_MSG(count_active_beams() == 1, "after release expected 1 beam");
    sim_btn_press(S->btn_b);
    quiet_run();                       // F3 fires (cooldown was 0)
    SIM_CHECK_MSG(count_active_beams() == 2,
                  "expected 2 beams after re-press; got %d (cooldown should reset on release)",
                  count_active_beams());
}

// ---- 6. After freeing slot 0 (e.g., it deactivates by going off-screen),
//        the next B press fills slot 0 again. ----
static void test_freed_slot_refills(void) {
    scenario(28, 0);
    // Place a beam at slot 0 already at proj_x=124. Next quiet_run() moves
    // it to 128 → off-screen → deactivates.
    sim_place_beam(S, 0, 124, 32, 0);
    sim_btn_release(S->btn_b);
    quiet_run();
    SIM_CHECK(!sim_beam_active(S, 0));            // slot 0 freed
    sim_btn_press(S->btn_b);
    quiet_run();
    SIM_CHECK(sim_beam_active(S, 0));             // refilled
    SIM_CHECK_MSG(sim_beam_x(S, 0) == 68, "x=%d", sim_beam_x(S, 0));
}

// ---- 7. B press picks the FIRST inactive slot scanning slots 0..3.
//        Place a beam at slot 1 only; press B; expect new beam in slot 0. ----
static void test_b_picks_first_inactive(void) {
    scenario(28, 0);
    sim_place_beam(S, 1, 80, 16, 0);              // pre-fill slot 1
    sim_btn_press(S->btn_b);
    quiet_run();
    SIM_CHECK(sim_beam_active(S, 0));             // slot 0 got new beam
    // slot 1 still has its pre-existing beam (now advanced by 4)
    SIM_CHECK_MSG(sim_beam_active(S, 1) && sim_beam_x(S, 1) == 84,
                  "slot 1 active=%d x=%d", sim_beam_active(S, 1), sim_beam_x(S, 1));
}

// ---- 8. beam_y reflects sprite_y at top of allowed range (sprite_y=8). ----
static void test_beam_y_at_top(void) {
    scenario(8, 0);
    sim_btn_press(S->btn_b);
    quiet_run();
    SIM_CHECK_MSG(sim_beam_y(S, 0) == 12, "expected 12, got %d", sim_beam_y(S, 0));
}

// ---- 9. beam_y reflects sprite_y at bottom of allowed range (sprite_y=47). ----
static void test_beam_y_at_bottom(void) {
    scenario(47, 0);
    sim_btn_press(S->btn_b);
    quiet_run();
    SIM_CHECK_MSG(sim_beam_y(S, 0) == 51, "expected 51, got %d", sim_beam_y(S, 0));
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <defender.elf>\n", argv[0]); return 2; }
    S = sim_boot(argv[1]);

    test_no_b_no_spawn();
    test_b_facing_right();
    test_b_facing_left();
    test_cooldown_blocks_held_b();
    test_cooldown_elapses_then_fires();
    test_release_resets_cooldown();
    test_freed_slot_refills();
    test_b_picks_first_inactive();
    test_beam_y_at_top();
    test_beam_y_at_bottom();

    sim_coverage_save_for(argv[0]);
    sim_print_summary("proj_spawn");
    if (sim_fail_count) { printf("FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
