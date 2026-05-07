// Layer tests for Humanoids (Phase 1).
// Contract:
//   - 8 Humanoids spawn at boot in entity slots 1..8.
//   - World_x = 16, 48, 80, 112, 144, 176, 208, 240 (every 32, offset 16).
//   - y = 48 (top of page 6, just above mountain row).
//   - update_humanoid is a no-op — they don't move from frame to frame.
//   - Beam-vs-humanoid: beam should NOT kill humanoids.
//   - Ship-vs-humanoid: should NOT trigger player death.
//   - Active flag survives many frames (passive entities stay alive).

#include <stdio.h>
#include <stdlib.h>

#include "sim_helpers.h"

#define TYPE_HUMANOID 7

static sim_t *S;

static int slot_type(int slot) {
    uint8_t b = sim_mem_r(S, S->sym_entities + slot*3);
    return (b >> 4) & 7;
}

static int slot_active(int slot) {
    return (sim_mem_r(S, S->sym_entities + slot*3) & 0x80) != 0;
}

static int slot_world_x(int slot) {
    return sim_mem_r(S, S->sym_entities + slot*3 + 1);
}

static int slot_y(int slot) {
    return sim_mem_r(S, S->sym_entities + slot*3 + 2) & 0x3F;
}

// ---- 1. At boot: 8 active humanoids in slots 1..8 at expected positions. ----
static void test_initial_layout(void) {
    sim_sync(S);
    static const uint8_t expected_x[8] = { 16, 48, 80, 112, 144, 176, 208, 240 };
    for (int i = 0; i < 8; i++) {
        int slot = 1 + i;
        SIM_CHECK_MSG(slot_active(slot), "slot %d not active", slot);
        SIM_CHECK_MSG(slot_type(slot) == TYPE_HUMANOID,
                      "slot %d type=%d (want %d)", slot, slot_type(slot), TYPE_HUMANOID);
        SIM_CHECK_MSG(slot_world_x(slot) == expected_x[i],
                      "slot %d world_x=%d (want %d)",
                      slot, slot_world_x(slot), expected_x[i]);
        SIM_CHECK_MSG(slot_y(slot) == 48,
                      "slot %d y=%d (want 48)", slot, slot_y(slot));
    }
}

// ---- 2. Humanoids don't move over many frames. ----
static void test_humanoids_dont_drift(void) {
    sim_sync(S);
    // Snapshot positions
    uint8_t initial_x[8];
    for (int i = 0; i < 8; i++) initial_x[i] = slot_world_x(1 + i);
    // Run 40 frames (well past Lander's 4-frame drift cycle)
    for (int i = 0; i < 40; i++) sim_run_frame(S);
    for (int i = 0; i < 8; i++) {
        int slot = 1 + i;
        SIM_CHECK_MSG(slot_active(slot), "slot %d disappeared", slot);
        SIM_CHECK_MSG(slot_world_x(slot) == initial_x[i],
                      "slot %d drifted: world_x=%d (want %d)",
                      slot, slot_world_x(slot), initial_x[i]);
        SIM_CHECK_MSG(slot_y(slot) == 48,
                      "slot %d y drifted to %d", slot, slot_y(slot));
    }
}

// ---- 3. Beam aimed at a humanoid kills it (Phase 2.5: humanoids are
//        vulnerable, not invulnerable). ----
static void test_beam_kills_humanoid(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_mem_w(S, S->sym_entities + 1*3 + 0, (1<<7) | (TYPE_HUMANOID<<4));
    sim_mem_w(S, S->sym_entities + 1*3 + 1, 80);
    sim_mem_w(S, S->sym_entities + 1*3 + 2, 48);
    sim_set_ship(S, 44, 0, 0);
    sim_place_beam(S, 0, 76, 48, 0);
    sim_run_frame(S);
    SIM_CHECK_MSG(!slot_active(1),
                  "humanoid should be killed by direct beam hit; byte0=0x%02X",
                  sim_mem_r(S, S->sym_entities + 3));
}

// ---- 4. Ship overlapping a humanoid does NOT trigger death. ----
static void test_ship_does_not_die_on_humanoid_contact(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_mem_w(S, S->sym_entities + 1*3 + 0, (1<<7) | (TYPE_HUMANOID<<4));
    sim_mem_w(S, S->sym_entities + 1*3 + 1, 60);   // screen_x = 60 = directly under ship
    sim_mem_w(S, S->sym_entities + 1*3 + 2, 28);   // y = 28 = same as ship
    sim_set_ship(S, 28, 0, 0);
    sim_run_frame(S);
    SIM_CHECK_MSG(sim_mem_r(S, S->sym_death_flash) == 0,
                  "BUG: ship died on humanoid contact (death_flash=%d)",
                  sim_mem_r(S, S->sym_death_flash));
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <defender.elf>\n", argv[0]); return 2; }
    S = sim_boot(argv[1]);

    test_initial_layout();
    test_humanoids_dont_drift();
    test_beam_kills_humanoid();
    test_ship_does_not_die_on_humanoid_contact();

    sim_coverage_save_for(argv[0]);
    sim_print_summary("humanoids");
    if (sim_fail_count) { printf("FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
