// Targeted repro tests for the bugs Ed reported on hardware:
//   #1. "The beam kills the closest lander and progresses to the next one
//        and kills it too." — single beam should deactivate exactly one
//        lander, then itself.
//   #2. "Sending landers on top of other landers." — already repro'd in
//        test_lander_spawn.c::test_no_world_x_overlap_on_spawn.
//   #3. "Collision detect is very flaky — sometimes works, sometimes
//        doesn't." — exhaustive boundary tests around the bbox edges to
//        catch any off-by-one in collision_check's X/Y predicates.
//
// These tests are written to PASS when the bug is absent. They will fail
// against the current firmware if the bug is real.

#include <stdio.h>
#include <stdlib.h>

#include "sim_helpers.h"

static sim_t *S;

static void scenario(int sprite_y, int scroll, int facing) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, sprite_y, scroll, facing);
    sim_btn_release(S->btn_b);
    sim_btn_release(S->btn_up);
    sim_btn_release(S->btn_down);
    sim_btn_release(S->btn_left);
    sim_btn_release(S->btn_right);
}

static void quiet_run_n(int n) {
    for (int i = 0; i < n; i++) {
        sim_mem_w(S, S->sym_frame_counter, 0);
        sim_run_frame(S);
    }
}

static int count_active_landers(int max_slot) {
    int n = 0;
    for (int i = 0; i < max_slot; i++) if (sim_lander_active(S, i)) n++;
    return n;
}

// ============================================================================
// Bug #1: single beam → at most one lander kill.
//
// Two Landers in the beam's path at the same y. One beam fires. After enough
// frames for the beam to traverse the entire screen, exactly ONE Lander
// should be dead.
// ============================================================================
static void test_single_beam_one_kill_only(void) {
    scenario(28, 0, 0);
    sim_place_lander(S, 0, /*world_x*/80,  /*y*/28);   // closer
    sim_place_lander(S, 1, /*world_x*/100, /*y*/28);   // further
    // Beam at proj_x=68, beam_y=32 (sprite_y=28+4), facing right.
    sim_place_beam(S, 0, 68, 32, 0);

    // Beam reaches col ~124 in (124-68)/4 = 14 frames; deactivates at 15.
    quiet_run_n(20);

    int alive = count_active_landers(2);
    SIM_CHECK_MSG(alive == 1,
                  "BUG: expected exactly 1 lander alive after one beam; got %d",
                  alive);
    // The further lander (slot 1, world_x=100) should be alive — beam
    // hits the closer one (slot 0) first.
    SIM_CHECK_MSG(sim_lander_active(S, 1) && !sim_lander_active(S, 0),
                  "BUG: wrong lander died — slot 0 active=%d, slot 1 active=%d",
                  sim_lander_active(S, 0), sim_lander_active(S, 1));
}

// Same setup but THREE landers in a line. A single beam should still
// kill exactly one.
static void test_single_beam_through_three_landers(void) {
    scenario(28, 0, 0);
    sim_place_lander(S, 0, 80,  28);
    sim_place_lander(S, 1, 100, 28);
    sim_place_lander(S, 2, 120, 28);
    sim_place_beam(S, 0, 68, 32, 0);
    quiet_run_n(20);
    int alive = count_active_landers(3);
    SIM_CHECK_MSG(alive == 2,
                  "BUG: single beam through 3 landers; alive=%d (want 2)",
                  alive);
}

// Multi-beam sanity: 2 beams aligned with 2 landers should kill 2 (one each).
// This validates that what looks like "1 beam → 2 kills" might actually be
// 2 beams from rapid B fire.
static void test_two_beams_kill_two_landers(void) {
    scenario(28, 0, 0);
    sim_place_lander(S, 0, 80,  28);
    sim_place_lander(S, 1, 100, 28);
    sim_place_beam(S, 0, 68, 32, 0);
    sim_place_beam(S, 1, 76, 32, 0);                   // second beam, 8 cols ahead
    quiet_run_n(20);
    int alive = count_active_landers(2);
    SIM_CHECK_MSG(alive == 0,
                  "expected both landers killed by 2 beams; alive=%d", alive);
}

// Fixed behavior: a human B-tap (~5 frames at 60 Hz) fires exactly ONE beam
// thanks to the auto-fire cooldown — so at most one lander dies per tap.
// Pre-fix this test was a *characterization* of the bug (expected >=2 kills);
// post-fix it pins the cooldown behavior.
static void test_human_btap_kills_at_most_one(void) {
    scenario(28, 0, 0);
    sim_place_lander(S, 0, 80,  28);
    sim_place_lander(S, 1, 100, 28);
    sim_place_lander(S, 2, 120, 28);
    sim_btn_press(S->btn_b);
    for (int i = 0; i < 5; i++) quiet_run_n(1);
    sim_btn_release(S->btn_b);
    quiet_run_n(20);
    int alive = count_active_landers(3);
    int kills = 3 - alive;
    SIM_CHECK_MSG(kills <= 1,
                  "BUG: a 5-frame B-tap killed %d landers (cooldown should cap at 1)",
                  kills);
}

// ============================================================================
// Bug #3: flaky collision — exhaustive boundary tests.
// X-overlap is collision_check's first conditional path; verify it fires
// AT bounds and does not fire JUST OUTSIDE bounds.
// ============================================================================
static void check_x_overlap_case(int beam_x_init, int lander_screen_x,
                                 int expect_hit, const char *tag) {
    scenario(28, 0, 0);
    sim_place_lander(S, 0, lander_screen_x, 28);       // scroll=0 → world_x = screen_x
    sim_place_beam(S, 0, beam_x_init, 32, 0);
    // Run enough frames for beam to either hit or escape.
    quiet_run_n(20);
    int hit = !sim_lander_active(S, 0);
    SIM_CHECK_MSG(hit == expect_hit,
                  "%s: beam_x=%d lander_x=%d expected %s, got %s",
                  tag, beam_x_init, lander_screen_x,
                  expect_hit ? "HIT" : "MISS", hit ? "HIT" : "MISS");
}

// Beam traveling right (proj_x: 68, 72, 76, ...) lands on multiples of 4
// from 68. So beam_x values reachable are {68, 72, 76, ..., 124}.
//
// For lander at screen_x = X, bbox is [X, X+7]. Beam hits when one of the
// reachable beam_x values lies in that range. With step=4 and width=8,
// every contiguous lander_x admits at least one hit, so all of these
// should HIT:
static void test_x_boundary_hits(void) {
    // Lander right at start: bbox [68, 75]; beam at 68 immediately overlaps.
    check_x_overlap_case(68, 68, /*hit*/1, "lander at beam start");
    // Bbox [69, 76]: beam hits at proj_x=72.
    check_x_overlap_case(68, 69, 1, "lander_x=69");
    // Bbox [75, 82]: beam at 76 overlaps.
    check_x_overlap_case(68, 75, 1, "lander_x=75");
    // Bbox [76, 83]: beam at 76, 80 overlap.
    check_x_overlap_case(68, 76, 1, "lander_x=76");
    // Bbox [120, 127]: beam reaches 120, 124 — both overlap.
    check_x_overlap_case(68, 120, 1, "lander_x=120");
    // Bbox [124, 131]: beam reaches 124, in range.
    check_x_overlap_case(68, 124, 1, "lander_x=124");
}

static void test_x_boundary_misses(void) {
    // Bbox [125, 132]: beam reaches 124 (last frame) which is NOT in range
    // [125, 132] — beam went off-screen first. Miss.
    check_x_overlap_case(68, 125, /*hit*/0, "lander_x=125 just past beam reach");
    // Bbox [60, 67]: lander LEFT of beam start. Beam moves +4 (away). Miss.
    check_x_overlap_case(68, 60, 0, "lander_x=60 left of beam, beam moves right");
}

// ============================================================================
// Y-overlap boundary tests with beam_y=32 (sprite_y=28).
// Hit window: lander_y in [25, 32] (so bbox y = [25..32, 32..39]).
// ============================================================================
static void check_y_overlap_case(int lander_y, int expect_hit, const char *tag) {
    scenario(28, 0, 0);
    // Lander screen_x=80, beam reaches 80 in 3 frames (proj_x: 72, 76, 80).
    sim_place_lander(S, 0, 80, lander_y);
    sim_place_beam(S, 0, 68, 32, 0);
    quiet_run_n(20);
    int hit = !sim_lander_active(S, 0);
    SIM_CHECK_MSG(hit == expect_hit,
                  "%s: lander_y=%d expected %s, got %s",
                  tag, lander_y,
                  expect_hit ? "HIT" : "MISS", hit ? "HIT" : "MISS");
}

static void test_y_boundary(void) {
    check_y_overlap_case(25, 1, "lander_y=25 (top of overlap)");
    check_y_overlap_case(28, 1, "lander_y=28 (centered)");
    check_y_overlap_case(32, 1, "lander_y=32 (bottom of overlap, beam_y=32)");
    check_y_overlap_case(24, 0, "lander_y=24 (just above hit range)");
    check_y_overlap_case(33, 0, "lander_y=33 (just below hit range)");
}

// At sprite_y=47 (bottom of sprite range), beam_y = 51. Overlap range:
// lander_y in [44, 51]. Lander max y after drift is 47, so the achievable
// hit range with bottom-aligned ship is lander_y in [44, 47].
static void test_bottom_altitude_collision(void) {
    scenario(47, 0, 0);
    sim_place_lander(S, 0, 80, 47);                    // bottom-aligned lander
    sim_place_beam(S, 0, 68, 51, 0);                   // beam from cockpit
    quiet_run_n(20);
    SIM_CHECK_MSG(!sim_lander_active(S, 0),
                  "BUG: bottom-aligned beam-vs-lander failed to collide");
}

// ============================================================================
// Combined / stress: rapid fire + multiple landers at varied y.
// Each beam should kill at most one lander.
// ============================================================================
static void test_rapid_fire_doesnt_double_kill(void) {
    scenario(28, 0, 0);
    sim_place_lander(S, 0, 80,  28);
    sim_place_lander(S, 1, 100, 28);
    // 2 beams (slot 0 about to hit lander 0, slot 1 4 cols behind).
    sim_place_beam(S, 0, 76, 32, 0);                    // proj_x=76 → next frame 80 → hits lander 0
    sim_place_beam(S, 1, 72, 32, 0);                    // proj_x=72 → next frame 76; should NOT also hit lander 0
    quiet_run_n(2);
    // After 2 frames: beam 0 should have hit lander 0 (and self-deactivated);
    // lander 1 still active; beam 1 still in flight, headed for lander 1.
    SIM_CHECK_MSG(!sim_lander_active(S, 0),
                  "expected lander 0 dead after frame 1");
    SIM_CHECK_MSG(sim_lander_active(S, 1),
                  "lander 1 should still be alive (beam 1 hasn't reached it)");
    SIM_CHECK_MSG(!sim_beam_active(S, 0),
                  "beam 0 should be dead after killing lander 0");
    SIM_CHECK_MSG(sim_beam_active(S, 1),
                  "beam 1 should still be in flight");
}

// Regression: main_loop's spawn gate must reload frame_counter from memory
// because audio_tick clobbers r25. The spawn trigger has since changed
// from "(frame_counter & 0x1F) == 0" to "frame_counter == 128", but the
// register-lifetime hazard is the same — if r25 isn't reloaded, the
// comparison sees the SFX state machine's sound_frame instead and the
// spawn is suppressed.
static void test_lander_spawn_not_blocked_by_active_sfx(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, 28, 0, 0);
    // Force the spawn to fire next iteration (countdown=0 → fire path).
    sim_mem_w(S, S->sym_spawn_countdown, 0);
    // Pretend SFX_FIRE is active; audio_tick will advance it and clobber r25.
    sim_mem_w(S, S->sym_sound_id, 1);
    sim_mem_w(S, S->sym_sound_frame, 0);
    sim_run_frame(S);
    SIM_CHECK_MSG(sim_lander_active(S, 0),
                  "BUG: Lander spawn was suppressed because audio_tick "
                  "clobbered r25 between increment and spawn-gate check");
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <defender.elf>\n", argv[0]); return 2; }
    S = sim_boot(argv[1]);

    // Bug #1 — multi-kill beam
    test_single_beam_one_kill_only();
    test_single_beam_through_three_landers();
    test_two_beams_kill_two_landers();
    test_human_btap_kills_at_most_one();
    test_lander_spawn_not_blocked_by_active_sfx();

    // Bug #3 — flaky collision: boundary sweeps
    test_x_boundary_hits();
    test_x_boundary_misses();
    test_y_boundary();
    test_bottom_altitude_collision();
    test_rapid_fire_doesnt_double_kill();

    sim_coverage_save_for(argv[0]);
    sim_print_summary("bug_repros");
    if (sim_fail_count) { printf("FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
