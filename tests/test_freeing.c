// Layer tests for Phase 2.5: shoot-the-carrier and shoot-the-humanoid.
//
// Contract:
//   - Beam vs. carrying Lander: Lander slot is rewritten as a falling
//     Humanoid (active | TYPE_HUMANOID | falling=1) at the Lander's
//     world_x/y. The beam is consumed.
//   - Beam vs. empty-handed Lander: just deactivate (existing behavior).
//   - Beam vs. grounded Humanoid: just deactivate (Phase 2.5: vulnerable).
//   - Beam vs. falling Humanoid: just deactivate (mid-air death is final).
//   - Falling Humanoid descends 1 row per 4 frames; on reaching y=48
//     the falling bit clears and the humanoid resumes normal grounded life.

#include <stdio.h>
#include <stdlib.h>

#include "sim_helpers.h"

#define TYPE_LANDER    1
#define TYPE_HUMANOID  7

static sim_t *S;

static void place_lander_carrying(int slot, int world_x, int y) {
    sim_mem_w(S, S->sym_entities + slot*3 + 0,
              (1<<7) | (TYPE_LANDER<<4) | 0x01);   // active + LANDER + carry
    sim_mem_w(S, S->sym_entities + slot*3 + 1, world_x);
    sim_mem_w(S, S->sym_entities + slot*3 + 2, y);
}

static void place_humanoid(int slot, int world_x, int y, int falling) {
    uint8_t b0 = (1<<7) | (TYPE_HUMANOID<<4) | (falling ? 0x01 : 0);
    sim_mem_w(S, S->sym_entities + slot*3 + 0, b0);
    sim_mem_w(S, S->sym_entities + slot*3 + 1, world_x);
    sim_mem_w(S, S->sym_entities + slot*3 + 2, y);
}

static int slot_byte0(int slot) {
    return sim_mem_r(S, S->sym_entities + slot*3);
}

static int slot_world_x(int slot) {
    return sim_mem_r(S, S->sym_entities + slot*3 + 1);
}

static int slot_y(int slot) {
    return sim_mem_r(S, S->sym_entities + slot*3 + 2) & 0x3F;
}

static int slot_type(int slot) {
    return (slot_byte0(slot) >> 4) & 0x07;
}

static int slot_active(int slot) {
    return (slot_byte0(slot) & 0x80) != 0;
}

static int slot_falling(int slot) {
    return slot_byte0(slot) & 0x01;
}

static void scenario_only(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, 28, 0, 0);
}

// ---- 1. Shooting a carrying Lander (top half) frees the humanoid ----
static void test_shoot_carrier_top_frees_humanoid(void) {
    scenario_only();
    place_lander_carrying(0, 80, 20);                // mid-air carrying
    sim_place_beam(S, 0, 76, 24, 0);                 // beam_y=24 in [20, 27]
    sim_run_frame(S);
    SIM_CHECK_MSG(slot_active(0),
                  "slot should still be active (transformed, not killed)");
    SIM_CHECK_MSG(slot_type(0) == TYPE_HUMANOID,
                  "slot should now be a HUMANOID; got type=%d", slot_type(0));
    SIM_CHECK_MSG(slot_falling(0),
                  "released humanoid should have falling bit set");
    SIM_CHECK_MSG(slot_world_x(0) == 80,
                  "freed humanoid should inherit lander's world_x; got %d",
                  slot_world_x(0));
    SIM_CHECK_MSG(slot_y(0) == 20,
                  "freed humanoid should inherit lander's y; got %d",
                  slot_y(0));
}

// ---- 1b. Shooting the dangling humanoid kills ONLY the humanoid.
//         The Lander loses its cargo (carry bit cleared), stays alive,
//         and resumes empty-handed (will descend and try to grab again). ----
static void test_shoot_dangling_humanoid_kills_humanoid_only(void) {
    scenario_only();
    place_lander_carrying(0, 80, 20);
    sim_place_beam(S, 0, 76, 30, 0);              // beam_y=30 → lower half
    sim_run_frame(S);
    SIM_CHECK_MSG(slot_active(0),
                  "Lander should still be alive after losing cargo");
    SIM_CHECK_MSG(slot_type(0) == TYPE_LANDER,
                  "slot type should remain LANDER; got %d", slot_type(0));
    SIM_CHECK_MSG((slot_byte0(0) & 0x01) == 0,
                  "carry bit should be cleared; byte0=0x%02X", slot_byte0(0));
    SIM_CHECK_MSG(slot_world_x(0) == 80,
                  "lander world_x should be unchanged; got %d", slot_world_x(0));
}

// ---- 1c. Empty-handed Lander still has 8-row bbox (not extended). ----
static void test_empty_lander_8row_bbox(void) {
    scenario_only();
    sim_place_lander(S, 0, 80, 20);                  // not carrying
    sim_place_beam(S, 0, 76, 30, 0);                  // beam_y=30, outside 8-row bbox
    sim_run_frame(S);
    SIM_CHECK_MSG(slot_active(0),
                  "non-carrying lander should not be hit at y=30 (out of 8-row bbox)");
}

// ---- 2. Shooting an empty-handed Lander just kills it (no slot survives) ----
static void test_shoot_empty_lander_just_kills(void) {
    scenario_only();
    sim_place_lander(S, 0, 80, 20);                  // helper places non-carrying
    sim_place_beam(S, 0, 76, 24, 0);
    sim_run_frame(S);
    SIM_CHECK_MSG(!slot_active(0), "empty lander should die outright");
    SIM_CHECK_MSG(slot_byte0(0) == 0,
                  "slot should be fully cleared; got 0x%02X", slot_byte0(0));
}

// ---- 3. Shooting a grounded humanoid kills it ----
static void test_shoot_grounded_humanoid(void) {
    scenario_only();
    place_humanoid(1, 80, 48, /*falling*/0);
    sim_set_ship(S, 44, 0, 0);                        // beam_y will be 48
    sim_place_beam(S, 0, 76, 48, 0);
    sim_run_frame(S);
    SIM_CHECK_MSG(!slot_active(1),
                  "grounded humanoid should die from beam");
}

// ---- 4. Shooting a falling humanoid kills it (mid-air death) ----
static void test_shoot_falling_humanoid_kills(void) {
    scenario_only();
    place_humanoid(1, 80, 30, /*falling*/1);
    sim_place_beam(S, 0, 76, 32, 0);                  // beam y=32 ∈ [30, 37]
    sim_run_frame(S);
    SIM_CHECK_MSG(!slot_active(1),
                  "falling humanoid should die from beam; byte0=0x%02X",
                  slot_byte0(1));
}

// ---- 5. Falling humanoid descends 1 row per 4 frames ----
static void test_falling_humanoid_drifts_down(void) {
    scenario_only();
    place_humanoid(1, 80, 20, /*falling*/1);
    int y0 = slot_y(1);
    // Run 4 frames → one drift event → y should advance by 1.
    for (int i = 0; i < 4; i++) sim_run_frame(S);
    int y1 = slot_y(1);
    SIM_CHECK_MSG(y1 == y0 + 1,
                  "falling humanoid: y went %d → %d (want +1)", y0, y1);
}

// ---- 6. Falling humanoid lands at y=48: falling bit cleared ----
static void test_falling_humanoid_lands_safely(void) {
    scenario_only();
    place_humanoid(1, 80, 47, /*falling*/1);          // one row above ground
    // 4 frames → drift → y=48 → land.
    for (int i = 0; i < 4; i++) sim_run_frame(S);
    SIM_CHECK_MSG(slot_active(1), "humanoid should still be alive");
    SIM_CHECK_MSG(slot_y(1) == 48,
                  "humanoid should be at ground; got y=%d", slot_y(1));
    SIM_CHECK_MSG(!slot_falling(1),
                  "falling bit should clear after landing; byte0=0x%02X",
                  slot_byte0(1));
}

// ---- 7. Grounded humanoid (no falling bit) doesn't drift ----
static void test_grounded_humanoid_no_drift(void) {
    scenario_only();
    place_humanoid(1, 80, 48, /*falling*/0);
    // Run many frames; ground humanoid should not move.
    for (int i = 0; i < 40; i++) sim_run_frame(S);
    SIM_CHECK_MSG(slot_y(1) == 48,
                  "grounded humanoid drifted to y=%d", slot_y(1));
    SIM_CHECK_MSG(slot_world_x(1) == 80, "world_x changed");
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <defender.elf>\n", argv[0]); return 2; }
    S = sim_boot(argv[1]);

    test_shoot_carrier_top_frees_humanoid();
    test_shoot_dangling_humanoid_kills_humanoid_only();
    test_empty_lander_8row_bbox();
    test_shoot_empty_lander_just_kills();
    test_shoot_grounded_humanoid();
    test_shoot_falling_humanoid_kills();
    test_falling_humanoid_drifts_down();
    test_falling_humanoid_lands_safely();
    test_grounded_humanoid_no_drift();

    sim_coverage_save_for(argv[0]);
    sim_print_summary("freeing");
    if (sim_fail_count) { printf("FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
