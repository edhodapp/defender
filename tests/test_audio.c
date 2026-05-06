// Layer tests for the SFX engine (audio.S).
//
// Contract:
//   - At boot, sound_id == 0 (silent).
//   - audio_tick advances sound_frame by 1 each frame; once the sentinel
//     0xFFFF is reached, sound_id resets to 0 and the timer halts.
//   - Trigger sites in defender.S call audio_play with the right id:
//       SFX_FIRE  (1) — successful B-press spawn (cooldown elapsed)
//       SFX_HIT   (2) — beam-vs-lander collision deactivation
//       SFX_DEATH (3) — ship-vs-lander collision (sprite_y reset)

#include <stdio.h>
#include <stdlib.h>

#include "sim_helpers.h"

#define SFX_FIRE   1
#define SFX_HIT    2
#define SFX_DEATH  3

#define SFX_FIRE_FRAMES   6
#define SFX_HIT_FRAMES    8
#define SFX_DEATH_FRAMES 30

static sim_t *S;

static void scenario(int sprite_y, int facing) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    sim_set_ship(S, sprite_y, 0, facing);
    sim_btn_release(S->btn_b);
    sim_btn_release(S->btn_up);
    sim_btn_release(S->btn_down);
    sim_btn_release(S->btn_left);
    sim_btn_release(S->btn_right);
}

static void quiet_run(void) {
    sim_mem_w(S, S->sym_frame_counter, 0);
    sim_run_frame(S);
}

static int sound_id(void)    { return sim_mem_r(S, S->sym_sound_id); }
static int sound_frame(void) { return sim_mem_r(S, S->sym_sound_frame); }

// ---- 1. Silence at boot ----
static void test_init_silent(void) {
    sim_sync(S);
    sim_clear_state_minimal(S);
    SIM_CHECK_MSG(sound_id() == 0, "sound_id=%d at boot (want 0)", sound_id());
}

// ---- 2. B-press triggers SFX_FIRE and frame advances ----
static void test_b_press_triggers_fire(void) {
    scenario(28, 0);
    sim_btn_press(S->btn_b);
    quiet_run();
    SIM_CHECK_MSG(sound_id() == SFX_FIRE, "sound_id=%d after fire", sound_id());
    // After 1 frame, sound_frame should already be 1 (audio_tick ran in this frame).
    // Note: audio_play is called LATE in the frame (at spawn site, after audio_tick),
    // so on the spawn frame, sound_frame is still 0 going into next iteration.
    SIM_CHECK_MSG(sound_frame() == 0,
                  "sound_frame=%d immediately after fire (want 0)", sound_frame());
}

// ---- 3. SFX_FIRE completes after its table runs out ----
static void test_fire_sfx_completes(void) {
    scenario(28, 0);
    sim_btn_press(S->btn_b);
    quiet_run();                                    // F1: spawn + audio_play(FIRE)
    sim_btn_release(S->btn_b);
    // 6 frames of OCR data + 1 frame to hit the 0xFFFF sentinel.
    for (int i = 0; i < SFX_FIRE_FRAMES + 2; i++) quiet_run();
    SIM_CHECK_MSG(sound_id() == 0,
                  "fire SFX should be silent after %d frames; sound_id=%d, frame=%d",
                  SFX_FIRE_FRAMES + 2, sound_id(), sound_frame());
}

// ---- 4. Beam-vs-lander HIT triggers SFX_HIT ----
static void test_lander_hit_triggers_hit_sfx(void) {
    scenario(28, 0);
    sim_place_lander(S, 0, 80, 28);
    sim_place_beam(S, 0, 76, 32, 0);                // adjacent, will hit next frame
    quiet_run();
    SIM_CHECK_MSG(!sim_lander_active(S, 0), "lander should be dead after hit");
    SIM_CHECK_MSG(sound_id() == SFX_HIT,
                  "expected SFX_HIT after beam hit; got sound_id=%d", sound_id());
}

// ---- 5. SFX_HIT completes after its table runs out ----
static void test_hit_sfx_completes(void) {
    scenario(28, 0);
    sim_place_lander(S, 0, 80, 28);
    sim_place_beam(S, 0, 76, 32, 0);
    quiet_run();                                    // hit frame
    for (int i = 0; i < SFX_HIT_FRAMES + 2; i++) quiet_run();
    SIM_CHECK_MSG(sound_id() == 0,
                  "hit SFX should be silent after %d frames; sound_id=%d",
                  SFX_HIT_FRAMES + 2, sound_id());
}

// ---- 6. Ship-vs-lander collision triggers SFX_DEATH ----
static void test_ship_collision_triggers_death_sfx(void) {
    scenario(28, 0);
    sim_place_lander(S, 0, 60, 28);                 // direct overlap with ship
    quiet_run();
    SIM_CHECK_MSG(sim_mem_r(S, S->sym_death_flash) == 7,
                  "death_flash=%d (want 7 indicating collision fired)",
                  sim_mem_r(S, S->sym_death_flash));
    SIM_CHECK_MSG(sound_id() == SFX_DEATH,
                  "expected SFX_DEATH; got sound_id=%d", sound_id());
}

// ---- 7. SFX_DEATH overrides an in-flight SFX_FIRE (latest call wins) ----
static void test_death_overrides_fire(void) {
    scenario(28, 0);
    // Fire first (id=FIRE will be playing).
    sim_btn_press(S->btn_b);
    quiet_run();
    sim_btn_release(S->btn_b);
    SIM_CHECK_MSG(sound_id() == SFX_FIRE, "expected FIRE pre-death");
    // Now stage a ship collision.
    sim_place_lander(S, 0, 60, 28);
    quiet_run();
    SIM_CHECK_MSG(sound_id() == SFX_DEATH,
                  "death should override fire; got sound_id=%d", sound_id());
    // audio_play resets sound_frame to 0 when DEATH starts; the first
    // audio_tick on it runs next frame.
    SIM_CHECK_MSG(sound_frame() == 0,
                  "death just queued; sound_frame should be 0, got %d", sound_frame());
    quiet_run();
    SIM_CHECK_MSG(sound_id() == SFX_DEATH && sound_frame() == 1,
                  "after one tick of DEATH: id=%d frame=%d (want 3,1)",
                  sound_id(), sound_frame());
}

// ---- 8. SFX_FIRE only triggers when cooldown allows (no fire on blocked frames) ----
static void test_no_fire_sfx_during_cooldown(void) {
    scenario(28, 0);
    sim_btn_press(S->btn_b);
    quiet_run();                                    // F1: fires + SFX_FIRE
    SIM_CHECK(sound_id() == SFX_FIRE);
    // Run 2 frames into cooldown (B still held, no new spawn).
    quiet_run();
    quiet_run();
    // sound_frame should be advancing through SFX_FIRE table, sound_id still FIRE.
    SIM_CHECK_MSG(sound_id() == SFX_FIRE,
                  "fire SFX should still be playing; sound_id=%d", sound_id());
    SIM_CHECK_MSG(sound_frame() >= 2,
                  "sound_frame should have advanced (>=2); got %d", sound_frame());
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <defender.elf>\n", argv[0]); return 2; }
    S = sim_boot(argv[1]);

    test_init_silent();
    test_b_press_triggers_fire();
    test_fire_sfx_completes();
    test_lander_hit_triggers_hit_sfx();
    test_hit_sfx_completes();
    test_ship_collision_triggers_death_sfx();
    test_death_overrides_fire();
    test_no_fire_sfx_during_cooldown();

    sim_coverage_save_for(argv[0]);
    sim_print_summary("audio");
    if (sim_fail_count) { printf("FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
