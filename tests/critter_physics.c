// critter_physics — per-enemy motion characterization tool.
//
// Spawns one enemy in isolation, runs the firmware for N frames, and
// prints a trajectory + summary metrics. Output is CSV-ish so it can
// be piped to a plotting script or spreadsheet later. This is a
// REPORT tool, not a regression test — it doesn't assert behaviors,
// it just measures them so we can see what each critter actually does
// and make informed tuning decisions.
//
// Pattern per scenario:
//   1. Park the simulator at main_loop entry.
//   2. Clear entities + projectiles + wave state; place ONE critter
//      under test (and any prerequisite entities, e.g., a humanoid
//      for the Lander to seek).
//   3. Disable the spawner (wave_to_spawn=0, enemies_alive=99 so
//      wave-end doesn't fire and re-arm spawning).
//   4. Run N frames, sampling position every K frames.
//   5. Print the rows + summary.
//
// Difficulty affects Swarmer behavior (nudge frequency table), so the
// Swarmer scenario runs once per difficulty with the same RNG seed
// for direct comparison.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_helpers.h"

#define TYPE_LANDER    1
#define TYPE_MUTANT    2
#define TYPE_POD       3
#define TYPE_SWARMER   5
#define TYPE_HUMANOID  7

static sim_t *S;

// ----- helpers --------------------------------------------------------

static void place_entity(int slot, uint8_t type, int world_x, int y) {
    sim_mem_w(S, S->sym_entities + slot * 3 + 0, (1u << 7) | (type << 4));
    sim_mem_w(S, S->sym_entities + slot * 3 + 1, (uint8_t)world_x);
    sim_mem_w(S, S->sym_entities + slot * 3 + 2, (uint8_t)y);
}

static int slot_active(int slot) {
    return sim_mem_r(S, S->sym_entities + slot * 3) & 0x80;
}
static int slot_type(int slot) {
    return (sim_mem_r(S, S->sym_entities + slot * 3) >> 4) & 0x07;
}
static int slot_world_x(int slot) {
    return sim_mem_r(S, S->sym_entities + slot * 3 + 1);
}
static int slot_y(int slot) {
    return sim_mem_r(S, S->sym_entities + slot * 3 + 2) & 0x3F;
}
static int slot_carry(int slot) {
    return sim_mem_r(S, S->sym_entities + slot * 3) & 0x01;
}

// Quiet world: no spawner activity, no wave-end re-arm, ship parked
// at default position, no humanoids to satisfy planet_check (which is
// disabled anyway in sim_clear_state_minimal).
static void quiet_world(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, 28, 0, 0);
    sim_mem_w(S, S->sym_wave_to_spawn, 0);
    sim_mem_w(S, S->sym_enemies_alive, 99);     // suppress wave-end re-arm
    sim_mem_w(S, S->sym_boot_warp_frames, 0);   // no boot pause for slot 0
}

static void run_n(int n) {
    for (int i = 0; i < n; i++) sim_run_frame(S);
}

static void set_difficulty(int diff) {
    sim_mem_w(S, S->sym_difficulty, (uint8_t)diff);
}

static const char *diff_name(int diff) {
    switch (diff) {
        case 0: return "LOW";
        case 1: return "MED";
        case 2: return "HIGH";
        default: return "?";
    }
}

// ----- scenarios ------------------------------------------------------

// Lander descent + seek: a Lander spawns at (world_x=200, y=10) while
// a single grounded humanoid sits at world_x=112, y=48. Trace 60
// drift events (240 frames ~ 4 s) and watch the descent + horizontal
// seek converge.
static void scenario_lander_seek(void) {
    printf("\n=== scenario: Lander seek (humanoid at wx=112) ===\n");
    printf("frame,wx,y,carry\n");
    quiet_world();
    place_entity(0, TYPE_HUMANOID, 112, 48);
    place_entity(1, TYPE_LANDER,   200, 10);

    int start_x = slot_world_x(1);
    int start_y = slot_y(1);
    int sample_n = 60;
    int frames_per_sample = 4;
    int total_frames = 0;

    printf("%d,%d,%d,%d\n", 0, slot_world_x(1), slot_y(1), slot_carry(1));
    for (int s = 1; s <= sample_n; s++) {
        run_n(frames_per_sample);
        total_frames += frames_per_sample;
        if (!slot_active(1)) {
            printf("# slot 1 deactivated at frame %d\n", total_frames);
            break;
        }
        printf("%d,%d,%d,%d\n",
               total_frames, slot_world_x(1), slot_y(1), slot_carry(1));
    }
    int end_x = slot_active(1) ? slot_world_x(1) : -1;
    int end_y = slot_active(1) ? slot_y(1) : -1;
    printf("# summary: started (%d,%d) ended (%d,%d) over %d frames\n",
           start_x, start_y, end_x, end_y, total_frames);
}

// Mutant chase: Mutant at (world_x=200, y=10), ship parked at the
// usual (world_x=60, y=28). Mutant should descend AND seek
// horizontally until it overlaps the ship. Trace ends when its
// distance to the ship is < 8 or after 240 frames.
static void scenario_mutant_chase(void) {
    printf("\n=== scenario: Mutant chase ===\n");
    printf("frame,wx,y,dx,dy\n");
    quiet_world();
    place_entity(1, TYPE_MUTANT, 200, 10);

    int frame = 0;
    while (frame < 240 && slot_active(1)) {
        int wx = slot_world_x(1);
        int y  = slot_y(1);
        int sx = (wx - 0) & 0xFF;                // scroll=0
        int dx = sx - 60;                         // distance to ship's screen x
        int dy = y  - 28;
        printf("%d,%d,%d,%d,%d\n", frame, wx, y, dx, dy);
        if (abs(dx) < 8 && abs(dy) < 8) {
            printf("# Mutant within hit-box of ship at frame %d\n", frame);
            break;
        }
        run_n(4);                                 // sample every 4 frames
        frame += 4;
    }
}

// Pod drift: same setup as Mutant but slower target. Pods just drift
// laterally toward the ship; no vertical motion. Useful to see the
// horizontal pace and decide whether Pods need acceleration.
static void scenario_pod_drift(void) {
    printf("\n=== scenario: Pod drift ===\n");
    printf("frame,wx,y\n");
    quiet_world();
    place_entity(1, TYPE_POD, 200, 10);

    int start_x = slot_world_x(1);
    int frame = 0;
    while (frame < 480 && slot_active(1)) {
        printf("%d,%d,%d\n", frame, slot_world_x(1), slot_y(1));
        run_n(8);                                 // pod moves every 8 frames
        frame += 8;
    }
    int end_x = slot_active(1) ? slot_world_x(1) : -1;
    printf("# summary: started wx=%d ended wx=%d over %d frames\n",
           start_x, end_x, frame);
}

// Swarmer chase across difficulties. RNG state seeded to a fixed
// value at scenario start so trajectories are reproducible and the
// only varying input is the per-difficulty nudge-skip threshold.
// Runs the same scenario 3x (LOW, MED, HIGH) and labels each.
static void scenario_swarmer_chase(int diff, uint8_t rng_seed) {
    printf("\n=== scenario: Swarmer chase (%s, rng=0x%02X) ===\n",
           diff_name(diff), rng_seed);
    printf("frame,wx,y,dir,dx,dy\n");
    quiet_world();
    set_difficulty(diff);
    sim_mem_w(S, S->sym_rng_state, rng_seed);
    place_entity(1, TYPE_SWARMER, 200, 10);

    int collision_frame = -1;
    int frame = 0;
    while (frame < 300 && slot_active(1)) {
        int wx  = slot_world_x(1);
        int y   = slot_y(1);
        int dir = sim_mem_r(S, S->sym_entities + 1 * 3) & 0x07;
        int sx  = wx & 0xFF;
        int dx  = sx - 60;
        int dy  = y  - 28;
        if (frame == 0 || (frame % 8) == 0) {
            printf("%d,%d,%d,%d,%d,%d\n", frame, wx, y, dir, dx, dy);
        }
        if (abs(dx) < 8 && abs(dy) < 8) {
            collision_frame = frame;
            break;
        }
        run_n(1);
        frame++;
    }
    if (collision_frame >= 0) {
        printf("# %s seed=0x%02X collision at frame %d (%.2fs)\n",
               diff_name(diff), rng_seed, collision_frame, collision_frame / 60.0);
    } else {
        printf("# %s seed=0x%02X no collision in %d frames\n",
               diff_name(diff), rng_seed, frame);
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <defender.elf>\n", argv[0]);
        return 2;
    }
    S = sim_boot(argv[1]);

    scenario_lander_seek();
    scenario_mutant_chase();
    scenario_pod_drift();

    // Swarmer scenario per difficulty, three RNG seeds each, so we
    // can compare distributions not just single trajectories.
    static const uint8_t seeds[] = { 0x4C, 0xA5, 0x17 };
    for (int d = 0; d < 3; d++) {
        for (size_t i = 0; i < sizeof seeds; i++) {
            scenario_swarmer_chase(d, seeds[i]);
        }
    }

    return 0;
}
