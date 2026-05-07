// Layer tests for Phase 2: Lander abduction of Humanoids.
//
// Sequence (Defender-faithful):
//   1. Lander descends (1 row per 4 frames).
//   2. On reaching y == 40 with an x-aligned active Humanoid (|dx| < 8),
//      Lander arms a pause counter (byte 0 bits 1-3 = 7).
//   3. Pause counter decrements every 4 frames; descent is frozen.
//   4. On the 7→0 transition (28 frames after arming), the actual grab
//      fires: Humanoid deactivated, Lander gets carry bit, SFX_GRAB queued.
//   5. Carrying Lander ascends 1 row per 4 frames; pinned at y == 0.

#include <stdio.h>
#include <stdlib.h>

#include "sim_helpers.h"

#define TYPE_LANDER    1
#define TYPE_HUMANOID  7
#define SFX_GRAB       4

static sim_t *S;

static void place_lander(int slot, int world_x, int y) {
    sim_mem_w(S, S->sym_entities + slot*3 + 0, (1<<7) | (TYPE_LANDER<<4));
    sim_mem_w(S, S->sym_entities + slot*3 + 1, world_x);
    sim_mem_w(S, S->sym_entities + slot*3 + 2, y);
}

static void place_humanoid(int slot, int world_x) {
    sim_mem_w(S, S->sym_entities + slot*3 + 0, (1<<7) | (TYPE_HUMANOID<<4));
    sim_mem_w(S, S->sym_entities + slot*3 + 1, world_x);
    sim_mem_w(S, S->sym_entities + slot*3 + 2, 48);
}

static int pause_counter(int slot) {
    return (sim_mem_r(S, S->sym_entities + slot*3) >> 1) & 0x07;
}

static void scenario_only(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, 28, 0, 0);
}

// ---- 1. Descent halts at y=40 over an aligned Humanoid; pause counter armed ----
static void test_descend_halts_on_humanoid_head(void) {
    scenario_only();
    place_humanoid(1, 80);
    place_lander(0, 80, 39);                     // one row above head
    // After 4 frames, drift fires: y advances to 40, alignment check arms pause.
    for (int i = 0; i < 4; i++) sim_run_frame(S);
    SIM_CHECK_MSG(sim_lander_y(S, 0) == 40,
                  "expected y=40 after one drift; got %d", sim_lander_y(S, 0));
    SIM_CHECK_MSG(pause_counter(0) == 7,
                  "expected pause armed at 7; got %d", pause_counter(0));
    SIM_CHECK_MSG(sim_lander_active(S, 1),
                  "humanoid still active during pause; got inactive");
    SIM_CHECK_MSG(!sim_lander_carrying(S, 0),
                  "lander shouldn't be carrying yet (still pausing)");
}

// ---- 2. Pause counter decrements every 4 frames ----
static void test_pause_counter_ticks_down(void) {
    scenario_only();
    place_humanoid(1, 80);
    place_lander(0, 80, 39);
    for (int i = 0; i < 4; i++) sim_run_frame(S);
    SIM_CHECK(pause_counter(0) == 7);
    // 4 more frames → next drift event → pause 7→6.
    for (int i = 0; i < 4; i++) sim_run_frame(S);
    SIM_CHECK_MSG(pause_counter(0) == 6,
                  "expected pause=6 after one drift tick; got %d", pause_counter(0));
    // 8 more → pause 6→5→4.
    for (int i = 0; i < 8; i++) sim_run_frame(S);
    SIM_CHECK_MSG(pause_counter(0) == 4,
                  "expected pause=4 after two more drifts; got %d", pause_counter(0));
}

// ---- 3. After full pause (7×4=28 frames), grab fires ----
static void test_grab_after_full_pause(void) {
    scenario_only();
    place_humanoid(1, 80);
    place_lander(0, 80, 39);
    // 4 frames to reach y=40 + arm pause + 7 drift-events to expire (28 frames)
    // = 32 frames. Add one more for the grab-tick to commit. Run 36 to be safe.
    for (int i = 0; i < 36; i++) sim_run_frame(S);
    SIM_CHECK_MSG(!sim_lander_active(S, 1),
                  "humanoid should be deactivated after grab");
    SIM_CHECK_MSG(sim_lander_carrying(S, 0),
                  "lander should be carrying after grab");
    SIM_CHECK_MSG(pause_counter(0) == 0,
                  "pause counter should be cleared after grab");
}

// ---- 4. With seek added (Phase 2.5b): a misaligned Lander hunts the
//        humanoid horizontally and freezes at y=40 until aligned, instead
//        of descending past it and parking at y=47. ----
static void test_misaligned_lander_hunts(void) {
    scenario_only();
    place_humanoid(1, 80);
    place_lander(0, 200, 30);
    // 100 frames = 25 drift events. Initial gap is 120 cols (going LEFT) —
    // not enough drift events to fully align (only 25 cols of x progress).
    // After 100 frames the lander should still be in flight, parked at y=40.
    for (int i = 0; i < 100; i++) sim_run_frame(S);
    SIM_CHECK_MSG(sim_lander_active(S, 1),
                  "humanoid should still be alive while lander is en route");
    SIM_CHECK_MSG(!sim_lander_carrying(S, 0),
                  "lander shouldn't be carrying yet; byte0=0x%02X",
                  sim_mem_r(S, S->sym_entities + 0));
    SIM_CHECK_MSG(sim_lander_y(S, 0) == 40,
                  "lander should freeze at grab altitude while hunting; got y=%d",
                  sim_lander_y(S, 0));
    SIM_CHECK_MSG(sim_lander_world_x(S, 0) < 200,
                  "lander should have advanced LEFT toward humanoid");
}

// ---- 5. Carrying Lander ascends ----
static void test_carrying_lander_ascends(void) {
    scenario_only();
    place_humanoid(1, 80);
    place_lander(0, 80, 39);
    for (int i = 0; i < 36; i++) sim_run_frame(S);   // through grab
    SIM_CHECK(sim_lander_carrying(S, 0));
    int y0 = sim_lander_y(S, 0);
    // Ascend 1 row per 4 frames; verify after 8 frames y has dropped by ≈ 2.
    for (int i = 0; i < 8; i++) sim_run_frame(S);
    int y1 = sim_lander_y(S, 0);
    SIM_CHECK_MSG(y1 == y0 - 2,
                  "carrying lander didn't ascend: y went %d → %d (want -2)",
                  y0, y1);
}

// ---- 6. Carrying Lander pins at y=8 (just below the radar bar) ----
static void test_carrying_lander_pins_below_radar(void) {
    scenario_only();
    place_humanoid(1, 80);
    place_lander(0, 80, 39);
    // ~36 frames to grab + ascend from y=40 to y=8 (32 rows × 4 = 128 frames)
    // + buffer.
    for (int i = 0; i < 240; i++) sim_run_frame(S);
    SIM_CHECK_MSG(sim_lander_y(S, 0) == 8,
                  "expected lander pinned at y=8 (just below radar); got y=%d",
                  sim_lander_y(S, 0));
    SIM_CHECK(sim_lander_carrying(S, 0));
}

// ---- 7. SFX_GRAB queued at the actual grab moment (post-pause) ----
static void test_sfx_grab_queued_at_grab_moment(void) {
    scenario_only();
    place_humanoid(1, 80);
    place_lander(0, 80, 39);
    // Pre-pause: nothing should have queued.
    sim_run_frame(S);
    SIM_CHECK(sim_mem_r(S, S->sym_sound_id) == 0);
    // Run to grab and one frame after (drain plays it on the grab frame).
    for (int i = 0; i < 36; i++) sim_run_frame(S);
    SIM_CHECK_MSG(sim_mem_r(S, S->sym_sound_id) == SFX_GRAB,
                  "expected SFX_GRAB after grab; sound_id=%d",
                  sim_mem_r(S, S->sym_sound_id));
}

// ---- 8. If humanoid is killed during pause, lander silently resumes idle ----
static void test_humanoid_killed_during_pause_aborts_grab(void) {
    scenario_only();
    place_humanoid(1, 80);
    place_lander(0, 80, 39);
    // Get into pause (4 frames descent).
    for (int i = 0; i < 4; i++) sim_run_frame(S);
    SIM_CHECK(pause_counter(0) == 7);
    // Kill the humanoid mid-pause.
    sim_mem_w(S, S->sym_entities + 1*3, 0);
    // Run through pause expiration.
    for (int i = 0; i < 32; i++) sim_run_frame(S);
    SIM_CHECK_MSG(!sim_lander_carrying(S, 0),
                  "lander shouldn't carry — humanoid was already dead at grab time");
    SIM_CHECK_MSG(pause_counter(0) == 0,
                  "pause counter should be cleared regardless");
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <defender.elf>\n", argv[0]); return 2; }
    S = sim_boot(argv[1]);

    test_descend_halts_on_humanoid_head();
    test_pause_counter_ticks_down();
    test_grab_after_full_pause();
    test_misaligned_lander_hunts();
    test_carrying_lander_ascends();
    test_carrying_lander_pins_below_radar();
    test_sfx_grab_queued_at_grab_moment();
    test_humanoid_killed_during_pause_aborts_grab();

    sim_coverage_save_for(argv[0]);
    sim_print_summary("abduction");
    if (sim_fail_count) { printf("FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
