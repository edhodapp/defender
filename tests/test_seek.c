// Layer tests for Phase 2.5b: Lander horizontal seeking toward humanoids.
//
// Contract:
//   - Each drift frame (every 4 frames), an idle (non-carrying, non-pausing)
//     Lander scans active grounded humanoids and moves world_x by 1 toward
//     the nearest one (signed delta → shortest wrap direction).
//   - If no active grounded humanoids exist, no horizontal movement.
//   - At y < 40: descend and seek every drift.
//   - At y in 40..46: seek every drift; descend ONLY if no humanoids alive.
//     This freezes the Lander at grab altitude until seeking brings it into
//     alignment, instead of overshooting past the abduction window.
//   - At y == 47: clamped, no further descent.
//   - Carrying Landers don't seek (use the ascending path).
//   - Falling humanoids and carrying Landers are NOT seek targets.

#include <stdio.h>
#include <stdlib.h>

#include "sim_helpers.h"

#define TYPE_LANDER    1
#define TYPE_HUMANOID  7

static sim_t *S;

static void place_lander(int slot, int world_x, int y) {
    sim_mem_w(S, S->sym_entities + slot*3 + 0, (1<<7) | (TYPE_LANDER<<4));
    sim_mem_w(S, S->sym_entities + slot*3 + 1, world_x);
    sim_mem_w(S, S->sym_entities + slot*3 + 2, y);
}

static void place_lander_carrying(int slot, int world_x, int y) {
    sim_mem_w(S, S->sym_entities + slot*3 + 0,
              (1<<7) | (TYPE_LANDER<<4) | 0x01);
    sim_mem_w(S, S->sym_entities + slot*3 + 1, world_x);
    sim_mem_w(S, S->sym_entities + slot*3 + 2, y);
}

static void place_humanoid(int slot, int world_x) {
    sim_mem_w(S, S->sym_entities + slot*3 + 0, (1<<7) | (TYPE_HUMANOID<<4));
    sim_mem_w(S, S->sym_entities + slot*3 + 1, world_x);
    sim_mem_w(S, S->sym_entities + slot*3 + 2, 48);
}

static void place_falling_humanoid(int slot, int world_x, int y) {
    sim_mem_w(S, S->sym_entities + slot*3 + 0,
              (1<<7) | (TYPE_HUMANOID<<4) | 0x01);            // falling
    sim_mem_w(S, S->sym_entities + slot*3 + 1, world_x);
    sim_mem_w(S, S->sym_entities + slot*3 + 2, y);
}

static void scenario_only(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, 28, 0, 0);
}

// ---- 1. Lander at world_x=200 moves toward humanoid at 80 (wrap-aware: LEFT) ----
//      80 - 200 = -120 → MSB set on sub → move LEFT.
//      After 4 frames (one drift event), world_x: 200 → 199.
static void test_seek_moves_toward_humanoid(void) {
    scenario_only();
    place_humanoid(1, 80);
    place_lander(0, 200, 30);
    int x0 = sim_lander_world_x(S, 0);
    for (int i = 0; i < 4; i++) sim_run_frame(S);
    int x1 = sim_lander_world_x(S, 0);
    SIM_CHECK_MSG(x1 == x0 - 1,
                  "expected seek to move LEFT one col; world_x %d → %d",
                  x0, x1);
}

// ---- 2. Wrap-aware seek: lander at 10 to humanoid at 200 — LEFT is shorter ----
//      200 - 10 = 190 (0xBE) → MSB set → LEFT (10 → 9 → … → 0 → 255 → … → 200).
static void test_seek_uses_world_wrap(void) {
    scenario_only();
    place_humanoid(1, 200);
    place_lander(0, 10, 30);
    for (int i = 0; i < 4; i++) sim_run_frame(S);
    SIM_CHECK_MSG(sim_lander_world_x(S, 0) == 9,
                  "expected wrap-aware seek to LEFT; got %d",
                  sim_lander_world_x(S, 0));
}

// ---- 3. Aligned Lander stays put horizontally ----
static void test_aligned_lander_no_horizontal_motion(void) {
    scenario_only();
    place_humanoid(1, 80);
    place_lander(0, 80, 30);
    for (int i = 0; i < 12; i++) sim_run_frame(S);            // 3 drift events
    SIM_CHECK_MSG(sim_lander_world_x(S, 0) == 80,
                  "aligned lander drifted to world_x=%d",
                  sim_lander_world_x(S, 0));
}

// ---- 4. No humanoids → no horizontal movement ----
static void test_no_humanoid_no_seek(void) {
    scenario_only();
    place_lander(0, 200, 30);
    for (int i = 0; i < 12; i++) sim_run_frame(S);
    SIM_CHECK_MSG(sim_lander_world_x(S, 0) == 200,
                  "lander moved with no targets; world_x=%d",
                  sim_lander_world_x(S, 0));
}

// ---- 5. Falling humanoids are NOT seek targets ----
static void test_falling_humanoid_not_targeted(void) {
    scenario_only();
    place_falling_humanoid(1, 80, 30);
    place_lander(0, 200, 30);
    for (int i = 0; i < 12; i++) sim_run_frame(S);
    SIM_CHECK_MSG(sim_lander_world_x(S, 0) == 200,
                  "lander shouldn't seek a falling humanoid; world_x=%d",
                  sim_lander_world_x(S, 0));
}

// ---- 6. At y=40 with humanoid alive but misaligned, Lander freezes ----
static void test_freeze_at_y40_while_seeking(void) {
    scenario_only();
    place_humanoid(1, 80);
    place_lander(0, 200, 39);
    // 40 frames = 10 drift events. y reaches 40 on drift 1, then freezes
    // (humanoid alive, not yet aligned). x advances 1 col left per drift.
    for (int i = 0; i < 40; i++) sim_run_frame(S);
    SIM_CHECK_MSG(sim_lander_y(S, 0) == 40,
                  "expected lander frozen at y=40 while hunting; got y=%d",
                  sim_lander_y(S, 0));
    SIM_CHECK_MSG(sim_lander_world_x(S, 0) < 200,
                  "expected lander to have advanced toward humanoid");
}

// ---- 7. With NO humanoids alive, Lander descends past y=40 to y=47 ----
static void test_descend_past_40_when_no_humanoids(void) {
    scenario_only();
    place_lander(0, 100, 39);
    for (int i = 0; i < 40; i++) sim_run_frame(S);
    SIM_CHECK_MSG(sim_lander_y(S, 0) > 40,
                  "expected descent past y=40 with no humanoids; got y=%d",
                  sim_lander_y(S, 0));
}

// ---- 8. Carrying Lander does NOT seek horizontally ----
static void test_carrying_lander_no_seek(void) {
    scenario_only();
    place_humanoid(1, 80);                        // would be a target if seek active
    place_lander_carrying(0, 100, 30);
    for (int i = 0; i < 12; i++) sim_run_frame(S);
    SIM_CHECK_MSG(sim_lander_world_x(S, 0) == 100,
                  "carrying lander shouldn't seek; world_x=%d",
                  sim_lander_world_x(S, 0));
}

// ---- 9. End-to-end: a Lander spawned offset from a humanoid eventually grabs
//      it via seek + descend + pause. ----
static void test_seeker_eventually_grabs(void) {
    scenario_only();
    place_humanoid(1, 90);
    place_lander(0, 100, 39);                     // 10 cols right of humanoid
    // Worst-case timeline:
    //   1 drift: y → 40, x → 99 (still 9 away)
    //   2 more drifts: x → 97 → |delta|=7 → aligned → pause arms (pause=7)
    //   7 drift events to decrement pause to 0 → grab fires
    //   ≈ 3 + 7 = 10 drift events = 40 frames. Allow buffer.
    for (int i = 0; i < 80; i++) sim_run_frame(S);
    SIM_CHECK_MSG(sim_lander_carrying(S, 0),
                  "seeker should have grabbed humanoid; carry=%d, x=%d, y=%d",
                  sim_lander_carrying(S, 0),
                  sim_lander_world_x(S, 0),
                  sim_lander_y(S, 0));
    SIM_CHECK_MSG(!sim_lander_active(S, 1),
                  "humanoid should be deactivated after grab");
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <defender.elf>\n", argv[0]); return 2; }
    S = sim_boot(argv[1]);

    test_seek_moves_toward_humanoid();
    test_seek_uses_world_wrap();
    test_aligned_lander_no_horizontal_motion();
    test_no_humanoid_no_seek();
    test_falling_humanoid_not_targeted();
    test_freeze_at_y40_while_seeking();
    test_descend_past_40_when_no_humanoids();
    test_carrying_lander_no_seek();
    test_seeker_eventually_grabs();

    sim_coverage_save_for(argv[0]);
    sim_print_summary("seek");
    if (sim_fail_count) { printf("FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
