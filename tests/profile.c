// Cycle profiler. Runs a representative gameplay workload and writes a
// per-PC cycle table that profile_report.py aggregates by function.
//
// Workload: ship in middle, 8 active landers across the world, B held to
// keep beam slots full. After 5 frames of warmup (so the entity drift
// pipeline reaches steady state), counters reset and we run 30 frames.

#include <stdio.h>
#include <stdlib.h>

#include "sim_helpers.h"

#define PROFILE_FRAMES 30
#define WARMUP_FRAMES   5

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <defender.elf>\n", argv[0]); return 2; }
    sim_t *S = sim_boot(argv[1]);

    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, 28, 0, 0);

    // 8 active landers spread across the world (some on-screen, some off).
    sim_place_lander(S, 0,  20, 10);
    sim_place_lander(S, 1,  50, 20);
    sim_place_lander(S, 2,  80, 30);
    sim_place_lander(S, 3, 110, 40);
    sim_place_lander(S, 4, 140, 15);
    sim_place_lander(S, 5, 170, 25);
    sim_place_lander(S, 6, 200, 35);
    sim_place_lander(S, 7, 230, 12);

    // 4 beams in flight at varying positions.
    sim_place_beam(S, 0,  80, 32, 0);
    sim_place_beam(S, 1,  96, 32, 0);
    sim_place_beam(S, 2, 110, 28, 0);
    sim_place_beam(S, 3,  72, 30, 0);

    // Hold B to keep refilling beam slots as they expire off-screen.
    sim_btn_press(S->btn_b);

    // Warmup so the pipeline (drift counter, beam decay, dispatch tables)
    // is in steady state before we start measuring.
    for (int i = 0; i < WARMUP_FRAMES; i++) sim_run_frame(S);

    sim_profile_reset();
    uint64_t cycles_start = S->avr->cycle;

    for (int i = 0; i < PROFILE_FRAMES; i++) sim_run_frame(S);

    uint64_t cycles_end  = S->avr->cycle;
    uint64_t total       = cycles_end - cycles_start;
    double   per_frame   = (double)total / PROFILE_FRAMES;
    double   hz          = 16000000.0 * PROFILE_FRAMES / (double)total;

    printf("profile: %d frames in %llu cycles\n",
           PROFILE_FRAMES, (unsigned long long)total);
    printf("         %.0f cycles/frame  ~%.2f Hz at 16 MHz\n",
           per_frame, hz);

    sim_profile_save_for(argv[0]);
    return 0;
}
