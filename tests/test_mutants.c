// Layer tests for Phase 3: Mutant transformation and behavior.
//
// Contract:
//   - When a carrying Lander reaches y=9 (one row above the radar-bar
//     pin), it transforms in-place: byte 0 keeps the active bit but
//     changes type LANDER→MUTANT, clears state (carry, pause).
//   - update_mutant: chases the player.
//       * Vertical: every 2 frames, move 1 row toward sprite_y.
//       * Horizontal: every 4 frames, move 1 col toward (scroll+60),
//         signed delta → shorter wrap direction.
//   - ship_collision_check: a Mutant overlapping the ship triggers
//     death (death_flash armed) just like a Lander.
//   - collision_check: a beam hits a Mutant (8-row bbox); HIT just
//     deactivates the slot.

#include <stdio.h>
#include <stdlib.h>

#include "sim_helpers.h"

#define TYPE_LANDER  1
#define TYPE_MUTANT  2

static sim_t *S;

static int slot_byte0(int slot) { return sim_mem_r(S, S->sym_entities + slot*3); }
static int slot_active(int slot) { return (slot_byte0(slot) & 0x80) != 0; }
static int slot_type(int slot) { return (slot_byte0(slot) >> 4) & 0x07; }
static int slot_world_x(int slot) { return sim_mem_r(S, S->sym_entities + slot*3 + 1); }
static int slot_y(int slot) { return sim_mem_r(S, S->sym_entities + slot*3 + 2) & 0x3F; }

static void place_lander_carrying(int slot, int world_x, int y) {
    sim_mem_w(S, S->sym_entities + slot*3 + 0,
              (1<<7) | (TYPE_LANDER<<4) | 0x01);
    sim_mem_w(S, S->sym_entities + slot*3 + 1, world_x);
    sim_mem_w(S, S->sym_entities + slot*3 + 2, y);
}

static void place_mutant(int slot, int world_x, int y) {
    sim_mem_w(S, S->sym_entities + slot*3 + 0, (1<<7) | (TYPE_MUTANT<<4));
    sim_mem_w(S, S->sym_entities + slot*3 + 1, world_x);
    sim_mem_w(S, S->sym_entities + slot*3 + 2, y);
}

static void scenario_only(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, 28, 0, 0);
}

// ---- 1. Carrying Lander at y=9 transforms to a Mutant ----
static void test_carrying_lander_y9_becomes_mutant(void) {
    scenario_only();
    place_lander_carrying(0, 100, 9);
    // 4 frames → drift fires → transform (don't decrement to 8).
    for (int i = 0; i < 4; i++) sim_run_frame(S);
    SIM_CHECK_MSG(slot_active(0), "transformed slot should still be active");
    SIM_CHECK_MSG(slot_type(0) == TYPE_MUTANT,
                  "expected type=MUTANT after transform; got %d", slot_type(0));
    SIM_CHECK_MSG((slot_byte0(0) & 0x0F) == 0,
                  "state bits (carry/pause) should clear; byte0=0x%02X",
                  slot_byte0(0));
    SIM_CHECK_MSG(slot_world_x(0) == 100,
                  "mutant should inherit world_x; got %d", slot_world_x(0));
}

// ---- 2. Mutant ABOVE player descends (1 row per 2 frames) ----
static void test_mutant_descends_toward_player(void) {
    scenario_only();
    sim_set_ship(S, 28, 0, 0);                    // sprite_y=28
    place_mutant(0, 100, 10);                     // mutant_y=10 < 28
    int y0 = slot_y(0);
    // 2 frames → one vertical event → y advances by 1.
    for (int i = 0; i < 2; i++) sim_run_frame(S);
    int y1 = slot_y(0);
    SIM_CHECK_MSG(y1 == y0 + 1,
                  "mutant should descend toward player: y went %d → %d",
                  y0, y1);
}

// ---- 3. Mutant BELOW player ascends ----
static void test_mutant_ascends_toward_player(void) {
    scenario_only();
    sim_set_ship(S, 8, 0, 0);                     // sprite_y=8 (top of motion range)
    place_mutant(0, 100, 30);                     // mutant_y=30 > 8
    int y0 = slot_y(0);
    for (int i = 0; i < 2; i++) sim_run_frame(S);
    int y1 = slot_y(0);
    SIM_CHECK_MSG(y1 == y0 - 1,
                  "mutant should ascend toward player: y went %d → %d",
                  y0, y1);
}

// ---- 4. Mutant aligned vertically: no v motion ----
static void test_mutant_v_aligned_no_motion(void) {
    scenario_only();
    sim_set_ship(S, 28, 0, 0);
    place_mutant(0, 100, 28);
    for (int i = 0; i < 4; i++) sim_run_frame(S);
    SIM_CHECK_MSG(slot_y(0) == 28,
                  "mutant aligned with player should not move v; got y=%d",
                  slot_y(0));
}

// ---- 5. Mutant moves horizontally toward (scroll+60) ----
static void test_mutant_seeks_horizontally(void) {
    scenario_only();
    sim_set_ship(S, 28, 0, 0);                    // scroll=0 → target world_x = 60
    place_mutant(0, 100, 28);                     // mutant world_x=100, target=60
    // 4 frames → one horizontal event. 60 - 100 = -40 → MSB set → LEFT.
    int x0 = slot_world_x(0);
    for (int i = 0; i < 4; i++) sim_run_frame(S);
    int x1 = slot_world_x(0);
    SIM_CHECK_MSG(x1 == x0 - 1,
                  "mutant should advance LEFT toward player; world_x %d → %d",
                  x0, x1);
}

// ---- 6. Mutant uses world-wrap for shorter direction ----
static void test_mutant_uses_world_wrap(void) {
    scenario_only();
    sim_set_ship(S, 28, 0, 0);                    // target world_x = 60
    place_mutant(0, 10, 28);                      // mutant_x=10, 60-10=+50 (RIGHT)
    int x0 = slot_world_x(0);
    for (int i = 0; i < 4; i++) sim_run_frame(S);
    int x1 = slot_world_x(0);
    SIM_CHECK_MSG(x1 == x0 + 1,
                  "mutant should advance RIGHT toward player; world_x %d → %d",
                  x0, x1);
}

// ---- 7. Mutant overlapping the ship arms death_flash ----
static void test_mutant_kills_ship(void) {
    scenario_only();
    place_mutant(0, 60, 28);                      // direct overlap with ship
    sim_run_frame(S);
    SIM_CHECK_MSG(sim_mem_r(S, S->sym_death_flash) == 7,
                  "mutant collision should arm death_flash; got %d",
                  sim_mem_r(S, S->sym_death_flash));
}

// ---- 8. Beam kills a Mutant (collision_check accepts TYPE_MUTANT) ----
static void test_beam_kills_mutant(void) {
    scenario_only();
    place_mutant(0, 80, 28);
    sim_set_ship(S, 24, 0, 0);                    // beam_y will be 28
    sim_place_beam(S, 0, 76, 28, 0);
    sim_run_frame(S);
    SIM_CHECK_MSG(!slot_active(0),
                  "mutant should be killed by beam hit; byte0=0x%02X",
                  slot_byte0(0));
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <defender.elf>\n", argv[0]); return 2; }
    S = sim_boot(argv[1]);

    test_carrying_lander_y9_becomes_mutant();
    test_mutant_descends_toward_player();
    test_mutant_ascends_toward_player();
    test_mutant_v_aligned_no_motion();
    test_mutant_seeks_horizontally();
    test_mutant_uses_world_wrap();
    test_mutant_kills_ship();
    test_beam_kills_mutant();

    sim_coverage_save_for(argv[0]);
    sim_print_summary("mutants");
    if (sim_fail_count) { printf("FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
