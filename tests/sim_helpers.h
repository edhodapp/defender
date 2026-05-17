// Shared test infrastructure: boot the firmware, look up BSS symbols,
// poke memory, run frames, inject button presses. Used by all layer
// functional tests.

#ifndef SIM_HELPERS_H
#define SIM_HELPERS_H

#include <stdint.h>
#include "sim_avr.h"
#include "sim_elf.h"

// Approximate wall-clock cycles per main_loop iteration (delay loop
// dominates). Generous so a single sim_run_frame() always lets exactly
// one main_loop iteration complete.
#define SIM_FRAME_CYCLES  700000ULL

typedef struct {
    avr_t          *avr;
    elf_firmware_t  fw;

    // BSS symbol addresses (looked up from ELF, masked to 16-bit data)
    uint16_t sym_sprite_y;
    uint16_t sym_scroll_offset;
    uint16_t sym_ship_facing;
    uint16_t sym_frame_counter;
    uint16_t sym_death_flash;
    uint16_t sym_spawn_pos_idx;
    uint16_t sym_fire_cooldown;
    uint16_t sym_sound_id;
    uint16_t sym_sound_frame;
    uint16_t sym_pending_sfx;
    uint16_t sym_wave_number;
    uint16_t sym_wave_to_spawn;
    uint16_t sym_wave_pods_to_spawn;
    uint16_t sym_enemies_alive;
    uint16_t sym_difficulty;
    uint16_t sym_skip_title_flag;
    uint16_t sym_lives;
    uint16_t sym_game_state;
    uint16_t sym_respawn_invuln;
    uint16_t sym_death_y;
    uint16_t sym_boot_warp_frames;
    uint16_t sym_planet_destroyed;
    uint16_t sym_planet_check_disabled;
    uint16_t sym_rng_state;
    uint16_t sym_spawn_countdown;
    uint16_t sym_projectiles;
    uint16_t sym_entities;
    uint16_t sym_framebuffer;
    uint16_t sym_main_loop;                  // PC value at main_loop entry (byte addr)

    // Button IRQs (active-low pins; raise to 0 = press, 1 = release)
    avr_irq_t *btn_up, *btn_down, *btn_left, *btn_right, *btn_a, *btn_b;
} sim_t;

// ---- Boot / teardown ----
sim_t *sim_boot(const char *elf_path);            // boot + skip-title + warmup
sim_t *sim_boot_no_warmup(const char *elf_path);  // boot + symbols only; caller drives warmup
uint16_t sim_lookup(const sim_t *s, const char *name);

// ---- Memory ----
void    sim_mem_w(sim_t *s, uint16_t addr, uint8_t v);
uint8_t sim_mem_r(const sim_t *s, uint16_t addr);

// ---- Time ----
void sim_run_cycles(sim_t *s, uint64_t n);

// Run until PC reaches main_loop. If we're already at main_loop, step once
// first so we don't return immediately.
void sim_run_to_next_iter(sim_t *s);

// Synonym for sim_run_to_next_iter — runs exactly one main_loop iteration.
void sim_run_frame(sim_t *s);

// Synchronize: park firmware at main_loop entry. Use before mem_w'ing
// scenario state so the firmware doesn't observe partial state mid-iter.
void sim_sync(sim_t *s);

// ---- Buttons (active-low, internal pull-ups) ----
void sim_btn_press(avr_irq_t *btn);
void sim_btn_release(avr_irq_t *btn);

// ---- Game-state manipulation (post-boot, before each scenario) ----
void sim_clear_entities(sim_t *s);
void sim_clear_projectiles(sim_t *s);
void sim_clear_state_minimal(sim_t *s);   // entities + projectiles + frame_counter + death_flash + spawn_pos_idx
void sim_set_ship(sim_t *s, int sprite_y, int scroll, int facing /* 0=right, 1=left */);
void sim_place_lander(sim_t *s, int slot, int world_x, int y);
void sim_place_beam(sim_t *s, int slot, int x, int y, int dx_sign);

// ---- State queries ----
int sim_lander_active(const sim_t *s, int slot);
int sim_lander_carrying(const sim_t *s, int slot);    // Phase 2: byte0 bit 0
int sim_beam_active  (const sim_t *s, int slot);
int sim_lander_y     (const sim_t *s, int slot);
int sim_lander_world_x(const sim_t *s, int slot);
int sim_beam_y       (const sim_t *s, int slot);
int sim_beam_x       (const sim_t *s, int slot);

// ---- Coverage / profile ----
// Word-indexed PC tables for the AVR firmware. avr->pc is a byte address;
// we divide by 2 to get the instruction-word index.
//   sim_pc_visits[w]  = # of avr_run() calls dispatched while pc==w
//   sim_pc_cycles[w]  = total avr->cycle delta observed across those calls
// Both are updated together, automatically, on every avr_run inside
// sim_run_cycles / sim_run_to_next_iter.
#define SIM_PC_TABLE_SIZE 16384
extern uint32_t sim_pc_visits[SIM_PC_TABLE_SIZE];
extern uint64_t sim_pc_cycles[SIM_PC_TABLE_SIZE];

// Write coverage (visits) to a binary file: raw uint32_t * SIM_PC_TABLE_SIZE.
void sim_coverage_save(const char *path);
void sim_coverage_save_for(const char *argv0);          // /tmp/sim_cov_<base>.bin

// Write profile (cycles) to a binary file: raw uint64_t * SIM_PC_TABLE_SIZE.
void sim_profile_save(const char *path);
void sim_profile_save_for(const char *argv0);           // /tmp/sim_prof_<base>.bin

// Zero both tables — call after warmup, before the measurement window.
void sim_profile_reset(void);

// ---- Test result tracking ----
extern int sim_pass_count;
extern int sim_fail_count;

// CHECK: if cond is false, print a message and increment the fail count.
#define SIM_CHECK(cond) do { \
    if (cond) { sim_pass_count++; } \
    else { sim_fail_count++; \
           fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define SIM_CHECK_MSG(cond, fmt, ...) do { \
    if (cond) { sim_pass_count++; } \
    else { sim_fail_count++; \
           fprintf(stderr, "FAIL %s:%d: %s | " fmt "\n", \
                   __FILE__, __LINE__, #cond, ##__VA_ARGS__); } \
} while (0)

void sim_print_summary(const char *suite_name);

#endif
