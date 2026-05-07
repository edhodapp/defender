; ============================================================================
; audio.S — square-wave SFX engine for Arduboy
;
; Hardware: PC6 = piezo + (Timer3 OC3A toggle), PC7 = piezo - (held low for
; half-volume single-pin drive; full BTL is a future enhancement).
;
; Timing: Timer3 in CTC mode, prescaler 8 → timer clock = 2 MHz. With OC3A
; toggle on compare match, audible pin frequency = 1 MHz / (OCR3A + 1).
;   OCR3A =  200 → ~5 kHz
;   OCR3A = 2000 → ~500 Hz
;
; State machine: a non-zero `sound_id` selects a per-effect frame table;
; each frame's 16-bit OCR3A value is written by audio_tick at 60 Hz.
; A 0xFFFF sentinel terminates the sound (timer stopped, pin low, id = 0).
;
; Public surface:
;   audio_init         called once in _reset; configures Timer3 and pins.
;   audio_tick         called once per main_loop frame; advances current sfx.
;   audio_play (r24)   request playback of sound id; latest call wins.
;
; Sound IDs (must match values used at trigger sites in defender.S):
;   1 = SFX_FIRE   — descending chirp on B-press spawn
;   2 = SFX_HIT    — noise burst on beam-vs-lander hit
;   3 = SFX_DEATH  — slow descending swoop on ship-vs-lander collision
; ============================================================================

#include <avr/io.h>

.equ SFX_FIRE,  1
.equ SFX_HIT,   2
.equ SFX_DEATH, 3
.equ SFX_GRAB,  4
.equ SFX_START, 5

.section .bss, "aw", @nobits

.global sound_id
sound_id:    .skip 1                              ; 0 = silent; else 1..3
.global sound_frame
sound_frame: .skip 1                              ; 0-based index into the sfx table
.global pending_sfx
pending_sfx: .skip 1                              ; deferred trigger; main_loop drains after each frame

.section .text, "ax", @progbits

; ----------------------------------------------------------------------------
; audio_init — pins, timer registers, state.
; Clobbers: r24
; ----------------------------------------------------------------------------
.global audio_init
audio_init:
    ; PC6 + PC7 as outputs, both low.
    sbi   _SFR_IO_ADDR(DDRC), 6
    sbi   _SFR_IO_ADDR(DDRC), 7
    cbi   _SFR_IO_ADDR(PORTC), 6
    cbi   _SFR_IO_ADDR(PORTC), 7
    ; Stop Timer3, no OC3A pin action.
    clr   r24
    sts   _SFR_MEM_ADDR(TCCR3A), r24
    sts   _SFR_MEM_ADDR(TCCR3B), r24
    sts   sound_id, r24
    sts   sound_frame, r24
    ; Enable Timer3 OC3A compare-match interrupt — its ISR toggles PC7 in
    ; anti-phase with the OC3A hardware toggle on PC6 for BTL audio.
    ldi   r24, (1<<OCIE3A)
    sts   _SFR_MEM_ADDR(TIMSK3), r24
    ret

; ----------------------------------------------------------------------------
; audio_play — start (or restart) the sound id passed in r24.
; Latest call wins (no priority gating yet — fine for current SFX set).
; Out-of-range id (0 or > SFX_DEATH) is ignored.
; Clobbers: r24, r25
; ----------------------------------------------------------------------------
.global audio_play
audio_play:
    cpi   r24, SFX_FIRE
    brlo  audio_play_done                 ; id 0 → ignore
    cpi   r24, SFX_START + 1
    brsh  audio_play_done                 ; id > SFX_START → ignore
    sts   sound_id, r24
    clr   r25
    sts   sound_frame, r25
    ; PC7 starts HIGH so it's anti-phase to PC6 (PC6 starts LOW because
    ; cbi'd in audio_init / when stopping; the first OC3A compare match
    ; toggles PC6 HIGH and the ISR simultaneously toggles PC7 LOW).
    sbi   _SFR_IO_ADDR(PORTC), 7
    ; Enable OC3A toggle on compare match, CTC mode, prescaler 8.
    ldi   r25, (1<<COM3A0)
    sts   _SFR_MEM_ADDR(TCCR3A), r25
    ldi   r25, (1<<WGM32) | (1<<CS31)
    sts   _SFR_MEM_ADDR(TCCR3B), r25
audio_play_done:
    ret

; ----------------------------------------------------------------------------
; audio_tick — advance the current sound by one frame. No-op if silent.
; Reads the next 16-bit OCR3A value from the per-sound table; on 0xFFFF
; sentinel stops the timer and marks silent.
; Clobbers: r0 (used as a local zero register), r24, r25, r26, r27, r30, r31
; ----------------------------------------------------------------------------
.global audio_tick
audio_tick:
    lds   r24, sound_id
    cpi   r24, 0
    breq  audio_tick_done
    ; Z = audio_dispatch[(id - 1)] (word pointer into program memory)
    dec   r24
    clr   r0                              ; local zero register
    ldi   r30, lo8(audio_dispatch)
    ldi   r31, hi8(audio_dispatch)
    add   r30, r24
    adc   r31, r0
    add   r30, r24
    adc   r31, r0
    lpm   r26, Z+
    lpm   r27, Z
    ; Advance to current frame: sfx_table + (sound_frame * 2)
    lds   r25, sound_frame
    add   r26, r25
    adc   r27, r0
    add   r26, r25
    adc   r27, r0
    movw  r30, r26
    lpm   r24, Z+
    lpm   r25, Z
    ; End sentinel: 0xFFFF → stop sound.
    cpi   r24, 0xFF
    brne  audio_tick_play
    cpi   r25, 0xFF
    brne  audio_tick_play
    ; Stop: clear sound_id, sound_frame; halt Timer3; both BTL pins low.
    clr   r24
    sts   sound_id, r24
    sts   sound_frame, r24
    sts   _SFR_MEM_ADDR(TCCR3A), r24
    sts   _SFR_MEM_ADDR(TCCR3B), r24
    cbi   _SFR_IO_ADDR(PORTC), 6
    cbi   _SFR_IO_ADDR(PORTC), 7
    rjmp  audio_tick_done
audio_tick_play:
    ; Write OCR3A: high byte first (16-bit temp register), then low.
    sts   _SFR_MEM_ADDR(OCR3AH), r25
    sts   _SFR_MEM_ADDR(OCR3AL), r24
    lds   r25, sound_frame
    inc   r25
    sts   sound_frame, r25
audio_tick_done:
    ret

; ----------------------------------------------------------------------------
; Sound tables — each is an array of .word OCR3A values, terminated by
; 0xFFFF. Pitch = 1 MHz / (OCR + 1). Larger OCR = lower pitch.
;
; 0xFFFF is reserved as the end-of-sound sentinel — a real frame may not
; legitimately use it. Practical OCR range is far below: at OCR=0xFFFF the
; pitch would be ~15 Hz, well below the audible/useful band.
;
; audio_dispatch holds 16-bit byte addresses of each per-sound table. The
; assembler emits `.word symbol` as the byte address of the symbol, which
; is what `lpm` consumes via Z. If the toolchain ever changes to emit
; word-addressed values (e.g., via `gs(...)` for IJMP tables), the lookup
; in audio_tick would silently break and sounds would be garbage.
; ----------------------------------------------------------------------------
audio_dispatch:
    .word sfx_fire_data
    .word sfx_hit_data
    .word sfx_death_data
    .word sfx_grab_data
    .word sfx_start_data

; SFX_FIRE — fast descending chirp, ~6 frames (= 100 ms @ 60 Hz).
; 5 kHz → 1.4 kHz.
sfx_fire_data:
    .word 200, 250, 300, 400, 500, 700
    .word 0xFFFF

; SFX_HIT — noise burst via rapid OCR jumps, ~8 frames (~133 ms).
; Pre-baked pseudo-random sequence; no LFSR needed at runtime.
sfx_hit_data:
    .word 900, 300, 1500, 600, 1100, 400, 1700, 800
    .word 0xFFFF

; SFX_DEATH — slow descending swoop, ~30 frames (~500 ms).
; 5 kHz → 85 Hz, geometric sweep.
sfx_death_data:
    .word  200,  230,  260,  300,  350,  400,  470,  540,  620,  720
    .word  830,  950, 1100, 1280, 1470, 1690, 1940, 2230, 2560, 2940
    .word 3380, 3880, 4460, 5120, 5880, 6760, 7770, 8930,10260,11790
    .word 0xFFFF

; SFX_GRAB — rising tone (~8 frames). Lander has snatched a humanoid;
; ascending pitch from ~660 Hz to ~4 kHz suggests "lifted up".
sfx_grab_data:
    .word 1500, 1200, 900, 700, 500, 350, 250
    .word 0xFFFF

; SFX_START — boot/level-start jingle. Three-note ascending arpeggio
; (~10 frames ≈ 167 ms). Pitches near C5 / G5 / C6 / E6 — major triad
; rising, "ready to play" feel.
sfx_start_data:
    .word 1908, 1908                      ; ~524 Hz (~C5)
    .word 1276, 1276                      ; ~784 Hz (~G5)
    .word 954,  954                       ; ~1.05 kHz (~C6)
    .word 759,  759                       ; ~1.32 kHz (~E6)
    .word 0xFFFF
