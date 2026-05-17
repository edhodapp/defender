// Pins R15.7-8: SFX_START is non-interruptible by lower-priority FX
// (FIRE, HIT, GRAB) but IS interruptible by higher-id sounds
// (SFX_DEATH, SFX_WAVE). This was the "no rising sound on game
// start" bug — a held B at title-confirm immediately fired SFX_FIRE
// in main_loop's first frame and clobbered SFX_START before any
// notes were audible.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_helpers.h"

#define SFX_FIRE   1
#define SFX_HIT    2
#define SFX_DEATH  3
#define SFX_GRAB   4
#define SFX_START  5
#define SFX_WAVE   6

static sim_t *S;
static uint16_t sym_sound_id;

// audio_play is r24=id; called via simavr by setting up a function
// call from outside is awkward. Instead we drive audio_play via the
// game's normal triggers: write the ID into pending_sfx and let
// main_alive call audio_play(pending_sfx).
//
// But the SFX_START itself is normally fired by skip_title_screen's
// call audio_play(5). For tests, we manually invoke by writing
// directly to sound_id (simulating "the engine just started this
// sound") and then write a different id into pending_sfx for the
// next frame to attempt an override.

static int sound_id(void) { return sim_mem_r(S, sym_sound_id); }

static void start_sfx(int id) {
    // Pretend the audio engine just started this sound: set sound_id
    // and sound_frame so audio_play's check sees an in-progress sfx.
    sim_mem_w(S, sym_sound_id, (uint8_t)id);
    sim_mem_w(S, S->sym_sound_frame, 0);
}

static void quiet_scenario(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, 28, 0, 0);
    // No spawner activity, no death state.
    sim_mem_w(S, S->sym_death_flash, 0);
    sim_mem_w(S, S->sym_respawn_invuln, 0);
    sim_mem_w(S, S->sym_spawn_countdown, 255);
    // Clear sound state so prior tests don't leak through.
    sim_mem_w(S, sym_sound_id, 0);
    sim_mem_w(S, S->sym_pending_sfx, 0);
}

// ---- R15.7: SFX_FIRE attempts during SFX_START are silently dropped

static void test_fire_does_not_clobber_start(void) {
    quiet_scenario();
    start_sfx(SFX_START);
    // pending_sfx will be picked up in main_alive's drain block and
    // funneled through audio_play. With the priority guard in place,
    // sound_id stays at SFX_START.
    sim_mem_w(S, S->sym_pending_sfx, SFX_FIRE);
    sim_run_frame(S);
    SIM_CHECK_MSG(sound_id() == SFX_START,
                  "R15.7: SFX_FIRE must not interrupt SFX_START; sound_id=%d",
                  sound_id());
}

static void test_hit_does_not_clobber_start(void) {
    quiet_scenario();
    start_sfx(SFX_START);
    sim_mem_w(S, S->sym_pending_sfx, SFX_HIT);
    sim_run_frame(S);
    SIM_CHECK_MSG(sound_id() == SFX_START,
                  "R15.7: SFX_HIT must not interrupt SFX_START; sound_id=%d",
                  sound_id());
}

static void test_grab_does_not_clobber_start(void) {
    quiet_scenario();
    start_sfx(SFX_START);
    sim_mem_w(S, S->sym_pending_sfx, SFX_GRAB);
    sim_run_frame(S);
    SIM_CHECK_MSG(sound_id() == SFX_START,
                  "R15.7: SFX_GRAB must not interrupt SFX_START; sound_id=%d",
                  sound_id());
}

// ---- R15.8: SFX_DEATH and SFX_WAVE bypass the SFX_START guard ----

static void test_death_overrides_start(void) {
    quiet_scenario();
    start_sfx(SFX_START);
    sim_mem_w(S, S->sym_pending_sfx, SFX_DEATH);
    sim_run_frame(S);
    SIM_CHECK_MSG(sound_id() == SFX_DEATH,
                  "R15.8: SFX_DEATH must override SFX_START; sound_id=%d",
                  sound_id());
}

static void test_wave_overrides_start(void) {
    quiet_scenario();
    start_sfx(SFX_START);
    sim_mem_w(S, S->sym_pending_sfx, SFX_WAVE);
    sim_run_frame(S);
    SIM_CHECK_MSG(sound_id() == SFX_WAVE,
                  "R15.8: SFX_WAVE must override SFX_START; sound_id=%d",
                  sound_id());
}

// ---- Negative: SFX_FIRE DOES interrupt a non-START sound ----------

// If there's no SFX_START playing, the priority guard doesn't engage
// and SFX_FIRE replaces whatever was previously playing. Pin this so
// a misguided refactor doesn't make ALL sounds non-interruptible.
static void test_fire_does_interrupt_when_no_start(void) {
    quiet_scenario();
    start_sfx(SFX_HIT);
    sim_mem_w(S, S->sym_pending_sfx, SFX_FIRE);
    sim_run_frame(S);
    SIM_CHECK_MSG(sound_id() == SFX_FIRE,
                  "R15.7 (negative): SFX_FIRE replaces SFX_HIT (no priority "
                  "guard between non-START sounds); sound_id=%d", sound_id());
}

// ---- driver --------------------------------------------------------

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <defender.elf>\n", argv[0]); return 2; }
    S = sim_boot(argv[1]);

    sym_sound_id = sim_lookup(S, "sound_id");

    test_fire_does_not_clobber_start();
    test_hit_does_not_clobber_start();
    test_grab_does_not_clobber_start();
    test_death_overrides_start();
    test_wave_overrides_start();
    test_fire_does_interrupt_when_no_start();

    sim_print_summary("audio_priority");
    return sim_fail_count ? 1 : 0;
}
