// Pins R16.1-4 + R4.9 (EEPROM high-score persistence). Reaches into
// simavr's EEPROM model via the avr_ioctl(AVR_IOCTL_EEPROM_GET/SET)
// API; the m32u4 core attaches the EEPROM block by default so reads
// and writes go through the firmware's normal EEAR/EEDR/EECR
// sequence and end up in avr->eeprom (~1 KB) under the hood.
//
// EEPROM layout (per defender.S):
//   byte 0     : magic 0xDF
//   bytes 1..3 : HI score LOW   (24-bit lo/mid/hi)
//   bytes 4..6 : HI score MED
//   bytes 7..9 : HI score HIGH
//
// Tests use avr_reset() to re-run _reset between scenarios; the
// simulator runs ~1.5M cycles after a reset to let eeprom_load_high_scores
// finish (10 EEPROM byte writes at ~3.4 ms each = ~540 K cycles on a
// virgin EEPROM, plus the rest of _reset / skip_title_screen).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_avr.h"
#include "sim_elf.h"
#include "avr_eeprom.h"
#include "sim_helpers.h"

#define TYPE_LANDER 1

static sim_t *S;

// ---- EEPROM helpers via simavr ioctl ----------------------------

static void eeprom_read(uint16_t offset, uint8_t *buf, uint32_t n) {
    avr_eeprom_desc_t desc = { .ee = buf, .offset = offset, .size = n };
    avr_ioctl(S->avr, AVR_IOCTL_EEPROM_GET, &desc);
}

static void eeprom_write(uint16_t offset, const uint8_t *buf, uint32_t n) {
    avr_eeprom_desc_t desc = { .ee = (uint8_t *)buf, .offset = offset, .size = n };
    avr_ioctl(S->avr, AVR_IOCTL_EEPROM_SET, &desc);
}

// avr_reset is destructive — wipes BSS and re-runs _reset. After a
// reset, advance just past stack init (500 cycles is plenty), then
// poke skip_title_flag=1 BEFORE the splash gate runs (it sits well
// after oled_init's ~5K-cycle I2C sequence). Then run enough cycles
// to let eeprom_load_high_scores finish — 10 EEPROM byte writes at
// ~3.4 ms each on a virgin EEPROM = ~540 K cycles — plus the rest
// of _reset / skip_title_screen.
static void reboot_to_main_loop(void) {
    avr_reset(S->avr);
    sim_run_cycles(S, 500);
    sim_mem_w(S, S->sym_skip_title_flag, 1);
    sim_run_cycles(S, 2000000);
}

// ---- R16.1: magic byte 0xDF written at byte 0 -------------------

static void test_magic_byte_written_at_boot(void) {
    // sim_boot already ran _reset on a virgin (0xFF) EEPROM; the
    // firmware should have laid down 0xDF + 9 zero bytes.
    uint8_t buf[10];
    eeprom_read(0, buf, 10);
    SIM_CHECK_MSG(buf[0] == 0xDF,
                  "R16.1: magic byte at EEPROM[0] = 0x%02X (want 0xDF)",
                  buf[0]);
    for (int i = 1; i < 10; i++) {
        SIM_CHECK_MSG(buf[i] == 0,
                      "R16.2: HI-score byte[%d] = 0x%02X (want 0x00 on virgin boot)",
                      i, buf[i]);
    }
}

// ---- R16.3: bad magic on cold boot → table reset to 0xDF + zeros

static void test_bad_magic_triggers_reset(void) {
    // Corrupt the magic + scores BEFORE re-running _reset.
    uint8_t bad[10] = { 0xAB, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99 };
    eeprom_write(0, bad, 10);
    reboot_to_main_loop();
    uint8_t buf[10];
    eeprom_read(0, buf, 10);
    SIM_CHECK_MSG(buf[0] == 0xDF,
                  "R16.3: magic restored after bad-magic reset; got 0x%02X",
                  buf[0]);
    for (int i = 1; i < 10; i++) {
        SIM_CHECK_MSG(buf[i] == 0,
                      "R16.3: HI-score byte[%d] zeroed; got 0x%02X",
                      i, buf[i]);
    }
}

// ---- R16.2: existing scores survive a normal boot ---------------

static void test_existing_scores_loaded_on_boot(void) {
    // Pre-load EEPROM with magic + 3 distinct 24-bit scores.
    uint8_t pre[10] = {
        0xDF,
        0x10, 0x27, 0x00,   // LOW  = 10000
        0xA0, 0x86, 0x01,   // MED  = 100000 → display will clamp to 99999
        0x40, 0x42, 0x0F,   // HIGH = 1000000
    };
    eeprom_write(0, pre, 10);
    reboot_to_main_loop();

    // After load, BSS high_scores should mirror the EEPROM bytes
    // 1..9 (low/mid/hi for each difficulty).
    uint16_t hs = sim_lookup(S, "high_scores");
    for (int i = 0; i < 9; i++) {
        uint8_t got = sim_mem_r(S, hs + i);
        SIM_CHECK_MSG(got == pre[i + 1],
                      "R16.2: high_scores[%d] = 0x%02X (want 0x%02X)",
                      i, got, pre[i + 1]);
    }
}

// ---- R4.9 / R16.4: high score writes ONLY on GAME OVER (and only
//                   if the run beat the stored value) --------------

static void test_high_score_written_on_game_over(void) {
    // Start with EEPROM zeros.
    uint8_t pre[10] = { 0xDF, 0,0,0, 0,0,0, 0,0,0 };
    eeprom_write(0, pre, 10);
    reboot_to_main_loop();

    // Play scenario: lives=0, score=12345, then trigger fatal hit.
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, 28, 0, 0);
    sim_mem_w(S, S->sym_death_flash, 0);
    sim_mem_w(S, S->sym_respawn_invuln, 0);
    sim_mem_w(S, S->sym_lives, 0);
    sim_mem_w(S, S->sym_difficulty, 1);    // MED
    sim_mem_w(S, sim_lookup(S, "score_lo"),  0x39);    // 12345 = 0x003039
    sim_mem_w(S, sim_lookup(S, "score_mid"), 0x30);
    sim_mem_w(S, sim_lookup(S, "score_hi"),  0x00);

    // Snapshot EEPROM MED slot (bytes 4..6) BEFORE the hit.
    uint8_t before[10];
    eeprom_read(0, before, 10);
    SIM_CHECK_MSG(before[4] == 0 && before[5] == 0 && before[6] == 0,
                  "R4.9 prereq: MED HI = 0 before hit");

    // Trigger fatal hit (overlapping Lander).
    sim_place_lander(S, 0, 60, 28);
    sim_run_frame(S);
    // The EEPROM write happens inside maybe_save_high_score; give the
    // 3.4-ms-per-byte cycle plenty of time to drain. 3 bytes ≈ 165 K
    // cycles, well within 500 K.
    sim_run_cycles(S, 500000);

    uint8_t after[10];
    eeprom_read(0, after, 10);
    SIM_CHECK_MSG(after[4] == 0x39 && after[5] == 0x30 && after[6] == 0x00,
                  "R4.9 / R16.4: MED HI updated to 0x%02X%02X%02X (want "
                  "0x003039 little-endian: 0x39 0x30 0x00)",
                  after[6], after[5], after[4]);
    // LOW and HIGH slots untouched.
    SIM_CHECK(after[1] == 0 && after[2] == 0 && after[3] == 0);
    SIM_CHECK(after[7] == 0 && after[8] == 0 && after[9] == 0);
}

static void test_lower_score_does_not_overwrite_high_score(void) {
    // Pre-load MED slot with a high HI score.
    uint8_t pre[10] = {
        0xDF, 0,0,0, 0xE8, 0x03, 0x00, 0,0,0,   // MED HI = 1000
    };
    eeprom_write(0, pre, 10);
    reboot_to_main_loop();

    // Trigger a fatal hit with score=500 (below the stored 1000).
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, 28, 0, 0);
    sim_mem_w(S, S->sym_death_flash, 0);
    sim_mem_w(S, S->sym_respawn_invuln, 0);
    sim_mem_w(S, S->sym_lives, 0);
    sim_mem_w(S, S->sym_difficulty, 1);    // MED
    // score = 500 → 0x01F4 little-endian
    sim_mem_w(S, sim_lookup(S, "score_lo"),  0xF4);
    sim_mem_w(S, sim_lookup(S, "score_mid"), 0x01);
    sim_mem_w(S, sim_lookup(S, "score_hi"),  0x00);
    sim_place_lander(S, 0, 60, 28);
    sim_run_frame(S);
    sim_run_cycles(S, 500000);

    uint8_t after[10];
    eeprom_read(0, after, 10);
    SIM_CHECK_MSG(after[4] == 0xE8 && after[5] == 0x03 && after[6] == 0,
                  "R16.4: MED HI preserved when run scored less; "
                  "got 0x%02X%02X%02X (want 0x0003E8)",
                  after[6], after[5], after[4]);
}

// ---- driver -----------------------------------------------------

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <defender.elf>\n", argv[0]); return 2; }
    S = sim_boot(argv[1]);

    test_magic_byte_written_at_boot();
    test_bad_magic_triggers_reset();
    test_existing_scores_loaded_on_boot();
    test_high_score_written_on_game_over();
    test_lower_score_does_not_overwrite_high_score();

    sim_print_summary("eeprom");
    return sim_fail_count ? 1 : 0;
}
