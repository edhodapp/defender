// Layer tests for Phase 4: wave structure.
//
// Contract:
//   - At boot: wave_number=1, wave_to_spawn=2, enemies_alive=1 (the
//     pre-placed Lander at world_x=100 counts toward wave 1's 3 total).
//   - try_spawn_lander only fires when wave_to_spawn > 0; on success
//     decrements wave_to_spawn and increments enemies_alive.
//   - Killing a Lander or Mutant decrements enemies_alive.
//   - Lander → Mutant transformation does NOT change enemies_alive
//     (one enemy replaced by another).
//   - Shooting a carrying Lander's TOP half kills the lander (decrement)
//     and frees a falling humanoid; LOWER half just clears the carry
//     bit (lander stays alive, no decrement).
//   - When wave_to_spawn == 0 AND enemies_alive == 0, the next frame's
//     wave-end check fires SFX_WAVE_CHANGE, increments wave_number
//     (capped at 6), and reloads wave_to_spawn = min(wave_number+2, 8).

#include <stdio.h>
#include <stdlib.h>

#include "sim_helpers.h"

#define TYPE_LANDER    1
#define TYPE_MUTANT    2
#define TYPE_HUMANOID  7
#define SFX_WAVE       6

static sim_t *S;

static int wave_number(void) { return sim_mem_r(S, S->sym_wave_number); }
static int wave_to_spawn(void) { return sim_mem_r(S, S->sym_wave_to_spawn); }
static int enemies_alive(void) { return sim_mem_r(S, S->sym_enemies_alive); }

static void place_lander(int slot, int world_x, int y) {
    sim_mem_w(S, S->sym_entities + slot*3 + 0, (1<<7) | (TYPE_LANDER<<4));
    sim_mem_w(S, S->sym_entities + slot*3 + 1, world_x);
    sim_mem_w(S, S->sym_entities + slot*3 + 2, y);
}

static void place_mutant(int slot, int world_x, int y) {
    sim_mem_w(S, S->sym_entities + slot*3 + 0, (1<<7) | (TYPE_MUTANT<<4));
    sim_mem_w(S, S->sym_entities + slot*3 + 1, world_x);
    sim_mem_w(S, S->sym_entities + slot*3 + 2, y);
}

static void place_lander_carrying(int slot, int world_x, int y) {
    sim_mem_w(S, S->sym_entities + slot*3 + 0,
              (1<<7) | (TYPE_LANDER<<4) | 0x01);
    sim_mem_w(S, S->sym_entities + slot*3 + 1, world_x);
    sim_mem_w(S, S->sym_entities + slot*3 + 2, y);
}

static void place_humanoid(int slot, int world_x, int y) {
    sim_mem_w(S, S->sym_entities + slot*3 + 0, (1<<7) | (TYPE_HUMANOID<<4));
    sim_mem_w(S, S->sym_entities + slot*3 + 1, world_x);
    sim_mem_w(S, S->sym_entities + slot*3 + 2, y);
}

// ---- 1. Initial wave state ----
static void test_initial_state(void) {
    sim_sync(S);
    SIM_CHECK_MSG(wave_number() == 1, "wave_number=%d (want 1)", wave_number());
    SIM_CHECK_MSG(wave_to_spawn() == 2, "wave_to_spawn=%d (want 2)", wave_to_spawn());
    SIM_CHECK_MSG(enemies_alive() == 1, "enemies_alive=%d (want 1)", enemies_alive());
}

// ---- 2. Periodic spawn decrements wave_to_spawn, increments enemies_alive ----
static void test_periodic_spawn_updates_counters(void) {
    sim_sync(S);
    sim_mem_w(S, S->sym_wave_to_spawn, 3);
    sim_mem_w(S, S->sym_enemies_alive, 1);
    sim_mem_w(S, S->sym_frame_counter, 127);
    sim_run_frame(S);                            // fc 127→128 → spawn fires
    SIM_CHECK_MSG(wave_to_spawn() == 2,
                  "wave_to_spawn after spawn=%d (want 2)", wave_to_spawn());
    SIM_CHECK_MSG(enemies_alive() == 2,
                  "enemies_alive after spawn=%d (want 2)", enemies_alive());
}

// ---- 3. With wave_to_spawn=0 and an enemy still alive, periodic spawn
//        doesn't fire (and wave-end doesn't trigger either). ----
static void test_no_spawn_when_wave_done_spawning(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_mem_w(S, S->sym_wave_to_spawn, 0);
    sim_mem_w(S, S->sym_enemies_alive, 1);     // keep wave open
    sim_mem_w(S, S->sym_frame_counter, 127);
    sim_run_frame(S);
    SIM_CHECK_MSG(!sim_lander_active(S, 0),
                  "no Lander should have spawned with wave_to_spawn=0");
    SIM_CHECK_MSG(wave_to_spawn() == 0, "wave_to_spawn unchanged; got %d",
                  wave_to_spawn());
}

// ---- 4. Killing a Lander decrements enemies_alive ----
static void test_lander_kill_decrements_count(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_mem_w(S, S->sym_enemies_alive, 5);
    place_lander(0, 80, 28);
    sim_set_ship(S, 24, 0, 0);
    sim_place_beam(S, 0, 76, 28, 0);
    sim_run_frame(S);
    SIM_CHECK_MSG(!sim_lander_active(S, 0), "Lander should be dead");
    SIM_CHECK_MSG(enemies_alive() == 4,
                  "enemies_alive after kill=%d (want 4)", enemies_alive());
}

// ---- 5. Killing a Mutant decrements enemies_alive ----
static void test_mutant_kill_decrements_count(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_mem_w(S, S->sym_enemies_alive, 3);
    place_mutant(0, 80, 28);
    sim_set_ship(S, 24, 0, 0);
    sim_place_beam(S, 0, 76, 28, 0);
    sim_run_frame(S);
    SIM_CHECK_MSG(!sim_lander_active(S, 0), "Mutant should be dead");
    SIM_CHECK_MSG(enemies_alive() == 2,
                  "enemies_alive after Mutant kill=%d (want 2)", enemies_alive());
}

// ---- 6. Lander → Mutant transformation keeps enemies_alive unchanged ----
static void test_lander_to_mutant_count_unchanged(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_mem_w(S, S->sym_enemies_alive, 1);
    place_lander_carrying(0, 80, 9);             // poised to transform
    int initial = enemies_alive();
    for (int i = 0; i < 4; i++) sim_run_frame(S);// drift fires → transform
    int b0 = sim_mem_r(S, S->sym_entities + 0);
    int type = (b0 >> 4) & 0x07;
    SIM_CHECK_MSG(type == TYPE_MUTANT, "expected Mutant after transform");
    SIM_CHECK_MSG(enemies_alive() == initial,
                  "enemies_alive should be unchanged across transform; was %d, now %d",
                  initial, enemies_alive());
}

// ---- 7. Shooting carrying Lander's TOP half decrements (lander died) ----
static void test_shoot_carrier_top_decrements(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_mem_w(S, S->sym_enemies_alive, 2);
    place_lander_carrying(0, 80, 20);
    sim_place_beam(S, 0, 76, 24, 0);             // top half (y in [20,27])
    sim_run_frame(S);
    SIM_CHECK_MSG(enemies_alive() == 1,
                  "carrying-lander top hit should decrement; got %d",
                  enemies_alive());
}

// ---- 8. Shooting carrying Lander's BOTTOM half does NOT decrement ----
static void test_shoot_carrier_bottom_keeps_count(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_mem_w(S, S->sym_enemies_alive, 2);
    place_lander_carrying(0, 80, 20);
    sim_place_beam(S, 0, 76, 30, 0);             // bottom half (y in [28,35])
    sim_run_frame(S);
    SIM_CHECK_MSG(enemies_alive() == 2,
                  "carrying-lander bottom hit should NOT decrement; got %d",
                  enemies_alive());
}

// ---- 9. Killing a humanoid does NOT decrement enemies_alive ----
static void test_humanoid_kill_no_decrement(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_mem_w(S, S->sym_enemies_alive, 4);
    place_humanoid(1, 80, 48);
    sim_set_ship(S, 44, 0, 0);
    sim_place_beam(S, 0, 76, 48, 0);
    sim_run_frame(S);
    SIM_CHECK_MSG(enemies_alive() == 4,
                  "humanoid kill should not affect enemies_alive; got %d",
                  enemies_alive());
}

// ---- 10. Wave end: when both counters are 0, advance wave + queue SFX ----
static void test_wave_end_advances_and_plays_sfx(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_mem_w(S, S->sym_wave_number, 1);
    sim_mem_w(S, S->sym_wave_to_spawn, 0);
    sim_mem_w(S, S->sym_enemies_alive, 0);
    sim_run_frame(S);                            // wave-end check fires
    SIM_CHECK_MSG(wave_number() == 2,
                  "wave_number should advance to 2; got %d", wave_number());
    // wave_to_spawn = (wave_number+2) - 1 because wave-end also kicks an
    // immediate try_spawn_lander to avoid a multi-second silent gap.
    SIM_CHECK_MSG(wave_to_spawn() == 3,
                  "next-wave size after immediate spawn = 3; got %d", wave_to_spawn());
    SIM_CHECK_MSG(enemies_alive() == 1,
                  "immediate spawn should bring enemies_alive to 1; got %d",
                  enemies_alive());
    SIM_CHECK_MSG(sim_mem_r(S, S->sym_sound_id) == SFX_WAVE,
                  "SFX_WAVE_CHANGE should be queued; sound_id=%d",
                  sim_mem_r(S, S->sym_sound_id));
}

// ---- 11. Wave-end caps wave_number at 6 and wave_to_spawn at 8 ----
static void test_wave_end_caps(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_mem_w(S, S->sym_wave_number, 6);          // already at cap
    sim_mem_w(S, S->sym_wave_to_spawn, 0);
    sim_mem_w(S, S->sym_enemies_alive, 0);
    sim_run_frame(S);
    SIM_CHECK_MSG(wave_number() == 6,
                  "wave_number should stay at cap 6; got %d", wave_number());
    SIM_CHECK_MSG(wave_to_spawn() == 7,
                  "wave_to_spawn after cap+immediate-spawn = 8-1 = 7; got %d",
                  wave_to_spawn());
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <defender.elf>\n", argv[0]); return 2; }
    S = sim_boot(argv[1]);

    test_initial_state();
    test_periodic_spawn_updates_counters();
    test_no_spawn_when_wave_done_spawning();
    test_lander_kill_decrements_count();
    test_mutant_kill_decrements_count();
    test_lander_to_mutant_count_unchanged();
    test_shoot_carrier_top_decrements();
    test_shoot_carrier_bottom_keeps_count();
    test_humanoid_kill_no_decrement();
    test_wave_end_advances_and_plays_sfx();
    test_wave_end_caps();

    sim_coverage_save_for(argv[0]);
    sim_print_summary("waves");
    if (sim_fail_count) { printf("FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
