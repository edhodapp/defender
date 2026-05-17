// Pins R17.2 (boot_warp_frames suppresses slot-0 Lander AI for the
// first 30 frames after game start) and R17.3's observable state
// (soft_reset_magic is set on game-over → B-edge and cleared by
// _reset's consume step). R17.1 (cli + SP at RAMEND) is structural
// and not directly testable without simavr internals — verified by
// code inspection.
//
// Important: these tests DO NOT call sim_clear_state_minimal, which
// would clear boot_warp_frames to 0 and undo the very behavior under
// test. They drive against the post-boot state directly.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_helpers.h"

#define TYPE_LANDER  1

static sim_t *S;

static int slot0_active(void) {
    return sim_mem_r(S, S->sym_entities + 0) & 0x80;
}
static int slot0_type(void) {
    return (sim_mem_r(S, S->sym_entities + 0) >> 4) & 0x07;
}
static int slot0_y(void) {
    return sim_mem_r(S, S->sym_entities + 2) & 0x3F;
}
static int slot0_world_x(void) {
    return sim_mem_r(S, S->sym_entities + 1);
}
static int boot_warp(void) {
    return sim_mem_r(S, S->sym_boot_warp_frames);
}

// ---- R17.2: boot Lander frozen during the warp grace period ------

// At entry to this test, sim_boot has already run through
// _reset → skip_title_screen → ~2 main_alive iterations. So
// boot_warp_frames is around 28 (not 30) and the slot-0 Lander
// is at its placement position. We don't run any more frames
// before checking — boot_warp must still be > 0 and slot 0 still
// in its initial (100, 10) state.
static void test_boot_warp_active_at_game_start(void) {
    sim_sync(S);
    int warp = boot_warp();
    SIM_CHECK_MSG(warp > 0 && warp <= 30,
                  "R17.2 prereq: boot_warp_frames in (0, 30] at game start; got %d",
                  warp);
    SIM_CHECK_MSG(slot0_active(),
                  "R17.2 prereq: slot 0 should hold the boot Lander");
    SIM_CHECK_MSG(slot0_type() == TYPE_LANDER,
                  "R17.2 prereq: slot 0 type should be LANDER; got %d",
                  slot0_type());
    int y_start = slot0_y();
    int x_start = slot0_world_x();
    SIM_CHECK_MSG(y_start == 10,
                  "R17.2 prereq: boot Lander spawns at y=10; got %d", y_start);
    SIM_CHECK_MSG(x_start == 100,
                  "R17.2 prereq: boot Lander spawns at world_x=100; got %d",
                  x_start);

    // Run enough frames to cover MULTIPLE Lander drift events
    // (drift fires every 4 frames). Without the warp suppression,
    // the Lander would descend several rows.
    for (int i = 0; i < 16; i++) sim_run_frame(S);

    SIM_CHECK_MSG(slot0_y() == 10,
                  "R17.2: boot Lander must stay frozen during warp; "
                  "y advanced to %d", slot0_y());
    SIM_CHECK_MSG(slot0_world_x() == x_start,
                  "R17.2: boot Lander world_x must stay frozen; "
                  "advanced from %d to %d", x_start, slot0_world_x());
    SIM_CHECK_MSG(boot_warp() < warp,
                  "R17.2 prereq: boot_warp_frames decremented; %d -> %d",
                  warp, boot_warp());
}

// After the warp expires, the Lander resumes normal AI motion.
static void test_boot_lander_moves_after_warp(void) {
    sim_sync(S);
    // Skip past the warp: poke it to 1 so the next frame's tick
    // decrements to 0.
    sim_mem_w(S, S->sym_boot_warp_frames, 1);
    sim_run_frame(S);
    SIM_CHECK_MSG(boot_warp() == 0,
                  "R17.2 prereq: warp should be 0 after tick; got %d",
                  boot_warp());
    int y_before = slot0_y();
    // Run 16 frames — 4 drift events — and observe motion.
    for (int i = 0; i < 16; i++) sim_run_frame(S);
    SIM_CHECK_MSG(slot0_y() > y_before,
                  "R17.2 (post-warp): boot Lander descends after warp expires; "
                  "y %d -> %d", y_before, slot0_y());
}

// ---- R17.3: soft_reset_magic written then consumed ---------------

// Verify the magic byte is observable post-fatal-hit-with-B-edge
// (set to 0xA5 by main_game_over_path) AND that _reset consumes
// it (reads then clears to 0).
static void test_soft_reset_magic_lifecycle(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, 28, 0, 0);
    sim_mem_w(S, S->sym_death_flash, 0);
    sim_mem_w(S, S->sym_respawn_invuln, 0);
    sim_mem_w(S, S->sym_lives, 0);
    uint16_t sym_srm = sim_lookup(S, "soft_reset_magic");
    sim_mem_w(S, sym_srm, 0);

    // Death frame — game_state=1, prev_buttons primed to 0xFF.
    sim_place_lander(S, 0, 60, 28);
    sim_run_frame(S);

    // The B-edge writes the magic, then rjmps _reset. After enough
    // cycles, _reset's consume step zeroes it.
    sim_run_frame(S);                           // one no-input frame so
                                                  // prev_buttons drops the
                                                  // 0xFF held-button mask
    sim_btn_press(S->btn_b);
    sim_run_cycles(S, 1500000);
    sim_btn_release(S->btn_b);

    SIM_CHECK_MSG(sim_mem_r(S, sym_srm) == 0,
                  "R17.3: _reset must consume soft_reset_magic (clear to 0); "
                  "got 0x%02X", sim_mem_r(S, sym_srm));
}

// ---- driver -------------------------------------------------------

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <defender.elf>\n", argv[0]); return 2; }
    S = sim_boot(argv[1]);

    test_boot_warp_active_at_game_start();
    test_boot_lander_moves_after_warp();
    test_soft_reset_magic_lifecycle();

    sim_print_summary("boot_init");
    return sim_fail_count ? 1 : 0;
}
