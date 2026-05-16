; Defender for Arduboy — step 4.2: scrolling horizon.
; LEFT/RIGHT scrolls the world (and flips the ship); ship is locked at the
; horizontal center; UP/DOWN moves the ship vertically (clamped above the
; mountains so it doesn't overlap them).

#include <avr/io.h>

; ===== Framebuffer + game state =====
.section .bss, "aw", @nobits

.global framebuffer
framebuffer:
    .skip 1024

.global sprite_y
sprite_y:       .skip 1
.global ship_facing
ship_facing:    .skip 1                  ; 0 = right, 1 = left
.global scroll_offset
scroll_offset:  .skip 1                  ; world column at framebuffer col 0
.global frame_counter
frame_counter:  .skip 1                  ; increments every frame; used as a divider for slow-drift entities
.global death_flash
death_flash:    .skip 1                  ; non-zero → invert framebuffer this frame, then decrement

; Extended ship-death state: counts down from 90 to 0 once per frame
; while the ship is dying or recovering.
;   90..61  = explosion phase (30 frames). Ship hidden, fragments
;             render outward from (64, death_y+4), input suppressed,
;             collisions disabled.
;   60..1   = respawn-blink phase (60 frames). Ship visible at
;             (60, sprite_y) but blinks every 8 frames; input enabled,
;             collisions still disabled (player can reposition without
;             instant re-death from the same enemy).
;   0       = normal play.
.global respawn_invuln
respawn_invuln: .skip 1

; Ship's y at the moment of the fatal collision — frozen as the
; explosion's vertical center. sprite_y itself is reset to 28 on
; death so the ship respawns at the default altitude after the
; explosion clears.
.global death_y
death_y:        .skip 1
.global spawn_pos_idx
spawn_pos_idx:  .skip 1                  ; advances each lander spawn; world_x = idx*32 + 16

.global fire_cooldown
fire_cooldown:  .skip 1                  ; auto-fire throttle; reloads to FIRE_COOLDOWN_RELOAD after each shot
.equ FIRE_COOLDOWN_RELOAD, 5             ; shot every (5+1)=6 frames @ 60 Hz = 10 shots/sec

.global frame_ready
frame_ready:    .skip 1                  ; set by Timer1 ISR; cleared after main_loop sees it

; Wave structure (Phase 4):
;   wave_number   : current wave (1..6, capped)
;   wave_to_spawn : enemies remaining to spawn this wave
;   enemies_alive : active Landers + Mutants currently in play
; A wave ends when both wave_to_spawn AND enemies_alive are zero; main_loop
; queues SFX_WAVE_CHANGE, increments wave_number, and reloads wave_to_spawn.
.global wave_number
wave_number:    .skip 1
.global wave_to_spawn
wave_to_spawn:  .skip 1

; Of the wave_to_spawn budget, how many should be Pods. The spawner
; spawns Pods first (greedy), then Landers fill the rest of the
; wave_to_spawn count. Re-derived from wave_pod_count_table on _reset
; and at every wave-end.
.global wave_pods_to_spawn
wave_pods_to_spawn: .skip 1

.global enemies_alive
enemies_alive:  .skip 1

; 24-bit score, low byte first (little-endian). 100 points per
; Lander/Mutant kill. Internal range 0..16M; the on-screen renderer
; clamps display at 99999 (5 digits).
.global score_lo
score_lo:       .skip 1
.global score_mid
score_mid:      .skip 1
.global score_hi
score_hi:       .skip 1

; Ship lives. Defender started with 3; bonus ship every 10000 points.
; Top of screen renders up to 3 ship icons regardless of internal value.
.global lives
lives:          .skip 1

; Score threshold of the next bonus-ship award (24-bit, matches score
; width so comparisons stay correct past 65535). Starts at 10000.
.global next_bonus_lo
next_bonus_lo:  .skip 1
.global next_bonus_mid
next_bonus_mid: .skip 1
.global next_bonus_hi
next_bonus_hi:  .skip 1

; Per-difficulty high scores, 24-bit each, indexed by difficulty
; (0=LOW, 1=MED, 2=HIGH). Loaded from EEPROM at boot, written back
; only on game-over transition when the current run beat the record.
.global high_scores
high_scores:    .skip 9

; 0 = playing, 1 = game over (player out of lives; main_loop renders the
; game-over screen and waits for B to soft-reset).
.global game_state
game_state:     .skip 1

; Periodic-spawn countdown timer. Decrements once per frame; on hit
; 0, try_spawn_enemy fires and the timer reloads from
; spawn_interval_table[difficulty]. Replaces the old uniform
; frame_counter & 0x1F gate so LOW/MED/HIGH can have different
; cadences without relying on power-of-2 masks.
.global spawn_countdown
spawn_countdown: .skip 1

; Test-only disable for the planet-destruction watcher. Production
; code keeps it 0; the unit-test harness sets it to 1 in
; sim_clear_state_minimal so the per-event check is a no-op when the
; test world has no humanoids. Without this, every test that places
; an entity without populating slots 1..8 would trip planet
; destruction and turn its placed Landers into Mutants.
.global planet_check_disabled
planet_check_disabled: .skip 1

; All-Mutant mode flag. 0 = normal (Landers / Pods can spawn,
; humanoids exist on the ground); 1 = planet destroyed (every Lander
; in flight turned Mutant, spawner installs Mutants only). Set when
; the last humanoid is consumed (shot, carried to the top, or fell
; off-screen); cleared at wave-end when a fresh batch of humanoids is
; respawned. Drives the Williams "all-Mutant skies" feel after the
; planet falls.
.global planet_destroyed
planet_destroyed: .skip 1

; 8-bit Galois LFSR state for in-game randomness (Swarmer turn
; decisions, future jitter sources). Seeded non-zero in _reset so the
; sequence has period 255; never let it reach 0 or it gets stuck.
.global rng_state
rng_state:      .skip 1

; Boot-warp grace period: the boot Lander placed at _reset doesn't run
; its AI for the first N frames after main_loop starts, so the player
; has a moment to see "there's a Lander up there at y=10" before it
; starts seeking. Without this, the SFX_START 30-frame jingle elapses
; with the Lander's AI running silently underneath, and the player
; finds it already mid-descent and seeking when control returns. The
; counter ticks down to 0 in main_alive's top-of-frame block.
.global boot_warp_frames
boot_warp_frames: .skip 1

; Magic byte distinguishing soft-reset (rjmp _reset from game-over) from
; cold power-on. SRAM survives the rjmp, so we can write 0xA5 here just
; before the rjmp; _reset reads and clears it. Any value other than 0xA5
; means cold boot. Risk on cold boot: 1/256 chance random SRAM matches
; the magic and we falsely take the soft-reset path. Consequence is
; benign — the soft-reset path forces the splash to show, which is also
; the cold-boot default (the only thing it bypasses is the
; "hold B at boot to skip splash" shortcut).
.global soft_reset_magic
soft_reset_magic: .skip 1

; Difficulty (0=LOW, 1=MED, 2=HIGH). Wave sizes scale via wave_sizes_table.
.global difficulty
difficulty:     .skip 1

; Title-screen cursor position (0=LOW, 1=MED, 2=HIGH, 3=RESET). Tracked
; separately from `difficulty` because RESET is a fourth selectable row
; that doesn't correspond to a play difficulty.
.global title_selection
title_selection: .skip 1

; Frame counter for hold-B-to-reset on the title's RESET row. Reset to
; 0 on every B-released frame; reaches 60 (~1 s @ 60 Hz) on a sustained
; hold, at which point the table wipe fires once.
.global reset_hold_frames
reset_hold_frames: .skip 1

; Title-screen edge-detect buffer for input_read returns.
.global prev_buttons
prev_buttons:   .skip 1

; Test-only override: when non-zero (set via sim_mem_w before _reset runs),
; the title screen is skipped regardless of B button state. Lets the test
; harness avoid the splash UI without depending on simavr's port-IRQ /
; pull-up interaction at the very first cycles after reset.
.global skip_title_flag
skip_title_flag: .skip 1

; 64 entity slots, 3 bytes each. Packed layout:
;   byte 0: bit 7   = active
;           bits 4-6 = type (0..7)
;           bits 0-3 = state (per-type)
;   byte 1: world_x (0..255, world wraps at 256)
;   byte 2: bits 6-7 = state2 (per-type extra state)
;           bits 0-5 = y (0..63 screen rows)
.equ ENTITY_SLOTS, 64
.equ ENTITY_SIZE,  3
.equ TYPE_NONE,     0
.equ TYPE_LANDER,   1
.equ TYPE_MUTANT,   2                    ; matches dispatch slot 2
.equ TYPE_POD,      3                    ; matches dispatch slot 3
.equ TYPE_SWARMER,  5                    ; matches dispatch slot 5
.equ TYPE_HUMANOID, 7                    ; matches dispatch slot 7
.equ POD_SWARMER_COUNT, 5                ; Swarmers spawned per Pod death
.global entities
entities:       .skip ENTITY_SLOTS * ENTITY_SIZE

; 4 projectile slots, 2 bytes each. Packed layout:
;   byte 0: bit 7 = active
;           bit 6 = dx_sign  (0 = +4 framebuffer cols/frame, 1 = -4)
;           bits 0-5 = y (0..63)
;   byte 1: x  (signed; MSB=1 means off-screen, deactivate)
; spawn_x is derived from dx_sign at render time (68 if positive, 59 if
; negative) — saves the byte by trading flash for RAM.
.equ PROJ_SLOTS, 4
.equ PROJ_SIZE,  2
.global projectiles
projectiles:    .skip PROJ_SLOTS * PROJ_SIZE

; --- Vector table ---
; Each vector slot is 4 bytes (room for one JMP) on >8 KB AVRs.
;   slot 1  byte 0x00 — RESET
;   slot 18 byte 0x44 — TIMER1 COMPA (frame-rate cap; sets frame_ready)
;   slot 33 byte 0x80 — TIMER3 COMPA (BTL audio: toggles PC7 in anti-phase)
.section .vectors, "ax", @progbits
.org 0x00
    rjmp _reset                                ; slot 1 (RESET)
.org 0x44                                      ; (18 - 1) * 4 — slot 18
    rjmp _timer1_compa_isr                     ; TIMER1 COMPA
.org 0x80                                      ; (33 - 1) * 4 — slot 33
    rjmp _timer3_compa_isr                     ; TIMER3 COMPA

.section .text, "ax", @progbits

; Timer1 OC1A interrupt — fires once per frame at 60 Hz. Sets the
; frame_ready flag; main_loop's sleep loop spins until the flag is set,
; ignoring (but consuming) any wakes from Timer3's audio interrupt.
.global _timer1_compa_isr
_timer1_compa_isr:
    push  r0
    in    r0, _SFR_IO_ADDR(SREG)
    push  r24
    ldi   r24, 1
    sts   frame_ready, r24
    pop   r24
    out   _SFR_IO_ADDR(SREG), r0
    pop   r0
    reti

; Timer3 OC3A interrupt — fires on every compare match. Hardware toggles
; OC3A on PC6; this ISR toggles PC7 in lock-step so the two pins drive
; the piezo BTL (anti-phase, ~2× the half-volume single-pin swing).
;
; sbi/cbi/PINx-bit-write don't touch SREG flags, so we can skip the
; SREG save and keep the ISR to just two instructions.
.global _timer3_compa_isr
_timer3_compa_isr:
    sbi   _SFR_IO_ADDR(PINC), 7
    reti

.global _reset
_reset:
    cli                                    ; soft-reset (rjmp from game-over)
                                           ; lands here with IRQs enabled.
    ldi   r16, lo8(RAMEND)
    out   _SFR_IO_ADDR(SPL), r16
    ldi   r16, hi8(RAMEND)
    out   _SFR_IO_ADDR(SPH), r16

    ; Hardware reset leaves SRAM undefined. Zero the flag bytes that
    ; need to start at 0 for deterministic boot — the test harness can
    ; override skip_title_flag *after* this (sim_boot runs a handful of
    ; cycles, then sim_mem_w's the flag, then continues warmup).
    clr   r16
    sts   skip_title_flag, r16

    ; Early one-shots use 22-bit `call` because the firmware has grown
    ; past the 13-bit rcall range from _reset to these external symbols.
    call  oled_init
    call  input_init
    call  audio_init

    ldi   r24, 28
    sts   sprite_y, r24
    ldi   r24, 0
    sts   ship_facing, r24
    sts   scroll_offset, r24
    sts   frame_counter, r24
    sts   death_flash, r24
    sts   spawn_pos_idx, r24
    sts   fire_cooldown, r24
    ; Clear all 4 projectile active flags
    sts   projectiles + 0*PROJ_SIZE, r24
    sts   projectiles + 1*PROJ_SIZE, r24
    sts   projectiles + 2*PROJ_SIZE, r24
    sts   projectiles + 3*PROJ_SIZE, r24
    ; Clear all entity active flags
    ldi   r28, lo8(entities)
    ldi   r29, hi8(entities)
    ldi   r25, ENTITY_SLOTS
1:  st    Y, r24
    adiw  r28, ENTITY_SIZE
    dec   r25
    brne  1b

    ; Spawn one Lander at world_x=100, y=10 (slot 0). This boot Lander
    ; counts toward wave 1's total — the periodic spawner brings the rest.
    ldi   r24, (1<<7) | (TYPE_LANDER<<4)   ; active, type=Lander, state=0
    sts   entities + 0, r24
    ldi   r24, 100
    sts   entities + 1, r24                ; world_x
    ldi   r24, 10
    sts   entities + 2, r24                ; y (state2 = 0)
    ; Hold the boot Lander still for ~half a second after main_loop
    ; starts (covers the SFX_START jingle and gives the player time to
    ; orient before AI seek begins).
    ldi   r24, 30
    sts   boot_warp_frames, r24

    ; Seed the LFSR. Any non-zero value works; 0x4C was picked because
    ; it produces a non-degenerate first ~10 outputs and is easy to
    ; spot in a hex dump.
    ldi   r24, 0x4C
    sts   rng_state, r24

    ; Wave 1 setup: 1 boot Lander already alive; the rest derive from
    ; wave_sizes_table[difficulty][0]. Default difficulty = MED.
    ldi   r24, 1
    sts   wave_number, r24
    sts   enemies_alive, r24
    sts   difficulty, r24                  ; 1 = MED
    sts   title_selection, r24             ; cursor starts on MED
    clr   r25
    sts   reset_hold_frames, r25
    rcall lookup_wave_size                 ; r24 = wave 1 size for current difficulty
    dec   r24                              ; subtract the boot Lander
    sts   wave_to_spawn, r24
    ; Score starts at 0; lives starts at 3 (Defender convention).
    clr   r24
    sts   score_lo, r24
    sts   score_mid, r24
    sts   score_hi, r24
    sts   game_state, r24
    ldi   r24, 3
    sts   lives, r24
    ; (Bonus-ship threshold init moved to skip_title_screen below — at
    ; this point `difficulty` is still the MED default. title_loop may
    ; change it, so the threshold must be derived AFTER title returns.)

    ; Spawn 8 Humanoids on the ground (slots 1..8) at world_x = 16, 48, 80, ...
    ldi   r28, lo8(entities + 1*ENTITY_SIZE)
    ldi   r29, hi8(entities + 1*ENTITY_SIZE)
    ldi   r25, 16                          ; world_x seed
    ldi   r24, 8                           ; count
hum_init_loop:
    ldi   r26, (1<<7) | (TYPE_HUMANOID<<4)
    st    Y+, r26
    st    Y+, r25                          ; world_x
    ldi   r26, 48                          ; y (page 6, just above mountains)
    st    Y+, r26
    subi  r25, -32                         ; world_x += 32 (mod 256 wraps)
    dec   r24
    brne  hum_init_loop

    ; --- Timer1: CTC, prescaler 64, OCR1A=4165 → 16e6/64/4166 = 59.99 Hz ---
    ; Compare-A interrupt fires every ~16.67 ms; main_loop sleeps on it.
    ldi   r24, 0
    sts   _SFR_MEM_ADDR(TCCR1A), r24       ; WGM11:WGM10 = 00
    ldi   r24, (1<<WGM12) | (1<<CS11) | (1<<CS10)
    sts   _SFR_MEM_ADDR(TCCR1B), r24       ; CTC + clk/64
    ldi   r24, hi8(4165)
    sts   _SFR_MEM_ADDR(OCR1AH), r24       ; high byte first (16-bit temp reg)
    ldi   r24, lo8(4165)
    sts   _SFR_MEM_ADDR(OCR1AL), r24
    ldi   r24, 0
    sts   _SFR_MEM_ADDR(TCNT1H), r24
    sts   _SFR_MEM_ADDR(TCNT1L), r24
    ldi   r24, (1<<OCIE1A)
    sts   _SFR_MEM_ADDR(TIMSK1), r24       ; enable Compare-A interrupt

    ; SMCR: SE=1, SM2:0=000 → SLEEP_IDLE.  Set once and leave armed; sleep
    ; only enters when the SLEEP instruction executes.
    ldi   r24, (1<<SE)
    out   _SFR_IO_ADDR(SMCR), r24

    sei                                    ; arm interrupts last

    ; ---- EEPROM high-score bootstrap ----
    ; EEPROM layout: byte 0 = magic 0xDF; bytes 1..9 = three 24-bit
    ; high scores (LOW, MED, HIGH). On first boot (or after a wipe via
    ; the title's RESET row), the magic is missing and the EEPROM cells
    ; read 0xFF; we lay down a fresh 0xDF + 9 zero bytes. On every
    ; subsequent boot we just slurp the 9 score bytes into BSS.
    call  eeprom_load_high_scores

    ; Splash screen — pick difficulty. Test harness can set skip_title_flag
    ; via sim_mem_w before warmup; on cold-boot hardware, holding B at
    ; reset also bypasses the splash for a quick-start. The cold-boot
    ; B-held shortcut MUST be skipped after a soft-reset from game-over,
    ; because the player is still physically holding B from the press
    ; that triggered the reset; otherwise the splash flashes for one
    ; frame and a new game starts immediately. soft_reset_magic survives
    ; the rjmp _reset (SRAM is preserved); we read and consume it here.
    lds   r25, soft_reset_magic
    clr   r24
    sts   soft_reset_magic, r24
    cpi   r25, 0xA5
    breq  show_title_force                 ; soft reset → always show title
    lds   r24, skip_title_flag
    cpi   r24, 0
    brne  skip_title_screen                ; test harness override
    in    r24, _SFR_IO_ADDR(PINB)
    sbrc  r24, 4                           ; B held = bit 4 clear
    rjmp  show_title_force                 ; B not held → show title
    rjmp  skip_title_screen                ; B held → skip
show_title_force:
    rcall title_loop
skip_title_screen:
    ; Re-derive wave 1 size from the chosen difficulty (boot Lander
    ; counts as 1, subtract from table value).
    rcall lookup_wave_size
    dec   r24
    sts   wave_to_spawn, r24

    ; Pod allotment for wave 1 from per-difficulty table.
    rcall lookup_pod_count
    sts   wave_pods_to_spawn, r24

    ; Seed the spawn-cadence countdown so the first enemy doesn't
    ; arrive immediately — the player has one full interval to settle
    ; in after the splash dismisses.
    rcall lookup_spawn_interval
    sts   spawn_countdown, r24

    ; First bonus-ship threshold from the per-difficulty table:
    ; LOW=3000, MED=6000, HIGH=10000 (Williams Defender). Done HERE,
    ; not pre-title, so it picks up the difficulty the player chose
    ; rather than the MED default that was in place at _reset entry.
    rcall lookup_bonus_increment           ; r25:r24 = increment (hi byte=0)
    sts   next_bonus_lo, r24
    sts   next_bonus_mid, r25
    clr   r24
    sts   next_bonus_hi, r24

    ; Boot jingle — short ascending arpeggio. Plays during the first
    ; few main_loop iterations after the title is dismissed.
    ldi   r24, 5                           ; SFX_START
    call  audio_play

.global main_loop
main_loop:
    ; Frame tick: bump frame counter (used as divider for drift timing)
    lds   r25, frame_counter
    inc   r25
    sts   frame_counter, r25

    ; Advance any active SFX by one frame
    call  audio_tick

    ; Game-over branch: if player is out of lives, render the GAME OVER
    ; screen and wait for B to soft-reset back to title. Use the inverted
    ; brne→rjmp pattern because main_game_over_path is well outside the
    ; 7-bit conditional-branch range.
    lds   r24, game_state
    cpi   r24, 0
    breq  main_alive
    rjmp  main_game_over_path
main_alive:

    ; Tick the extended respawn-invuln counter. While > 0 the ship is
    ; either exploding (>60) or blinking (<=60); either way collisions
    ; are suppressed by ship_collision_check.
    lds   r25, respawn_invuln
    cpi   r25, 0
    breq  ri_tick_done
    dec   r25
    sts   respawn_invuln, r25
ri_tick_done:

    ; Tick the boot-warp counter. While > 0, update_lander short-circuits
    ; for slot 0 so the boot Lander stays parked at its placement spot.
    lds   r25, boot_warp_frames
    cpi   r25, 0
    breq  bw_tick_done
    dec   r25
    sts   boot_warp_frames, r25
bw_tick_done:

    ; Periodic enemy spawn — gated by spawn_countdown, which reloads
    ; from spawn_interval_table[difficulty] after every fire. LOW gets
    ; 96 frames (~1.6 s) between spawns; HIGH stays at 32 (~0.53 s,
    ; Williams pace). Decrement-and-fire is a per-difficulty cadence
    ; with no power-of-2 mask restriction.
    lds   r25, spawn_countdown
    cpi   r25, 0
    breq  sc_fire
    dec   r25
    sts   spawn_countdown, r25
    rjmp  no_spawn_check
sc_fire:
    rcall try_spawn_enemy
    rcall lookup_spawn_interval
    sts   spawn_countdown, r24
no_spawn_check:

    call  input_read                     ; r24 = button bitmask
    ; Suppress all input during the explosion phase (respawn_invuln in
    ; (60, 90]) — the player has no ship to control yet.
    lds   r25, respawn_invuln
    cpi   r25, 61
    brlo  input_passthrough
    clr   r24
input_passthrough:

    lds   r17, sprite_y

    ; UP (bit 0) — clamp at 8 so sprite stays below the radar bar in page 0
    sbrs  r24, 0
    rjmp  not_up
    cpi   r17, 9
    brlo  not_up
    dec   r17
not_up:

    ; DOWN (bit 1) — clamp at 47 so ship can't overlap mountains in page 7
    sbrs  r24, 1
    rjmp  not_down
    cpi   r17, 47
    brsh  not_down
    inc   r17
not_down:

    ; LEFT (bit 2): face left, scroll--
    sbrs  r24, 2
    rjmp  not_left
    ldi   r25, 1
    sts   ship_facing, r25
    lds   r25, scroll_offset
    dec   r25
    sts   scroll_offset, r25
not_left:

    ; RIGHT (bit 3): face right, scroll++
    sbrs  r24, 3
    rjmp  not_right
    ldi   r25, 0
    sts   ship_facing, r25
    lds   r25, scroll_offset
    inc   r25
    sts   scroll_offset, r25
not_right:

    sts   sprite_y, r17

    ; --- Update all projectile slots: move if active, deactivate if off-screen ---
    ldi   r28, lo8(projectiles)
    ldi   r29, hi8(projectiles)
    ldi   r18, PROJ_SLOTS
proj_update_loop:
    ld    r25, Y                         ; r25 = byte 0
    sbrs  r25, 7                         ; skip ahead if active (bit 7 set)
    rjmp  proj_update_next                ; inactive slot
    ldd   r26, Y+1                       ; r26 = x
    sbrs  r25, 6                         ; bit 6 = dx_sign; set = -4
    rjmp  proj_dx_pos
    subi  r26, 4                         ; dx_sign=1: x -= 4
    rjmp  proj_check_oob
proj_dx_pos:
    subi  r26, 0xFC                      ; dx_sign=0: x += 4 (=  -(-4))
proj_check_oob:
    std   Y+1, r26
    sbrs  r26, 7                         ; MSB set = off-screen
    rjmp  proj_update_next
    clr   r25
    st    Y, r25                         ; clear byte 0 → active=0
proj_update_next:
    adiw  r28, PROJ_SIZE
    dec   r18
    brne  proj_update_loop

    ; --- Auto-fire: gated by fire_cooldown so a held B fires once every
    ; FIRE_COOLDOWN_RELOAD+1 frames. Releasing B clears the cooldown so
    ; the next tap fires immediately. ---
    sbrs  r24, 5
    rjmp  b_released
    lds   r25, fire_cooldown
    cpi   r25, 0
    breq  b_can_fire
    dec   r25
    sts   fire_cooldown, r25
    rjmp  no_b_press
b_released:
    clr   r25
    sts   fire_cooldown, r25
    rjmp  no_b_press
b_can_fire:
    ldi   r28, lo8(projectiles)
    ldi   r29, hi8(projectiles)
    ldi   r18, PROJ_SLOTS
find_slot_loop:
    ld    r25, Y
    cpi   r25, 0
    breq  found_free_slot
    adiw  r28, PROJ_SIZE
    dec   r18
    brne  find_slot_loop
    rjmp  no_b_press                     ; all slots full
found_free_slot:
    ; Build byte 0: active(7) | (facing<<6) | (proj_y & 0x3F)
    lds   r25, sprite_y
    subi  r25, -4                        ; r25 = sprite_y + 4 (= proj_y)
    andi  r25, 0x3F                      ; ensure 6 bits
    ori   r25, 0x80                      ; active bit
    lds   r19, ship_facing
    cpi   r19, 0
    breq  no_dx_bit
    ori   r25, 0x40                      ; dx_sign = 1 (facing left)
no_dx_bit:
    st    Y, r25
    ; Set spawn x in byte 1 (68 for right, 59 for left)
    cpi   r19, 0
    brne  spawn_x_left
    ldi   r25, 68
    rjmp  spawn_store_x
spawn_x_left:
    ldi   r25, 59
spawn_store_x:
    std   Y+1, r25
    ; Successful spawn — reload cooldown and trigger fire SFX
    ldi   r25, FIRE_COOLDOWN_RELOAD
    sts   fire_cooldown, r25
    ldi   r24, 1                          ; SFX_FIRE
    call  audio_play
no_b_press:

    ; --- Update game entities (Landers, etc.) ---
    rcall update_entities

    ; --- Collision: beams vs. enemies ---
    rcall collision_check

    ; --- Ship vs. enemy collision (player death) ---
    rcall ship_collision_check

    ; --- Drain any deferred SFX queued from inner collision loops ---
    lds   r24, pending_sfx
    cpi   r24, 0
    breq  no_pending_sfx
    call  audio_play
    clr   r24
    sts   pending_sfx, r24
no_pending_sfx:

    ; --- Wave-end check: when nothing is left to spawn AND nothing's alive,
    ;     advance the wave, queue SFX_WAVE_CHANGE, reload wave_to_spawn. ---
    lds   r24, wave_to_spawn
    cpi   r24, 0
    brne  wave_in_progress
    lds   r24, enemies_alive
    cpi   r24, 0
    brne  wave_in_progress
    ; Wave done. Increment wave_number (cap at 8 since the table has only 8
    ; rows; values clamp anyway via lookup_wave_size's index cap).
    lds   r24, wave_number
    cpi   r24, 8
    brsh  wave_at_max
    inc   r24
    sts   wave_number, r24
wave_at_max:
    ; New wave_to_spawn from the difficulty table.
    rcall lookup_wave_size
    sts   wave_to_spawn, r24
    ; Per-wave Pod allotment.
    rcall lookup_pod_count
    sts   wave_pods_to_spawn, r24
    ; New planet — clear the destroyed flag and respawn humanoids.
    ; If the previous wave never tripped destruction, this is just an
    ; idempotent overwrite (8 humanoids in slots 1..8 already exist;
    ; they get rewritten with the same values). Cheap.
    clr   r24
    sts   planet_destroyed, r24
    rcall respawn_humanoids
    ldi   r24, 6                         ; SFX_WAVE_CHANGE
    call  audio_play
    ; Kick the new wave: spawn one immediately so the player sees a fresh
    ; enemy without waiting for the next periodic fc==128 tick.
    rcall try_spawn_enemy
wave_in_progress:

    ; --- Render this frame ---
    rcall clear_fb
    rcall render_horizon
    rcall render_radar
    rcall render_wave_indicator
    rcall render_lives
    rcall render_score
    rcall render_projectiles
    rcall render_entities

    ; --- Ship / explosion rendering, gated on respawn_invuln phase ---
    lds   r25, respawn_invuln
    cpi   r25, 0
    breq  draw_ship_normal               ; not dying → normal blit
    cpi   r25, 61
    brsh  draw_explosion_phase           ; 61..90 → fragments, no ship
    ; 1..60: blink. Skip the ship every other 8-frame slot so the
    ; player sees a 4-Hz flicker that signals "you're invulnerable".
    sbrc  r25, 3
    rjmp  skip_ship_render
draw_ship_normal:
    ; Z = ship_right + facing*8
    lds   r25, ship_facing
    ldi   r30, lo8(ship_right)
    ldi   r31, hi8(ship_right)
    lsl   r25
    lsl   r25
    lsl   r25
    add   r30, r25
    ldi   r25, 0
    adc   r31, r25
    ; Blit ship: x locked at 60, y from sprite_y (reload — entity
    ; render may have overwritten r17 with another sprite's y).
    ldi   r16, 60
    lds   r17, sprite_y
    call  blit_8x8
    rjmp  skip_ship_render
draw_explosion_phase:
    rcall render_explosion
skip_ship_render:

    ; --- Death flash: if active, invert the entire framebuffer ---
    rcall invert_if_dead

    ; --- Render ---
    ldi   r30, lo8(framebuffer)
    ldi   r31, hi8(framebuffer)
    call  oled_render

    rjmp  ml_wait

main_game_over_path:
    rcall render_game_over
    ldi   r30, lo8(framebuffer)
    ldi   r31, hi8(framebuffer)
    call  oled_render
    call  input_read                     ; r24 = current button mask
    lds   r25, prev_buttons              ; r25 = last frame's mask
    sts   prev_buttons, r24              ; remember for next frame
    com   r25                            ; r25 = ~prev
    and   r25, r24                       ; r25 = newly-pressed edges
    sbrs  r25, 5                         ; B edge?
    rjmp  ml_wait
    ; B-edge: arm the soft-reset magic so _reset shows the splash
    ; instead of running the cold-boot "B-held = skip splash" path
    ; (B is almost certainly still held from the press we just
    ; detected as an edge).
    ldi   r25, 0xA5
    sts   soft_reset_magic, r25
    rjmp  _reset

    ; --- Frame cap: sleep until Timer1 says the next frame is due. ---
    ; The audio engine fires Timer3 interrupts mid-frame to drive PC7 in
    ; anti-phase with PC6; those wakes return to the instruction after
    ; sleep with frame_ready still 0, so we loop back to sleep and resume.
ml_wait:
    sleep
    lds   r25, frame_ready
    cpi   r25, 0
    breq  ml_wait
    clr   r25
    sts   frame_ready, r25
    rjmp  main_loop

; ============================================================================
clear_fb:
    ldi   r30, lo8(framebuffer)
    ldi   r31, hi8(framebuffer)
    clr   r25
    ldi   r24, 4
1:  clr   r26
2:  st    Z+, r25
    dec   r26
    brne  2b
    dec   r24
    brne  1b
    ret

; ============================================================================
; render_horizon: fill framebuffer page 7 (bottom 8 rows) with a mountain
; profile derived from `height_table` and the current scroll_offset.
;
;   for fb_col c in 0..127:
;     world_col = (scroll_offset + c) & 0xFF     ; full 256-col world
;     h         = height_table[world_col]
;     framebuffer[7*128 + c] = mask_table[h]
;
; Clobbers: r24, r25, r26, r27, Z, Y
; ============================================================================
render_horizon:
    ldi   r28, lo8(framebuffer + 7*128)
    ldi   r29, hi8(framebuffer + 7*128)
    ldi   r18, 128                       ; column counter
    clr   r24                            ; c = 0
    lds   r25, scroll_offset
1:
    ; world_col = (scroll + c) mod 256 (uint8 wraps automatically)
    mov   r26, r25
    add   r26, r24

    ; r26 = height_table[world_col]
    ldi   r30, lo8(height_table)
    ldi   r31, hi8(height_table)
    add   r30, r26
    clr   r27
    adc   r31, r27
    lpm   r26, Z

    ; r26 = mask_table[height]
    ldi   r30, lo8(mask_table)
    ldi   r31, hi8(mask_table)
    add   r30, r26
    clr   r27
    adc   r31, r27
    lpm   r26, Z

    st    Y+, r26                        ; framebuffer[7*128 + c] = mask byte
    inc   r24
    dec   r18
    brne  1b
    ret

; ============================================================================
; render_radar: draw a 64-px-wide bar at framebuffer page 0, cols [32, 96),
; with 4:1 compression of the 256-col world.
;   bit 0 = top border
;   bit 6 = mountain dot (height_table[radar_col*4] >= 4)
;   bit 7 = bottom border
;   bit (1 + sprite_y/8)   = ship marker at its altitude
;   bit (1 + lander_y/8)   = lander marker at its altitude (per active lander)
;
; First pass: per-col loop builds borders + mountains + ship marker.
; Second pass: iterate active entities and plot a dot per lander.
;
; Clobbers: r0, r1, r18, r19, r20, r24, r25, r26, r27, Z, Y
; ============================================================================
render_radar:
    ; ship_radar_col = ((scroll + 60) >> 2) & 0x3F
    lds   r19, scroll_offset
    ldi   r20, 60
    add   r19, r20
    lsr   r19
    lsr   r19
    andi  r19, 0x3F                      ; r19 = ship_radar_col

    ; ship_radar_y = min(5, (sprite_y>>3) + 1)  — always above mountain row
    lds   r20, sprite_y
    lsr   r20
    lsr   r20
    lsr   r20
    inc   r20
    cpi   r20, 6
    brlo  ship_y_ok
    ldi   r20, 5
ship_y_ok:
    ldi   r30, lo8(bit_mask_lut)
    ldi   r31, hi8(bit_mask_lut)
    add   r30, r20
    clr   r25
    adc   r31, r25
    lpm   r20, Z                          ; r20 = ship_marker_bit

    ldi   r28, lo8(framebuffer + 32)
    ldi   r29, hi8(framebuffer + 32)
    ldi   r18, 64
    clr   r24                            ; radar_col = 0
1:
    ldi   r26, 0x81                      ; borders

    ; world_col = radar_col * 4
    mov   r25, r24
    lsl   r25
    lsl   r25

    ldi   r30, lo8(height_table)
    ldi   r31, hi8(height_table)
    add   r30, r25
    clr   r27
    adc   r31, r27
    lpm   r25, Z

    cpi   r25, 4
    brlo  2f
    ori   r26, 0x40                      ; mountain
2:
    cp    r24, r19
    brne  3f
    or    r26, r20                       ; ship marker at its altitude
3:
    st    Y+, r26
    inc   r24
    dec   r18
    brne  1b

    ; --- Second pass: plot a dot for each active Lander ---
    ldi   r28, lo8(entities)
    ldi   r29, hi8(entities)
    ldi   r18, ENTITY_SLOTS
rr_ent_loop:
    ld    r24, Y
    sbrs  r24, 7
    rjmp  rr_ent_next                    ; inactive
    mov   r25, r24
    swap  r25
    andi  r25, 0x07
    cpi   r25, TYPE_LANDER
    breq  rr_ent_plot
    cpi   r25, TYPE_MUTANT
    breq  rr_ent_plot
    rjmp  rr_ent_next                    ; only enemies plot on radar
rr_ent_plot:

    ldd   r25, Y+1                       ; world_x
    lsr   r25
    lsr   r25
    andi  r25, 0x3F                      ; lander_radar_col
    ldi   r24, 32
    add   r24, r25                       ; r24 = fb_col for radar dot

    ldd   r25, Y+2
    andi  r25, 0x3F                      ; lander y
    lsr   r25
    lsr   r25
    lsr   r25
    inc   r25                            ; lander_radar_y = (y>>3)+1
    cpi   r25, 6
    brlo  lander_y_ok
    ldi   r25, 5                         ; clamp above mountain row
lander_y_ok:

    push  r28
    push  r29
    push  r18
    rcall draw_pixel                     ; draw_pixel(r24=x, r25=y)
    pop   r18
    pop   r29
    pop   r28
rr_ent_next:
    adiw  r28, ENTITY_SIZE
    dec   r18
    brne  rr_ent_loop
    ret

; ============================================================================
; render_wave_indicator: render the wave number as a single 5x7 digit at
; (0, 0) in the otherwise-empty top-left corner. Wave caps at 8 so a
; single digit always fits. Clobbers blit_8x8's set + r24, r25, Z.
; ============================================================================
render_wave_indicator:
    lds   r24, wave_number
    cpi   r24, 1
    brlo  rwi_done
    cpi   r24, 10
    brsh  rwi_done                       ; safety: > 9 → don't render
    subi  r24, -(FONT_DIGIT_BASE)        ; r24 = 13 + wave_number
    rcall blit_glyph_at_origin
rwi_done:
    ret

; blit_glyph_at_origin: blit font_table[r24] at (0, 0). Helper used by
; the wave indicator and the score (with the score caller adjusting
; r16 between digit blits).
; Inputs:  r24 = glyph index
;          r16 = x  (caller-provided; this helper does not modify it)
;          r17 = y
; Clobbers: blit_8x8's clobber set + Z.
blit_glyph:
    ; offset = index * 8. Fits in one byte for our 23-glyph table.
    lsl   r24
    lsl   r24
    lsl   r24
    ldi   r30, lo8(font_table)
    ldi   r31, hi8(font_table)
    add   r30, r24
    ldi   r24, 0
    adc   r31, r24
    call  blit_8x8
    ret

blit_glyph_at_origin:
    ldi   r16, 0
    ldi   r17, 0
    rjmp  blit_glyph

; ============================================================================
; render_game_over: clear the framebuffer and draw "GAME" + "OVER" on
; two stacked rows, plus the final score below. Player presses B to
; soft-reset back into the title screen.
; ============================================================================
render_game_over:
    rcall clear_fb
    ; "GAME" at row 16, centered: 4 letters × 8 = 32 cols, x_start = 48.
    ldi   r17, 16
    ldi   r16, 48
    ldi   r24, 11                                 ; G
    rcall blit_glyph
    subi  r16, -8
    ldi   r24, FONT_A
    rcall blit_glyph
    subi  r16, -8
    ldi   r24, 8                                  ; M
    rcall blit_glyph
    subi  r16, -8
    ldi   r24, 1                                  ; E
    rcall blit_glyph
    ; "OVER" at row 32.
    ldi   r17, 32
    ldi   r16, 48
    ldi   r24, 6                                  ; O
    rcall blit_glyph
    subi  r16, -8
    ldi   r24, FONT_V
    rcall blit_glyph
    subi  r16, -8
    ldi   r24, 1                                  ; E
    rcall blit_glyph
    subi  r16, -8
    ldi   r24, 4                                  ; R
    rcall blit_glyph
    ; Final score in its normal top-right slot for closure.
    rcall render_score
    ret

; ============================================================================
; render_lives: draw up to 3 ship icons in cols 8/16/24 (between the
; wave-number digit and the radar bar). Caps the visible count at 3 even
; if the player has accumulated more — internal `lives` keeps the real
; count for the bonus-ship convention.
; ============================================================================
render_lives:
    lds   r21, lives                     ; r21 preserved across blit_8x8
    cpi   r21, 4
    brlo  rl_have_count
    ldi   r21, 3                         ; cap visible at 3
rl_have_count:
    cpi   r21, 0
    breq  rl_done
    ldi   r16, 8
    ldi   r17, 0
rl_loop:
    ldi   r30, lo8(ship_right)
    ldi   r31, hi8(ship_right)
    call  blit_8x8
    subi  r16, -8                        ; advance x by 8
    dec   r21
    brne  rl_loop
rl_done:
    ret

; ============================================================================
; render_score: render the 24-bit score as 5 fixed-width digits in the
; top-right corner (page 0, row 0). Each digit at +6 cols → 30 cols wide
; starting at col 99. The internal score range is 0..16M; display is
; clamped at 99999. Thin wrapper around render_score_value.
; ============================================================================
render_score:
    push  r2
    push  r3
    push  r4
    lds   r2, score_lo
    lds   r3, score_mid
    lds   r4, score_hi
    ldi   r16, 99
    ldi   r17, 0
    rcall render_score_value
    pop   r4
    pop   r3
    pop   r2
    ret

; ============================================================================
; render_score_value: render the 24-bit value in r4:r3:r2 as 5 digits at
; (r16, r17). Caller chooses location; used by render_score for in-game
; HUD and by the title screen for the per-difficulty HI rows. Clamps
; values >= 100000 to 99999.
; ============================================================================
render_score_value:
    push  r2
    push  r3
    rcall clamp_score_24_to_99999        ; r3:r2 = min(value, 99999)
    ; Digit 0..3 via repeated subtraction; r16/r17 kept across blit_glyph.
    ldi   r24, lo8(10000)
    ldi   r25, hi8(10000)
    rcall rs_extract_digit
    ldi   r24, lo8(1000)
    ldi   r25, hi8(1000)
    rcall rs_extract_digit
    ldi   r24, 100
    ldi   r25, 0
    rcall rs_extract_digit
    ldi   r24, 10
    ldi   r25, 0
    rcall rs_extract_digit
    ; Digit 4: ones — r2 already in [0..9].
    mov   r24, r2
    subi  r24, -(FONT_DIGIT_BASE)
    rcall blit_glyph
    pop   r3
    pop   r2
    ret

; rs_extract_digit: count how many times r25:r24 fits into r4:r3:r2,
; subtract from r4:r3:r2, then blit that digit at (r16, r17) and advance r16.
; r4:r3:r2 is the running 24-bit score remainder. The subtrahend is at
; most 10000 so its high byte is always 0; r26 holds that zero.
rs_extract_digit:
    ldi   r18, 0                         ; digit value
    ldi   r26, 0                         ; zero high-byte for 24-bit subtract
red_loop:
    cp    r2, r24
    cpc   r3, r25
    cpc   r4, r26
    brlo  red_done                       ; remainder < power → done
    sub   r2, r24
    sbc   r3, r25
    sbc   r4, r26
    inc   r18
    rjmp  red_loop
red_done:
    push  r2
    push  r3
    push  r4
    push  r16
    push  r24
    push  r25
    mov   r24, r18
    subi  r24, -(FONT_DIGIT_BASE)
    rcall blit_glyph
    pop   r25
    pop   r24
    pop   r16
    pop   r4
    pop   r3
    pop   r2
    subi  r16, -6                        ; advance x by 6 px
    ret

; ============================================================================
; clamp_score_24_to_99999: r4:r3:r2 in -> r4:r3:r2 out, clamped to 99999.
; If the 24-bit value is >= 100000, replace with 99999 = 0x01869F so the
; 24-bit decimal extractor produces five '9' digits.
; Clobbers: r24, r25, r26.
; ============================================================================
clamp_score_24_to_99999:
    ; Compare against 100000 = 0x0186A0 (lo=0xA0, mid=0x86, hi=0x01).
    ldi   r24, 0xA0
    ldi   r25, 0x86
    ldi   r26, 0x01
    cp    r2, r24
    cpc   r3, r25
    cpc   r4, r26
    brsh  cs_clamp                       ; >= 100000 -> clamp to 99999
    ret
cs_clamp:
    ; 99999 = 0x01869F.
    ldi   r24, 0x9F
    mov   r2, r24
    ldi   r24, 0x86
    mov   r3, r24
    ldi   r24, 0x01
    mov   r4, r24
    ret

; ============================================================================
; EEPROM persistence for the per-difficulty high-score table.
; Layout (10 bytes starting at EEPROM addr 0):
;   0     : EEPROM_MAGIC (0xDF)
;   1..3  : HI_LOW  (24-bit lo/mid/hi)
;   4..6  : HI_MED
;   7..9  : HI_HIGH
; The magic byte distinguishes our data from virgin EEPROM (0xFF) and
; from data left by an unrelated firmware. Writers always touch only
; what changed, never the whole block — bonus-ship bumps don't burn
; cells.
; ============================================================================
.equ EEPROM_MAGIC, 0xDF

; eeprom_load_high_scores: at boot, validate magic and either lay down a
; fresh empty table or copy the 9 score bytes into BSS.
; Clobbers: r22, r23, r24, r25, X.
.global eeprom_load_high_scores
eeprom_load_high_scores:
    ldi   r24, 0
    ldi   r25, 0
    call  eeprom_read_byte               ; r24 = magic
    cpi   r24, EEPROM_MAGIC
    breq  ehs_load
    ; Fresh / corrupted table: write magic + 9 zero score bytes, then
    ; fall through to load (which will see all zeros).
    ldi   r24, 0
    ldi   r25, 0
    ldi   r22, EEPROM_MAGIC
    call  eeprom_write_byte
    ldi   r23, 9                         ; remaining bytes to clear
    ldi   r24, 1
    ldi   r25, 0
ehs_init_loop:
    push  r23
    push  r24
    push  r25
    ldi   r22, 0
    call  eeprom_write_byte
    pop   r25
    pop   r24
    pop   r23
    adiw  r24, 1
    dec   r23
    brne  ehs_init_loop
ehs_load:
    ; Read 9 bytes from EEPROM addr 1..9 into high_scores[0..8].
    ldi   r26, lo8(high_scores)
    ldi   r27, hi8(high_scores)
    ldi   r23, 9
    ldi   r24, 1
    ldi   r25, 0
ehs_load_loop:
    push  r23
    push  r24
    push  r25
    push  r26
    push  r27
    call  eeprom_read_byte               ; r24 = byte
    pop   r27
    pop   r26
    st    X+, r24
    pop   r25
    pop   r24
    pop   r23
    adiw  r24, 1
    dec   r23
    brne  ehs_load_loop
    ret

; eeprom_save_high_score_for_difficulty: write back the 3-byte slot for
; the difficulty given in r24 (0/1/2). Reads the current values from
; the high_scores BSS (caller updates BSS first, then calls this).
; Clobbers: r0..r3, r22..r27, X.
.global eeprom_save_high_score_for_difficulty
eeprom_save_high_score_for_difficulty:
    ; offset_in_bss   = diff * 3
    ; eeprom_addr     = 1 + diff * 3
    mov   r18, r24
    lsl   r24
    add   r24, r18                       ; r24 = diff * 3
    ldi   r26, lo8(high_scores)
    ldi   r27, hi8(high_scores)
    add   r26, r24
    ldi   r18, 0
    adc   r27, r18                       ; X -> high_scores[diff*3]
    inc   r24                            ; +1 to skip magic byte
    ldi   r25, 0
    ldi   r23, 3                         ; 3 bytes to write
ehssh_loop:
    push  r23
    push  r24
    push  r25
    push  r26
    push  r27
    ld    r22, X
    call  eeprom_write_byte
    pop   r27
    pop   r26
    adiw  r26, 1
    pop   r25
    pop   r24
    pop   r23
    adiw  r24, 1
    dec   r23
    brne  ehssh_loop
    ret

; maybe_save_high_score: called once on the game-over transition. If the
; just-finished run's score exceeds the stored high score for the
; current difficulty, update both BSS and EEPROM. No-op otherwise.
; Clobbers: r0..r4, r18..r27, X.
.global maybe_save_high_score
maybe_save_high_score:
    ; X = high_scores + difficulty * 3
    lds   r24, difficulty
    mov   r18, r24
    lsl   r24
    add   r24, r18
    ldi   r26, lo8(high_scores)
    ldi   r27, hi8(high_scores)
    add   r26, r24
    ldi   r18, 0
    adc   r27, r18
    ; Compare current score (24-bit) against record at X.
    lds   r2, score_lo
    lds   r3, score_mid
    lds   r4, score_hi
    ld    r19, X+
    ld    r20, X+
    ld    r21, X
    ; If score > record, store. Use cp/cpc on (record - score); branch if
    ; record < score (i.e., record was strictly less).
    cp    r19, r2
    cpc   r20, r3
    cpc   r21, r4
    brsh  mshs_done                       ; record >= score → no update
    sbiw  r26, 2                          ; rewind X to base
    st    X+, r2
    st    X+, r3
    st    X, r4
    ; Persist the updated 3-byte slot via EEPROM.
    lds   r24, difficulty
    call  eeprom_save_high_score_for_difficulty
mshs_done:
    ret

; eeprom_wipe_high_scores: zero the BSS table and re-write all 9 score
; bytes in EEPROM (magic preserved). Used by the title's RESET row.
; Clobbers: r22..r27, X.
.global eeprom_wipe_high_scores
eeprom_wipe_high_scores:
    ldi   r26, lo8(high_scores)
    ldi   r27, hi8(high_scores)
    ldi   r23, 9
    clr   r24
ehw_zero_bss:
    st    X+, r24
    dec   r23
    brne  ehw_zero_bss
    ldi   r23, 9
    ldi   r24, 1
    ldi   r25, 0
ehw_loop:
    push  r23
    push  r24
    push  r25
    ldi   r22, 0
    call  eeprom_write_byte
    pop   r25
    pop   r24
    pop   r23
    adiw  r24, 1
    dec   r23
    brne  ehw_loop
    ret

; ============================================================================
; render_projectiles: iterate the 4 slots; for each active slot, derive
; spawn_x from dx_sign and draw the beam to draw_beam.
; Clobbers: r0, r1, r18, r19, r20, r24, r25, r26, r27, Z, Y
; ============================================================================
render_projectiles:
    ldi   r28, lo8(projectiles)
    ldi   r29, hi8(projectiles)
    ldi   r18, PROJ_SLOTS
rp_slot_loop:
    ld    r24, Y                         ; byte 0
    sbrs  r24, 7
    rjmp  rp_next_slot                   ; inactive
    mov   r25, r24
    andi  r25, 0x3F                      ; r25 = y
    ; spawn_x = dx_sign==0 ? 68 : 59
    ldi   r26, 68
    sbrc  r24, 6                         ; if dx_sign set
    ldi   r26, 59
    ldd   r24, Y+1                       ; current x
    push  r28
    push  r29
    push  r18
    rcall draw_beam
    pop   r18
    pop   r29
    pop   r28
rp_next_slot:
    adiw  r28, PROJ_SIZE
    dec   r18
    brne  rp_slot_loop
    ret

; ----------------------------------------------------------------------------
; draw_beam: OR a horizontal line from r24 to r26 (inclusive) at row r25.
; Clobbers: r0, r1, r19, r20, r24, r25, r26, r27, Z, Y
; ----------------------------------------------------------------------------
draw_beam:
    ; Order endpoints so r24 = low, r26 = high
    cp    r24, r26
    brlo  1f
    mov   r19, r24
    mov   r24, r26
    mov   r26, r19
1:
    ; r19 = high - low + 1 (loop count)
    mov   r19, r26
    sub   r19, r24
    inc   r19

    ; Y = framebuffer + (y/8)*128 + r24
    mov   r27, r25
    lsr   r27
    lsr   r27
    lsr   r27                            ; r27 = page
    ldi   r20, 128
    mul   r27, r20
    ldi   r28, lo8(framebuffer)
    ldi   r29, hi8(framebuffer)
    add   r28, r0
    adc   r29, r1
    clr   r1
    add   r28, r24
    clr   r20
    adc   r29, r20

    ; r20 = bit_mask_lut[y & 7]
    andi  r25, 0x07
    ldi   r30, lo8(bit_mask_lut)
    ldi   r31, hi8(bit_mask_lut)
    add   r30, r25
    clr   r20
    adc   r31, r20
    lpm   r20, Z

    ; OR the bit-mask along the row, r19 times
2:
    ld    r25, Y
    or    r25, r20
    st    Y+, r25
    dec   r19
    brne  2b
    ret

bit_mask_lut:
    .byte 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80
    .balign 2

; ----------------------------------------------------------------------------
; draw_pixel: OR a single pixel at (r24, r25) into the framebuffer.
; Clobbers: r0, r1, r24, r25, r26, r27, Z, Y
; ----------------------------------------------------------------------------
draw_pixel:
    mov   r26, r25
    lsr   r26
    lsr   r26
    lsr   r26                            ; r26 = page
    ldi   r28, lo8(framebuffer)
    ldi   r29, hi8(framebuffer)
    ldi   r27, 128
    mul   r26, r27
    add   r28, r0
    adc   r29, r1
    clr   r1
    add   r28, r24
    ldi   r27, 0
    adc   r29, r27

    andi  r25, 0x07
    ldi   r30, lo8(bit_mask_lut)
    ldi   r31, hi8(bit_mask_lut)
    add   r30, r25
    clr   r27
    adc   r31, r27
    lpm   r26, Z

    ld    r27, Y
    or    r27, r26
    st    Y, r27
    ret

; ============================================================================
; try_spawn_lander: find the first inactive entity slot. If found, install a
; Lander with world_x = spawn_pos_idx*32 + 16 and y = 10, then advance the
; index. If all slots are full, return without spawning.
;
; Clobbers: r18, r24, r25, Y
; ============================================================================

; blit_letter: blit font_table[r24] at (r16, r17). Advances r16 by 8 so
; sequential calls render a string left to right. Clobbers blit_8x8's
; clobber set + Z; preserves r16/r17 (with r16 advanced).
blit_letter:
    push  r16
    push  r17
    mov   r30, r24
    ldi   r31, 0
    lsl   r30
    rol   r31
    lsl   r30
    rol   r31
    lsl   r30
    rol   r31
    subi  r30, lo8(-(font_table))
    sbci  r31, hi8(-(font_table))
    call  blit_8x8
    pop   r17
    pop   r16
    subi  r16, -8                          ; advance x by 8
    ret

; render_title: clear-then-paint of the splash screen.
;   Row  8-15: "DEFENDER" centered (cols 32..95)
;   Row 24-31: "LOW   xxxxx"   (label x=28, hi-score 5 digits at x=72)
;   Row 32-39: "MED   xxxxx"
;   Row 40-47: "HIGH  xxxxx"
;   Row 48-55: "RESET"
;   Selector arrow at col 20, y = 24 + title_selection*8.
render_title:
    rcall clear_fb
    ; --- DEFENDER header ---
    ldi   r16, 32
    ldi   r17, 8
    ldi   r24, 0
    rcall blit_letter
    ldi   r24, 1
    rcall blit_letter
    ldi   r24, 2
    rcall blit_letter
    ldi   r24, 1
    rcall blit_letter
    ldi   r24, 3
    rcall blit_letter
    ldi   r24, 0
    rcall blit_letter
    ldi   r24, 1
    rcall blit_letter
    ldi   r24, 4
    rcall blit_letter
    ; --- LOW row + HI score for difficulty 0 ---
    ldi   r16, 28
    ldi   r17, 24
    ldi   r24, 5
    rcall blit_letter
    ldi   r24, 6
    rcall blit_letter
    ldi   r24, 7
    rcall blit_letter
    lds   r2, high_scores + 0
    lds   r3, high_scores + 1
    lds   r4, high_scores + 2
    ldi   r16, 72
    ldi   r17, 24
    rcall render_score_value
    ; --- MED row + HI score for difficulty 1 ---
    ldi   r16, 28
    ldi   r17, 32
    ldi   r24, 8
    rcall blit_letter
    ldi   r24, 1
    rcall blit_letter
    ldi   r24, 0
    rcall blit_letter
    lds   r2, high_scores + 3
    lds   r3, high_scores + 4
    lds   r4, high_scores + 5
    ldi   r16, 72
    ldi   r17, 32
    rcall render_score_value
    ; --- HIGH row + HI score for difficulty 2 ---
    ldi   r16, 28
    ldi   r17, 40
    ldi   r24, 9
    rcall blit_letter
    ldi   r24, 10
    rcall blit_letter
    ldi   r24, 11
    rcall blit_letter
    ldi   r24, 9
    rcall blit_letter
    lds   r2, high_scores + 6
    lds   r3, high_scores + 7
    lds   r4, high_scores + 8
    ldi   r16, 72
    ldi   r17, 40
    rcall render_score_value
    ; --- RESET row ---
    ldi   r16, 28
    ldi   r17, 48
    ldi   r24, 4                            ; R
    rcall blit_letter
    ldi   r24, 1                            ; E
    rcall blit_letter
    ldi   r24, FONT_S
    rcall blit_letter
    ldi   r24, 1                            ; E
    rcall blit_letter
    ldi   r24, FONT_T
    rcall blit_letter
    ; --- Selector arrow at col 20, y = 24 + selection*8 ---
    ldi   r16, 20
    lds   r24, title_selection
    lsl   r24
    lsl   r24
    lsl   r24
    subi  r24, -24
    mov   r17, r24
    ldi   r24, 12                           ; ARROW
    rcall blit_letter
    ret

; title_loop: render the splash screen, edge-detect UP/DOWN/B button
; presses to choose difficulty, exit on B. Sleep-paced via Timer1 like
; main_loop. Returns once the player confirms.
title_loop:
    ; Sample current input as the prev-buttons baseline so any button
    ; held when title_loop is entered (e.g., B still held after a
    ; game-over → soft-reset transition) doesn't register as a fresh
    ; edge on frame 1 and instantly confirm a difficulty selection.
    call  input_read
    sts   prev_buttons, r24
    clr   r24
    sts   reset_hold_frames, r24
title_frame:
    rcall render_title
    ldi   r30, lo8(framebuffer)
    ldi   r31, hi8(framebuffer)
    call  oled_render
    call  input_read                       ; r24 = current pressed mask
    mov   r18, r24                         ; r18 = current mask (kept for hold check)
    lds   r17, prev_buttons
    sts   prev_buttons, r24
    com   r17
    and   r17, r24                         ; r17 = newly-pressed edges

    ; UP edge: move selector up (clamp at 0)
    sbrs  r17, 0
    rjmp  title_no_up
    lds   r25, title_selection
    cpi   r25, 0
    breq  title_no_up
    dec   r25
    sts   title_selection, r25
title_no_up:
    ; DOWN edge: move selector down (clamp at 3)
    sbrs  r17, 1
    rjmp  title_no_down
    lds   r25, title_selection
    cpi   r25, 3
    brsh  title_no_down
    inc   r25
    sts   title_selection, r25
title_no_down:
    ; B handling. On RESET row (selection==3) we use a hold-counter so
    ; an accidental tap can't wipe the table. On the difficulty rows
    ; the existing edge-press confirms immediately.
    lds   r25, title_selection
    cpi   r25, 3
    brne  title_confirm_check

    ; RESET row: count consecutive frames with B held; at 60 frames,
    ; wipe and reset the counter so the action fires once per hold.
    sbrs  r18, 5                           ; B not held → reset counter
    rjmp  title_reset_clear
    lds   r25, reset_hold_frames
    cpi   r25, 60
    brsh  title_wait                       ; already fired this hold
    inc   r25
    sts   reset_hold_frames, r25
    cpi   r25, 60
    brne  title_wait
    call  eeprom_wipe_high_scores
    rjmp  title_wait
title_reset_clear:
    clr   r25
    sts   reset_hold_frames, r25
    rjmp  title_wait

title_confirm_check:
    ; Difficulty row (0..2): B edge-press confirms — set difficulty
    ; from selection and exit.
    sbrs  r17, 5
    rjmp  title_wait
    lds   r25, title_selection
    sts   difficulty, r25
    ret
title_wait:
    sleep
    lds   r25, frame_ready
    cpi   r25, 0
    breq  title_wait
    clr   r25
    sts   frame_ready, r25
    rjmp  title_frame

; lookup_wave_size: returns r24 = wave_sizes_table[difficulty][min(wave_number-1, 7)].
; Reads wave_number and difficulty; uses Z. Clobbers r24, r25, Z.
lookup_wave_size:
    lds   r25, wave_number
    cpi   r25, 0
    brne  lws_have_wave
    ldi   r25, 1
lws_have_wave:
    dec   r25                              ; r25 = wave_number - 1
    cpi   r25, 8
    brlo  lws_clamped
    ldi   r25, 7                           ; cap at index 7
lws_clamped:
    lds   r24, difficulty
    lsl   r24
    lsl   r24
    lsl   r24                              ; r24 = difficulty * 8
    add   r24, r25                         ; offset within table
    ldi   r30, lo8(wave_sizes_table)
    ldi   r31, hi8(wave_sizes_table)
    add   r30, r24
    ldi   r24, 0
    adc   r31, r24
    lpm   r24, Z
    ret

; lookup_pod_count: returns r24 = wave_pod_count_table[difficulty][min(wave_number-1, 7)].
; Same indexing logic as lookup_wave_size; just a different table base.
lookup_pod_count:
    lds   r25, wave_number
    cpi   r25, 0
    brne  lpc_have_wave
    ldi   r25, 1
lpc_have_wave:
    dec   r25
    cpi   r25, 8
    brlo  lpc_clamped
    ldi   r25, 7
lpc_clamped:
    lds   r24, difficulty
    lsl   r24
    lsl   r24
    lsl   r24
    add   r24, r25
    ldi   r30, lo8(wave_pod_count_table)
    ldi   r31, hi8(wave_pod_count_table)
    add   r30, r24
    ldi   r24, 0
    adc   r31, r24
    lpm   r24, Z
    ret

; ============================================================================
; check_planet_destruction: scan entity slots once per frame; if no
; humanoids remain (active humanoid entities + carrying Landers all 0)
; and the planet wasn't already destroyed, set planet_destroyed and
; instantly transform every empty (non-carrying) Lander on screen into
; a Mutant. The all-Mutant spawner mode then takes over for the rest
; of the wave; wave_at_max re-spawns humanoids and clears the flag at
; wave end.
; Clobbers: r17, r18, r19, r24, r25, X (via transform_landers_to_mutants),
;           Y if transform fires.
; ============================================================================
check_planet_destruction:
    lds   r24, planet_check_disabled
    cpi   r24, 0
    brne  cpd_done                         ; test-mode opt-out
    lds   r24, planet_destroyed
    cpi   r24, 0
    brne  cpd_done                         ; already in all-Mutant mode
    ldi   r28, lo8(entities)
    ldi   r29, hi8(entities)
    ldi   r18, ENTITY_SLOTS
    clr   r19                              ; r19 = humanoid count
cpd_loop:
    ld    r24, Y
    sbrs  r24, 7
    rjmp  cpd_next
    mov   r25, r24
    swap  r25
    andi  r25, 0x07                        ; r25 = type
    cpi   r25, TYPE_HUMANOID
    breq  cpd_inc                          ; humanoid (grounded or falling)
    cpi   r25, TYPE_LANDER
    brne  cpd_next
    sbrs  r24, 0                           ; bit 0 = carrying
    rjmp  cpd_next
cpd_inc:
    inc   r19
cpd_next:
    adiw  r28, ENTITY_SIZE
    dec   r18
    brne  cpd_loop
    cpi   r19, 0
    brne  cpd_done                         ; humanoids remain
    ldi   r24, 1
    sts   planet_destroyed, r24
    rcall transform_landers_to_mutants
    ; Audible cue — the SFX_DEATH swoop doubles as a planet-destruction
    ; sound (descending pitch, dramatic). Keeps the audio table small
    ; while still signalling the transition.
    ldi   r24, 3                           ; SFX_DEATH
    sts   pending_sfx, r24
cpd_done:
    ret

; transform_landers_to_mutants: iterate slots, in-place change every
; non-carrying Lander to a Mutant (TYPE_MUTANT, state cleared, position
; preserved). Carrying Landers are left alone so they can finish their
; abduction trip and become Mutants via the existing y==9 ul_become_mutant
; path; that path also dispatches when planet_destroyed is set.
; Clobbers: r18, r24, r25, Y.
transform_landers_to_mutants:
    ldi   r28, lo8(entities)
    ldi   r29, hi8(entities)
    ldi   r18, ENTITY_SLOTS
ttm_loop:
    ld    r24, Y
    sbrs  r24, 7
    rjmp  ttm_next
    mov   r25, r24
    swap  r25
    andi  r25, 0x07
    cpi   r25, TYPE_LANDER
    brne  ttm_next
    sbrc  r24, 0                           ; carrying → leave alone
    rjmp  ttm_next
    andi  r24, 0x80                        ; clear type/state, keep active bit
    ori   r24, (TYPE_MUTANT << 4)
    st    Y, r24
ttm_next:
    adiw  r28, ENTITY_SIZE
    dec   r18
    brne  ttm_loop
    ret

; respawn_humanoids: install 8 humanoids in slots 1..8 at the same
; world_x positions used at boot (16, 48, 80, ..., 240). Called from
; _reset and from wave_at_max when the previous wave saw a planet
; destruction (so the next wave starts with a fresh, populated planet).
; Clobbers: r24, r25, r26, Y.
respawn_humanoids:
    ldi   r28, lo8(entities + 1*ENTITY_SIZE)
    ldi   r29, hi8(entities + 1*ENTITY_SIZE)
    ldi   r25, 16                          ; world_x seed
    ldi   r24, 8                           ; count
rh_loop:
    ldi   r26, (1<<7) | (TYPE_HUMANOID<<4)
    st    Y+, r26
    st    Y+, r25                          ; world_x
    ldi   r26, 48                          ; y (page 6, just above mountains)
    st    Y+, r26
    subi  r25, -32                         ; world_x += 32
    dec   r24
    brne  rh_loop
    ret

; try_spawn_enemy: at every periodic-spawn tick (and at every wave-end
; kick), try to install one new attacker. Picks Pod first if any pods
; remain in the per-wave allotment; otherwise installs a Lander. While
; planet_destroyed is set, every spawn comes out as a Mutant instead.
; All paths share spawn position scheduling (spawn_pos_idx) and the
; wave_to_spawn/enemies_alive bookkeeping.
try_spawn_enemy:
    ; Wave gate: skip if the wave has no more attackers to spawn.
    lds   r24, wave_to_spawn
    cpi   r24, 0
    breq  tsl_skip
    ldi   r28, lo8(entities)
    ldi   r29, hi8(entities)
    ldi   r18, ENTITY_SLOTS
tsl_find_loop:
    ld    r24, Y
    sbrs  r24, 7
    rjmp  tsl_have_slot                  ; bit 7 clear = inactive
    adiw  r28, ENTITY_SIZE
    dec   r18
    brne  tsl_find_loop
tsl_skip:
    ret                                  ; no free slots OR wave full
tsl_have_slot:
    ; world_x = spawn_pos_idx * 32 + 16
    lds   r25, spawn_pos_idx
    lsl   r25
    lsl   r25
    lsl   r25
    lsl   r25
    lsl   r25                            ; r25 = idx * 32 (mod 256)
    subi  r25, -16                       ; +16
    ; Type choice. Planet-destroyed mode overrides everything: post-
    ; planet-loss waves are all-Mutant per Williams. Otherwise spawn
    ; a Pod first (if the per-wave allotment has any), else a Lander.
    lds   r24, planet_destroyed
    cpi   r24, 0
    breq  tse_normal_choice
    ldi   r24, (1<<7) | (TYPE_MUTANT<<4)
    rjmp  tse_install
tse_normal_choice:
    lds   r24, wave_pods_to_spawn
    cpi   r24, 0
    breq  tse_install_lander
    dec   r24
    sts   wave_pods_to_spawn, r24
    ldi   r24, (1<<7) | (TYPE_POD<<4)
    rjmp  tse_install
tse_install_lander:
    ldi   r24, (1<<7) | (TYPE_LANDER<<4)
tse_install:
    st    Y, r24
    std   Y+1, r25
    ldi   r24, 10                        ; y
    std   Y+2, r24
    ; advance spawn_pos_idx
    lds   r24, spawn_pos_idx
    inc   r24
    sts   spawn_pos_idx, r24
    ; Wave bookkeeping: one fewer to spawn, one more alive.
    lds   r24, wave_to_spawn
    dec   r24
    sts   wave_to_spawn, r24
    lds   r24, enemies_alive
    inc   r24
    sts   enemies_alive, r24
    ret

; ============================================================================
; collision_check: for each active beam, scan all active Landers; on overlap
; (leading-edge pixel inside the Lander's 8×8 box), deactivate both.
; Per Defender, only the leading-edge pixel hits — the trail is harmless.
;
; Clobbers: r16, r17, r18, r19, r24, r25, r26, r27, Y
; ============================================================================
collision_check:
    ldi   r28, lo8(projectiles)
    ldi   r29, hi8(projectiles)
    ldi   r18, PROJ_SLOTS
cc_proj_loop:
    ld    r24, Y
    sbrs  r24, 7
    rjmp  cc_proj_next
    ; Beam state
    mov   r17, r24
    andi  r17, 0x3F                    ; r17 = beam_y
    ldd   r16, Y+1                     ; r16 = beam_x
    push  r28
    push  r29
    push  r18
    ldi   r28, lo8(entities)
    ldi   r29, hi8(entities)
    ldi   r19, ENTITY_SLOTS
cc_ent_loop:
    ld    r25, Y
    sbrs  r25, 7
    rjmp  cc_ent_next
    ; Type check — beam can hit Landers and Humanoids. Use breq + rjmp so
    ; the "skip" target (cc_ent_next) is reached via rjmp's 12-bit range
    ; rather than a 7-bit conditional that overflows now that the body has
    ; grown past 64 words.
    mov   r26, r25
    swap  r26
    andi  r26, 0x07
    cpi   r26, TYPE_LANDER
    breq  cc_ent_typed
    cpi   r26, TYPE_MUTANT
    breq  cc_ent_typed
    cpi   r26, TYPE_POD
    breq  cc_ent_typed
    cpi   r26, TYPE_SWARMER
    breq  cc_ent_typed
    cpi   r26, TYPE_HUMANOID
    breq  cc_ent_typed
    rjmp  cc_ent_next
cc_ent_typed:
    ; bbox y-height (r24): 16 if a carrying Lander (covers the dangling
    ; humanoid sprite at lander_y+8); else 8.  r25 still holds byte 0,
    ; r26 still holds type.
    ldi   r24, 8
    cpi   r26, TYPE_LANDER
    brne  cc_have_height
    sbrs  r25, 0
    rjmp  cc_have_height
    ldi   r24, 16
cc_have_height:
    ; screen_x = world_x - scroll
    ldd   r26, Y+1
    lds   r27, scroll_offset
    sub   r26, r27
    sbrc  r26, 7
    rjmp  cc_ent_next
    cp    r16, r26
    brlo  cc_ent_next
    mov   r27, r26
    subi  r27, -8
    cp    r16, r27
    brsh  cc_ent_next
    ; beam_y in [entity_y, entity_y + r24)?
    ldd   r25, Y+2
    andi  r25, 0x3F
    cp    r17, r25
    brlo  cc_ent_next
    mov   r27, r25
    add   r27, r24
    cp    r17, r27
    brsh  cc_ent_next
    rcall cc_handle_hit
    pop   r18
    pop   r29
    pop   r28
    clr   r25
    st    Y, r25
    ldi   r25, 2                          ; SFX_HIT
    sts   pending_sfx, r25
    rjmp  cc_proj_next
cc_ent_next:
    adiw  r28, ENTITY_SIZE
    dec   r19
    breq  cc_ent_loop_done
    rjmp  cc_ent_loop                    ; rjmp range covers the larger body
cc_ent_loop_done:
    pop   r18
    pop   r29
    pop   r28
cc_proj_next:
    adiw  r28, PROJ_SIZE
    dec   r18
    breq  cc_proj_loop_done
    rjmp  cc_proj_loop                   ; rjmp for the same range reason
cc_proj_loop_done:
    ret

; cc_handle_hit: a beam has just landed on the entity at Y. Decide what to
; do with the slot:
;   * carrying Lander, beam in upper half [y, y+7]    → free humanoid (slot
;     becomes a falling humanoid; the lander dies)
;   * carrying Lander, beam in lower half [y+8, y+15] → kill humanoid only
;     (clear the lander's carry bit; lander stays alive and resumes empty)
;   * anything else (empty Lander, grounded Humanoid, falling Humanoid)
;                                                     → just deactivate
;
; Reads r17 (beam_y, set up at the start of cc_proj_loop and preserved
; through cc_ent_loop). Y is preserved across the call.
cc_handle_hit:
    ld    r25, Y
    sbrs  r25, 0
    rjmp  chh_kill                       ; bit 0 clear → not a carrying lander
    mov   r27, r25
    swap  r27
    andi  r27, 0x07
    cpi   r27, TYPE_LANDER
    brne  chh_kill                       ; falling humanoid (HUMANOID + bit 0)
    ; Carrying Lander hit. Lower half → drop the cargo; upper half → free.
    ldd   r26, Y+2
    andi  r26, 0x3F                      ; entity_y
    mov   r27, r17                       ; beam_y
    sub   r27, r26                       ; r27 = beam_y - entity_y
    cpi   r27, 8
    brsh  chh_drop_cargo                 ; lower half — kill humanoid only
    ; Upper half — free the humanoid as a falling one (lander dies).
    ldi   r25, 0x80 | (TYPE_HUMANOID << 4) | 0x01
    st    Y, r25
    rjmp  chh_dec_enemy                  ; lander died — decrement count
chh_drop_cargo:
    ; Clear carry bit; lander stays active. Enemy count unchanged,
    ; but the carried humanoid was just consumed, so the planet
    ; may have fallen.
    andi  r25, 0xFE
    st    Y, r25
    rcall check_planet_destruction
    ret
chh_kill:
    ; Pre-clear: capture original type so we know whether to decrement,
    ; and so we can dispatch on Pod (which has a special death effect).
    ; All chh_dec_enemy branches go through a local trampoline (chk_dec)
    ; because chh_dec_enemy lives outside the 7-bit cond-branch range.
    ld    r25, Y
    mov   r27, r25
    swap  r27
    andi  r27, 0x07                      ; r27 = type
    cpi   r27, TYPE_POD
    brne  chk_not_pod
    rjmp  chh_pod                        ; Pod: spawn Swarmers FIRST, then count
chk_not_pod:
    clr   r25
    st    Y, r25                         ; deactivate slot
    cpi   r27, TYPE_LANDER
    breq  chk_dec
    cpi   r27, TYPE_MUTANT
    breq  chk_dec
    cpi   r27, TYPE_SWARMER
    breq  chk_dec
    ; Humanoid kill path (or unknown type) — no enemy-count change,
    ; but a humanoid was just removed so the planet may have fallen.
    rjmp  chk_planet_check
chk_dec:
    rjmp  chh_dec_enemy
chk_planet_check:
    rcall check_planet_destruction
    ret

chh_pod:
    ; Capture pod position before clearing the slot, then deactivate
    ; the pod and spawn POD_SWARMER_COUNT Swarmers at that position.
    ; The Pod itself counts as a normal kill (chh_dec_enemy → 100 pts +
    ; bonus check), and each Swarmer adds 1 to enemies_alive so the
    ; wave doesn't close until all Swarmers die too.
    ldd   r24, Y+1                       ; pod_world_x
    ldd   r25, Y+2
    andi  r25, 0x3F                      ; pod_y
    clr   r26
    st    Y, r26                         ; deactivate pod slot
    rcall spawn_swarmers                 ; preserves Y; uses X internally
    rjmp  chh_dec_enemy

; spawn_swarmers: install up to POD_SWARMER_COUNT Swarmers at offsets
; from (r24=world_x, r25=y). Searches entity slots via X (r26:r27) so
; Y is preserved for the caller (chh inside cc_proj_loop's hot path).
; Per-swarmer (dx, dy) offsets come from swarmer_spawn_offsets — without
; them all five Swarmers would start at the same world position with
; identical chase state and never separate (they'd cluster as one big
; target). Offsets fan them out into a small cross/X pattern so they
; converge on the ship from different angles.
; Clobbers: r2, r3, r4 (push/pop), r19, r20, r21, r24, r25, r26, r27,
; r30, r31.
spawn_swarmers:
    push  r2
    push  r3
    push  r4
    push  r5
    mov   r2, r24                        ; cache base_x
    mov   r3, r25                        ; cache base_y (already 6-bit clean)
    ldi   r24, POD_SWARMER_COUNT
    mov   r4, r24                        ; remaining swarmers to install
    rcall rng_step                       ; one rng draw per Pod death
    mov   r5, r24                        ; r5 = dir-rotation base (0..255 → mod 8)
    ldi   r26, lo8(entities)
    ldi   r27, hi8(entities)
    ldi   r19, ENTITY_SLOTS
ss_find_loop:
    ld    r20, X
    sbrs  r20, 7
    rjmp  ss_install                     ; bit 7 clear → slot is free
    adiw  r26, ENTITY_SIZE
    dec   r19
    brne  ss_find_loop
    rjmp  ss_done                        ; out of free slots — spawn fewer
ss_install:
    ; spawn_idx = POD_SWARMER_COUNT - r4  (0 for first install, 4 for last)
    ldi   r20, POD_SWARMER_COUNT
    sub   r20, r4
    push  r20                            ; stash spawn_idx; dir lookup needs it too

    ; --- Position offset (2 bytes per entry, dx then dy) ---
    mov   r24, r20
    lsl   r24                            ; *2
    ldi   r30, lo8(swarmer_spawn_offsets)
    ldi   r31, hi8(swarmer_spawn_offsets)
    add   r30, r24
    ldi   r24, 0
    adc   r31, r24
    lpm   r24, Z+                        ; r24 = signed dx (offset)
    lpm   r25, Z                         ; r25 = signed dy (offset)
    push  r25
    mov   r17, r24                       ; r17 = position dx (preserve over Z reuse)
    pop   r18                            ; r18 = position dy

    ; --- Initial direction = (initial_dir_offsets[idx] + r5) mod 8 ---
    pop   r20                            ; restore spawn_idx
    ldi   r30, lo8(swarmer_initial_dir_offsets)
    ldi   r31, hi8(swarmer_initial_dir_offsets)
    add   r30, r20
    ldi   r24, 0
    adc   r31, r24
    lpm   r20, Z                         ; r20 = base offset (0/2/4/6/3)
    add   r20, r5                        ; + per-Pod random rotation
    andi  r20, 0x07                      ; mod 8 → 3-bit dir

    ; --- Compute final position ---
    mov   r24, r2
    add   r24, r17                       ; world_x + dx (mod-256 wrap is fine)
    mov   r25, r3
    add   r25, r18                       ; y + dy
    andi  r25, 0x3F                      ; clear state2 bits

    ; --- Pack byte 0: active | type=SWARMER | dir ---
    ldi   r21, (1<<7) | (TYPE_SWARMER<<4)
    or    r21, r20

    ; AVR limitation: X (r26:r27) doesn't support std with displacement.
    ; st X+ x 3 walks bytes 0/1/2 and advances X to the next slot,
    ; replacing the explicit adiw the find-loop would otherwise need.
    st    X+, r21
    st    X+, r24
    st    X+, r25
    lds   r20, enemies_alive
    inc   r20
    sts   enemies_alive, r20
    dec   r19
    breq  ss_done                        ; no more slots to scan
    dec   r4
    brne  ss_find_loop                   ; still want more swarmers
ss_done:
    pop   r5
    pop   r4
    pop   r3
    pop   r2
    ret

; Offsets applied to a Pod's (world_x, y) when spawning the 5 Swarmers
; that replace it. Spreads them in a small "+" pattern so they don't
; converge as one indistinguishable clump. Each entry is (dx, dy).
swarmer_spawn_offsets:
    .byte  0,  0      ; index 0: at the pod's exact center
    .byte -4, -2      ; NW
    .byte  4, -2      ; NE
    .byte -4,  2      ; SW
    .byte  4,  2      ; SE
    .balign 2

; Initial direction OFFSETS (3-bit compass dirs) assigned to the 5
; Swarmers when spawning. The actual direction stored in each Swarmer's
; state field is (offsets[i] + rng_base) mod 8, where rng_base comes
; from a single rng_step call at the top of spawn_swarmers — so each
; Pod kill spawns 5 Swarmers facing 5 distinct compass directions, but
; the absolute direction varies between kills. This is the "RNG-shuffled
; across 8 compass dirs" star-burst Ed asked for.
swarmer_initial_dir_offsets:
    .byte 0, 2, 4, 6, 3
    .balign 2

; Periodic-spawn cadence per difficulty (frames between enemy spawns).
; LOW gives the player ~1.6 s between threats; HIGH matches the
; Williams arcade pace. Values are loaded into spawn_countdown via
; lookup_spawn_interval at game start and after every successful
; spawn fire. Picked from the first profile_gameplay baseline:
; PASSIVE-pilot wave 1 was overwhelming a stationary ship in 10.6 s
; on every difficulty (peak 19 enemies in that window) because the
; spawn rate was uniform.
spawn_interval_table:
    .byte 96      ; LOW  → spawn every 96 frames (~1.6 s)
    .byte 64      ; MED  → every 64 frames (~1.07 s)
    .byte 32      ; HIGH → every 32 frames (~0.53 s, Williams pace)
    .balign 2

; Per-difficulty Swarmer nudge "skip threshold" — how often Swarmers
; ignore their decision frame and keep current heading instead of
; rotating toward the player. See lookup_swarm_skip_thresh for what
; the values map to.
swarm_skip_thresh_table:
    .byte 2      ; LOW  → 25% nudge (drunker Swarmers, not really hunting)
    .byte 1      ; MED  → 50% nudge
    .byte 0      ; HIGH → 75% nudge (Williams-faithful)
    .balign 2

; 8-direction velocity tables for Swarmers. Indexed by 3-bit compass
; direction stored in byte 0 bits 0-2:
;   0 = E,  1 = SE, 2 = S,  3 = SW
;   4 = W,  5 = NW, 6 = N,  7 = NE
; Each Swarmer translates by (dx, dy) every frame, giving 1 px/frame
; cardinal speed and ~1.4 px/frame diagonal. update_swarmer rotates
; the direction by ±1 step toward the player every 8 frames (75% of
; the time) — that gives jittery movement with inertia.
swarmer_dx_table:
    .byte  1,  1,  0, -1, -1, -1,  0,  1
swarmer_dy_table:
    .byte  0,  1,  1,  1,  0, -1, -1, -1
    .balign 2
chh_dec_enemy:
    lds   r25, enemies_alive
    cpi   r25, 0
    breq  chh_dec_done                   ; saturate at 0
    dec   r25
    sts   enemies_alive, r25
chh_dec_done:
    ; Award 100 points per Lander/Mutant kill (24-bit add to score).
    ; Two's-complement -100 in 24 bits = 0xFFFF9C.
    lds   r24, score_lo
    lds   r25, score_mid
    lds   r26, score_hi
    subi  r24, 0x9C
    sbci  r25, 0xFF
    sbci  r26, 0xFF
    sts   score_lo, r24
    sts   score_mid, r25
    sts   score_hi, r26
    ; Bonus-ship check: 24-bit compare against next_bonus.
    lds   r18, next_bonus_lo
    lds   r19, next_bonus_mid
    lds   r20, next_bonus_hi
    cp    r24, r18
    cpc   r25, r19
    cpc   r26, r20
    brlo  chh_no_bonus
    ; Cap lives at 255 so a long run can't wrap the byte to 0 and
    ; trip a spurious game-over the next time the ship is hit.
    lds   r24, lives
    cpi   r24, 0xFF
    breq  chh_skip_life_inc
    inc   r24
    sts   lives, r24
chh_skip_life_inc:
    ; next_bonus += per-difficulty bonus increment (24-bit add).
    rcall lookup_bonus_increment           ; r25:r24 = increment (hi byte=0)
    lds   r18, next_bonus_lo
    lds   r19, next_bonus_mid
    lds   r20, next_bonus_hi
    add   r18, r24
    adc   r19, r25
    ldi   r24, 0
    adc   r20, r24
    sts   next_bonus_lo, r18
    sts   next_bonus_mid, r19
    sts   next_bonus_hi, r20
chh_no_bonus:
    ret

; ============================================================================
; lookup_bonus_increment: read `difficulty` (0=LOW, 1=MED, 2=HIGH) and
; return the per-difficulty bonus-ship threshold/increment in r25:r24.
; HIGH = 10000 matches Williams Defender; MED and LOW are scaled down so
; less-skilled players still see bonus ships at a reasonable cadence.
; The high byte is always 0 (max 10000 < 65536); callers can rely on
; that to skip a third add when extending to 24 bits.
; Uses r24 itself for the difficulty test so the clobber list is just
; the return registers — easier on callers, no scratch-register
; bookkeeping required.
; Clobbers: r24, r25 (return value).
; ============================================================================
.global lookup_bonus_increment
lookup_bonus_increment:
    lds   r24, difficulty
    cpi   r24, 0
    breq  lbi_low
    cpi   r24, 1
    breq  lbi_med
    ldi   r24, lo8(10000)                  ; HIGH (and any unexpected value)
    ldi   r25, hi8(10000)
    ret
lbi_low:
    ldi   r24, lo8(3000)
    ldi   r25, hi8(3000)
    ret
lbi_med:
    ldi   r24, lo8(6000)
    ldi   r25, hi8(6000)
    ret

; ============================================================================
; lookup_spawn_interval: returns r24 = frames-between-spawns for the
; current difficulty (96 / 64 / 32 for LOW/MED/HIGH). Used both at
; game start (seed spawn_countdown) and on every spawn fire (reload).
; Clobbers: r24, r25, Z.
; ============================================================================
lookup_spawn_interval:
    lds   r24, difficulty
    cpi   r24, 3
    brlo  lsi_in_range
    ldi   r24, 0                          ; unknown → LOW (most forgiving)
lsi_in_range:
    ldi   r30, lo8(spawn_interval_table)
    ldi   r31, hi8(spawn_interval_table)
    add   r30, r24
    ldi   r24, 0
    adc   r31, r24
    lpm   r24, Z
    ret

; ============================================================================
; lookup_swarm_skip_thresh: returns r24 = the "skip threshold" for the
; current difficulty. Each Swarmer's decision frame draws a random byte;
; (rand & 3) is in 0..3. We skip the nudge (keep current direction) if
; that value <= threshold; nudge otherwise. So:
;   thresh 0 → skip 1 of 4 → 75% nudge → HIGH (Williams-faithful)
;   thresh 1 → skip 2 of 4 → 50% nudge → MED
;   thresh 2 → skip 3 of 4 → 25% nudge → LOW (Swarmers wander, mostly)
; Effect: lower difficulties produce drunker Swarmers that don't track
; the player as tightly. Player still has to dodge but isn't being
; hunted.
; Clobbers: r24, r25, Z.
; ============================================================================
lookup_swarm_skip_thresh:
    lds   r24, difficulty
    cpi   r24, 3
    brlo  lst_in_range
    ldi   r24, 0                          ; unknown difficulty → HIGH default
lst_in_range:
    ldi   r30, lo8(swarm_skip_thresh_table)
    ldi   r31, hi8(swarm_skip_thresh_table)
    add   r30, r24
    ldi   r24, 0
    adc   r31, r24
    lpm   r24, Z
    ret

; ============================================================================
; rng_step: 8-bit Galois LFSR step. Returns r24 = next pseudo-random
; byte, also stored in rng_state. Polynomial 0xB8 → period 255 (every
; non-zero state is visited exactly once before the sequence repeats).
; Cheap (4 instructions on the hot path) so update_swarmer can call it
; on every decision frame for every Swarmer.
; Clobbers: r24, r25.
; ============================================================================
.global rng_step
rng_step:
    lds   r24, rng_state
    lsr   r24
    brcc  rs_no_xor                       ; old bit 0 was 0 → just shift
    ldi   r25, 0xB8
    eor   r24, r25
rs_no_xor:
    sts   rng_state, r24
    ret

; ============================================================================
; ship_collision_check: scan active Landers; on overlap with the ship's 8×8
; box (cols 60..67, rows sprite_y..sprite_y+7), reset ship to y=28 and arm
; the death-flash counter. Lander survives (Defender behavior).
;
; Clobbers: r17, r18, r24, r25, r26, r27, Y
; ============================================================================
ship_collision_check:
    ; Skip while the ship is dying or respawning. death_flash is the short
    ; screen-flash window; respawn_invuln is the longer explosion + blink
    ; window. Either being non-zero means the ship is non-collidable.
    ; Invariant: at arming time (sc_check on hit), death_flash <= respawn_invuln
    ; — both are armed atomically with 8 vs 90, so respawn_invuln strictly
    ; outlives death_flash. The OR here is defensive; if anyone bumps
    ; death_flash above 90 or shrinks respawn_invuln, the longer-of-the-two
    ; is what gates collision regardless.
    lds   r24, death_flash
    cpi   r24, 0
    brne  sc_skip
    lds   r24, respawn_invuln
    cpi   r24, 0
    brne  sc_skip
    rjmp  sc_alive
sc_skip:
    ret
sc_alive:
    lds   r17, sprite_y
    ldi   r28, lo8(entities)
    ldi   r29, hi8(entities)
    ldi   r18, ENTITY_SLOTS
sc_loop:
    ld    r25, Y
    sbrs  r25, 7
    rjmp  sc_next
    mov   r26, r25
    swap  r26
    andi  r26, 0x07
    cpi   r26, TYPE_LANDER
    breq  sc_typed
    cpi   r26, TYPE_MUTANT
    breq  sc_typed
    cpi   r26, TYPE_POD
    breq  sc_typed
    cpi   r26, TYPE_SWARMER
    breq  sc_typed
    rjmp  sc_next
sc_typed:
    ; lander/mutant screen_x in r26
    ldd   r26, Y+1
    lds   r27, scroll_offset
    sub   r26, r27
    sbrc  r26, 7
    rjmp  sc_next                       ; off-screen left
    ; X overlap: lander_screen_x in [60-7, 67] = [53, 67]
    cpi   r26, 53
    brlo  sc_next
    cpi   r26, 68
    brsh  sc_next
    ; Y overlap: |sprite_y - lander_y| <= 7
    ldd   r24, Y+2
    andi  r24, 0x3F                      ; lander_y
    mov   r26, r24
    sub   r26, r17                       ; r26 = lander_y - sprite_y
    sbrs  r26, 7
    rjmp  sc_check
    neg   r26                            ; |delta|
sc_check:
    cpi   r26, 8
    brsh  sc_next
    ; HIT — capture death_y for the explosion center, reset sprite_y so
    ; the ship reappears at the default altitude after the explosion,
    ; arm the screen flash AND the extended respawn-invuln window, queue
    ; SFX_DEATH. main_loop drains after.
    lds   r25, sprite_y
    sts   death_y, r25
    ldi   r25, 28
    sts   sprite_y, r25
    ldi   r25, 8
    sts   death_flash, r25
    ldi   r25, 90
    sts   respawn_invuln, r25
    ldi   r25, 3                          ; SFX_DEATH (overrides any pending HIT)
    sts   pending_sfx, r25
    ; Williams convention: `lives` is the RESERVE count shown on the HUD;
    ; the active ship is implicit. Player gets reserves + 1 total chances.
    ; If reserves > 0 → consume one and respawn (animation will play).
    ; If reserves == 0 → no reserve to swap in; this hit ends the game.
    lds   r25, lives
    cpi   r25, 0
    breq  sc_hit_fatal
    dec   r25
    sts   lives, r25
    ret
sc_hit_fatal:
    ldi   r25, 1
    sts   game_state, r25
    ; Suppress the very first game-over frame's edge detection: pretend
    ; every button was held last frame, so whatever the player is
    ; physically holding (almost always B, the fire button) doesn't
    ; register as a fresh edge and instantly soft-reset the screen.
    ldi   r25, 0xFF
    sts   prev_buttons, r25
    rcall maybe_save_high_score           ; persist if this run beat the record
    ret
sc_next:
    adiw  r28, ENTITY_SIZE
    dec   r18
    breq  sc_loop_done                   ; 7-bit cond → nearby label
    rjmp  sc_loop                        ; rjmp's 12-bit range fits
sc_loop_done:
    ret

; ============================================================================
; invert_if_dead: if death_flash > 0, COM all 1024 framebuffer bytes
; (one's-complement, i.e., negate the display) and decrement death_flash.
;
; Clobbers: r24, r25, r26, Y
; ============================================================================
invert_if_dead:
    lds   r25, death_flash
    cpi   r25, 0
    breq  ifd_done
    dec   r25
    sts   death_flash, r25
    ldi   r28, lo8(framebuffer)
    ldi   r29, hi8(framebuffer)
    ldi   r24, 4                         ; 4 outer × 256 = 1024
ifd_outer:
    clr   r26
ifd_inner:
    ld    r25, Y
    com   r25
    st    Y+, r25
    dec   r26
    brne  ifd_inner
    dec   r24
    brne  ifd_outer
ifd_done:
    ret

; ============================================================================
; render_explosion: draw 8 fragments radiating from (64, death_y+4) based
; on respawn_invuln. Only renders during the explosion phase
; (respawn_invuln in [61, 90]); caller has already gated on this. Each
; fragment is a single pixel; per-frame extent = (90 - respawn_invuln + 1)
; pixels along its (dx, dy) direction. Off-screen fragments are skipped.
;
; Clobbers: r0, r1, r16, r17, r18, r19, r20, r21, r24, r25, r26, r30, r31.
; (Y left intact; X is touched only inside setpixel.)
; ============================================================================
render_explosion:
    lds   r18, respawn_invuln
    ldi   r19, 91
    sub   r19, r18                       ; r19 = frame_num (1..30)
    lds   r20, death_y
    subi  r20, -4                        ; r20 = cy (=death_y + 4)
    ldi   r21, 8                         ; fragment count
    ldi   r16, 0                         ; fragment index 0..7
rex_loop:
    ; Z = explosion_velocity_table + fragment_idx * 2
    ldi   r30, lo8(explosion_velocity_table)
    ldi   r31, hi8(explosion_velocity_table)
    mov   r17, r16
    lsl   r17                            ; idx * 2 bytes per entry
    add   r30, r17
    ldi   r17, 0
    adc   r31, r17
    lpm   r17, Z+                        ; r17 = signed dx
    lpm   r18, Z                         ; r18 = signed dy

    ; signed_x = 64 + dx * t
    muls  r17, r19                       ; r1:r0 = signed dx * t
    ldi   r25, 64
    add   r0, r25
    ldi   r25, 0
    adc   r1, r25
    mov   r24, r0
    mov   r25, r1
    clr   r1                             ; restore zero-reg
    cpi   r25, 0
    brne  rex_skip                       ; signed off-screen (negative or > 255)
    cpi   r24, 128
    brsh  rex_skip                       ; >= 128 off-screen
    push  r24                            ; stash x

    ; signed_y = cy + dy * t
    muls  r18, r19                       ; r1:r0 = signed dy * t
    add   r0, r20
    ldi   r25, 0
    adc   r1, r25
    mov   r24, r0
    mov   r25, r1
    clr   r1
    pop   r26                            ; r26 = x (saved earlier)
    cpi   r25, 0
    brne  rex_skip
    cpi   r24, 64
    brsh  rex_skip
    ; setpixel(x=r26, y=r24)
    mov   r25, r24
    mov   r24, r26
    rcall setpixel
rex_skip:
    inc   r16
    dec   r21
    brne  rex_loop
    ret

; ============================================================================
; setpixel: OR a single pixel into the framebuffer at (x=r24, y=r25).
; Caller must range-check (x < 128, y < 64). Used by render_explosion.
; Clobbers: r0, r1, r24, r26, r27, r30, r31. r24 is destroyed by the
; load-OR-store at the end (overwriting the input x).
; ============================================================================
setpixel:
    ; offset = (y >> 3) * 128 + x
    mov   r26, r25
    lsr   r26
    lsr   r26
    lsr   r26                            ; r26 = page (0..7)
    ldi   r27, 128
    mul   r26, r27                       ; r1:r0 = page * 128
    ldi   r30, lo8(framebuffer)
    ldi   r31, hi8(framebuffer)
    add   r30, r0
    adc   r31, r1
    clr   r1
    add   r30, r24
    ldi   r27, 0
    adc   r31, r27
    ; mask = 1 << (y & 7)
    mov   r26, r25
    andi  r26, 7
    ldi   r27, 1
sp_shift:
    cpi   r26, 0
    breq  sp_or
    lsl   r27
    dec   r26
    rjmp  sp_shift
sp_or:
    ld    r24, Z
    or    r24, r27
    st    Z, r24
    ret

; Eight unit-direction vectors covering compass octants. Speeds chosen
; so the explosion's outermost fragments traverse ~60-90 pixels over
; the 30-frame burst — visibly leaving the screen, matching the
; Williams-style "kaboom" feel rather than a contained puff.
explosion_velocity_table:
    .byte  3,  0    ; E
    .byte  2,  2    ; SE
    .byte  0,  3    ; S
    .byte -2,  2    ; SW
    .byte -3,  0    ; W
    .byte -2, -2    ; NW
    .byte  0, -3    ; N
    .byte  2, -2    ; NE
    .balign 2

; ============================================================================
; update_entities: iterate the entity slots; for each active one, dispatch
; through update_dispatch by type. Each handler may modify the slot pointed
; to by Y; handlers must preserve Y.
; ============================================================================
update_entities:
    ldi   r28, lo8(entities)
    ldi   r29, hi8(entities)
    ldi   r18, ENTITY_SLOTS
ue_loop:
    ld    r24, Y                         ; byte 0
    sbrs  r24, 7
    rjmp  ue_next
    ; Extract type (bits 4-6)
    mov   r25, r24
    swap  r25                            ; nibble swap: type now in bits 0-2
    andi  r25, 0x07
    ldi   r30, pm_lo8(update_dispatch)
    ldi   r31, pm_hi8(update_dispatch)
    add   r30, r25
    clr   r25
    adc   r31, r25
    icall                                 ; handler returns via RET
ue_next:
    adiw  r28, ENTITY_SIZE
    dec   r18
    brne  ue_loop
    ret

update_dispatch:
    rjmp  update_noop                    ; type 0 = none
    rjmp  update_lander                  ; type 1 = Lander
    rjmp  update_mutant                  ; type 2 = Mutant
    rjmp  update_pod                     ; type 3 = Pod
    rjmp  update_noop                    ; type 4 = (Bomber)
    rjmp  update_swarmer                 ; type 5 = Swarmer
    rjmp  update_noop                    ; type 6 = (Baiter)
    rjmp  update_humanoid                ; type 7 = Humanoid

update_noop:
    ret

; Mutant update — chase the player.
;   Vertical: every 2 frames, move 1 row toward sprite_y.
;   Horizontal: every 4 frames, move 1 col toward (scroll_offset + 60),
;               with signed-delta wrap-aware direction.
; Y points at the slot's byte 0; preserved on return.
update_mutant:
    ; Vertical (1 row / 2 frames).
    lds   r25, frame_counter
    sbrc  r25, 0
    rjmp  um_skip_v                      ; odd frame, skip
    ldd   r25, Y+2
    mov   r24, r25
    andi  r24, 0xC0                      ; preserve state2
    andi  r25, 0x3F                      ; mutant_y
    lds   r26, sprite_y
    cp    r25, r26
    breq  um_skip_v                      ; aligned vertically
    brlo  um_v_down                      ; mutant_y < sprite_y → move down
    dec   r25
    rjmp  um_v_store
um_v_down:
    inc   r25
um_v_store:
    or    r25, r24
    std   Y+2, r25
um_skip_v:
    ; Horizontal (1 col / 4 frames).
    lds   r25, frame_counter
    andi  r25, 0x03
    brne  um_done
    ldd   r24, Y+1                       ; mutant_world_x
    lds   r25, scroll_offset
    subi  r25, -60                       ; r25 = scroll_offset + 60 = ship's world_x
    cp    r24, r25
    breq  um_done
    sub   r25, r24                       ; r25 = target - mutant
    sbrc  r25, 7
    rjmp  um_h_left
    inc   r24
    std   Y+1, r24
    rjmp  um_done
um_h_left:
    dec   r24
    std   Y+1, r24
um_done:
    ret

; Humanoid update — usually a no-op. If byte 0 bit 0 is set, the humanoid is
; "falling" (was just freed from a shot Lander) and descends 1 row per 4
; frames until it touches the ground at y = 48; on landing the falling bit
; is cleared and the humanoid resumes normal grounded behavior.
; Y points at the slot's byte 0 on entry; preserved on return.
update_humanoid:
    ld    r24, Y
    sbrs  r24, 0
    ret                                  ; not falling — nothing to do
    lds   r25, frame_counter
    andi  r25, 0x03
    brne  uh_done                        ; not a drift frame
    ldd   r25, Y+2
    mov   r24, r25
    andi  r24, 0xC0                      ; preserve state2
    andi  r25, 0x3F                      ; isolate y
    cpi   r25, 48
    brsh  uh_at_ground                   ; already at ground — just clear bit
    inc   r25
    or    r25, r24
    std   Y+2, r25
    andi  r25, 0x3F
    cpi   r25, 48
    brlo  uh_done                        ; still falling
uh_at_ground:
    ld    r24, Y
    andi  r24, 0xFE
    st    Y, r24
uh_done:
    ret

; Pod update — slow horizontal drift toward the player. 1 column every
; 8 frames (half-Mutant horizontal speed); no vertical movement (Pod
; stays near top edge until shot). Wrap-aware direction picks the
; shorter of the two world directions.
; Y points at the slot's byte 0 on entry; preserved on return.
update_pod:
    lds   r25, frame_counter
    andi  r25, 0x07
    brne  upod_done                      ; act 1/8 frames
    ldd   r24, Y+1                       ; pod_world_x
    lds   r25, scroll_offset
    subi  r25, -60                       ; r25 = ship_world_x
    cp    r24, r25
    breq  upod_done                      ; aligned
    sub   r25, r24                       ; r25 = ship - pod (sign = direction)
    sbrc  r25, 7
    rjmp  upod_left                      ; ship to the left → dec pod_x
    inc   r24
    std   Y+1, r24
    rjmp  upod_done
upod_left:
    dec   r24
    std   Y+1, r24
upod_done:
    ret

; Swarmer update — direction-based motion with periodic player-biased
; nudging. Persistent direction (3 bits in byte 0) gives inertia: most
; frames the Swarmer just translates by 1 px in its current heading.
; Every 8 frames, an LFSR coin flip decides whether to keep course
; (25%) or rotate ±1 step toward the player's compass bearing (75%).
; The bias drives eventual convergence; the inertia prevents the
; "teleport every frame" feel of pure RNG. Initial directions are set
; in spawn_swarmers from a randomized base so each Pod kill produces a
; unique 5-direction starburst.
;
; Y points at the slot's byte 0 on entry; preserved on return.
update_swarmer:
    ; --- Apply velocity for this frame ---
    ld    r24, Y
    mov   r25, r24
    andi  r25, 0x07                      ; r25 = current dir (0..7)

    ldi   r30, lo8(swarmer_dx_table)
    ldi   r31, hi8(swarmer_dx_table)
    add   r30, r25
    ldi   r24, 0
    adc   r31, r24
    lpm   r17, Z                         ; r17 = signed dx

    ldi   r30, lo8(swarmer_dy_table)
    ldi   r31, hi8(swarmer_dy_table)
    add   r30, r25
    adc   r31, r24                       ; r24 still 0
    lpm   r19, Z                         ; r19 = signed dy
                                          ; (using r19 not r18 because
                                          ;  r18 is update_entities's
                                          ;  outer loop counter and must
                                          ;  survive this updater)

    ldd   r24, Y+1                       ; world_x
    add   r24, r17
    std   Y+1, r24

    ldd   r24, Y+2                       ; state2 | y
    mov   r25, r24
    andi  r25, 0xC0                      ; preserve state2
    andi  r24, 0x3F                      ; isolate y
    add   r24, r19
    andi  r24, 0x3F                      ; wrap into 6-bit field
    or    r24, r25
    std   Y+2, r24

    ; --- Decision frame? Only every 8 frames per Swarmer ---
    lds   r25, frame_counter
    andi  r25, 0x07
    breq  usw_decision
    ret

usw_decision:
    rcall rng_step                       ; r24 = random byte
    andi  r24, 0x03                      ; rand_2bits in 0..3
    push  r24                            ; stash for compare after lookup
    rcall lookup_swarm_skip_thresh       ; r24 = thresh (0/1/2 for HIGH/MED/LOW)
    mov   r25, r24
    pop   r24                            ; rand_2bits back in r24
    cp    r25, r24                       ; flags = thresh - rand
    brsh  usw_done                       ; rand <= thresh → keep current dir
                                          ; (HIGH: skip 1/4, MED: skip 2/4,
                                          ;  LOW:  skip 3/4)

    ; rand > thresh → nudge dir 1 step toward the ideal player bearing.
    rcall swarmer_ideal_dir              ; r24 = ideal dir (0..7)
    ld    r25, Y
    mov   r17, r25
    andi  r17, 0x07                      ; r17 = current dir
    sub   r24, r17                       ; r24 = ideal - current
    andi  r24, 0x07                      ; mod 8
    breq  usw_done                       ; already aligned
    cpi   r24, 5
    brsh  usw_rotate_minus
    inc   r17                            ; rotate +1 (shorter way)
    rjmp  usw_store_dir
usw_rotate_minus:
    dec   r17                            ; rotate -1
usw_store_dir:
    andi  r17, 0x07                      ; mod 8
    andi  r25, 0xF8                      ; clear old dir bits
    or    r25, r17
    st    Y, r25
usw_done:
    ret

; swarmer_ideal_dir: returns r24 = compass dir (0..7) the Swarmer at Y
; would face if heading directly at the ship. Wrap-aware on x (the
; signed sub gives the shorter direction); not wrapped on y. Y is
; preserved; clobbers r17, r24, r25.
swarmer_ideal_dir:
    ldd   r24, Y+1                       ; swarmer world_x
    lds   r25, scroll_offset
    subi  r25, -60                       ; ship's world_x
    sub   r25, r24                       ; r25 = signed dx (wrap-aware)

    ldd   r24, Y+2
    andi  r24, 0x3F                      ; swarmer y
    lds   r17, sprite_y
    sub   r17, r24                       ; r17 = signed dy

    cpi   r25, 0
    breq  sid_dx_zero
    sbrc  r25, 7
    rjmp  sid_dx_neg
    ; dx > 0 (player to the right)
    cpi   r17, 0
    breq  sid_e
    sbrc  r17, 7
    rjmp  sid_ne
    ldi   r24, 1                         ; SE
    ret
sid_e:
    ldi   r24, 0                         ; E
    ret
sid_ne:
    ldi   r24, 7                         ; NE
    ret
sid_dx_neg:
    cpi   r17, 0
    breq  sid_w
    sbrc  r17, 7
    rjmp  sid_nw
    ldi   r24, 3                         ; SW
    ret
sid_w:
    ldi   r24, 4                         ; W
    ret
sid_nw:
    ldi   r24, 5                         ; NW
    ret
sid_dx_zero:
    cpi   r17, 0
    breq  sid_aligned
    sbrc  r17, 7
    rjmp  sid_n
    ldi   r24, 2                         ; S
    ret
sid_n:
    ldi   r24, 6                         ; N
    ret
sid_aligned:
    ldi   r24, 0                         ; nudge will skip (no rotation needed)
    ret

; Lander update — descend to humanoid altitude, hover on its head, pause,
; grab, ascend. Defender-faithful abduction.
;
; State encoding (byte 0 bits 0-3):
;   bit 0     = carry flag (1 = carrying humanoid)
;   bits 1-3  = pause counter (0..7). Set to 7 the first time we land on
;               an aligned humanoid's head (y == 40). Decrements every 4
;               frames; the 1→0 transition fires the actual grab. 7 × 4 =
;               28 pause frames ≈ 470 ms at 60 Hz.
;
; Y points at the slot's byte 0 on entry; preserved on return.
update_lander:
    ; Boot-warp gate: while boot_warp_frames > 0 AND we're processing
    ; slot 0 (the boot Lander), skip the AI so the lander stays parked
    ; at its placement spot. Y points at byte 0 of the current slot, so
    ; Y == lo8/hi8(entities) means slot 0.
    ldi   r24, lo8(entities)
    cp    r28, r24
    brne  ul_after_warp
    ldi   r24, hi8(entities)
    cp    r29, r24
    brne  ul_after_warp
    lds   r25, boot_warp_frames
    cpi   r25, 0
    breq  ul_after_warp
    ret
ul_after_warp:
    ld    r24, Y
    sbrc  r24, 0
    rjmp  ul_carrying
    mov   r25, r24
    andi  r25, 0x0E                      ; pause counter
    brne  ul_pausing

    ; Idle — drift frame: hunt + conditional descend.
    lds   r25, frame_counter
    andi  r25, 0x03
    brne  ul_check_align
    ; Step 1: horizontal seek toward nearest grounded humanoid.
    ; Returns r24 = 1 if any active grounded humanoid exists, else 0.
    rcall ul_seek_humanoid_x
    ; Step 2: vertical descent, gated by altitude and humanoid presence.
    ldd   r25, Y+2
    mov   r26, r25
    andi  r26, 0xC0                      ; preserve state2
    andi  r25, 0x3F
    cpi   r25, 47
    brsh  ul_check_align                 ; clamped at world bottom
    cpi   r25, 40
    brlo  ul_do_descend                  ; below grab altitude — always descend
    cpi   r24, 0
    brne  ul_check_align                 ; humanoid alive → freeze at 40..46
ul_do_descend:
    inc   r25
    or    r25, r26
    std   Y+2, r25
ul_check_align:
    ; If exactly at y == 40 and an active humanoid is x-aligned (|dx| < 8),
    ; arm the pause counter.
    ldd   r25, Y+2
    andi  r25, 0x3F
    cpi   r25, 40
    brne  ul_done
    rcall ul_humanoid_aligned
    cpi   r24, 0
    breq  ul_done
    ld    r24, Y
    ori   r24, 0x0E                      ; pause = 7 (bits 1-3)
    st    Y, r24
    rjmp  ul_done

ul_pausing:
    lds   r25, frame_counter
    andi  r25, 0x03
    brne  ul_done                        ; not a drift frame
    ld    r24, Y
    subi  r24, 0x02                      ; pause -= 1 (bit 1 = "ones" of pause)
    st    Y, r24
    andi  r24, 0x0E
    brne  ul_done                        ; pause > 0; keep waiting
    rcall ul_do_grab                     ; pause expired — grab now
    rjmp  ul_done

ul_carrying:
    lds   r25, frame_counter
    andi  r25, 0x03
    brne  ul_done
    ldd   r25, Y+2
    mov   r24, r25
    andi  r24, 0xC0
    andi  r25, 0x3F
    cpi   r25, 9
    brlo  ul_done                        ; y < 9 (shouldn't happen — safety net)
    breq  ul_become_mutant               ; y == 9 → transform at the threshold
    dec   r25
    or    r25, r24
    std   Y+2, r25
ul_done:
    ret

; ul_become_mutant: in-place type change LANDER → MUTANT.
; Preserves the active bit and overwrites everything else (carry, pause).
; world_x and y are inherited so the Mutant pops in where the Lander was.
; A carrying Lander reaching the top consumes the humanoid it was
; carrying, so we run the planet-destruction check after the type swap.
ul_become_mutant:
    ld    r25, Y
    andi  r25, 0x80
    ori   r25, (TYPE_MUTANT << 4)
    st    Y, r25
    rcall check_planet_destruction
    ret

; ul_humanoid_aligned: returns r24 = 1 if any active humanoid is within ±7
; world cols of the lander, else r24 = 0. Y on entry points at the lander
; slot; preserved on return.
ul_humanoid_aligned:
    ldd   r25, Y+1                       ; lander world_x
    push  r28
    push  r29
    push  r18
    ldi   r28, lo8(entities)
    ldi   r29, hi8(entities)
    ldi   r18, ENTITY_SLOTS
uha_loop:
    ld    r26, Y
    sbrs  r26, 7
    rjmp  uha_next
    mov   r27, r26
    swap  r27
    andi  r27, 0x07
    cpi   r27, TYPE_HUMANOID
    brne  uha_next
    ldd   r27, Y+1
    sub   r27, r25
    sbrc  r27, 7
    neg   r27
    cpi   r27, 8
    brsh  uha_next
    pop   r18
    pop   r29
    pop   r28
    ldi   r24, 1
    ret
uha_next:
    adiw  r28, ENTITY_SIZE
    dec   r18
    brne  uha_loop
    pop   r18
    pop   r29
    pop   r28
    ldi   r24, 0
    ret

; ul_seek_humanoid_x: scan entity slots for active GROUNDED humanoids
; (TYPE_HUMANOID, byte 0 bit 0 = 0); find the closest one (signed-delta,
; wrap-aware) and move the lander's world_x by 1 col toward it. No-op when
; no candidates exist.
;
; Returns r24 = 1 if any candidate was found, else r24 = 0. The caller uses
; this flag to gate descent at the grab-altitude band (40..46): freeze if a
; humanoid is alive somewhere, descend through if not.
;
; Y on entry points at the lander's slot byte 0; preserved on return.
ul_seek_humanoid_x:
    ldd   r24, Y+1                       ; lander world_x
    push  r28
    push  r29
    push  r18
    ldi   r28, lo8(entities)
    ldi   r29, hi8(entities)
    ldi   r18, ENTITY_SLOTS
    ldi   r25, 0xFF                      ; min |delta| sentinel
    ldi   r26, 0                         ; best-target world_x
ush_loop:
    ld    r19, Y
    sbrs  r19, 7
    rjmp  ush_next                       ; inactive slot
    sbrc  r19, 0
    rjmp  ush_next                       ; falling humanoid / carrying lander — skip
    mov   r20, r19
    swap  r20
    andi  r20, 0x07
    cpi   r20, TYPE_HUMANOID
    brne  ush_next
    ldd   r20, Y+1
    sub   r20, r24
    sbrc  r20, 7
    neg   r20                            ; r20 = |signed delta| (shorter wrap)
    cp    r20, r25
    brsh  ush_next                       ; not closer than current best
    mov   r25, r20
    ldd   r26, Y+1
ush_next:
    adiw  r28, ENTITY_SIZE
    dec   r18
    brne  ush_loop
    pop   r18
    pop   r29
    pop   r28
    cpi   r25, 0xFF
    breq  ush_no_target
    cpi   r25, 0
    breq  ush_aligned                    ; already aligned — no movement
    ; Move world_x by 1 toward target (sign of (target - lander) → direction)
    sub   r26, r24
    sbrc  r26, 7
    rjmp  ush_left
    inc   r24
    rjmp  ush_store
ush_left:
    dec   r24
ush_store:
    std   Y+1, r24
ush_aligned:
    ldi   r24, 1
    ret
ush_no_target:
    ldi   r24, 0
    ret

; ul_do_grab: pause counter just expired. Find the aligned humanoid (still
; should be at the same x), deactivate it, set the lander's carry bit, and
; queue SFX_GRAB. If the humanoid was killed during the pause (e.g. by a
; beam), this scan finds no match and the lander silently resumes its idle
; state (pause bits are already 0 from the caller's subi).
; Y points at the lander slot; preserved.
ul_do_grab:
    ldd   r25, Y+1
    push  r28
    push  r29
    push  r18
    ldi   r28, lo8(entities)
    ldi   r29, hi8(entities)
    ldi   r18, ENTITY_SLOTS
udg_loop:
    ld    r26, Y
    sbrs  r26, 7
    rjmp  udg_next
    mov   r27, r26
    swap  r27
    andi  r27, 0x07
    cpi   r27, TYPE_HUMANOID
    brne  udg_next
    ldd   r27, Y+1
    sub   r27, r25
    sbrc  r27, 7
    neg   r27
    cpi   r27, 8
    brsh  udg_next
    clr   r27
    st    Y, r27
    pop   r18
    pop   r29
    pop   r28
    ld    r27, Y
    ori   r27, 1
    st    Y, r27
    ldi   r27, 4                         ; SFX_GRAB
    sts   pending_sfx, r27
    ret
udg_next:
    adiw  r28, ENTITY_SIZE
    dec   r18
    brne  udg_loop
    pop   r18
    pop   r29
    pop   r28
    ret

; ============================================================================
; render_entities: iterate the entity slots; for each active one, dispatch through
; render_dispatch by type. Each handler may use blit_8x8 (which clobbers Y),
; so the dispatch saves/restores Y around the call.
; ============================================================================
render_entities:
    ldi   r28, lo8(entities)
    ldi   r29, hi8(entities)
    ldi   r18, ENTITY_SLOTS
re_loop:
    ld    r24, Y
    sbrs  r24, 7
    rjmp  re_next
    mov   r25, r24
    swap  r25
    andi  r25, 0x07
    push  r28
    push  r29
    push  r18
    ldi   r30, pm_lo8(render_dispatch)
    ldi   r31, pm_hi8(render_dispatch)
    add   r30, r25
    clr   r25
    adc   r31, r25
    icall
    pop   r18
    pop   r29
    pop   r28
re_next:
    adiw  r28, ENTITY_SIZE
    dec   r18
    brne  re_loop
    ret

render_dispatch:
    rjmp  render_noop
    rjmp  render_lander
    rjmp  render_mutant                  ; type 2 = Mutant
    rjmp  render_pod                     ; type 3 = Pod
    rjmp  render_noop                    ; type 4 = (Bomber)
    rjmp  render_swarmer                 ; type 5 = Swarmer
    rjmp  render_noop                    ; type 6 = (Baiter)  — Phase 4
    rjmp  render_humanoid                ; type 7 = Humanoid

render_noop:
    ret

; render_lander: convert world_x → screen_x; if visible, blit lander sprite.
; If the Lander is carrying a humanoid (byte 0 bit 0), also blit the humanoid
; sprite directly below.
; Y points at the slot's byte 0 on entry. Allowed to clobber Y (caller saves).
render_lander:
    ldd   r20, Y+1                       ; world_x
    lds   r19, scroll_offset
    sub   r20, r19                       ; screen_x = world_x - scroll (mod 256)
    sbrc  r20, 7
    rjmp  rl_offscreen                   ; negative → off the left edge
    cpi   r20, 121
    brsh  rl_offscreen                   ; >= 121 → too close to right edge
    ld    r22, Y                         ; byte 0 (for carry-bit check)
    ldd   r21, Y+2
    andi  r21, 0x3F                      ; r21 = y
    mov   r16, r20                       ; x for blit_8x8
    mov   r17, r21                       ; y
    push  r20
    push  r21
    push  r22
    ldi   r30, lo8(lander_sprite)
    ldi   r31, hi8(lander_sprite)
    call  blit_8x8
    pop   r22
    pop   r21
    pop   r20
    sbrs  r22, 0                         ; carrying humanoid?
    rjmp  rl_offscreen
    ; Blit humanoid 8 rows below; clamp if off-screen vertically.
    mov   r17, r21
    subi  r17, -8                        ; r17 = lander_y + 8
    cpi   r17, 56
    brsh  rl_offscreen                   ; below visible area
    mov   r16, r20
    ldi   r30, lo8(humanoid_sprite)
    ldi   r31, hi8(humanoid_sprite)
    call  blit_8x8
rl_offscreen:
    ret

; render_mutant: same wrapping logic as render_lander; uses mutant_sprite.
render_mutant:
    ldd   r20, Y+1
    lds   r19, scroll_offset
    sub   r20, r19
    sbrc  r20, 7
    rjmp  rm_offscreen
    cpi   r20, 121
    brsh  rm_offscreen
    ldd   r21, Y+2
    andi  r21, 0x3F
    mov   r16, r20
    mov   r17, r21
    ldi   r30, lo8(mutant_sprite)
    ldi   r31, hi8(mutant_sprite)
    call  blit_8x8
rm_offscreen:
    ret

; render_humanoid: convert world_x → screen_x; if visible, blit humanoid.
; Same wrapping discipline as render_lander.
render_humanoid:
    ldd   r20, Y+1
    lds   r19, scroll_offset
    sub   r20, r19
    sbrc  r20, 7
    rjmp  rh_offscreen
    cpi   r20, 121
    brsh  rh_offscreen
    ldd   r21, Y+2
    andi  r21, 0x3F
    mov   r16, r20
    mov   r17, r21
    ldi   r30, lo8(humanoid_sprite)
    ldi   r31, hi8(humanoid_sprite)
    call  blit_8x8
rh_offscreen:
    ret

; render_pod / render_swarmer: same wrapping logic as render_lander.
render_pod:
    ldd   r20, Y+1
    lds   r19, scroll_offset
    sub   r20, r19
    sbrc  r20, 7
    rjmp  rpod_offscreen
    cpi   r20, 121
    brsh  rpod_offscreen
    ldd   r21, Y+2
    andi  r21, 0x3F
    mov   r16, r20
    mov   r17, r21
    ldi   r30, lo8(pod_sprite)
    ldi   r31, hi8(pod_sprite)
    call  blit_8x8
rpod_offscreen:
    ret

render_swarmer:
    ldd   r20, Y+1
    lds   r19, scroll_offset
    sub   r20, r19
    sbrc  r20, 7
    rjmp  rsw_offscreen
    cpi   r20, 121
    brsh  rsw_offscreen
    ldd   r21, Y+2
    andi  r21, 0x3F
    mov   r16, r20
    mov   r17, r21
    ldi   r30, lo8(swarmer_sprite)
    ldi   r31, hi8(swarmer_sprite)
    call  blit_8x8
rsw_offscreen:
    ret

; Lander sprite — 8x8, simple bug-like shape, framebuffer-column order.
; Visual layout:
;     ........
;     ..XXXX..
;     .XXXXXX.
;     XXXXXXXX
;     XXXXXXXX
;     .XXXXXX.
;     ..XXXX..
;     ........
lander_sprite:
    .byte 0x18, 0x3C, 0x7E, 0x7E, 0x7E, 0x7E, 0x3C, 0x18
    .balign 2

; humanoid_sprite — 8×8 stick figure (head, arms out, body, legs splay).
; Visual:
;     ..XXXX..   row 0
;     ..XXXX..   row 1   (head)
;     ...XX...   row 2   (neck)
;     .XXXXXX.   row 3   (arms)
;     ...XX...   row 4   (body)
;     ...XX...   row 5
;     ..X..X..   row 6   (legs apart)
;     .XX..XX.   row 7   (feet)
humanoid_sprite:
    .byte 0x00, 0x88, 0xCB, 0x3F, 0x3F, 0xCB, 0x88, 0x00
    .balign 2

; mutant_sprite — 8×8 angular alien (head with antennae, splayed claws/legs).
; Visual:
;     X.X..X.X   row 0  (antennae)
;     .XXXXXX.   row 1
;     XXX..XXX   row 2  (outstretched claws)
;     .XXXXXX.   row 3
;     ..XXXX..   row 4  (body)
;     .X.XX.X.   row 5
;     X..XX..X   row 6  (legs)
;     .X....X.   row 7
mutant_sprite:
    .byte 0x45, 0xAE, 0x1F, 0x7A, 0x7A, 0x1F, 0xAE, 0x45
    .balign 2

; pod_sprite — 8×8 hollow oval. Visually distinct from the solid Lander
; (lander is filled, pod is a ring) so the player can tell them apart
; at a glance. Slow drifter, fat target.
; Visual:
;     ...XX...   row 0
;     ..X..X..   row 1
;     .X....X.   row 2
;     X......X   row 3
;     X......X   row 4
;     .X....X.   row 5
;     ..X..X..   row 6
;     ...XX...   row 7
pod_sprite:
    .byte 0x18, 0x24, 0x42, 0x81, 0x81, 0x42, 0x24, 0x18
    .balign 2

; swarmer_sprite — small "X" inside the 8×8 box. Tiny target, hard to
; hit; spawns 5-at-a-time from a killed Pod.
; Visual:
;     ........   row 0
;     ........   row 1
;     ..X..X..   row 2
;     ...XX...   row 3
;     ...XX...   row 4
;     ..X..X..   row 5
;     ........   row 6
;     ........   row 7
swarmer_sprite:
    .byte 0x00, 0x00, 0x24, 0x18, 0x18, 0x24, 0x00, 0x00
    .balign 2

; ============================================================================
; wave_sizes_table — total Lander spawns per (difficulty, wave). 8 columns
; indexed by min(wave_number-1, 7); 3 rows for LOW / MED / HIGH.
; HIGH row matches Defenduino (15 wave 1, 20 thereafter).
; ============================================================================
wave_sizes_table:
    .byte  5,  7,  8,  9, 10, 12, 12, 12      ; LOW  (gentle)
    .byte 10, 13, 14, 16, 18, 20, 20, 20      ; MED  (moderate)
    .byte 15, 20, 20, 20, 20, 20, 20, 20      ; HIGH (Defenduino-equivalent)
    .balign 2

; wave_pod_count_table — Pods (out of wave_to_spawn) per (difficulty, wave).
; Pods spawn first; remaining wave_to_spawn count is Landers. Each Pod
; killed becomes 5 Swarmers, so a single Pod adds substantial pressure
; to the wave even if the table says "1".
wave_pod_count_table:
    .byte  0,  0,  0,  1,  1,  1,  2,  2      ; LOW
    .byte  0,  0,  1,  1,  2,  2,  2,  3      ; MED
    .byte  0,  1,  1,  2,  2,  3,  3,  3      ; HIGH
    .balign 2

; ============================================================================
; 8x8 letter sprites for the title screen. Indexed by font index:
;   0=D 1=E 2=F 3=N 4=R 5=L 6=O 7=W 8=M 9=H 10=I 11=G 12=ARROW
; Each letter occupies the leftmost 5 cols; cols 5-7 are zero padding so
; consecutive blits at +8 px look like inter-letter spacing.
; ============================================================================
font_table:
    .byte 0x7F, 0x41, 0x41, 0x41, 0x3E, 0x00, 0x00, 0x00   ;  0 D
    .byte 0x7F, 0x49, 0x49, 0x49, 0x41, 0x00, 0x00, 0x00   ;  1 E
    .byte 0x7F, 0x09, 0x09, 0x09, 0x01, 0x00, 0x00, 0x00   ;  2 F
    .byte 0x7F, 0x02, 0x04, 0x08, 0x7F, 0x00, 0x00, 0x00   ;  3 N
    .byte 0x7F, 0x09, 0x19, 0x29, 0x46, 0x00, 0x00, 0x00   ;  4 R
    .byte 0x7F, 0x40, 0x40, 0x40, 0x40, 0x00, 0x00, 0x00   ;  5 L
    .byte 0x3E, 0x41, 0x41, 0x41, 0x3E, 0x00, 0x00, 0x00   ;  6 O
    .byte 0x7F, 0x20, 0x18, 0x20, 0x7F, 0x00, 0x00, 0x00   ;  7 W
    .byte 0x7F, 0x02, 0x04, 0x02, 0x7F, 0x00, 0x00, 0x00   ;  8 M
    .byte 0x7F, 0x08, 0x08, 0x08, 0x7F, 0x00, 0x00, 0x00   ;  9 H
    .byte 0x41, 0x41, 0x7F, 0x41, 0x41, 0x00, 0x00, 0x00   ; 10 I
    .byte 0x3E, 0x41, 0x49, 0x49, 0x3A, 0x00, 0x00, 0x00   ; 11 G
    .byte 0x00, 0x7F, 0x3E, 0x1C, 0x08, 0x00, 0x00, 0x00   ; 12 ARROW (>)
    ; Digits 0-9 at indices 13..22. Each is also a 5×7 glyph in 8 bytes.
    .byte 0x3E, 0x49, 0x45, 0x43, 0x3E, 0x00, 0x00, 0x00   ; 13 '0' (slashed)
    .byte 0x42, 0x7F, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00   ; 14 '1'
    .byte 0x42, 0x61, 0x51, 0x49, 0x46, 0x00, 0x00, 0x00   ; 15 '2'
    .byte 0x22, 0x49, 0x49, 0x49, 0x36, 0x00, 0x00, 0x00   ; 16 '3'
    .byte 0x0F, 0x08, 0x08, 0x7F, 0x08, 0x00, 0x00, 0x00   ; 17 '4'
    .byte 0x4F, 0x49, 0x49, 0x49, 0x31, 0x00, 0x00, 0x00   ; 18 '5'
    .byte 0x3E, 0x49, 0x49, 0x49, 0x31, 0x00, 0x00, 0x00   ; 19 '6'
    .byte 0x01, 0x01, 0x79, 0x05, 0x03, 0x00, 0x00, 0x00   ; 20 '7'
    .byte 0x36, 0x49, 0x49, 0x49, 0x36, 0x00, 0x00, 0x00   ; 21 '8'
    .byte 0x0E, 0x51, 0x51, 0x51, 0x7E, 0x00, 0x00, 0x00   ; 22 '9'
    .byte 0x7E, 0x09, 0x09, 0x09, 0x7E, 0x00, 0x00, 0x00   ; 23 A
    .byte 0x1F, 0x20, 0x40, 0x20, 0x1F, 0x00, 0x00, 0x00   ; 24 V
    .byte 0x46, 0x49, 0x49, 0x49, 0x31, 0x00, 0x00, 0x00   ; 25 S
    .byte 0x3E, 0x41, 0x41, 0x41, 0x22, 0x00, 0x00, 0x00   ; 26 C
    .byte 0x01, 0x01, 0x7F, 0x01, 0x01, 0x00, 0x00, 0x00   ; 27 T
    .balign 2

.equ FONT_DIGIT_BASE, 13                              ; index of '0' in font_table
.equ FONT_A,          23
.equ FONT_V,          24
.equ FONT_S,          25
.equ FONT_C,          26
.equ FONT_T,          27

; ============================================================================
; Ship sprites (per ship_shape.txt). Drawn in framebuffer-column order;
; col 0 of each row maps to framebuffer col 0 = visual right after the
; 0xA1 mirror, so a right-facing ship has its cockpit at LOW fb cols.
;
; ship_right:
;     ........            row 0
;     ........            row 1
;     .XX.....            row 2  (cockpit)
;     XXXX....            row 3  (upper hull)
;     XXXXXXXX            row 4  (body, full)
;     .XXXXX..            row 5  (lower hull, tail-tapered)
;     ........            row 6
;     ........            row 7
.global ship_right
ship_right:
    .byte 0x18, 0x3C, 0x3C, 0x38, 0x30, 0x30, 0x10, 0x10
.global ship_left
ship_left:
    .byte 0x10, 0x10, 0x30, 0x30, 0x38, 0x3C, 0x3C, 0x18
    .balign 2

; ============================================================================
; height_table — 256 bytes of mountain heights (0..7), one per world col.
; Generated by hash h(i) = (i ^ (i>>3) ^ (i>>5)) & 7 — period 256.
; render_horizon indexes by (scroll+c) mod 256; render_radar samples
; every 4th entry for 4:1 compression.
.global height_table
height_table:
    .byte 0, 1, 2, 3, 4, 5, 6, 7, 1, 0, 3, 2, 5, 4, 7, 6
    .byte 2, 3, 0, 1, 6, 7, 4, 5, 3, 2, 1, 0, 7, 6, 5, 4
    .byte 5, 4, 7, 6, 1, 0, 3, 2, 4, 5, 6, 7, 0, 1, 2, 3
    .byte 7, 6, 5, 4, 3, 2, 1, 0, 6, 7, 4, 5, 2, 3, 0, 1
    .byte 2, 3, 0, 1, 6, 7, 4, 5, 3, 2, 1, 0, 7, 6, 5, 4
    .byte 0, 1, 2, 3, 4, 5, 6, 7, 1, 0, 3, 2, 5, 4, 7, 6
    .byte 7, 6, 5, 4, 3, 2, 1, 0, 6, 7, 4, 5, 2, 3, 0, 1
    .byte 5, 4, 7, 6, 1, 0, 3, 2, 4, 5, 6, 7, 0, 1, 2, 3
    .byte 4, 5, 6, 7, 0, 1, 2, 3, 5, 4, 7, 6, 1, 0, 3, 2
    .byte 6, 7, 4, 5, 2, 3, 0, 1, 7, 6, 5, 4, 3, 2, 1, 0
    .byte 1, 0, 3, 2, 5, 4, 7, 6, 0, 1, 2, 3, 4, 5, 6, 7
    .byte 3, 2, 1, 0, 7, 6, 5, 4, 2, 3, 0, 1, 6, 7, 4, 5
    .byte 6, 7, 4, 5, 2, 3, 0, 1, 7, 6, 5, 4, 3, 2, 1, 0
    .byte 4, 5, 6, 7, 0, 1, 2, 3, 5, 4, 7, 6, 1, 0, 3, 2
    .byte 3, 2, 1, 0, 7, 6, 5, 4, 2, 3, 0, 1, 6, 7, 4, 5
    .byte 1, 0, 3, 2, 5, 4, 7, 6, 0, 1, 2, 3, 4, 5, 6, 7

; mask_table — 9 entries; mask_table[h] = (0xFF << (8-h)) & 0xFF.
; Bottom h pixels of an 8-pixel column-byte lit, top 8-h dark.
.global mask_table
mask_table:
    .byte 0x00, 0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE, 0xFF
    .balign 2
