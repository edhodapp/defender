// Pins R5.10 (Lander beam kill = +100 pts), R6.4 (Mutant beam kill =
// +100 pts), R10.5 (humanoid kill = 0 pts), plus a few small
// audio/spawn requirements:
//   R12.3 — first spawn fires after one full interval at game start.
//   R12.8 — wave_pods_to_spawn loaded from the per-(diff, wave) table
//           on wave-end re-derive.
//   R15.5 — SFX_START plays on splash dismiss (game start).
//   R15.6 — SFX_WAVE_CHANGE plays at wave-end transition.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_helpers.h"

#define TYPE_LANDER    1
#define TYPE_MUTANT    2
#define TYPE_HUMANOID  7

#define SFX_DEATH      3
#define SFX_START      5
#define SFX_WAVE       6

static sim_t *S;
static uint16_t sym_score_lo, sym_score_mid, sym_score_hi;
static uint16_t sym_sound_id;

static uint32_t score(void) {
    return (uint32_t)sim_mem_r(S, sym_score_lo)
         | ((uint32_t)sim_mem_r(S, sym_score_mid) << 8)
         | ((uint32_t)sim_mem_r(S, sym_score_hi)  << 16);
}

static void place(int slot, uint8_t b0, int world_x, int y) {
    sim_mem_w(S, S->sym_entities + slot * 3 + 0, b0);
    sim_mem_w(S, S->sym_entities + slot * 3 + 1, (uint8_t)world_x);
    sim_mem_w(S, S->sym_entities + slot * 3 + 2, (uint8_t)y);
}

static void scenario(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, 28, 0, 0);
    sim_mem_w(S, S->sym_death_flash, 0);
    sim_mem_w(S, S->sym_respawn_invuln, 0);
    sim_mem_w(S, sym_score_lo,  0);
    sim_mem_w(S, sym_score_mid, 0);
    sim_mem_w(S, sym_score_hi,  0);
    sim_mem_w(S, sym_sound_id, 0);
}

// ---- R5.10: Lander beam kill awards 100 pts ----------------------

static void test_lander_beam_kill_awards_100(void) {
    scenario();
    place(0, (1 << 7) | (TYPE_LANDER << 4), 80, 28);
    sim_place_beam(S, 0, 76, 32, 0);
    for (int i = 0; i < 8; i++) sim_run_frame(S);
    SIM_CHECK_MSG(score() == 100,
                  "R5.10: Lander beam kill should award +100 pts; got %u",
                  score());
}

// ---- R6.4: Mutant beam kill awards 100 pts -----------------------

static void test_mutant_beam_kill_awards_100(void) {
    scenario();
    place(0, (1 << 7) | (TYPE_MUTANT << 4), 80, 28);
    sim_place_beam(S, 0, 76, 32, 0);
    for (int i = 0; i < 8; i++) sim_run_frame(S);
    SIM_CHECK_MSG(score() == 100,
                  "R6.4: Mutant beam kill should award +100 pts; got %u",
                  score());
}

// ---- R10.5: Humanoid kill awards 0 pts ---------------------------

static void test_humanoid_kill_awards_zero(void) {
    scenario();
    // Park a grounded humanoid at world_x=80, y=48; shoot it.
    place(0, (1 << 7) | (TYPE_HUMANOID << 4), 80, 48);
    sim_place_beam(S, 0, 76, 50, 0);
    for (int i = 0; i < 8; i++) sim_run_frame(S);
    SIM_CHECK_MSG(score() == 0,
                  "R10.5: shooting a humanoid awards 0 pts; got %u",
                  score());
}

// ---- R15.5: SFX_START sets sound_id at game start ----------------

// sim_boot runs through skip_title_screen which fires
// audio_play(SFX_START). After warmup (2 frames), the start
// arpeggio is still playing (it's 30 frames long), so sound_id
// should still be SFX_START.
static void test_sfx_start_plays_at_game_start(void) {
    sim_sync(S);
    // Don't sim_clear_state_minimal — that resets sound state.
    int id = sim_mem_r(S, sym_sound_id);
    SIM_CHECK_MSG(id == SFX_START,
                  "R15.5: SFX_START should be in progress shortly after "
                  "game start; sound_id=%d", id);
}

// ---- R15.6: SFX_WAVE_CHANGE plays on wave-end --------------------

// Force the wave-end path (wave_to_spawn=0 + enemies_alive=0). The
// main_alive wave-end block calls audio_play(SFX_WAVE).
static void test_sfx_wave_change_on_wave_end(void) {
    scenario();
    sim_mem_w(S, S->sym_wave_to_spawn, 0);
    sim_mem_w(S, S->sym_enemies_alive, 0);
    sim_run_frame(S);
    int id = sim_mem_r(S, sym_sound_id);
    SIM_CHECK_MSG(id == SFX_WAVE,
                  "R15.6: SFX_WAVE_CHANGE should fire on wave-end; "
                  "sound_id=%d", id);
}

// ---- R12.3: First spawn fires after one full interval -----------

// Default boot path: difficulty=MED → spawn_countdown seeded to 64
// in skip_title_screen. sim_boot's warmup ticks N main_alive iters
// (count depends on how many frames the simulated 60-Hz Timer1
// fires inside 1.4M cycles — empirically ~7). So we just assert
// that the countdown decremented from 64 but hasn't yet fired a
// spawn — pinning the contract "first spawn waits a full interval"
// without nailing the exact count.
static void test_first_spawn_interval_at_boot(void) {
    sim_sync(S);
    int cd = sim_mem_r(S, S->sym_spawn_countdown);
    SIM_CHECK_MSG(cd > 0 && cd < 64,
                  "R12.3: spawn_countdown should have decremented from 64 "
                  "but not yet fired a spawn; got %d", cd);
    // No new Landers should have spawned (only the boot Lander at slot 0).
    int landers = 0;
    for (int i = 0; i < 64; i++) {
        uint8_t b0 = sim_mem_r(S, S->sym_entities + i * 3);
        if ((b0 & 0x80) && ((b0 >> 4) & 7) == TYPE_LANDER) landers++;
    }
    SIM_CHECK_MSG(landers == 1,
                  "R12.3: only the boot Lander should exist post-boot; got %d",
                  landers);
}

// ---- R12.8: Pod allotment from wave_pod_count_table -------------

// Force a wave-end at each (difficulty, wave) cell and verify the
// reload value matches the table. Spot-check one cell per difficulty.
static void check_pod_allotment(int diff, int wave, int expected) {
    scenario();
    sim_mem_w(S, S->sym_difficulty, (uint8_t)diff);
    sim_mem_w(S, S->sym_wave_number, (uint8_t)wave);
    sim_mem_w(S, S->sym_wave_to_spawn, 0);
    sim_mem_w(S, S->sym_enemies_alive, 0);
    sim_run_frame(S);
    int got = sim_mem_r(S, S->sym_wave_pods_to_spawn);
    SIM_CHECK_MSG(got == expected,
                  "R12.8: diff=%d wave=%d → pods=%d (want %d)",
                  diff, wave, got, expected);
}

// check_pod_allotment: forces a wave-end with all entity slots full
// so the kick spawn at the end of wave_at_max finds no free slot and
// bails out without decrementing wave_pods_to_spawn. That lets us
// observe the table lookup result directly.
//
// Table (per wave_pod_count_table in defender.S):
//   LOW : 0 0 0 1 1 1 2 2
//   MED : 0 0 1 1 2 2 2 3
//   HIGH: 0 1 1 2 2 3 3 3
// Wave-end increments wave_number BEFORE the lookup, so setting
// wave_number=N produces the allotment for wave N+1.
static void check_pod_allotment_full_slots(int diff, int wave, int expected) {
    scenario();
    sim_mem_w(S, S->sym_difficulty, (uint8_t)diff);
    sim_mem_w(S, S->sym_wave_number, (uint8_t)wave);
    sim_mem_w(S, S->sym_wave_to_spawn, 0);
    sim_mem_w(S, S->sym_enemies_alive, 0);
    // Fill every slot with an active Lander so the kick spawn can't
    // find space and won't consume a Pod from the freshly-loaded
    // allotment.
    for (int i = 0; i < 64; i++) place(i, (1 << 7) | (TYPE_LANDER << 4), 100, 10);
    sim_run_frame(S);
    int got = sim_mem_r(S, S->sym_wave_pods_to_spawn);
    SIM_CHECK_MSG(got == expected,
                  "R12.8: diff=%d wave=%d → pods=%d (want %d)",
                  diff, wave, got, expected);
}

static void test_pod_allotment_low_wave3(void)  { check_pod_allotment_full_slots(0, 3, 1); } // wave 4 cell
static void test_pod_allotment_med_wave2(void)  { check_pod_allotment_full_slots(1, 2, 1); } // wave 3 cell
static void test_pod_allotment_high_wave1(void) { check_pod_allotment_full_slots(2, 1, 1); } // wave 2 cell

// ---- driver -----------------------------------------------------

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <defender.elf>\n", argv[0]); return 2; }
    S = sim_boot(argv[1]);

    sym_score_lo  = sim_lookup(S, "score_lo");
    sym_score_mid = sim_lookup(S, "score_mid");
    sym_score_hi  = sim_lookup(S, "score_hi");
    sym_sound_id  = sim_lookup(S, "sound_id");

    // R15.5 and R12.3 must run BEFORE anything that calls scenario()
    // (which resets sound_id, spawn_countdown, and runs frames).
    // Both check post-boot state directly.
    test_sfx_start_plays_at_game_start();
    test_first_spawn_interval_at_boot();

    test_lander_beam_kill_awards_100();
    test_mutant_beam_kill_awards_100();
    test_humanoid_kill_awards_zero();
    test_sfx_wave_change_on_wave_end();
    test_pod_allotment_low_wave3();
    test_pod_allotment_med_wave2();
    test_pod_allotment_high_wave1();

    sim_print_summary("score_events");
    return sim_fail_count ? 1 : 0;
}
