// screenshot — render a named scenario in simavr and dump the SSD1306
// framebuffer as a PBM (1-bit Netpbm). Catches render bugs without
// needing the hardware.
//
// Usage: screenshot <elf> <scenario> [out.pbm]
//
// Scenarios:
//   title          splash screen as it appears at power-on
//   title-low      splash with selector on LOW (UP pressed once)
//   title-high     splash with selector on HIGH (DOWN pressed once)
//   game           default game state (skip-title path), default MED
//   abduction      Lander hovering on a humanoid's head
//   carrying       Lander mid-ascent with humanoid attached
//   mutant         a Mutant in flight chasing the player
//
// PBM is the simplest possible 1-bit format; any image viewer reads it.
// Convert to PNG with: `convert in.pbm out.png` (ImageMagick).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sim_helpers.h"
#include "ssd1306.h"

#define TYPE_LANDER    1
#define TYPE_MUTANT    2
#define TYPE_HUMANOID  7

static void dump_pbm(const ssd1306_t *d, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    fprintf(f, "P4\n%d %d\n", SSD1306_WIDTH, SSD1306_HEIGHT);
    for (int y = 0; y < SSD1306_HEIGHT; y++) {
        for (int xb = 0; xb < SSD1306_WIDTH; xb += 8) {
            uint8_t b = 0;
            for (int bit = 0; bit < 8; bit++) {
                int x = xb + bit;
                int page = y / 8;
                int row = y % 8;
                if ((d->fb[page * SSD1306_WIDTH + x] >> row) & 1) {
                    b |= (0x80 >> bit);
                }
            }
            fputc(b, f);
        }
    }
    fclose(f);
    fprintf(stderr, "wrote %s (%dx%d)\n", path, SSD1306_WIDTH, SSD1306_HEIGHT);
}

static void place_lander(sim_t *S, int slot, int world_x, int y) {
    sim_mem_w(S, S->sym_entities + slot*3 + 0, (1<<7) | (TYPE_LANDER<<4));
    sim_mem_w(S, S->sym_entities + slot*3 + 1, world_x);
    sim_mem_w(S, S->sym_entities + slot*3 + 2, y);
}

static void place_lander_carrying(sim_t *S, int slot, int world_x, int y) {
    sim_mem_w(S, S->sym_entities + slot*3 + 0,
              (1<<7) | (TYPE_LANDER<<4) | 0x01);
    sim_mem_w(S, S->sym_entities + slot*3 + 1, world_x);
    sim_mem_w(S, S->sym_entities + slot*3 + 2, y);
}

static void place_mutant(sim_t *S, int slot, int world_x, int y) {
    sim_mem_w(S, S->sym_entities + slot*3 + 0, (1<<7) | (TYPE_MUTANT<<4));
    sim_mem_w(S, S->sym_entities + slot*3 + 1, world_x);
    sim_mem_w(S, S->sym_entities + slot*3 + 2, y);
}

static void place_humanoid(sim_t *S, int slot, int world_x, int y) {
    sim_mem_w(S, S->sym_entities + slot*3 + 0, (1<<7) | (TYPE_HUMANOID<<4));
    sim_mem_w(S, S->sym_entities + slot*3 + 1, world_x);
    sim_mem_w(S, S->sym_entities + slot*3 + 2, y);
}

// Boot the firmware INTO the title screen — don't set skip_title_flag.
static sim_t *boot_into_title(const char *elf_path, ssd1306_t *d) {
    sim_t *S = sim_boot_no_warmup(elf_path);
    ssd1306_init_model(d);
    ssd1306_attach(d, S->avr);
    sim_run_cycles(S, 3 * SIM_FRAME_CYCLES);
    return S;
}

// Boot the firmware past the title (skip_title_flag set). Same as
// sim_boot's normal behavior but also attaches a fresh SSD1306 model.
static sim_t *boot_past_title(const char *elf_path, ssd1306_t *d) {
    sim_t *S = sim_boot_no_warmup(elf_path);
    ssd1306_init_model(d);
    ssd1306_attach(d, S->avr);
    sim_run_cycles(S, 500);
    sim_mem_w(S, S->sym_skip_title_flag, 1);
    sim_run_cycles(S, 2 * SIM_FRAME_CYCLES);
    return S;
}

// title scenarios just observe the title; press buttons before the dump
// to move the selector.
static void scenario_title(sim_t *S) { (void)S; }

static void scenario_title_low(sim_t *S) {
    sim_btn_press(S->btn_up);
    sim_run_cycles(S, SIM_FRAME_CYCLES);
    sim_btn_release(S->btn_up);
    sim_run_cycles(S, SIM_FRAME_CYCLES);
}

static void scenario_title_high(sim_t *S) {
    sim_btn_press(S->btn_down);
    sim_run_cycles(S, SIM_FRAME_CYCLES);
    sim_btn_release(S->btn_down);
    sim_run_cycles(S, SIM_FRAME_CYCLES);
}

static void scenario_abduction(sim_t *S) {
    sim_clear_state_minimal(S);
    sim_set_ship(S, 28, 0, 0);
    place_humanoid(S, 1, 80, 48);
    place_lander(S, 0, 80, 39);                         // poised to descend onto the head
    for (int i = 0; i < 4; i++) sim_run_frame(S);       // one drift → y=40 → pause arms
    sim_run_frame(S);                                   // present-state render
}

static void scenario_carrying(sim_t *S) {
    sim_clear_state_minimal(S);
    sim_set_ship(S, 28, 0, 0);
    place_lander_carrying(S, 0, 80, 24);                // mid-ascent
    sim_run_frame(S);
}

static void scenario_mutant(sim_t *S) {
    sim_clear_state_minimal(S);
    sim_set_ship(S, 28, 0, 0);
    place_mutant(S, 0, 90, 14);
    sim_run_frame(S);
}

static void scenario_game(sim_t *S) {
    // already past title — entities are in their boot configuration
    sim_run_frame(S);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <elf> <scenario> [out.pbm]\n", argv[0]);
        fprintf(stderr, "scenarios: title title-low title-high game "
                        "abduction carrying mutant\n");
        return 2;
    }
    const char *elf      = argv[1];
    const char *scenario = argv[2];
    const char *out      = argc >= 4 ? argv[3] : "/tmp/screenshot.pbm";

    ssd1306_t d;
    sim_t *S;

    if (strcmp(scenario, "title") == 0) {
        S = boot_into_title(elf, &d);
        scenario_title(S);
    } else if (strcmp(scenario, "title-low") == 0) {
        S = boot_into_title(elf, &d);
        scenario_title_low(S);
    } else if (strcmp(scenario, "title-high") == 0) {
        S = boot_into_title(elf, &d);
        scenario_title_high(S);
    } else if (strcmp(scenario, "game") == 0) {
        S = boot_past_title(elf, &d);
        scenario_game(S);
    } else if (strcmp(scenario, "abduction") == 0) {
        S = boot_past_title(elf, &d);
        scenario_abduction(S);
    } else if (strcmp(scenario, "carrying") == 0) {
        S = boot_past_title(elf, &d);
        scenario_carrying(S);
    } else if (strcmp(scenario, "mutant") == 0) {
        S = boot_past_title(elf, &d);
        scenario_mutant(S);
    } else {
        fprintf(stderr, "unknown scenario: %s\n", scenario);
        return 2;
    }

    dump_pbm(&d, out);
    return 0;
}
