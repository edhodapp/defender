#include "sim_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_irq.h"
#include "avr_ioport.h"

#define F_CPU 16000000UL

int sim_pass_count = 0;
int sim_fail_count = 0;
uint32_t sim_pc_visits[SIM_PC_TABLE_SIZE];
uint64_t sim_pc_cycles[SIM_PC_TABLE_SIZE];

void sim_coverage_save(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fwrite(sim_pc_visits, sizeof(uint32_t), SIM_PC_TABLE_SIZE, f);
    fclose(f);
}

void sim_coverage_save_for(const char *argv0) {
    const char *base = strrchr(argv0, '/');
    base = base ? base + 1 : argv0;
    char path[512];
    snprintf(path, sizeof(path), "/tmp/sim_cov_%s.bin", base);
    sim_coverage_save(path);
}

void sim_profile_save(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fwrite(sim_pc_cycles, sizeof(uint64_t), SIM_PC_TABLE_SIZE, f);
    fclose(f);
}

void sim_profile_save_for(const char *argv0) {
    const char *base = strrchr(argv0, '/');
    base = base ? base + 1 : argv0;
    char path[512];
    snprintf(path, sizeof(path), "/tmp/sim_prof_%s.bin", base);
    sim_profile_save(path);
}

void sim_profile_reset(void) {
    memset(sim_pc_visits, 0, sizeof(sim_pc_visits));
    memset(sim_pc_cycles, 0, sizeof(sim_pc_cycles));
}

uint16_t sim_lookup(const sim_t *s, const char *name) {
    for (uint32_t i = 0; i < s->fw.symbolcount; i++) {
        if (strcmp(s->fw.symbol[i]->symbol, name) == 0) {
            return (uint16_t)(s->fw.symbol[i]->addr & 0xFFFF);
        }
    }
    fprintf(stderr, "ERR: symbol not found: %s\n", name);
    exit(2);
}

static avr_irq_t *btn_irq(avr_t *avr, char port, int bit) {
    return avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ(port), bit);
}

sim_t *sim_boot(const char *elf_path) {
    sim_t *s = calloc(1, sizeof(*s));
    if (!s) { fprintf(stderr, "calloc failed\n"); exit(2); }
    if (elf_read_firmware(elf_path, &s->fw) != 0) {
        fprintf(stderr, "elf_read_firmware failed for %s\n", elf_path); exit(2);
    }
    s->avr = avr_make_mcu_by_name("atmega32u4");
    if (!s->avr) { fprintf(stderr, "avr_make_mcu_by_name failed\n"); exit(2); }
    avr_init(s->avr);
    s->avr->frequency = F_CPU;
    avr_load_firmware(s->avr, &s->fw);

    s->sym_sprite_y       = sim_lookup(s, "sprite_y");
    s->sym_scroll_offset  = sim_lookup(s, "scroll_offset");
    s->sym_ship_facing    = sim_lookup(s, "ship_facing");
    s->sym_frame_counter  = sim_lookup(s, "frame_counter");
    s->sym_death_flash    = sim_lookup(s, "death_flash");
    s->sym_spawn_pos_idx  = sim_lookup(s, "spawn_pos_idx");
    s->sym_fire_cooldown  = sim_lookup(s, "fire_cooldown");
    s->sym_sound_id       = sim_lookup(s, "sound_id");
    s->sym_sound_frame    = sim_lookup(s, "sound_frame");
    s->sym_pending_sfx    = sim_lookup(s, "pending_sfx");
    s->sym_wave_number    = sim_lookup(s, "wave_number");
    s->sym_wave_to_spawn  = sim_lookup(s, "wave_to_spawn");
    s->sym_enemies_alive  = sim_lookup(s, "enemies_alive");
    s->sym_projectiles    = sim_lookup(s, "projectiles");
    s->sym_entities       = sim_lookup(s, "entities");
    s->sym_framebuffer    = sim_lookup(s, "framebuffer");
    s->sym_main_loop      = sim_lookup(s, "main_loop");

    s->btn_up    = btn_irq(s->avr, 'F', 7);
    s->btn_down  = btn_irq(s->avr, 'F', 4);
    s->btn_left  = btn_irq(s->avr, 'F', 5);
    s->btn_right = btn_irq(s->avr, 'F', 6);
    s->btn_a     = btn_irq(s->avr, 'E', 6);
    s->btn_b     = btn_irq(s->avr, 'B', 4);

    // Run a couple of frames so _reset finishes and main_loop reaches its
    // steady-state delay loop.
    sim_run_cycles(s, 2 * SIM_FRAME_CYCLES);
    return s;
}

void    sim_mem_w(sim_t *s, uint16_t addr, uint8_t v) { s->avr->data[addr] = v; }
uint8_t sim_mem_r(const sim_t *s, uint16_t addr)       { return s->avr->data[addr]; }

void sim_run_cycles(sim_t *s, uint64_t n) {
    uint64_t target = s->avr->cycle + n;
    while (s->avr->cycle < target) {
        uint16_t pc_word = s->avr->pc / 2;
        uint64_t before = s->avr->cycle;
        int st = avr_run(s->avr);
        if (pc_word < SIM_PC_TABLE_SIZE) {
            sim_pc_visits[pc_word]++;
            sim_pc_cycles[pc_word] += (s->avr->cycle - before);
        }
        if (st == cpu_Crashed) {
            fprintf(stderr, "ERR: cpu_Crashed at cycle %llu\n",
                    (unsigned long long)s->avr->cycle);
            exit(2);
        }
    }
}

void sim_run_to_next_iter(sim_t *s) {
    uint16_t target = s->sym_main_loop;
    uint64_t safety = s->avr->cycle + 4 * SIM_FRAME_CYCLES;
    // Step off the current PC if we're already parked at main_loop, so we
    // don't return immediately.
    if (s->avr->pc == target) {
        uint16_t pc_word = s->avr->pc / 2;
        uint64_t before = s->avr->cycle;
        avr_run(s->avr);
        if (pc_word < SIM_PC_TABLE_SIZE) {
            sim_pc_visits[pc_word]++;
            sim_pc_cycles[pc_word] += (s->avr->cycle - before);
        }
    }
    while (s->avr->pc != target) {
        if (s->avr->cycle >= safety) {
            fprintf(stderr,
                "ERR: sim_run_to_next_iter timeout (pc=0x%X target=0x%X)\n",
                s->avr->pc, target);
            exit(2);
        }
        uint16_t pc_word = s->avr->pc / 2;
        uint64_t before = s->avr->cycle;
        int st = avr_run(s->avr);
        if (pc_word < SIM_PC_TABLE_SIZE) {
            sim_pc_visits[pc_word]++;
            sim_pc_cycles[pc_word] += (s->avr->cycle - before);
        }
        if (st == cpu_Crashed) {
            fprintf(stderr, "ERR: cpu_Crashed during run_to_next_iter\n");
            exit(2);
        }
    }
}

void sim_run_frame(sim_t *s) { sim_run_to_next_iter(s); }
void sim_sync(sim_t *s)      { sim_run_to_next_iter(s); }

void sim_btn_press(avr_irq_t *btn)   { avr_raise_irq(btn, 0); }
void sim_btn_release(avr_irq_t *btn) { avr_raise_irq(btn, 1); }

void sim_clear_entities(sim_t *s) {
    for (int i = 0; i < 64; i++) sim_mem_w(s, s->sym_entities + i*3, 0);
}

void sim_clear_projectiles(sim_t *s) {
    for (int i = 0; i < 4; i++) sim_mem_w(s, s->sym_projectiles + i*2, 0);
}

void sim_clear_state_minimal(sim_t *s) {
    sim_clear_entities(s);
    sim_clear_projectiles(s);
    sim_mem_w(s, s->sym_frame_counter, 0);
    sim_mem_w(s, s->sym_death_flash, 0);
    sim_mem_w(s, s->sym_spawn_pos_idx, 0);
    sim_mem_w(s, s->sym_fire_cooldown, 0);
    sim_mem_w(s, s->sym_sound_id, 0);
    sim_mem_w(s, s->sym_sound_frame, 0);
    sim_mem_w(s, s->sym_pending_sfx, 0);
    // Reset wave state so spawn-related tests get unconstrained behavior
    // (entities cleared → no enemies; wave_to_spawn=8 gives plenty of room
    // for any test that just wants to see the spawner fire without setting
    // up its own wave state).
    sim_mem_w(s, s->sym_wave_number, 1);
    sim_mem_w(s, s->sym_wave_to_spawn, 8);
    sim_mem_w(s, s->sym_enemies_alive, 0);
}

void sim_set_ship(sim_t *s, int sprite_y, int scroll, int facing) {
    sim_mem_w(s, s->sym_sprite_y, sprite_y);
    sim_mem_w(s, s->sym_scroll_offset, scroll);
    sim_mem_w(s, s->sym_ship_facing, facing);
}

void sim_place_lander(sim_t *s, int slot, int world_x, int y) {
    sim_mem_w(s, s->sym_entities + slot*3 + 0, (1<<7) | (1<<4));   // active + LANDER
    sim_mem_w(s, s->sym_entities + slot*3 + 1, world_x);
    sim_mem_w(s, s->sym_entities + slot*3 + 2, y);
}

void sim_place_beam(sim_t *s, int slot, int x, int y, int dx_sign) {
    uint8_t b0 = 0x80 | ((dx_sign & 1) << 6) | (y & 0x3F);
    sim_mem_w(s, s->sym_projectiles + slot*2 + 0, b0);
    sim_mem_w(s, s->sym_projectiles + slot*2 + 1, x);
}

int sim_lander_active(const sim_t *s, int slot) {
    return (sim_mem_r(s, s->sym_entities + slot*3) & 0x80) != 0;
}
int sim_lander_carrying(const sim_t *s, int slot) {
    return (sim_mem_r(s, s->sym_entities + slot*3) & 0x01) != 0;
}
int sim_beam_active(const sim_t *s, int slot) {
    return (sim_mem_r(s, s->sym_projectiles + slot*2) & 0x80) != 0;
}
int sim_lander_y(const sim_t *s, int slot) {
    return sim_mem_r(s, s->sym_entities + slot*3 + 2) & 0x3F;
}
int sim_lander_world_x(const sim_t *s, int slot) {
    return sim_mem_r(s, s->sym_entities + slot*3 + 1);
}
int sim_beam_y(const sim_t *s, int slot) {
    return sim_mem_r(s, s->sym_projectiles + slot*2) & 0x3F;
}
int sim_beam_x(const sim_t *s, int slot) {
    return sim_mem_r(s, s->sym_projectiles + slot*2 + 1);
}

void sim_print_summary(const char *suite_name) {
    fprintf(stderr, "----- %s: %d passed, %d failed -----\n",
            suite_name, sim_pass_count, sim_fail_count);
}
