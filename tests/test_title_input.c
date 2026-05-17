// Pins R1.2-4 / R16.5 — title-screen input flow:
//
//   R1.2  Cursor UP/DOWN edges move title_selection.
//   R1.3  B-edge on a difficulty row sets `difficulty` and exits.
//   R1.4  Holding B on the RESET row for 60 frames wipes the
//         high-score table.
//   R16.5 Same RESET-row wipe (preserves the magic byte at EEPROM[0],
//         zeroes bytes 1..9 AND the high_scores BSS table).
//
// The tests boot into title_loop directly — no skip_title_flag set —
// by using sim_boot_no_warmup, then advancing cycles through _reset
// and into title_loop's sleep wait. sim_run_to_next_iter wouldn't
// work because title_loop never reaches the main_loop entry point;
// we use sim_run_cycles throughout.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_avr.h"
#include "avr_eeprom.h"
#include "sim_helpers.h"

static sim_t *S;
static uint16_t sym_title_selection;
static uint16_t sym_reset_hold_frames;
static uint16_t sym_high_scores;

// Roughly one title_loop iteration in cycles. title_loop sleeps until
// the Timer1 frame ISR (every ~16.67 ms = 267 K cycles at 16 MHz),
// plus render takes ~30 K cycles. 400 K is comfortably one iter; 1 M
// is ~3 iters which is enough margin for input-edge detection.
#define TITLE_ITER_CYCLES   400000ULL
#define TITLE_ITERS(n)      ((n) * TITLE_ITER_CYCLES)

// ---- EEPROM helpers (same pattern as test_eeprom.c) -------------

static void eeprom_read(uint16_t offset, uint8_t *buf, uint32_t n) {
    avr_eeprom_desc_t desc = { .ee = buf, .offset = offset, .size = n };
    avr_ioctl(S->avr, AVR_IOCTL_EEPROM_GET, &desc);
}
static void eeprom_write(uint16_t offset, const uint8_t *buf, uint32_t n) {
    avr_eeprom_desc_t desc = { .ee = (uint8_t *)buf, .offset = offset, .size = n };
    avr_ioctl(S->avr, AVR_IOCTL_EEPROM_SET, &desc);
}

// Boot into title_loop. avr_reset wipes BSS so skip_title_flag stays 0,
// and the firmware's splash gate falls through to rcall title_loop
// (PINB bit 4 = 1 when B is not held → "B not held → show title" path).
static void boot_into_title(void) {
    avr_reset(S->avr);
    sim_run_cycles(S, 2500000);   // ~6 title_loop iters → cursor settled
}

// ---- R1.2: cursor UP / DOWN move title_selection ----------------

static void test_cursor_up_moves_selection(void) {
    boot_into_title();
    sim_mem_w(S, sym_title_selection, 2);     // HIGH row

    sim_btn_press(S->btn_up);
    sim_run_cycles(S, TITLE_ITERS(2));        // enough for one edge + settle
    sim_btn_release(S->btn_up);
    sim_run_cycles(S, TITLE_ITERS(1));

    SIM_CHECK_MSG(sim_mem_r(S, sym_title_selection) == 1,
                  "R1.2: UP edge should move selection 2 → 1; got %d",
                  sim_mem_r(S, sym_title_selection));
}

static void test_cursor_down_moves_selection(void) {
    boot_into_title();
    sim_mem_w(S, sym_title_selection, 1);     // MED row

    sim_btn_press(S->btn_down);
    sim_run_cycles(S, TITLE_ITERS(2));
    sim_btn_release(S->btn_down);
    sim_run_cycles(S, TITLE_ITERS(1));

    SIM_CHECK_MSG(sim_mem_r(S, sym_title_selection) == 2,
                  "R1.2: DOWN edge should move selection 1 → 2; got %d",
                  sim_mem_r(S, sym_title_selection));
}

// Boundaries: UP at the top must clamp at 0; DOWN past 3 (RESET row)
// must clamp at 3.
static void test_cursor_clamped_at_top(void) {
    boot_into_title();
    sim_mem_w(S, sym_title_selection, 0);
    sim_btn_press(S->btn_up);
    sim_run_cycles(S, TITLE_ITERS(2));
    sim_btn_release(S->btn_up);
    SIM_CHECK_MSG(sim_mem_r(S, sym_title_selection) == 0,
                  "R1.2: UP at top must clamp; got %d",
                  sim_mem_r(S, sym_title_selection));
}

static void test_cursor_clamped_at_bottom(void) {
    boot_into_title();
    sim_mem_w(S, sym_title_selection, 3);
    sim_btn_press(S->btn_down);
    sim_run_cycles(S, TITLE_ITERS(2));
    sim_btn_release(S->btn_down);
    SIM_CHECK_MSG(sim_mem_r(S, sym_title_selection) == 3,
                  "R1.2: DOWN at bottom (RESET row) must clamp; got %d",
                  sim_mem_r(S, sym_title_selection));
}

// ---- R1.3: B-edge on a difficulty row sets `difficulty` ---------

static void check_b_confirm(int row, int expected_diff) {
    boot_into_title();
    sim_mem_w(S, sym_title_selection, (uint8_t)row);

    sim_btn_press(S->btn_b);
    sim_run_cycles(S, TITLE_ITERS(2));
    sim_btn_release(S->btn_b);
    // After B-edge on a difficulty row, title_loop returns; main_loop
    // begins (or is about to). difficulty should mirror title_selection.
    sim_run_cycles(S, TITLE_ITERS(2));

    SIM_CHECK_MSG(sim_mem_r(S, S->sym_difficulty) == expected_diff,
                  "R1.3: B-edge on row %d sets difficulty=%d (want %d)",
                  row, sim_mem_r(S, S->sym_difficulty), expected_diff);
}

static void test_b_confirm_low(void)  { check_b_confirm(0, 0); }
static void test_b_confirm_med(void)  { check_b_confirm(1, 1); }
static void test_b_confirm_high(void) { check_b_confirm(2, 2); }

// ---- R1.4 / R16.5: hold-B on RESET row wipes high scores --------

static void test_reset_row_b_hold_wipes_scores(void) {
    boot_into_title();
    // Pre-load EEPROM with non-zero scores so we can detect the wipe.
    uint8_t pre[10] = { 0xDF, 0x11, 0x22, 0x33,
                              0x44, 0x55, 0x66,
                              0x77, 0x88, 0x99 };
    eeprom_write(0, pre, 10);
    // Mirror the values in BSS high_scores so we can verify the BSS
    // copy is also zeroed (eeprom_wipe_high_scores writes both).
    for (int i = 0; i < 9; i++) {
        sim_mem_w(S, sym_high_scores + i, pre[i + 1]);
    }

    // Park cursor on RESET row, pre-load the hold counter just below
    // the trigger threshold so a single iteration with B held tips it
    // over. Without this trick the test would need 60 simulated frames
    // (~24 M cycles) — too slow.
    sim_mem_w(S, sym_title_selection, 3);
    sim_mem_w(S, sym_reset_hold_frames, 59);

    sim_btn_press(S->btn_b);
    sim_run_cycles(S, TITLE_ITERS(2));        // one iter ticks counter to 60
                                              // → wipe fires
    // Give the EEPROM write cycle 9 × 3.4 ms = ~540 K cycles to drain.
    sim_run_cycles(S, 1500000);
    sim_btn_release(S->btn_b);

    uint8_t after[10];
    eeprom_read(0, after, 10);
    SIM_CHECK_MSG(after[0] == 0xDF,
                  "R16.5: magic byte preserved across wipe; got 0x%02X",
                  after[0]);
    for (int i = 1; i <= 9; i++) {
        SIM_CHECK_MSG(after[i] == 0,
                      "R16.5: EEPROM byte %d should be zeroed; got 0x%02X",
                      i, after[i]);
    }
    for (int i = 0; i < 9; i++) {
        SIM_CHECK_MSG(sim_mem_r(S, sym_high_scores + i) == 0,
                      "R1.4: high_scores[%d] in BSS should be zeroed; got 0x%02X",
                      i, sim_mem_r(S, sym_high_scores + i));
    }
}

// ---- driver -----------------------------------------------------

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <defender.elf>\n", argv[0]); return 2; }
    S = sim_boot_no_warmup(argv[1]);

    sym_title_selection   = sim_lookup(S, "title_selection");
    sym_reset_hold_frames = sim_lookup(S, "reset_hold_frames");
    sym_high_scores       = sim_lookup(S, "high_scores");

    test_cursor_up_moves_selection();
    test_cursor_down_moves_selection();
    test_cursor_clamped_at_top();
    test_cursor_clamped_at_bottom();
    test_b_confirm_low();
    test_b_confirm_med();
    test_b_confirm_high();
    test_reset_row_b_hold_wipes_scores();

    sim_print_summary("title_input");
    return sim_fail_count ? 1 : 0;
}
