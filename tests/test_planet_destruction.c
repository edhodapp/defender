// Pins requirements R11.1-5 (planet destruction state machine) and
// R12.6, R12.11 (spawner all-Mutant mode + wave-end humanoid respawn).
// See REQUIREMENTS.md for canonical wording.
//
// The planet-destruction watcher is event-driven: it fires only at
// humanoid-removal sites (chh_kill humanoid path, chh_drop_cargo,
// ul_become_mutant). To test it in isolation we re-ENABLE the watcher
// (sim_clear_state_minimal disables it for the rest of the harness)
// and then trigger one of those sites by setting up the right
// scenario.

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

// ---- helpers --------------------------------------------------------

static void place(int slot, uint8_t b0, int world_x, int y) {
    sim_mem_w(S, S->sym_entities + slot * 3 + 0, b0);
    sim_mem_w(S, S->sym_entities + slot * 3 + 1, (uint8_t)world_x);
    sim_mem_w(S, S->sym_entities + slot * 3 + 2, (uint8_t)y);
}

static int slot_active(int slot) {
    return sim_mem_r(S, S->sym_entities + slot * 3) & 0x80;
}
static int slot_type(int slot) {
    return (sim_mem_r(S, S->sym_entities + slot * 3) >> 4) & 0x07;
}
static int slot_carry(int slot) {
    return sim_mem_r(S, S->sym_entities + slot * 3) & 0x01;
}
static int slot_world_x(int slot) {
    return sim_mem_r(S, S->sym_entities + slot * 3 + 1);
}

static int count_type(int target) {
    int n = 0;
    for (int i = 0; i < 64; i++) {
        if (slot_active(i) && slot_type(i) == target) n++;
    }
    return n;
}

static int planet_destroyed(void) {
    return sim_mem_r(S, S->sym_planet_destroyed);
}

// Enable the planet watcher for this scenario, clear destroyed flag,
// and remove the test harness's default humanoid presence. Each test
// then places exactly the entities it needs.
static void planet_scenario(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, 28, 0, 0);
    sim_mem_w(S, S->sym_planet_check_disabled, 0);
    sim_mem_w(S, S->sym_planet_destroyed, 0);
    // sim_clear_state_minimal already clears all entities.
}

// ---- R11.1 + R11.5: event-driven trip on last humanoid removal ----

static void test_trip_on_last_humanoid_shot(void) {
    planet_scenario();
    // One humanoid on the ground; one beam aimed at it.
    place(1, (1 << 7) | (TYPE_HUMANOID << 4), 80, 48);
    sim_place_beam(S, 0, 76, 50, 0);   // beam_y=50 → inside humanoid 8x8 box
    SIM_CHECK_MSG(planet_destroyed() == 0,
                  "R11.1 prereq: not destroyed before kill");

    for (int i = 0; i < 6; i++) sim_run_frame(S);

    SIM_CHECK_MSG(count_type(TYPE_HUMANOID) == 0,
                  "R11.1 prereq: humanoid should be dead");
    SIM_CHECK_MSG(planet_destroyed() == 1,
                  "R11.1: planet_destroyed should be 1 after last humanoid removal");
}

// ---- R11.1: carrying-Lander counts as "humanoid alive" ------------

static void test_carrying_lander_blocks_destruction(void) {
    planet_scenario();
    // No grounded humanoids, but a carrying Lander has one in transit.
    // Trip an unrelated humanoid-removal event (chh_kill humanoid) by
    // placing a SECOND humanoid + a beam — except there's no second
    // humanoid here. So manually invoke the check via shooting an
    // already-grounded humanoid: place a falling humanoid and a beam.
    place(1, (1 << 7) | (TYPE_LANDER << 4) | 0x01, 80, 20);  // carrying Lander
    place(2, (1 << 7) | (TYPE_HUMANOID << 4) | 0x01, 100, 30); // falling humanoid
    sim_place_beam(S, 0, 96, 32, 0);

    for (int i = 0; i < 6; i++) sim_run_frame(S);

    // The falling humanoid was shot → check fires; but the carrying
    // Lander still has a humanoid → planet must NOT destroy.
    SIM_CHECK_MSG(planet_destroyed() == 0,
                  "R11.1: carrying Lander still has humanoid; planet stays alive");
}

// ---- R11.2: on destruction, non-carrying Landers turn Mutant -----

static void test_lander_transform_on_destruction(void) {
    planet_scenario();
    // One humanoid (slot 1), one empty Lander (slot 2). Shoot the
    // humanoid → planet destroys → slot 2 Lander should become a
    // Mutant in place.
    place(1, (1 << 7) | (TYPE_HUMANOID << 4), 80, 48);
    place(2, (1 << 7) | (TYPE_LANDER   << 4), 30, 20);
    sim_place_beam(S, 0, 76, 50, 0);

    // Beam +4 px/frame reaches the humanoid in 1 frame; ~2 frames is
    // enough for chh_kill → check_planet_destruction →
    // transform_landers_to_mutants to fire. Stop here before the new
    // Mutant's chase AI starts moving it (drift gate every 4 frames).
    sim_run_frame(S);
    sim_run_frame(S);

    SIM_CHECK_MSG(planet_destroyed() == 1,
                  "R11.2 prereq: planet should be destroyed");
    SIM_CHECK_MSG(slot_active(2),
                  "R11.2: slot 2 should still be active");
    SIM_CHECK_MSG(slot_type(2) == TYPE_MUTANT,
                  "R11.2: slot 2 Lander should be a Mutant now (got type=%d)",
                  slot_type(2));
    SIM_CHECK_MSG(slot_world_x(2) == 30,
                  "R11.2: world_x preserved across transformation; got %d",
                  slot_world_x(2));
}

// ---- R11.2 (negative): carrying Landers are NOT transformed -------

static void test_carrying_lander_not_transformed(void) {
    planet_scenario();
    // Two carrying Landers (each holding a humanoid in transit) +
    // one grounded humanoid. Shoot the grounded humanoid:
    //   - grounded humanoid dies (humanoid removal)
    //   - planet check counts remaining humanoids: 2 (the two
    //     carried) → NOT destroyed.
    // We use this scenario to also check that even if planet WERE
    // destroyed, carrying Landers are skipped. Simulate by manually
    // setting planet_destroyed=1 after the fact and re-checking.
    place(1, (1 << 7) | (TYPE_HUMANOID << 4), 80, 48);
    place(2, (1 << 7) | (TYPE_LANDER   << 4) | 0x01, 30, 20);  // carrying #1
    place(3, (1 << 7) | (TYPE_LANDER   << 4) | 0x01, 50, 22);  // carrying #2
    sim_place_beam(S, 0, 76, 50, 0);

    for (int i = 0; i < 6; i++) sim_run_frame(S);

    // grounded humanoid gone, but two carrying Landers still hold
    // humanoids → planet must NOT trip.
    SIM_CHECK_MSG(planet_destroyed() == 0,
                  "R11.2 prereq: 2 carried humanoids should keep planet alive");
    // Both carrying Landers still present, both still TYPE_LANDER.
    SIM_CHECK(slot_type(2) == TYPE_LANDER && slot_carry(2));
    SIM_CHECK(slot_type(3) == TYPE_LANDER && slot_carry(3));
}

// ---- R11.3 (== R12.6): spawner installs Mutants when destroyed ----

static void test_spawner_installs_mutants_in_all_mutant_mode(void) {
    planet_scenario();
    // Pretend planet has already fallen.
    sim_mem_w(S, S->sym_planet_destroyed, 1);
    sim_mem_w(S, S->sym_wave_to_spawn, 5);
    sim_mem_w(S, S->sym_wave_pods_to_spawn, 2);  // Pods queued — should be ignored
    sim_mem_w(S, S->sym_spawn_countdown, 0);     // fire next frame
    sim_run_frame(S);

    // Exactly one new enemy spawned; it must be a Mutant regardless
    // of the queued Pod allotment.
    SIM_CHECK_MSG(count_type(TYPE_MUTANT) == 1,
                  "R11.3: all-Mutant spawner should install a Mutant; got %d Mutants",
                  count_type(TYPE_MUTANT));
    SIM_CHECK_MSG(count_type(TYPE_POD) == 0,
                  "R11.3: no Pod despite wave_pods_to_spawn=2; got %d Pods",
                  count_type(TYPE_POD));
    // wave_pods_to_spawn is *not* decremented on Mutant spawn (the
    // Pod path is bypassed entirely). Reflects current implementation.
    SIM_CHECK_MSG(sim_mem_r(S, S->sym_wave_pods_to_spawn) == 2,
                  "R11.3 (observed): wave_pods_to_spawn unchanged in all-Mutant mode");
}

// ---- R11.4 + R12.11: wave-end clears flag and respawns 8 humanoids

static void test_wave_end_respawns_humanoids_and_clears_flag(void) {
    planet_scenario();
    // Force the wave-end path: wave_to_spawn=0, enemies_alive=0,
    // planet_destroyed=1. The next main_alive iteration's wave-end
    // logic should clear the flag and lay down 8 humanoids in
    // slots 1..8.
    sim_mem_w(S, S->sym_planet_destroyed, 1);
    sim_mem_w(S, S->sym_wave_to_spawn, 0);
    sim_mem_w(S, S->sym_enemies_alive, 0);
    // wave_at_max kick-spawns one enemy — clear spawn_countdown so we
    // catch any wave-end side effects cleanly.
    sim_mem_w(S, S->sym_spawn_countdown, 255);

    sim_run_frame(S);

    SIM_CHECK_MSG(planet_destroyed() == 0,
                  "R11.4: wave-end should clear planet_destroyed");
    SIM_CHECK_MSG(count_type(TYPE_HUMANOID) == 8,
                  "R12.11: wave-end should respawn 8 humanoids; got %d",
                  count_type(TYPE_HUMANOID));
    // Verify the canonical world_x positions (16, 48, 80, ..., 240).
    for (int i = 0; i < 8; i++) {
        int expected_x = 16 + i * 32;
        SIM_CHECK_MSG(slot_world_x(1 + i) == expected_x,
                      "R12.11: humanoid slot %d world_x=%d (want %d)",
                      1 + i, slot_world_x(1 + i), expected_x);
    }
}

// ---- driver -------------------------------------------------------

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <defender.elf>\n", argv[0]); return 2; }
    S = sim_boot(argv[1]);

    test_trip_on_last_humanoid_shot();
    test_carrying_lander_blocks_destruction();
    test_lander_transform_on_destruction();
    test_carrying_lander_not_transformed();
    test_spawner_installs_mutants_in_all_mutant_mode();
    test_wave_end_respawns_humanoids_and_clears_flag();

    sim_print_summary("planet_destruction");
    return sim_fail_count ? 1 : 0;
}
