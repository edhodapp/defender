// Lives lifecycle: ship deaths → game-over, bonus ships per difficulty,
// and the HUD ship-icon count. Sits between test_ship_collision (which
// covers the collision detector itself) and test_score_render (which
// pins HUD score rendering); this file pins lives state transitions
// and the per-difficulty bonus-threshold table.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sim_helpers.h"

static sim_t  *S;
static uint16_t sym_score_lo, sym_score_mid, sym_score_hi;
static uint16_t sym_next_bonus_lo, sym_next_bonus_mid, sym_next_bonus_hi;

// Must match the ship_right sprite in defender.S (HUD uses ship_right
// for the lives icons regardless of player's facing).
static const uint8_t ship_right[8] = {
    0x18, 0x3C, 0x3C, 0x38, 0x30, 0x30, 0x10, 0x10
};

static int lives(void)      { return sim_mem_r(S, S->sym_lives); }
static int game_state(void) { return sim_mem_r(S, S->sym_game_state); }

static uint32_t read24(uint16_t lo, uint16_t mid, uint16_t hi) {
    return (uint32_t)sim_mem_r(S, lo)
         | ((uint32_t)sim_mem_r(S, mid) << 8)
         | ((uint32_t)sim_mem_r(S, hi)  << 16);
}

static void write24(uint16_t lo, uint16_t mid, uint16_t hi, uint32_t v) {
    sim_mem_w(S, lo,  v & 0xFF);
    sim_mem_w(S, mid, (v >> 8) & 0xFF);
    sim_mem_w(S, hi,  (v >> 16) & 0xFF);
}

static void scenario(int sprite_y, int scroll) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, sprite_y, scroll, 0);
    write24(sym_score_lo, sym_score_mid, sym_score_hi, 0);
}

static void quiet_run(void) {
    sim_mem_w(S, S->sym_frame_counter, 0);
    sim_run_frame(S);
}

// ---- Lives lifecycle (deaths and game-over transition) ----------------

static void test_initial_lives(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    SIM_CHECK_MSG(lives() == 3, "initial lives = %d (want 3)", lives());
    SIM_CHECK_MSG(game_state() == 0, "initial game_state = %d (want 0)", game_state());
}

static void test_collision_decrements_lives(void) {
    scenario(28, 0);
    sim_place_lander(S, 0, 60, 28);
    quiet_run();
    SIM_CHECK_MSG(lives() == 2, "lives after one hit = %d (want 2)", lives());
    SIM_CHECK_MSG(game_state() == 0,
                  "game_state after one hit = %d (want 0 — still alive)",
                  game_state());
}

// ship_collision_check early-exits when death_flash > 0 (the death-flash
// invulnerability window). For lifecycle tests we want sequential hits,
// so we clear death_flash to 0 before each frame to keep the ship
// vulnerable.
static void hit_frame(void) {
    sim_mem_w(S, S->sym_death_flash, 0);
    quiet_run();
}

static void test_three_hits_to_game_over(void) {
    scenario(28, 0);
    sim_place_lander(S, 0, 60, 28);

    hit_frame();                                          // hit 1
    SIM_CHECK_MSG(lives() == 2 && game_state() == 0,
                  "after hit 1: lives=%d game_state=%d", lives(), game_state());
    hit_frame();                                          // hit 2
    SIM_CHECK_MSG(lives() == 1 && game_state() == 0,
                  "after hit 2: lives=%d game_state=%d", lives(), game_state());
    hit_frame();                                          // hit 3 → game over
    SIM_CHECK_MSG(lives() == 0,
                  "after hit 3: lives=%d (want 0)", lives());
    SIM_CHECK_MSG(game_state() == 1,
                  "after hit 3: game_state=%d (want 1)", game_state());
}

static void test_game_over_freezes_lives(void) {
    scenario(28, 0);
    sim_place_lander(S, 0, 60, 28);
    for (int i = 0; i < 3; i++) hit_frame();              // drain to game-over
    SIM_CHECK(game_state() == 1);
    // Run more frames; main_loop short-circuits to main_game_over_path
    // and lives must not underflow / wrap to 255.
    for (int i = 0; i < 5; i++) quiet_run();
    SIM_CHECK_MSG(lives() == 0, "lives must stay 0 in game-over (got %d)", lives());
    SIM_CHECK_MSG(game_state() == 1, "game_state must stay 1 (got %d)", game_state());
}

// ---- HUD lives count -------------------------------------------------

static void check_hud_lives(int n) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, 28, 0, 0);
    sim_mem_w(S, S->sym_lives, n);
    quiet_run();

    int visible = (n >= 3) ? 3 : (n < 0 ? 0 : n);
    uint16_t fb = S->sym_framebuffer;

    // Slots at x=8, 16, 24 — each 8 cols wide.
    for (int slot = 0; slot < 3; slot++) {
        int x = 8 + slot * 8;
        for (int c = 0; c < 8; c++) {
            uint8_t got  = sim_mem_r(S, fb + x + c);
            uint8_t want = (slot < visible) ? ship_right[c] : 0x00;
            SIM_CHECK_MSG(got == want,
                          "lives=%d slot=%d col=%d: got=0x%02X want=0x%02X",
                          n, slot, x + c, got, want);
        }
    }
}

static void test_hud_lives_zero(void)  { check_hud_lives(0); }
static void test_hud_lives_one(void)   { check_hud_lives(1); }
static void test_hud_lives_two(void)   { check_hud_lives(2); }
static void test_hud_lives_three(void) { check_hud_lives(3); }
static void test_hud_lives_four_caps_at_three(void) { check_hud_lives(4); }

// ---- Bonus-ship awards (per difficulty) ------------------------------

// Set up a single-beam-kills-single-lander scenario with a controlled
// score and threshold, run enough frames for the kill, then verify the
// resulting lives, score, and threshold values.
static void run_bonus_kill(int diff, uint32_t init_score, uint32_t init_threshold) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, 28, 0, 0);
    sim_mem_w(S, S->sym_difficulty, (uint8_t)diff);
    write24(sym_score_lo, sym_score_mid, sym_score_hi, init_score);
    write24(sym_next_bonus_lo, sym_next_bonus_mid, sym_next_bonus_hi, init_threshold);
    sim_place_lander(S, 0, 80, 28);
    sim_place_beam(S, 0, 68, 32, 0);          // facing right; reaches at frame ~3
    for (int i = 0; i < 20; i++) quiet_run();
}

static void test_bonus_ship_low(void) {
    run_bonus_kill(0, 2900, 3000);            // LOW = 3000 increment
    SIM_CHECK_MSG(lives() == 4, "LOW: lives=%d (want 4)", lives());
    uint32_t s = read24(sym_score_lo, sym_score_mid, sym_score_hi);
    SIM_CHECK_MSG(s == 3000, "LOW: score=%u (want 3000)", s);
    uint32_t nb = read24(sym_next_bonus_lo, sym_next_bonus_mid, sym_next_bonus_hi);
    SIM_CHECK_MSG(nb == 6000, "LOW: next_bonus=%u (want 6000)", nb);
}

static void test_bonus_ship_med(void) {
    run_bonus_kill(1, 5900, 6000);            // MED = 6000 increment
    SIM_CHECK_MSG(lives() == 4, "MED: lives=%d (want 4)", lives());
    uint32_t s = read24(sym_score_lo, sym_score_mid, sym_score_hi);
    SIM_CHECK_MSG(s == 6000, "MED: score=%u (want 6000)", s);
    uint32_t nb = read24(sym_next_bonus_lo, sym_next_bonus_mid, sym_next_bonus_hi);
    SIM_CHECK_MSG(nb == 12000, "MED: next_bonus=%u (want 12000)", nb);
}

static void test_bonus_ship_high(void) {
    run_bonus_kill(2, 9900, 10000);           // HIGH = 10000 increment (Williams)
    SIM_CHECK_MSG(lives() == 4, "HIGH: lives=%d (want 4)", lives());
    uint32_t s = read24(sym_score_lo, sym_score_mid, sym_score_hi);
    SIM_CHECK_MSG(s == 10000, "HIGH: score=%u (want 10000)", s);
    uint32_t nb = read24(sym_next_bonus_lo, sym_next_bonus_mid, sym_next_bonus_hi);
    SIM_CHECK_MSG(nb == 20000, "HIGH: next_bonus=%u (want 20000)", nb);
}

// Non-exact crossing: score lands ABOVE the threshold, not on it. Pins
// the comparison as score >= next_bonus (not == next_bonus, which would
// only fire on perfect-100-multiple thresholds and silently skip awards
// when intermediate scoring sources later land non-multiples on score).
static void test_bonus_ship_overshoots_threshold(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, 28, 0, 0);
    sim_mem_w(S, S->sym_difficulty, 1);                   // MED
    write24(sym_score_lo, sym_score_mid, sym_score_hi, 5950);
    write24(sym_next_bonus_lo, sym_next_bonus_mid, sym_next_bonus_hi, 6000);
    sim_place_lander(S, 0, 80, 28);
    sim_place_beam(S, 0, 68, 32, 0);
    for (int i = 0; i < 20; i++) quiet_run();
    SIM_CHECK_MSG(lives() == 4, "overshoot: lives=%d (want 4)", lives());
    uint32_t s = read24(sym_score_lo, sym_score_mid, sym_score_hi);
    SIM_CHECK_MSG(s == 6050, "overshoot: score=%u (want 6050)", s);
    uint32_t nb = read24(sym_next_bonus_lo, sym_next_bonus_mid, sym_next_bonus_hi);
    SIM_CHECK_MSG(nb == 12000, "overshoot: next_bonus=%u (want 12000)", nb);
}

static void test_no_bonus_when_score_below_threshold(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, 28, 0, 0);
    sim_mem_w(S, S->sym_difficulty, 1);
    write24(sym_score_lo, sym_score_mid, sym_score_hi, 100);
    write24(sym_next_bonus_lo, sym_next_bonus_mid, sym_next_bonus_hi, 6000);
    sim_place_lander(S, 0, 80, 28);
    sim_place_beam(S, 0, 68, 32, 0);
    for (int i = 0; i < 20; i++) quiet_run();
    SIM_CHECK_MSG(lives() == 3, "no-bonus: lives=%d (want 3)", lives());
    uint32_t s = read24(sym_score_lo, sym_score_mid, sym_score_hi);
    SIM_CHECK_MSG(s == 200, "no-bonus: score=%u (want 200)", s);
    uint32_t nb = read24(sym_next_bonus_lo, sym_next_bonus_mid, sym_next_bonus_hi);
    SIM_CHECK_MSG(nb == 6000, "no-bonus: next_bonus=%u (want 6000)", nb);
}

static void test_lives_clamp_at_255(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, 28, 0, 0);
    sim_mem_w(S, S->sym_lives, 255);
    sim_mem_w(S, S->sym_difficulty, 1);
    write24(sym_score_lo, sym_score_mid, sym_score_hi, 5900);
    write24(sym_next_bonus_lo, sym_next_bonus_mid, sym_next_bonus_hi, 6000);
    sim_place_lander(S, 0, 80, 28);
    sim_place_beam(S, 0, 68, 32, 0);
    for (int i = 0; i < 20; i++) quiet_run();
    SIM_CHECK_MSG(lives() == 255,
                  "clamp: lives=%d (want 255 — must not wrap to 0)", lives());
    uint32_t nb = read24(sym_next_bonus_lo, sym_next_bonus_mid, sym_next_bonus_hi);
    SIM_CHECK_MSG(nb == 12000,
                  "clamp: next_bonus=%u (want 12000 — must still advance)", nb);
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <defender.elf>\n", argv[0]); return 2; }
    S = sim_boot(argv[1]);

    sym_score_lo       = sim_lookup(S, "score_lo");
    sym_score_mid      = sim_lookup(S, "score_mid");
    sym_score_hi       = sim_lookup(S, "score_hi");
    sym_next_bonus_lo  = sim_lookup(S, "next_bonus_lo");
    sym_next_bonus_mid = sim_lookup(S, "next_bonus_mid");
    sym_next_bonus_hi  = sim_lookup(S, "next_bonus_hi");

    test_initial_lives();
    test_collision_decrements_lives();
    test_three_hits_to_game_over();
    test_game_over_freezes_lives();

    test_hud_lives_zero();
    test_hud_lives_one();
    test_hud_lives_two();
    test_hud_lives_three();
    test_hud_lives_four_caps_at_three();

    test_bonus_ship_low();
    test_bonus_ship_med();
    test_bonus_ship_high();
    test_bonus_ship_overshoots_threshold();
    test_no_bonus_when_score_below_threshold();
    test_lives_clamp_at_255();

    sim_print_summary("lives");
    return sim_fail_count ? 1 : 0;
}
