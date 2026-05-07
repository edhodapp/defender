// Functional tests for the periodic Lander-spawn layer (try_spawn_lander).
// Layer contract:
//   - Triggered by main_loop when (frame_counter & 0x1F) == 0 AND fc != 0.
//   - Picks the first inactive entity slot scanning slots 0..63.
//   - Writes Lander at world_x = (spawn_pos_idx * 32 + 16) mod 256, y = 10.
//   - Increments spawn_pos_idx after a successful spawn.
//   - When all 8 slots are full → no spawn, spawn_pos_idx unchanged.
//   - Crucially: a new spawn must NOT place a Lander at the same world_x
//     as any currently-active Lander (overlap bug).

#include <stdio.h>
#include <stdlib.h>

#include "sim_helpers.h"

static sim_t *S;

static void scenario(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, 28, 0, 0);
}

static int count_active_landers(void) {
    int n = 0;
    for (int i = 0; i < 8; i++) if (sim_lander_active(S, i)) n++;
    return n;
}

// Return the first slot containing a lander at the given world_x, or -1.
static int find_lander_at(int world_x, int skip_slot) {
    for (int i = 0; i < 8; i++) {
        if (i == skip_slot) continue;
        if (sim_lander_active(S, i) && sim_lander_world_x(S, i) == world_x) return i;
    }
    return -1;
}

// ---- 1. Spawn fires when frame_counter advances to 128 ----
static void test_spawn_fires_at_fc128(void) {
    scenario();
    sim_mem_w(S, S->sym_frame_counter, 127);                // next iter → fc=128
    sim_run_frame(S);
    SIM_CHECK_MSG(sim_lander_active(S, 0), "no lander spawned at fc=128");
    SIM_CHECK_MSG(sim_lander_world_x(S, 0) == 16,
                  "expected world_x=16; got %d", sim_lander_world_x(S, 0));
}

// ---- 2. spawn_pos_idx starts at 0 and increments after each spawn ----
static void test_idx_increments(void) {
    scenario();
    SIM_CHECK_MSG(sim_mem_r(S, S->sym_spawn_pos_idx) == 0, "initial idx=%d",
                  sim_mem_r(S, S->sym_spawn_pos_idx));
    sim_mem_w(S, S->sym_frame_counter, 127);
    sim_run_frame(S);
    SIM_CHECK_MSG(sim_mem_r(S, S->sym_spawn_pos_idx) == 1, "after 1 spawn idx=%d",
                  sim_mem_r(S, S->sym_spawn_pos_idx));
}

// ---- 3. Two periodic spawns land at world_x=16 and 48 in distinct slots ----
static void test_two_spawns_distinct_positions(void) {
    scenario();
    sim_mem_w(S, S->sym_frame_counter, 127);
    sim_run_frame(S);                                        // first spawn
    sim_mem_w(S, S->sym_frame_counter, 127);
    sim_run_frame(S);                                        // second spawn
    SIM_CHECK_MSG(count_active_landers() == 2, "got %d landers", count_active_landers());
    SIM_CHECK_MSG(sim_lander_world_x(S, 0) == 16, "slot 0 wx=%d", sim_lander_world_x(S, 0));
    SIM_CHECK_MSG(sim_lander_world_x(S, 1) == 48, "slot 1 wx=%d", sim_lander_world_x(S, 1));
}

// ---- 4. When ALL 64 entity slots are full, periodic spawn must not fire
//        and spawn_pos_idx must not advance ----
static void test_full_table_no_spawn(void) {
    scenario();
    for (int i = 0; i < 64; i++) sim_place_lander(S, i, 100, 10);
    sim_mem_w(S, S->sym_spawn_pos_idx, 5);
    sim_mem_w(S, S->sym_frame_counter, 127);
    sim_run_frame(S);
    SIM_CHECK_MSG(sim_mem_r(S, S->sym_spawn_pos_idx) == 5,
                  "idx changed to %d", sim_mem_r(S, S->sym_spawn_pos_idx));
}

// ---- 5. Spawn picks the FIRST inactive slot scanning from 0 ----
static void test_picks_first_inactive(void) {
    scenario();
    sim_place_lander(S, 0, 100, 10);
    sim_mem_w(S, S->sym_spawn_pos_idx, 0);
    sim_mem_w(S, S->sym_frame_counter, 127);
    sim_run_frame(S);
    SIM_CHECK(sim_lander_active(S, 1));
    SIM_CHECK_MSG(sim_lander_world_x(S, 1) == 16, "got %d", sim_lander_world_x(S, 1));
    SIM_CHECK(sim_lander_active(S, 0));
    SIM_CHECK(sim_lander_world_x(S, 0) == 100);
}

// ---- 6. world_x formula wraps with idx via &0xFF. idx=8 → world_x = 16 ----
static void test_idx_wraps_world_x(void) {
    scenario();
    sim_place_lander(S, 1, 200, 10);
    sim_mem_w(S, S->sym_spawn_pos_idx, 8);
    sim_mem_w(S, S->sym_frame_counter, 127);
    sim_run_frame(S);
    SIM_CHECK(sim_lander_active(S, 0));
    SIM_CHECK_MSG(sim_lander_world_x(S, 0) == 16,
                  "expected 16 (idx=8); got %d", sim_lander_world_x(S, 0));
}

// ---- 7. CHARACTERIZATION: when spawn_pos_idx wraps onto a world_x that
//        already has a Lander, the periodic spawner WILL place a second
//        Lander on top of it. This is an accepted arcade quirk per design
//        decision — original Williams Defender did the same. The test pins
//        the behavior so future changes that "fix" it surface here. ----
static void test_overlap_is_accepted_arcade_quirk(void) {
    scenario();
    sim_place_lander(S, 1, 16, 10);
    sim_place_lander(S, 2, 48, 10);
    sim_place_lander(S, 3, 80, 10);
    sim_place_lander(S, 4, 112, 10);
    sim_place_lander(S, 5, 144, 10);
    sim_place_lander(S, 6, 176, 10);
    sim_place_lander(S, 7, 208, 10);
    sim_mem_w(S, S->sym_spawn_pos_idx, 8);                  // overlap-inducing idx
    sim_mem_w(S, S->sym_frame_counter, 127);
    sim_run_frame(S);
    SIM_CHECK_MSG(sim_lander_active(S, 0), "expected new spawn in slot 0");
    int new_x = sim_lander_world_x(S, 0);
    int dup_slot = find_lander_at(new_x, 0);
    SIM_CHECK_MSG(dup_slot != -1,
                  "expected overlap (slot %d at same world_x=%d as slot 0); "
                  "if this fires, the spawner gained collision avoidance",
                  dup_slot, new_x);
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <defender.elf>\n", argv[0]); return 2; }
    S = sim_boot(argv[1]);

    test_spawn_fires_at_fc128();
    test_idx_increments();
    test_two_spawns_distinct_positions();
    test_full_table_no_spawn();
    test_picks_first_inactive();
    test_idx_wraps_world_x();
    test_overlap_is_accepted_arcade_quirk();

    sim_coverage_save_for(argv[0]);
    sim_print_summary("lander_spawn");
    if (sim_fail_count) { printf("FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
