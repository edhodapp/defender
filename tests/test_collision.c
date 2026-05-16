// Collision-detection scenario harness.
//
// Loads defender.elf, lets _reset run, then directly pokes the BSS
// (via simavr's data memory + ELF symbol table) to set up arbitrary
// scenarios: ship position, scroll, a single Lander at a known
// (world_x, y), a beam in flight at known (x, y, direction). Runs
// the simulator for N frames and asserts whether collision happened.
//
// Each test resets the relevant state at the start; we intentionally
// clear `frame_counter` every frame so the periodic Lander spawn
// (every 32 frames) never triggers and pollutes the test world.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_avr.h"
#include "sim_elf.h"
#include "sim_irq.h"
#include "avr_ioport.h"

#define F_CPU         16000000UL
#define FRAME_CYCLES  600000UL                  // ~30 fps frame budget

static avr_t          *g_avr;
static elf_firmware_t  g_fw;

// BSS symbol addresses (masked to data space)
static uint16_t a_sprite_y, a_scroll_offset, a_projectiles, a_entities;
static uint16_t a_frame_counter, a_spawn_pos_idx, a_death_flash, a_ship_facing;

static uint16_t sym(const char *name) {
    for (uint32_t i = 0; i < g_fw.symbolcount; i++) {
        if (strcmp(g_fw.symbol[i]->symbol, name) == 0) {
            // AVR data symbols use 0x800000 prefix; mask to 16-bit data addr.
            return (uint16_t)(g_fw.symbol[i]->addr & 0xFFFF);
        }
    }
    fprintf(stderr, "ERR: symbol not found: %s\n", name);
    exit(2);
}

static void mem_w(uint16_t addr, uint8_t v) { g_avr->data[addr] = v; }
static uint8_t mem_r(uint16_t addr)         { return g_avr->data[addr]; }

static void run_cycles(uint64_t n) {
    uint64_t target = g_avr->cycle + n;
    while (g_avr->cycle < target) avr_run(g_avr);
}

static void boot(const char *elf_path) {
    memset(&g_fw, 0, sizeof(g_fw));
    if (elf_read_firmware(elf_path, &g_fw) != 0) {
        fprintf(stderr, "elf_read_firmware failed for %s\n", elf_path); exit(2);
    }
    g_avr = avr_make_mcu_by_name("atmega32u4");
    if (!g_avr) { fprintf(stderr, "avr_make_mcu_by_name failed\n"); exit(2); }
    avr_init(g_avr);
    g_avr->frequency = F_CPU;
    avr_load_firmware(g_avr, &g_fw);

    a_sprite_y       = sym("sprite_y");
    a_scroll_offset  = sym("scroll_offset");
    a_projectiles    = sym("projectiles");
    a_entities       = sym("entities");
    a_frame_counter  = sym("frame_counter");
    a_spawn_pos_idx  = sym("spawn_pos_idx");
    a_death_flash    = sym("death_flash");
    a_ship_facing    = sym("ship_facing");

    // Set the test-mode skip-title flag AFTER _reset's stack init zeros it.
    run_cycles(500);
    g_avr->data[sym("skip_title_flag")] = 1;
    run_cycles(2 * FRAME_CYCLES);
}

// Wipe all the state that scenarios care about.
static void clear_state(void) {
    for (int i = 0; i < 4; i++)  mem_w(a_projectiles + i*2, 0);
    for (int i = 0; i < 64; i++) mem_w(a_entities + i*3, 0);
    mem_w(a_frame_counter, 0);
    mem_w(a_spawn_pos_idx, 0);
    mem_w(a_death_flash, 0);
    // Park spawn_countdown high so the periodic spawner doesn't fire
    // during a 16-frame scenario. Without this, accumulated frames
    // across the sweep-y / sweep-x loops eventually drag the counter
    // to 0 and a stray Lander spawns mid-scenario, contaminating the
    // single-Lander collision check.
    uint16_t sym_spawn_countdown = sym("spawn_countdown");
    mem_w(sym_spawn_countdown, 255);
}

static void place_ship(int sprite_y, int scroll, int facing /* 0=right, 1=left */) {
    mem_w(a_sprite_y, sprite_y);
    mem_w(a_scroll_offset, scroll);
    mem_w(a_ship_facing, facing);
}

static void place_lander(int slot, int world_x, int y) {
    mem_w(a_entities + slot*3 + 0, (1<<7) | (1<<4));   // active + TYPE_LANDER
    mem_w(a_entities + slot*3 + 1, world_x);
    mem_w(a_entities + slot*3 + 2, y);
}

static void place_beam(int slot, int x, int y, int dx_sign /* 0=right(+4), 1=left(-4) */) {
    uint8_t b0 = 0x80 | ((dx_sign & 1) << 6) | (y & 0x3F);
    mem_w(a_projectiles + slot*2 + 0, b0);
    mem_w(a_projectiles + slot*2 + 1, x);
}

static int beam_active(int slot)   { return (mem_r(a_projectiles + slot*2) & 0x80) != 0; }
static int lander_active(int slot) { return (mem_r(a_entities    + slot*3) & 0x80) != 0; }

// Run up to max_frames; each frame, clamp frame_counter to 0 so the periodic
// spawn timer never triggers. Return the frame index at which the lander in
// slot 0 was deactivated, or -1 if it survived all frames.
static int run_for_collision(int max_frames) {
    for (int f = 1; f <= max_frames; f++) {
        mem_w(a_frame_counter, 0);                      // suppress periodic spawn
        run_cycles(FRAME_CYCLES);
        if (!lander_active(0)) return f;
    }
    return -1;
}

// ---- Predictor: would this scenario collide if collision_check is correct? ----
// Beam moves +4 (right) or -4 (left) each frame. Collision triggers when the
// beam's current x is in [lander_screen_x, lander_screen_x+7] AND
// beam_y is in [lander_y, lander_y+7].
static int predict_collide(int beam_x_init, int beam_y, int dx_sign,
                           int world_x, int lander_y, int scroll,
                           int max_frames) {
    if (beam_y < lander_y || beam_y > lander_y + 7) return 0;
    int lander_sx = (world_x - scroll) & 0xFF;
    if (lander_sx > 120) return 0;                      // off-screen
    int x = beam_x_init;
    int dx = (dx_sign == 0) ? 4 : -4;
    for (int f = 0; f < max_frames; f++) {
        x += dx;
        if (x < 0 || x > 127) return 0;
        if (x >= lander_sx && x <= lander_sx + 7) return 1;
    }
    return 0;
}

// ---- Test cases ----

static int g_pass = 0, g_fail = 0;

static void check_scenario(const char *tag,
                           int sprite_y, int scroll, int facing,
                           int world_x, int lander_y,
                           int beam_x_init, int beam_y, int dx_sign,
                           int max_frames) {
    clear_state();
    place_ship(sprite_y, scroll, facing);
    place_lander(0, world_x, lander_y);
    place_beam(0, beam_x_init, beam_y, dx_sign);

    int expected = predict_collide(beam_x_init, beam_y, dx_sign, world_x, lander_y, scroll, max_frames);
    int hit_at = run_for_collision(max_frames);
    int actual = (hit_at != -1);

    if (expected == actual) {
        g_pass++;
    } else {
        g_fail++;
        fprintf(stderr, "FAIL  %s\n", tag);
        fprintf(stderr, "    sprite_y=%d scroll=%d facing=%d world_x=%d lander_y=%d\n",
                sprite_y, scroll, facing, world_x, lander_y);
        fprintf(stderr, "    beam start (%d,%d) dx_sign=%d  max_frames=%d\n",
                beam_x_init, beam_y, dx_sign, max_frames);
        fprintf(stderr, "    expected %s, actual %s (hit_at=%d)\n",
                expected ? "HIT" : "MISS", actual ? "HIT" : "MISS", hit_at);
    }
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <defender.elf>\n", argv[0]); return 2; }
    boot(argv[1]);

    // Targeted single-scenario probes — the bug Ed reported.
    // Lander at the bottom (y=47), ship aligned (sprite_y=43 → beam_y=47),
    // facing right, lander on-screen ahead of the beam.
    check_scenario("bottom-aligned: ship 43 / lander y47 / x80",
        /*sprite_y*/43, /*scroll*/0, /*facing*/0,
        /*world_x*/80, /*lander_y*/47,
        /*beam_x*/68, /*beam_y*/47, /*dx_sign*/0,
        /*max_frames*/16);

    check_scenario("bottom-aligned: ship 47 / lander y47 / x80",
        47, 0, 0,  80, 47,  68, 51, 0,  16);

    check_scenario("bottom-aligned: ship 43 / lander y47 / x100",
        43, 0, 0,  100, 47,  68, 47, 0,  16);

    check_scenario("bottom-aligned: ship 43 / lander y47 / x120",
        43, 0, 0,  120, 47,  68, 47, 0,  16);

    // Lander at top (y=10), ship aligned (sprite_y=8 → beam_y=12, in [10..17])
    check_scenario("top-aligned: ship 8 / lander y10 / x80",
        8, 0, 0,  80, 10,  68, 12, 0,  16);

    // Lander at middle, beam aligned
    check_scenario("mid-aligned: ship 28 / lander y28 / x80",
        28, 0, 0,  80, 28,  68, 32, 0,  16);

    // Beam OUT of y-range (no collision expected)
    check_scenario("no-y: ship 8 / lander y47 / x80 (default sprite range, beam high)",
        8, 0, 0,  80, 47,  68, 12, 0,  16);

    // Lander left of ship → beam (facing right, moves +4) cannot reach
    check_scenario("no-x: lander left of ship, beam right",
        28, 0, 0,  20, 28,  68, 32, 0,  20);

    // Lander right of ship, very close — beam reaches in 1 frame
    check_scenario("close-x: lander screen_x=72",
        28, 0, 0,  72, 28,  68, 32, 0,  4);

    // Sweep over lander_y values (with ship matched) to find any boundary issues
    for (int ly = 0; ly <= 47; ly++) {
        int sy_target = ly - 4;                   // beam_y = sprite_y+4 lands on lander_y
        if (sy_target < 8) sy_target = 8;
        int by = sy_target + 4;
        char tag[80];
        snprintf(tag, sizeof(tag), "sweep-y: lander y=%d sprite_y=%d beam_y=%d", ly, sy_target, by);
        check_scenario(tag,
            sy_target, 0, 0,
            80, ly,
            68, by, 0,
            16);
    }

    // Sweep across world_x at the bottom altitude — does any horizontal
    // position behave differently?
    for (int wx = 70; wx <= 120; wx += 4) {
        char tag[80];
        snprintf(tag, sizeof(tag), "sweep-x: world_x=%d at bottom (lander y=47)", wx);
        check_scenario(tag,
            43, 0, 0,
            wx, 47,
            68, 47, 0,
            20);
    }

    // Sweep across scroll values with lander at bottom
    for (int sc = 0; sc <= 200; sc += 25) {
        char tag[80];
        snprintf(tag, sizeof(tag), "sweep-scroll: scroll=%d lander world_x=100 y=47", sc);
        check_scenario(tag,
            43, sc, 0,
            100, 47,
            68, 47, 0,
            20);
    }

    fprintf(stderr, "\n----- collision summary: %d passed, %d failed -----\n", g_pass, g_fail);
    if (g_fail) printf("TESTS FAILED\n");
    else        printf("PASS\n");
    return g_fail ? 1 : 0;
}
