// Integration test: scrolling horizon + locked-x ship + flip + y motion.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_avr.h"
#include "sim_elf.h"
#include "sim_irq.h"
#include "avr_ioport.h"

#include "ssd1306.h"

#define F_CPU       16000000UL
#define MAX_CYCLES  (15000000UL)

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        rc = 1; \
    } \
} while (0)

// Must match defender.S sprite tables (framebuffer-column order).
static const uint8_t ship_right[8]    = { 0x18, 0x3C, 0x3C, 0x38, 0x30, 0x30, 0x10, 0x10 };
static const uint8_t ship_left[8]     = { 0x10, 0x10, 0x30, 0x30, 0x38, 0x3C, 0x3C, 0x18 };
static const uint8_t lander_sprite[8] = { 0x18, 0x3C, 0x7E, 0x7E, 0x7E, 0x7E, 0x3C, 0x18 };
static const uint8_t humanoid_sprite[8] = { 0x00, 0x88, 0xCB, 0x3F, 0x3F, 0xCB, 0x88, 0x00 };

// Humanoids are spawned at fixed world_x in entity slots 1..8 by _reset.
// y = 48 (top of page 6, just above mountain row).
static const uint8_t humanoid_world_x[8] = { 16, 48, 80, 112, 144, 176, 208, 240 };
#define HUMANOID_Y 48
#define HUMANOID_SCREEN_LIMIT 121               // matches render_humanoid's `cpi 121, brsh off`

// Must match defender.S height_table (256 bytes; hash period 256).
static const uint8_t height_table[256] = {
    0, 1, 2, 3, 4, 5, 6, 7, 1, 0, 3, 2, 5, 4, 7, 6,
    2, 3, 0, 1, 6, 7, 4, 5, 3, 2, 1, 0, 7, 6, 5, 4,
    5, 4, 7, 6, 1, 0, 3, 2, 4, 5, 6, 7, 0, 1, 2, 3,
    7, 6, 5, 4, 3, 2, 1, 0, 6, 7, 4, 5, 2, 3, 0, 1,
    2, 3, 0, 1, 6, 7, 4, 5, 3, 2, 1, 0, 7, 6, 5, 4,
    0, 1, 2, 3, 4, 5, 6, 7, 1, 0, 3, 2, 5, 4, 7, 6,
    7, 6, 5, 4, 3, 2, 1, 0, 6, 7, 4, 5, 2, 3, 0, 1,
    5, 4, 7, 6, 1, 0, 3, 2, 4, 5, 6, 7, 0, 1, 2, 3,
    4, 5, 6, 7, 0, 1, 2, 3, 5, 4, 7, 6, 1, 0, 3, 2,
    6, 7, 4, 5, 2, 3, 0, 1, 7, 6, 5, 4, 3, 2, 1, 0,
    1, 0, 3, 2, 5, 4, 7, 6, 0, 1, 2, 3, 4, 5, 6, 7,
    3, 2, 1, 0, 7, 6, 5, 4, 2, 3, 0, 1, 6, 7, 4, 5,
    6, 7, 4, 5, 2, 3, 0, 1, 7, 6, 5, 4, 3, 2, 1, 0,
    4, 5, 6, 7, 0, 1, 2, 3, 5, 4, 7, 6, 1, 0, 3, 2,
    3, 2, 1, 0, 7, 6, 5, 4, 2, 3, 0, 1, 6, 7, 4, 5,
    1, 0, 3, 2, 5, 4, 7, 6, 0, 1, 2, 3, 4, 5, 6, 7,
};

// Must match defender.S mask_table.
static const uint8_t mask_table[9] = {
    0x00, 0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE, 0xFF,
};

static int run_until_data(avr_t *avr, ssd1306_t *d, uint32_t target) {
    while (avr->cycle < MAX_CYCLES && d->data_count < target) {
        int s = avr_run(avr);
        if (s == cpu_Done || s == cpu_Crashed) return s;
    }
    return cpu_Running;
}

static int sprite_pixel(const uint8_t *sprite, int col, int row) {
    return (sprite[col] >> row) & 1;
}

static uint8_t expected_mountain_byte(int fb_col, uint8_t scroll) {
    return mask_table[height_table[(uint8_t)(scroll + fb_col)]];
}

// expected_radar_byte covers only the radar bar's [32, 96) cols. The
// wave-indicator digit (~cols 0-7) and the 5-digit score (cols 98+)
// live outside this range and aren't checked by the integration test —
// the screenshot harness covers them visually instead.
static uint8_t expected_radar_byte(int fb_col, uint8_t scroll, int sprite_y,
                                   int lander_world_x, int lander_y) {
    if (fb_col < 32 || fb_col >= 96) return 0;
    int radar_col = fb_col - 32;
    uint8_t b = 0x81;                                       // borders
    if (height_table[radar_col * 4] >= 4) b |= 0x40;        // mountain dot

    int ship_radar_col = ((scroll + 60) >> 2) & 0x3F;
    if (radar_col == ship_radar_col) {
        int ry = (sprite_y >> 3) + 1;
        if (ry > 5) ry = 5;
        b |= (uint8_t)(1 << ry);                            // ship at its altitude
    }

    if (lander_world_x >= 0) {
        int lander_radar_col = (lander_world_x >> 2) & 0x3F;
        if (radar_col == lander_radar_col) {
            int ry = (lander_y >> 3) + 1;
            if (ry > 5) ry = 5;
            b |= (uint8_t)(1 << ry);
        }
    }
    return b;
}

// Compute the on-screen x for each humanoid given the current scroll.
// Returns -1 for off-screen (matches firmware: MSB-set or screen_x >= 121).
static int humanoid_screen_x(int idx, uint8_t scroll) {
    int sx = (humanoid_world_x[idx] - (int)scroll) & 0xFF;
    if (sx & 0x80) return -1;
    if (sx >= HUMANOID_SCREEN_LIMIT) return -1;
    return sx;
}

// Verify sprite footprint, horizon, radar, optional projectile beam,
// optional lander, the 8 humanoids on the ground, and stray pixels in pages 1-6.
static int verify_frame(const ssd1306_t *d, int sx, int sy,
                        const uint8_t *sprite, uint8_t scroll,
                        int proj_spawn_x, int proj_x, int proj_y,
                        int lander_x, int lander_y,
                        const char *tag) {
    int rc = 0;
    int sprite_bad = 0, horizon_bad = 0, radar_bad = 0, lander_bad = 0, stray = 0;
    int humanoid_bad = 0;

    // Cache humanoid screen positions for both verify-pass and stray-mask use.
    int hum_sx[8];
    for (int i = 0; i < 8; i++) hum_sx[i] = humanoid_screen_x(i, scroll);

    // Sprite box
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            int x = sx + col, y = sy + row;
            int want = sprite_pixel(sprite, col, row);
            int got  = ssd1306_pixel(d, x, y);
            if (got != want) {
                if (sprite_bad == 0)
                    fprintf(stderr, "%s: first sprite mismatch at (%d,%d) got=%d want=%d\n",
                            tag, x, y, got, want);
                sprite_bad++;
            }
        }
    }

    // Page 7 horizon
    for (int c = 0; c < SSD1306_WIDTH; c++) {
        uint8_t got  = d->fb[7 * SSD1306_WIDTH + c];
        uint8_t want = expected_mountain_byte(c, scroll);
        if (got != want) {
            if (horizon_bad == 0)
                fprintf(stderr, "%s: first horizon byte mismatch at col=%d got=0x%02X want=0x%02X\n",
                        tag, c, got, want);
            horizon_bad++;
        }
    }

    // Page 0 radar bar — check only [32, 96). The wave-indicator digit
    // and the 5-digit score occupy the page-0 margins outside this range.
    int lander_world_x = (lander_x >= 0) ? ((lander_x + scroll) & 0xFF) : -1;
    for (int c = 32; c < 96; c++) {
        uint8_t got  = d->fb[0 * SSD1306_WIDTH + c];
        uint8_t want = expected_radar_byte(c, scroll, sy,
                                           lander_world_x, lander_y);
        if (got != want) {
            if (radar_bad == 0)
                fprintf(stderr, "%s: first radar byte mismatch at col=%d got=0x%02X want=0x%02X\n",
                        tag, c, got, want);
            radar_bad++;
        }
    }

    // Projectile beam
    int proj_bad = 0;
    int beam_lo = -1, beam_hi = -1;
    if (proj_x >= 0) {
        beam_lo = (proj_spawn_x < proj_x) ? proj_spawn_x : proj_x;
        beam_hi = (proj_spawn_x > proj_x) ? proj_spawn_x : proj_x;
        for (int x = beam_lo; x <= beam_hi; x++) {
            if (!ssd1306_pixel(d, x, proj_y)) {
                if (proj_bad == 0)
                    fprintf(stderr, "%s: beam pixel missing at (%d,%d)\n",
                            tag, x, proj_y);
                proj_bad++;
            }
        }
    }

    // Lander
    if (lander_x >= 0) {
        for (int row = 0; row < 8; row++) {
            for (int col = 0; col < 8; col++) {
                int x = lander_x + col, y = lander_y + row;
                int want = (lander_sprite[col] >> row) & 1;
                int got  = ssd1306_pixel(d, x, y);
                if (got != want) {
                    if (lander_bad == 0)
                        fprintf(stderr, "%s: first lander mismatch at (%d,%d) got=%d want=%d\n",
                                tag, x, y, got, want);
                    lander_bad++;
                }
            }
        }
    }

    // Humanoids (each visible one verifies pixel-perfect)
    for (int i = 0; i < 8; i++) {
        if (hum_sx[i] < 0) continue;
        for (int row = 0; row < 8; row++) {
            for (int col = 0; col < 8; col++) {
                int x = hum_sx[i] + col, y = HUMANOID_Y + row;
                int want = (humanoid_sprite[col] >> row) & 1;
                int got  = ssd1306_pixel(d, x, y);
                if (got != want) {
                    if (humanoid_bad == 0)
                        fprintf(stderr, "%s: first humanoid[%d] mismatch at (%d,%d) got=%d want=%d\n",
                                tag, i, x, y, got, want);
                    humanoid_bad++;
                }
            }
        }
    }

    // Stray pixels in pages 1-6, excluding sprite box, beam, lander, and humanoid boxes.
    for (int y = 8; y < 56; y++) {
        for (int x = 0; x < SSD1306_WIDTH; x++) {
            int in_sprite = (x >= sx && x < sx+8 && y >= sy && y < sy+8);
            int is_beam   = (proj_x >= 0 && y == proj_y &&
                             x >= beam_lo && x <= beam_hi);
            int in_lander = (lander_x >= 0 && x >= lander_x && x < lander_x+8 &&
                             y >= lander_y && y < lander_y+8);
            int in_humanoid = 0;
            if (y >= HUMANOID_Y && y < HUMANOID_Y + 8) {
                for (int i = 0; i < 8; i++) {
                    if (hum_sx[i] >= 0 && x >= hum_sx[i] && x < hum_sx[i] + 8) {
                        in_humanoid = 1; break;
                    }
                }
            }
            if (!in_sprite && !is_beam && !in_lander && !in_humanoid && ssd1306_pixel(d, x, y)) {
                if (stray == 0)
                    fprintf(stderr, "%s: first stray lit pixel at (%d,%d)\n", tag, x, y);
                stray++;
            }
        }
    }

    if (sprite_bad)   { rc = 1; fprintf(stderr, "%s: %d sprite mismatches\n", tag, sprite_bad); }
    if (horizon_bad)  { rc = 1; fprintf(stderr, "%s: %d horizon mismatches\n", tag, horizon_bad); }
    if (radar_bad)    { rc = 1; fprintf(stderr, "%s: %d radar mismatches\n", tag, radar_bad); }
    if (lander_bad)   { rc = 1; fprintf(stderr, "%s: %d lander mismatches\n", tag, lander_bad); }
    if (humanoid_bad) { rc = 1; fprintf(stderr, "%s: %d humanoid mismatches\n", tag, humanoid_bad); }
    if (proj_bad)     { rc = 1; }
    if (stray)        { rc = 1; fprintf(stderr, "%s: %d stray pixels\n", tag, stray); }
    return rc;
}

static avr_irq_t *btn_irq(avr_t *avr, char port, int bit) {
    return avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ(port), bit);
}

static void press(avr_irq_t *irq)   { avr_raise_irq(irq, 0); }
static void release(avr_irq_t *irq) { avr_raise_irq(irq, 1); }

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <defender.elf>\n", argv[0]);
        return 2;
    }

    elf_firmware_t fw;
    memset(&fw, 0, sizeof(fw));
    if (elf_read_firmware(argv[1], &fw) != 0) {
        fprintf(stderr, "elf_read_firmware failed for %s\n", argv[1]);
        return 2;
    }

    avr_t *avr = avr_make_mcu_by_name("atmega32u4");
    if (!avr) { fprintf(stderr, "avr_make_mcu_by_name failed\n"); return 2; }
    avr_init(avr);
    avr->frequency = F_CPU;
    avr_load_firmware(avr, &fw);

    ssd1306_t display;
    ssd1306_init_model(&display);
    ssd1306_attach(&display, avr);

    avr_irq_t *up    = btn_irq(avr, 'F', 7);
    avr_irq_t *down  = btn_irq(avr, 'F', 4);
    avr_irq_t *left  = btn_irq(avr, 'F', 5);
    avr_irq_t *right = btn_irq(avr, 'F', 6);
    avr_irq_t *b_btn = btn_irq(avr, 'B', 4);

    int rc = 0;

    // Run past _reset's stack init (which zeroes skip_title_flag), then
    // set the flag so the firmware bypasses the splash UI.
    while (avr->cycle < 500) avr_run(avr);
    avr_symbol_t **sym_arr = fw.symbol;
    for (uint32_t i = 0; i < fw.symbolcount; i++) {
        if (strcmp(sym_arr[i]->symbol, "skip_title_flag") == 0) {
            avr->data[sym_arr[i]->addr & 0xFFFF] = 1;
            break;
        }
    }

    // Phase 1: idle. Ship at (60,28) facing right, scroll=0, no projectile.
    if (run_until_data(avr, &display, 1024) == cpu_Crashed) {
        fprintf(stderr, "FAIL: cpu_Crashed before frame 1\n"); return 1;
    }
    // Lander spawns at world_x=100, y=10. Drifts 1 row every 4 frames AND
    // seeks 1 col toward the closest humanoid (at world_x=112) per drift,
    // so world_x advances by 1 per drift event too.
    rc |= verify_frame(&display, 60, 28, ship_right, 0,  -1, -1, -1, 100, 10, "idle");

    // Phase 2: 3x RIGHT. x locked, scroll=3, facing still right.
    // 1 drift fired (fc=4): world_x=101, y=11. screen_x=101-3=98.
    press(right);
    run_until_data(avr, &display, 4 * 1024);
    release(right);
    rc |= verify_frame(&display, 60, 28, ship_right, 3,  -1, -1, -1,  98, 11, "after right");

    // Phase 3: 3x UP. y=25, scroll unchanged at 3, facing unchanged.
    // No new drift in this window. Lander unchanged: world_x=101, y=11.
    press(up);
    run_until_data(avr, &display, 7 * 1024);
    release(up);
    rc |= verify_frame(&display, 60, 25, ship_right, 3,  -1, -1, -1,  98, 11, "after up");

    // Phase 4: 3x LEFT. scroll back to 0, facing flips to left.
    // 1 more drift (fc=8): world_x=102, y=12. screen_x=102.
    press(left);
    run_until_data(avr, &display, 10 * 1024);
    release(left);
    rc |= verify_frame(&display, 60, 25, ship_left, 0,  -1, -1, -1, 102, 12, "after left");

    // Phase 5: 3x DOWN. y=28, scroll unchanged at 0, facing unchanged.
    // 1 more drift (fc=12): world_x=103, y=13. screen_x=103.
    press(down);
    run_until_data(avr, &display, 13 * 1024);
    release(down);
    rc |= verify_frame(&display, 60, 28, ship_left, 0,  -1, -1, -1, 103, 13, "after down");

    // Phase 6: flip back to facing right (1x RIGHT), scroll=1; no drift.
    press(right);
    run_until_data(avr, &display, 14 * 1024);
    release(right);

    // Phase 7: press B for one frame; projectile spawns at fb col 68.
    // No drift in the window: lander still world_x=103, y=13. screen_x=103-1=102.
    press(b_btn);
    run_until_data(avr, &display, 15 * 1024);
    release(b_btn);
    rc |= verify_frame(&display, 60, 28, ship_right, 1,  68, 68, 32, 102, 13, "after fire B");

    // Phase 8: 3 more frames; proj_x = 68 + 4*3 = 80, beam from 68 to 80.
    // 1 more drift (fc=16): world_x=104, y=14. screen_x=104-1=103.
    run_until_data(avr, &display, 18 * 1024);
    rc |= verify_frame(&display, 60, 28, ship_right, 1,  68, 80, 32, 103, 14, "projectile in flight");

    printf("defender: cycles=%llu data=%u (after frame 18)\n",
           (unsigned long long)avr->cycle, display.data_count);

    if (rc == 0) printf("PASS\n");
    return rc;
}
