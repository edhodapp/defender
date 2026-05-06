// Asserts the firmware paces itself at 60 Hz at 16 MHz CPU clock.
// 16,000,000 / 60 = 266,667 cycles per main_loop iteration.
//
// This test fails against the original busy-wait throttle (which delivers
// ~24 Hz / ~660,000 cycles per frame) and passes once main_loop sleeps on
// a 60 Hz Timer1 Compare A interrupt.

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#include "sim_helpers.h"

#define F_CPU       16000000ULL
#define TARGET_HZ   60ULL
#define TARGET_CYC  (F_CPU / TARGET_HZ)              // 266,666
#define TOL_CYC     (TARGET_CYC / 50)                // ±2% = ±5,333 cycles

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <defender.elf>\n", argv[0]); return 2; }
    sim_t *S = sim_boot(argv[1]);

    sim_sync(S);

    // Warm up: let the timer steady-state, OLED init delays settle.
    for (int i = 0; i < 5; i++) sim_run_frame(S);

    uint64_t prev = S->avr->cycle;
    int n = 8;
    for (int i = 0; i < n; i++) {
        sim_run_frame(S);
        uint64_t cur = S->avr->cycle;
        int64_t delta = (int64_t)(cur - prev);
        int64_t lo = (int64_t)(TARGET_CYC - TOL_CYC);
        int64_t hi = (int64_t)(TARGET_CYC + TOL_CYC);
        SIM_CHECK_MSG(delta >= lo && delta <= hi,
                      "frame %d cycles=%" PRId64 " (want %" PRIu64 " ±%" PRIu64 ")",
                      i, delta, (uint64_t)TARGET_CYC, (uint64_t)TOL_CYC);
        prev = cur;
    }

    sim_print_summary("frame_rate");
    if (sim_fail_count) { printf("FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
