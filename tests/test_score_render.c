// Verifies the 5-digit HUD score rendering across the 24-bit score range.
// Pokes score_lo/mid/hi, runs one main_loop frame, then decodes the
// framebuffer at the digit slots (page 0, cols 99/105/111/117/123) by
// matching against the known font glyphs for '0'..'9'.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sim_helpers.h"

// Font glyphs for digits '0'..'9'. Five data cols per glyph; the
// trailing 3 zero pad cols are not checked (they're all zero in the
// font and irrelevant to which character was blitted).
static const uint8_t digit_glyph[10][5] = {
    {0x3E, 0x49, 0x45, 0x43, 0x3E},  // 0 (slashed zero)
    {0x42, 0x7F, 0x40, 0x00, 0x00},  // 1
    {0x42, 0x61, 0x51, 0x49, 0x46},  // 2
    {0x22, 0x49, 0x49, 0x49, 0x36},  // 3
    {0x0F, 0x08, 0x08, 0x7F, 0x08},  // 4
    {0x4F, 0x49, 0x49, 0x49, 0x31},  // 5
    {0x3E, 0x49, 0x49, 0x49, 0x31},  // 6
    {0x01, 0x01, 0x79, 0x05, 0x03},  // 7
    {0x36, 0x49, 0x49, 0x49, 0x36},  // 8
    {0x0E, 0x51, 0x51, 0x51, 0x7E},  // 9
};

static int decode_score(sim_t *s, char out[6]) {
    uint16_t fb = s->sym_framebuffer;
    static const int digit_x[5] = {99, 105, 111, 117, 123};
    for (int d = 0; d < 5; d++) {
        int x = digit_x[d];
        out[d] = '?';
        for (int g = 0; g < 10; g++) {
            int match = 1;
            for (int c = 0; c < 5; c++) {
                uint8_t got = sim_mem_r(s, fb + 0 * 128 + x + c);
                if (got != digit_glyph[g][c]) { match = 0; break; }
            }
            if (match) { out[d] = '0' + g; break; }
        }
    }
    out[5] = '\0';
    for (int d = 0; d < 5; d++) if (out[d] == '?') return -1;
    return 0;
}

static void run_case(sim_t *s, uint16_t sym_lo, uint16_t sym_mid, uint16_t sym_hi,
                     const char *name, uint8_t lo, uint8_t mid, uint8_t hi,
                     const char *expected) {
    sim_sync(s);
    sim_clear_state_minimal(s);
    sim_set_ship(s, 28, 0, 0);
    sim_mem_w(s, sym_lo,  lo);
    sim_mem_w(s, sym_mid, mid);
    sim_mem_w(s, sym_hi,  hi);
    sim_run_frame(s);

    char got[6];
    int ok = decode_score(s, got);
    if (ok != 0) {
        sim_fail_count++;
        fprintf(stderr, "FAIL %s: undecoded glyph in '%s' (raw cols at digits):\n",
                name, got);
        uint16_t fb = s->sym_framebuffer;
        for (int x = 99; x < 128; x++) {
            fprintf(stderr, "  col %3d: 0x%02X\n", x, sim_mem_r(s, fb + x));
        }
        return;
    }
    if (strcmp(got, expected) != 0) {
        sim_fail_count++;
        fprintf(stderr, "FAIL %s: lo=0x%02X mid=0x%02X hi=0x%02X "
                        "got='%s' want='%s'\n",
                name, lo, mid, hi, got, expected);
    } else {
        sim_pass_count++;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s firmware.elf\n", argv[0]);
        return 1;
    }
    sim_t *s = sim_boot(argv[1]);

    uint16_t sym_lo  = sim_lookup(s, "score_lo");
    uint16_t sym_mid = sim_lookup(s, "score_mid");
    uint16_t sym_hi  = sim_lookup(s, "score_hi");

    // Within 16-bit (always worked).
    run_case(s, sym_lo, sym_mid, sym_hi, "zero",  0x00, 0x00, 0x00, "00000");
    run_case(s, sym_lo, sym_mid, sym_hi, "99",    99,   0x00, 0x00, "00099");
    run_case(s, sym_lo, sym_mid, sym_hi, "65535", 0xFF, 0xFF, 0x00, "65535");

    // Past 16-bit — was broken before the 24-bit decimal extractor.
    run_case(s, sym_lo, sym_mid, sym_hi, "65536",  0x00, 0x00, 0x01, "65536");
    run_case(s, sym_lo, sym_mid, sym_hi, "99999",  0x9F, 0x86, 0x01, "99999");

    // At/above the display clamp.
    run_case(s, sym_lo, sym_mid, sym_hi, "100000", 0xA0, 0x86, 0x01, "99999");
    run_case(s, sym_lo, sym_mid, sym_hi, "max",    0xFF, 0xFF, 0xFF, "99999");

    sim_print_summary("score_render");
    return sim_fail_count ? 1 : 0;
}
