// Pins requirements R7.3-5 (Pod lethality + Pod→Swarmer chain) and
// R8.4-5 (Swarmer lethality + beam kill). See REQUIREMENTS.md for the
// authoritative wording of each. This is a regression-test file, not
// a characterization tool — every check asserts an expected value
// taken straight from the requirements doc.
//
// Setup pattern per test:
//   sim_clear_state_minimal(S)  — clean entities, planet_check off,
//                                 lives=3, spawn_countdown=255.
//   sim_set_ship(...)           — ship at canonical (60, 28) position.
//   place the critter under test directly into an entity slot.
//   optionally place a beam (sim_place_beam) to test beam-kill paths.
//   sim_run_frame(S).
//   inspect lives / entity table / score.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_helpers.h"

#define TYPE_NONE      0
#define TYPE_LANDER    1
#define TYPE_MUTANT    2
#define TYPE_POD       3
#define TYPE_SWARMER   5
#define TYPE_HUMANOID  7

static sim_t *S;
static uint16_t sym_score_lo, sym_score_mid, sym_score_hi;

// ---- helpers ----------------------------------------------------------

static void place(int slot, uint8_t type_with_state, int world_x, int y) {
    sim_mem_w(S, S->sym_entities + slot * 3 + 0, type_with_state);
    sim_mem_w(S, S->sym_entities + slot * 3 + 1, (uint8_t)world_x);
    sim_mem_w(S, S->sym_entities + slot * 3 + 2, (uint8_t)y);
}

static int slot_active(int slot) {
    return sim_mem_r(S, S->sym_entities + slot * 3) & 0x80;
}
static int slot_type(int slot) {
    return (sim_mem_r(S, S->sym_entities + slot * 3) >> 4) & 0x07;
}
static int slot_world_x(int slot) {
    return sim_mem_r(S, S->sym_entities + slot * 3 + 1);
}
static int slot_y(int slot) {
    return sim_mem_r(S, S->sym_entities + slot * 3 + 2) & 0x3F;
}
static int slot_dir(int slot) {
    return sim_mem_r(S, S->sym_entities + slot * 3) & 0x07;
}

static int count_type(int target_type) {
    int n = 0;
    for (int i = 0; i < 64; i++) {
        if (slot_active(i) && slot_type(i) == target_type) n++;
    }
    return n;
}

static int lives(void)      { return sim_mem_r(S, S->sym_lives); }
static int game_state(void) { return sim_mem_r(S, S->sym_game_state); }

static uint32_t read_score(void) {
    return (uint32_t)sim_mem_r(S, sym_score_lo)
         | ((uint32_t)sim_mem_r(S, sym_score_mid) << 8)
         | ((uint32_t)sim_mem_r(S, sym_score_hi)  << 16);
}

static void scenario(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, 28, 0, 0);
    // Tests below need death_flash and respawn_invuln cleared so a
    // single-frame collision actually fires.
    sim_mem_w(S, S->sym_death_flash, 0);
    sim_mem_w(S, S->sym_respawn_invuln, 0);
    // Reset score.
    sim_mem_w(S, sym_score_lo,  0);
    sim_mem_w(S, sym_score_mid, 0);
    sim_mem_w(S, sym_score_hi,  0);
}

// ---- R7.3: Pod lethal on ship contact -------------------------------

static void test_pod_lethal_on_contact(void) {
    scenario();
    // Place Pod overlapping the ship's 8x8 footprint at (60..67, 28..35).
    place(0, (1 << 7) | (TYPE_POD << 4), 60, 28);
    int lives_before = lives();
    sim_run_frame(S);
    SIM_CHECK_MSG(lives() == lives_before - 1,
                  "R7.3: Pod contact must decrement lives; %d -> %d",
                  lives_before, lives());
    SIM_CHECK_MSG(game_state() == 0,
                  "R7.3: still have reserves, game_state should stay 0");
}

// ---- R7.4: Pod kill spawns 5 Swarmers + awards 100 pts --------------

static void test_pod_kill_spawns_5_swarmers(void) {
    scenario();
    // Pod parked at world_x=80, y=28. Beam at screen_x=76, beam_y=32
    // facing right (+4/frame); travels 0,4,8,... pixels until it lands
    // on the Pod.
    place(0, (1 << 7) | (TYPE_POD << 4), 80, 28);
    sim_place_beam(S, 0, 76, 32, 0);

    // Run 8 frames — more than enough for the beam to reach.
    for (int i = 0; i < 8; i++) sim_run_frame(S);

    // Pod is dead — but spawn_swarmers scans from slot 0 looking for
    // free slots, and the just-cleared Pod slot is the first one it
    // finds, so slot 0 typically becomes Swarmer #1. Check for "no Pod
    // remaining" rather than "slot 0 inactive".
    SIM_CHECK_MSG(count_type(TYPE_POD) == 0,
                  "R7.4: no active Pods after kill; got %d",
                  count_type(TYPE_POD));
    int swarmers = count_type(TYPE_SWARMER);
    SIM_CHECK_MSG(swarmers == 5,
                  "R7.4: killing a Pod must spawn exactly 5 Swarmers; got %d",
                  swarmers);
    uint32_t s = read_score();
    SIM_CHECK_MSG(s == 100,
                  "R7.4: Pod kill awards 100 pts; got %u", s);
}

// ---- R7.5: 5 Swarmers from Pod death get distinct compass directions

static void test_swarmer_burst_directions_distinct(void) {
    scenario();
    place(0, (1 << 7) | (TYPE_POD << 4), 80, 28);
    sim_place_beam(S, 0, 76, 32, 0);
    for (int i = 0; i < 8; i++) sim_run_frame(S);

    // Collect the dirs of all active Swarmers.
    int dirs[8] = {0};
    int count = 0;
    for (int i = 0; i < 64; i++) {
        if (slot_active(i) && slot_type(i) == TYPE_SWARMER) {
            int d = slot_dir(i);
            dirs[d]++;
            count++;
        }
    }
    SIM_CHECK_MSG(count == 5, "R7.5 prereq: expected 5 Swarmers, got %d", count);
    int distinct = 0;
    for (int i = 0; i < 8; i++) if (dirs[i] > 0) distinct++;
    SIM_CHECK_MSG(distinct == 5,
                  "R7.5: 5 Swarmers must have 5 distinct compass dirs; got %d",
                  distinct);
}

// ---- R8.4: Swarmer lethal on ship contact ---------------------------

static void test_swarmer_lethal_on_contact(void) {
    scenario();
    // Swarmer overlapping the ship. Direction 0 (E) means it'll move
    // RIGHT one pixel this frame, but still inside the 8x8 hit box.
    place(0, (1 << 7) | (TYPE_SWARMER << 4), 60, 28);
    int lives_before = lives();
    sim_run_frame(S);
    SIM_CHECK_MSG(lives() == lives_before - 1,
                  "R8.4: Swarmer contact must decrement lives; %d -> %d",
                  lives_before, lives());
}

// ---- R8.5: Beam kills Swarmer for 100 pts ---------------------------

static void test_beam_kills_swarmer(void) {
    scenario();
    // Swarmer at world_x=80, y=28. Beam at screen_x=76, beam_y=32
    // facing right. Direction 0 (E) means the Swarmer drifts right
    // 1 px/frame; over the few frames the beam needs to reach, the
    // Swarmer moves only a few cols away, still within beam path.
    place(0, (1 << 7) | (TYPE_SWARMER << 4), 80, 28);
    sim_place_beam(S, 0, 76, 32, 0);
    for (int i = 0; i < 8; i++) sim_run_frame(S);

    SIM_CHECK_MSG(!slot_active(0),
                  "R8.5: Swarmer slot should be inactive after beam kill");
    uint32_t s = read_score();
    SIM_CHECK_MSG(s == 100, "R8.5: Swarmer kill awards 100 pts; got %u", s);
}

// ---- driver ---------------------------------------------------------

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <defender.elf>\n", argv[0]); return 2; }
    S = sim_boot(argv[1]);

    sym_score_lo  = sim_lookup(S, "score_lo");
    sym_score_mid = sim_lookup(S, "score_mid");
    sym_score_hi  = sim_lookup(S, "score_hi");

    test_pod_lethal_on_contact();
    test_pod_kill_spawns_5_swarmers();
    test_swarmer_burst_directions_distinct();
    test_swarmer_lethal_on_contact();
    test_beam_kills_swarmer();

    sim_print_summary("critter_kills");
    return sim_fail_count ? 1 : 0;
}
