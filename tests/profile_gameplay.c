// profile_gameplay — simulated playthrough metrics.
//
// Drives the firmware with a programmatic "test pilot" — a per-frame
// policy that observes BSS state and decides which buttons to press.
// Runs for a fixed frame budget per (pilot × difficulty) combination
// and reports gameplay-shape metrics: time alive, kills by type,
// peak enemies on screen, lives lost, score.
//
// Two pilots:
//
//   PASSIVE   — no input ever. Reveals the unimpeded buildup of
//               threats over time. If a passive ship survives N
//               seconds, that's the "free time" the difficulty
//               gives before being overwhelmed.
//
//   DEFENSIVE — moves away from the nearest threat; fires when an
//               enemy is roughly y-aligned. Models a cautious human.
//
// (Aggressive pilot deferred — defensive already exercises both fire
// and movement; a "hold-fire and drift" mode is easy to add later.)
//
// Output format: one header block per scenario, then per-50-frame
// snapshots, then a summary line. CSV-ish, pipeable to a plotter.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_helpers.h"

#define TYPE_NONE      0
#define TYPE_LANDER    1
#define TYPE_MUTANT    2
#define TYPE_POD       3
#define TYPE_SWARMER   5
#define TYPE_HUMANOID  7

// Button mask bits — match input_read's return shape.
#define BTN_UP    (1 << 0)
#define BTN_DOWN  (1 << 1)
#define BTN_LEFT  (1 << 2)
#define BTN_RIGHT (1 << 3)
#define BTN_A     (1 << 4)
#define BTN_B     (1 << 5)

static sim_t *S;

// ----- world snapshot helpers ----------------------------------------

typedef struct {
    int lives;
    int game_state;
    int score;
    int sprite_y;
    int scroll;
    int n_enemies;         // active TYPE_{LANDER,MUTANT,POD,SWARMER}
    int n_humanoids;       // active TYPE_HUMANOID (grounded + falling)
    int nearest_enemy_sx;  // signed dx from ship; INT_MAX if none
    int nearest_enemy_y;
    int nearest_enemy_type;
} world_t;

static int signed_byte(int v) { return (v & 0x80) ? (v - 256) : v; }

static void snapshot(world_t *w) {
    w->lives      = sim_mem_r(S, S->sym_lives);
    w->game_state = sim_mem_r(S, S->sym_game_state);
    int lo = sim_mem_r(S, sim_lookup(S, "score_lo"));
    int mi = sim_mem_r(S, sim_lookup(S, "score_mid"));
    int hi = sim_mem_r(S, sim_lookup(S, "score_hi"));
    w->score = lo | (mi << 8) | (hi << 16);
    w->sprite_y = sim_mem_r(S, S->sym_sprite_y);
    w->scroll   = sim_mem_r(S, S->sym_scroll_offset);

    int ship_wx = (w->scroll + 60) & 0xFF;
    w->n_enemies = 0;
    w->n_humanoids = 0;
    int best_abs_dx = 1000;
    w->nearest_enemy_sx = 1000;
    w->nearest_enemy_y = 0;
    w->nearest_enemy_type = TYPE_NONE;

    for (int slot = 0; slot < 64; slot++) {
        uint8_t b0 = sim_mem_r(S, S->sym_entities + slot * 3 + 0);
        if (!(b0 & 0x80)) continue;
        int t = (b0 >> 4) & 0x07;
        if (t == TYPE_HUMANOID) {
            w->n_humanoids++;
            continue;
        }
        if (t != TYPE_LANDER && t != TYPE_MUTANT &&
            t != TYPE_POD    && t != TYPE_SWARMER) continue;
        w->n_enemies++;
        int wx = sim_mem_r(S, S->sym_entities + slot * 3 + 1);
        int y  = sim_mem_r(S, S->sym_entities + slot * 3 + 2) & 0x3F;
        int dx = signed_byte((wx - ship_wx) & 0xFF);    // wrap-aware
        int adx = abs(dx);
        if (adx < best_abs_dx) {
            best_abs_dx = adx;
            w->nearest_enemy_sx = dx;
            w->nearest_enemy_y  = y;
            w->nearest_enemy_type = t;
        }
    }
}

// ----- pilots ---------------------------------------------------------

typedef uint8_t (*pilot_fn)(const world_t *);

static uint8_t pilot_passive(const world_t *w) {
    (void)w;
    return 0;
}

// Defensive: if an enemy is within 32 cols and within 12 rows of the
// ship's y, move AWAY horizontally and reposition vertically toward
// the safest band (y=8 to 47 valid). Fire when an enemy is roughly
// y-aligned (within 8 rows).
static uint8_t pilot_defensive(const world_t *w) {
    uint8_t mask = 0;
    int dx = w->nearest_enemy_sx;
    int dy = (w->nearest_enemy_type != TYPE_NONE)
             ? (w->nearest_enemy_y - w->sprite_y) : 0;

    if (w->nearest_enemy_type != TYPE_NONE && abs(dx) < 32) {
        // Enemy close: move away.
        if (dx > 0) mask |= BTN_LEFT; else mask |= BTN_RIGHT;
    } else {
        // Otherwise drift right slowly.
        mask |= BTN_RIGHT;
    }
    if (w->nearest_enemy_type != TYPE_NONE && abs(dy) > 6) {
        if (dy > 0) mask |= BTN_DOWN; else mask |= BTN_UP;
    }
    if (w->nearest_enemy_type != TYPE_NONE && abs(dy) <= 8) {
        mask |= BTN_B;
    }
    return mask;
}

// ----- button driver -------------------------------------------------

static uint8_t prev_mask = 0;

static void apply_buttons(uint8_t mask) {
    // Active-low pins via simavr IRQs. press(btn) sets to 0; release
    // sets to 1. We diff against prev_mask to minimize churn.
    struct {
        uint8_t bit;
        avr_irq_t *irq;
    } map[] = {
        {BTN_UP,    S->btn_up},    {BTN_DOWN, S->btn_down},
        {BTN_LEFT,  S->btn_left},  {BTN_RIGHT, S->btn_right},
        {BTN_A,     S->btn_a},     {BTN_B,    S->btn_b},
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        int now  = (mask & map[i].bit) != 0;
        int before = (prev_mask & map[i].bit) != 0;
        if (now && !before) sim_btn_press(map[i].irq);
        else if (!now && before) sim_btn_release(map[i].irq);
    }
    prev_mask = mask;
}

static void reset_buttons(void) {
    apply_buttons(0);   // release any held buttons from previous scenario
}

// ----- driver --------------------------------------------------------

typedef struct {
    int frames_alive;
    int final_score;
    int final_lives;
    int peak_enemies;
    int lives_lost;
    int reached_game_over;
} run_metrics_t;

static void run_scenario(const char *pilot_name, pilot_fn pilot,
                         int diff, int frame_budget,
                         run_metrics_t *out) {
    printf("\n=== %s pilot, %s, %d frames ===\n",
           pilot_name,
           diff == 0 ? "LOW" : diff == 1 ? "MED" : "HIGH",
           frame_budget);

    sim_sync(S);
    sim_clear_state_minimal(S);
    // Re-enable the planet-destruction watcher AND humanoids: this is
    // a real-gameplay simulation, not a unit scenario.
    sim_mem_w(S, S->sym_planet_check_disabled, 0);
    for (int i = 0; i < 8; i++) {
        uint16_t base = S->sym_entities + (1 + i) * 3;
        sim_mem_w(S, base + 0, (1 << 7) | (TYPE_HUMANOID << 4));
        sim_mem_w(S, base + 1, 16 + i * 32);
        sim_mem_w(S, base + 2, 48);
    }
    // Boot a wave-1-style state.
    sim_mem_w(S, S->sym_difficulty, (uint8_t)diff);
    sim_mem_w(S, S->sym_wave_number, 1);
    sim_mem_w(S, S->sym_planet_destroyed, 0);
    sim_mem_w(S, S->sym_enemies_alive, 0);
    sim_mem_w(S, S->sym_wave_to_spawn, 20);    // generous so wave doesn't end
    sim_mem_w(S, S->sym_lives, 3);
    sim_mem_w(S, S->sym_game_state, 0);
    sim_set_ship(S, 28, 0, 0);
    reset_buttons();

    int starting_lives = 3;
    int peak_enemies = 0;
    int frame = 0;
    world_t w;
    snapshot(&w);

    printf("# frame,lives,score,n_enemies,n_humanoids,planet_destroyed\n");
    for (frame = 0; frame < frame_budget; frame++) {
        snapshot(&w);
        if (w.n_enemies > peak_enemies) peak_enemies = w.n_enemies;

        if (frame % 50 == 0) {
            int pd = sim_mem_r(S, S->sym_planet_destroyed);
            printf("%d,%d,%d,%d,%d,%d\n",
                   frame, w.lives, w.score, w.n_enemies, w.n_humanoids, pd);
        }
        if (w.game_state) {
            printf("# game over at frame %d (%.2fs)\n",
                   frame, frame / 60.0);
            break;
        }

        uint8_t mask = pilot(&w);
        apply_buttons(mask);
        sim_run_frame(S);
    }
    reset_buttons();
    snapshot(&w);

    out->frames_alive = frame;
    out->final_score  = w.score;
    out->final_lives  = w.lives;
    out->peak_enemies = peak_enemies;
    out->lives_lost   = starting_lives - w.lives;
    out->reached_game_over = w.game_state ? 1 : 0;

    printf("# summary: frames_alive=%d (%.2fs)  score=%d  lives=%d  "
           "lives_lost=%d  peak_enemies=%d  game_over=%d\n",
           out->frames_alive, out->frames_alive / 60.0,
           out->final_score, out->final_lives,
           out->lives_lost, out->peak_enemies, out->reached_game_over);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <defender.elf>\n", argv[0]);
        return 2;
    }
    S = sim_boot(argv[1]);

    static const struct {
        const char *name;
        pilot_fn fn;
    } pilots[] = {
        { "PASSIVE",   pilot_passive },
        { "DEFENSIVE", pilot_defensive },
    };

    // 2000 frames ~ 33s of game time. Long enough to see at least one
    // wave play out and tell whether a difficulty traps the player
    // quickly. Six scenarios at 2000 frames is roughly 90 s wall
    // clock — keeps the tool usable during interactive tuning.
    int budget = 2000;

    printf("\n--------- gameplay shape ---------\n");
    printf("pilot,diff,frames_alive,score,lives_lost,peak_enemies,game_over\n");

    for (size_t p = 0; p < sizeof pilots / sizeof pilots[0]; p++) {
        for (int d = 0; d < 3; d++) {
            run_metrics_t m;
            run_scenario(pilots[p].name, pilots[p].fn, d, budget, &m);
            // Mid-line summary row mirrors the header above so the
            // final block is greppable.
            printf("# ROW: %s,%d,%d,%d,%d,%d,%d\n",
                   pilots[p].name, d,
                   m.frames_alive, m.final_score, m.lives_lost,
                   m.peak_enemies, m.reached_game_over);
        }
    }
    return 0;
}
