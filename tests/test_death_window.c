// Pins R4.6-8: input suppression during the explosion phase and the
// edge-detected B-press required to leave GAME OVER. These were
// hardware-reported bug areas (held-B at the moment of death used to
// trip an instant soft-reset; the death animation used to ignore the
// 30-frame "no input" rule). The tests assert the contract that the
// fixes established so we don't regress.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_helpers.h"

#define TYPE_LANDER 1
#define BTN_B_BIT   5  // bit index in input_read's mask

static sim_t *S;

// ---- helpers --------------------------------------------------------

static int sprite_y(void)      { return sim_mem_r(S, S->sym_sprite_y); }
static int scroll(void)        { return sim_mem_r(S, S->sym_scroll_offset); }
static int respawn_invuln(void){ return sim_mem_r(S, S->sym_respawn_invuln); }
static int game_state(void)    { return sim_mem_r(S, S->sym_game_state); }
static int prev_buttons(void)  { return sim_mem_r(S, sim_lookup(S, "prev_buttons")); }

// ---- R4.6: input suppressed during explosion phase (61..90) -------

// Place a Lander overlapping the ship → first frame kills ship,
// respawn_invuln becomes 90 (explosion phase). During the explosion
// (respawn_invuln in [61, 90]) the player must not be able to move.
// We hold RIGHT for several frames and assert scroll_offset stays at
// 0 until the explosion ends and the blink phase starts.
static void test_input_suppressed_in_explosion(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, 28, 0, 0);
    sim_mem_w(S, S->sym_death_flash, 0);
    sim_mem_w(S, S->sym_respawn_invuln, 0);
    sim_place_lander(S, 0, 60, 28);     // overlap ship → instant hit
    sim_run_frame(S);
    SIM_CHECK_MSG(respawn_invuln() == 90,
                  "R4.6 prereq: explosion armed; got respawn_invuln=%d",
                  respawn_invuln());
    int scroll_before = scroll();
    int y_before = sprite_y();

    // Hold RIGHT for ~25 frames — well inside the 30-frame explosion
    // phase. main_alive's input mask should zero r24 each frame.
    sim_btn_press(S->btn_right);
    sim_btn_press(S->btn_up);
    for (int i = 0; i < 25; i++) sim_run_frame(S);
    sim_btn_release(S->btn_right);
    sim_btn_release(S->btn_up);

    SIM_CHECK_MSG(scroll() == scroll_before,
                  "R4.6: RIGHT during explosion must NOT scroll; "
                  "scroll %d -> %d", scroll_before, scroll());
    SIM_CHECK_MSG(sprite_y() == y_before,
                  "R4.6: UP during explosion must NOT move sprite_y; "
                  "%d -> %d", y_before, sprite_y());
    // We should still be in the invuln window (or just past it).
    SIM_CHECK_MSG(respawn_invuln() >= 60 && respawn_invuln() <= 65,
                  "R4.6 sanity: ~65 frames remain in invuln; got %d",
                  respawn_invuln());
}

// During the blink phase (respawn_invuln 1..60), input IS allowed.
// Continue the previous scenario's pattern: after the explosion ends,
// pressing RIGHT should scroll normally.
static void test_input_allowed_in_blink_phase(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, 28, 0, 0);
    sim_mem_w(S, S->sym_death_flash, 0);
    sim_mem_w(S, S->sym_respawn_invuln, 0);
    sim_place_lander(S, 0, 60, 28);
    sim_run_frame(S);
    // Skip past the explosion (~30 frames) without input.
    for (int i = 0; i < 32; i++) sim_run_frame(S);
    SIM_CHECK_MSG(respawn_invuln() > 0 && respawn_invuln() <= 60,
                  "R4.6 prereq: should be in blink phase; got %d",
                  respawn_invuln());
    int scroll_before = scroll();
    sim_btn_press(S->btn_right);
    for (int i = 0; i < 8; i++) sim_run_frame(S);
    sim_btn_release(S->btn_right);
    SIM_CHECK_MSG(scroll() > scroll_before,
                  "R4.6 (negative): RIGHT during blink phase MUST scroll; "
                  "scroll %d -> %d", scroll_before, scroll());
}

// ---- R4.7 + R4.8: GAME OVER → B-edge soft-reset (no held-B bug) ---

// Drain to game-over. Hold B continuously through the death — the
// firmware MUST NOT soft-reset on the first GAME OVER frame just
// because B is still held. The reset must wait for a fresh B edge
// (release-then-press).
static void test_game_over_held_b_does_not_reset(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, 28, 0, 0);
    sim_mem_w(S, S->sym_death_flash, 0);
    sim_mem_w(S, S->sym_respawn_invuln, 0);
    sim_mem_w(S, S->sym_lives, 0);          // last life — next hit ends game

    // Hit the ship: place an overlapping Lander, press B (player was
    // firing when they died), then run a frame.
    sim_btn_press(S->btn_b);
    sim_place_lander(S, 0, 60, 28);
    sim_run_frame(S);
    SIM_CHECK_MSG(game_state() == 1,
                  "R4.7 prereq: lives=0 hit should land in GAME OVER; got game_state=%d",
                  game_state());
    // The fatal hit's prev_buttons gets primed to 0xFF in sc_hit_fatal
    // so the very first game-over-frame's edge detection sees no edge
    // on B even though it's held.
    SIM_CHECK_MSG(prev_buttons() == 0xFF,
                  "R4.8 prereq: sc_hit_fatal should have primed prev_buttons=0xFF "
                  "for held-button suppression; got 0x%02X", prev_buttons());

    // Hold B for several more frames — must NOT soft-reset.
    for (int i = 0; i < 10; i++) sim_run_frame(S);
    SIM_CHECK_MSG(game_state() == 1,
                  "R4.8: held B during GAME OVER must NOT trip soft-reset; "
                  "game_state=%d", game_state());
    sim_btn_release(S->btn_b);
}

// After GAME OVER, a fresh B-edge SHOULD soft-reset. The reset path
// is rjmp _reset → _reset's BSS clears → eventually rcall title_loop
// (forced because soft_reset_magic == 0xA5). title_loop sits in a
// sleep wait that sim_run_to_next_iter would deadlock on, so the
// test uses sim_run_cycles to advance through the _reset init far
// enough to witness game_state being cleared and spawn_pos_idx
// being re-zeroed — both happen before the rcall title_loop.
static void test_game_over_b_edge_does_reset(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, 28, 0, 0);
    sim_mem_w(S, S->sym_death_flash, 0);
    sim_mem_w(S, S->sym_respawn_invuln, 0);
    sim_mem_w(S, S->sym_lives, 0);

    sim_place_lander(S, 0, 60, 28);
    sim_run_frame(S);
    SIM_CHECK(game_state() == 1);
    // sc_hit_fatal primes prev_buttons=0xFF to suppress held-button
    // resets. Run one frame with no input so prev_buttons drops to
    // 0x00 — only then will the next B-press register as an edge.
    sim_run_frame(S);

    // Witness markers — these should be overwritten by _reset's init
    // block once soft-reset fires.
    sim_mem_w(S, S->sym_spawn_pos_idx, 7);

    // Fresh B-edge. Use sim_run_cycles (not sim_run_frame) because the
    // rjmp _reset path lands in title_loop's sleep wait and never
    // returns to main_loop entry — sim_run_to_next_iter would timeout.
    // 1.5M cycles ≈ 2 main_loop iters worth, enough for the edge to
    // fire, rjmp _reset, and the BSS-clear block to zero game_state
    // and spawn_pos_idx well before title_loop's first sleep.
    sim_btn_press(S->btn_b);
    sim_run_cycles(S, 1500000);
    sim_btn_release(S->btn_b);

    SIM_CHECK_MSG(game_state() == 0,
                  "R4.7: B-edge in GAME OVER soft-resets; game_state still %d",
                  game_state());
    SIM_CHECK_MSG(sim_mem_r(S, S->sym_spawn_pos_idx) == 0,
                  "R4.7 (via spawn_pos_idx witness): _reset re-init fired; "
                  "spawn_pos_idx=%d (want 0)", sim_mem_r(S, S->sym_spawn_pos_idx));
}

// ---- driver --------------------------------------------------------

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <defender.elf>\n", argv[0]); return 2; }
    S = sim_boot(argv[1]);

    test_input_suppressed_in_explosion();
    test_input_allowed_in_blink_phase();
    test_game_over_held_b_does_not_reset();
    test_game_over_b_edge_does_reset();

    sim_print_summary("death_window");
    return sim_fail_count ? 1 : 0;
}
