// Pins R10.6 — ship-falling-humanoid catch awards +500 points and
// deactivates the humanoid. (Williams also awarded +1000 for
// returning the humanoid to the ground; that's a deferred enhancement
// because it requires tracking per-humanoid carry state on the ship.)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_helpers.h"

#define TYPE_HUMANOID 7

static sim_t *S;

static void place_falling_humanoid(int slot, int world_x, int y) {
    // byte 0: active | TYPE_HUMANOID | bit 0 set (falling)
    sim_mem_w(S, S->sym_entities + slot * 3 + 0,
              (1 << 7) | (TYPE_HUMANOID << 4) | 0x01);
    sim_mem_w(S, S->sym_entities + slot * 3 + 1, (uint8_t)world_x);
    sim_mem_w(S, S->sym_entities + slot * 3 + 2, (uint8_t)y);
}

static int slot_active(int slot) {
    return sim_mem_r(S, S->sym_entities + slot * 3) & 0x80;
}

static uint32_t score(void) {
    return (uint32_t)sim_mem_r(S, sim_lookup(S, "score_lo"))
         | ((uint32_t)sim_mem_r(S, sim_lookup(S, "score_mid")) << 8)
         | ((uint32_t)sim_mem_r(S, sim_lookup(S, "score_hi"))  << 16);
}

static void clean(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, 28, 0, 0);
    sim_mem_w(S, S->sym_death_flash, 0);
    sim_mem_w(S, S->sym_respawn_invuln, 0);
    sim_mem_w(S, sim_lookup(S, "score_lo"),  0);
    sim_mem_w(S, sim_lookup(S, "score_mid"), 0);
    sim_mem_w(S, sim_lookup(S, "score_hi"),  0);
}

// ---- R10.6: catching a falling humanoid awards +500 -------------

static void test_catch_falling_humanoid_awards_500(void) {
    clean();
    // Place falling humanoid overlapping ship's hit box (60..67, 28..35).
    place_falling_humanoid(0, 60, 30);
    sim_run_frame(S);
    SIM_CHECK_MSG(!slot_active(0),
                  "R10.6: humanoid should be deactivated on catch");
    SIM_CHECK_MSG(score() == 500,
                  "R10.6: catch awards +500 pts; got %u", score());
}

// Grounded humanoid (bit 0 clear) at the same overlap position must
// NOT be caught — those just sit on the ground and the ship can pass
// over them.
static void test_grounded_humanoid_not_caught(void) {
    clean();
    // Active TYPE_HUMANOID without bit 0 set = grounded.
    sim_mem_w(S, S->sym_entities + 0, (1 << 7) | (TYPE_HUMANOID << 4));
    sim_mem_w(S, S->sym_entities + 1, 60);
    sim_mem_w(S, S->sym_entities + 2, 30);
    sim_run_frame(S);
    SIM_CHECK_MSG(slot_active(0),
                  "R10.6 (negative): grounded humanoid should NOT be caught");
    SIM_CHECK_MSG(score() == 0,
                  "R10.6 (negative): no score for non-catch; got %u", score());
}

// Falling humanoid far from the ship → no catch.
static void test_far_falling_humanoid_no_catch(void) {
    clean();
    place_falling_humanoid(0, 100, 30);
    sim_run_frame(S);
    SIM_CHECK(slot_active(0));
    SIM_CHECK(score() == 0);
}

// During the death window (respawn_invuln > 0), the catch logic must
// skip — otherwise a ghost ship in invuln blink could vacuum up
// falling humanoids.
static void test_no_catch_during_death_window(void) {
    clean();
    sim_mem_w(S, S->sym_respawn_invuln, 30);  // mid-explosion
    place_falling_humanoid(0, 60, 30);
    sim_run_frame(S);
    SIM_CHECK_MSG(slot_active(0),
                  "R10.6: catch must skip during death window");
    SIM_CHECK(score() == 0);
}

// ---- driver -----------------------------------------------------

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <defender.elf>\n", argv[0]); return 2; }
    S = sim_boot(argv[1]);

    test_catch_falling_humanoid_awards_500();
    test_grounded_humanoid_not_caught();
    test_far_falling_humanoid_no_catch();
    test_no_catch_during_death_window();

    sim_print_summary("humanoid_catch");
    return sim_fail_count ? 1 : 0;
}
