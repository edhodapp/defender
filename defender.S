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
.global spawn_pos_idx
spawn_pos_idx:  .skip 1                  ; advances each lander spawn; world_x = idx*32 + 16

.global fire_cooldown
fire_cooldown:  .skip 1                  ; auto-fire throttle; reloads to FIRE_COOLDOWN_RELOAD after each shot
.equ FIRE_COOLDOWN_RELOAD, 5             ; shot every (5+1)=6 frames @ 60 Hz = 10 shots/sec

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
.equ TYPE_HUMANOID, 7                    ; matches dispatch slot 7
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
; Each vector slot is 4 bytes (room for one JMP) on >8 KB AVRs. Only RESET
; and TIMER1_COMPA are used; everything else is uninitialized (=0=nop)
; because no other interrupt source is enabled.
;   slot 1  byte 0x00 — RESET
;   slot 18 byte 0x44 — TIMER1 COMPA  (= main_loop wakeup)
.section .vectors, "ax", @progbits
.org 0x00
    rjmp _reset                                ; slot 1 (RESET)
.org 0x44                                      ; (18 - 1) * 4 — slot 18
    rjmp _timer1_compa_isr                     ; TIMER1 COMPA

.section .text, "ax", @progbits

; ISR is a pure wakeup stub — its only purpose is to bring the CPU out of
; SLEEP_IDLE so main_loop continues for the next frame.
.global _timer1_compa_isr
_timer1_compa_isr:
    reti

.global _reset
_reset:
    ldi   r16, lo8(RAMEND)
    out   _SFR_IO_ADDR(SPL), r16
    ldi   r16, hi8(RAMEND)
    out   _SFR_IO_ADDR(SPH), r16

    rcall oled_init
    rcall input_init
    rcall audio_init

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

    ; Spawn one Lander at world_x=100, y=10 (slot 0)
    ldi   r24, (1<<7) | (TYPE_LANDER<<4)   ; active, type=Lander, state=0
    sts   entities + 0, r24
    ldi   r24, 100
    sts   entities + 1, r24                ; world_x
    ldi   r24, 10
    sts   entities + 2, r24                ; y (state2 = 0)

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

.global main_loop
main_loop:
    ; Frame tick: bump frame counter (used as divider for drift timing)
    lds   r25, frame_counter
    inc   r25
    sts   frame_counter, r25

    ; Advance any active SFX by one frame
    rcall audio_tick

    ; Periodic Lander spawn: once per 256-frame counter cycle, fired at
    ; frame_counter == 128. ~4.3 s at 60 Hz — longer than a worst-case
    ; humanoid fall (160 frames from y=8 to y=48), so a freed humanoid
    ; reaches the ground before the next Lander appears.
    ;
    ; Reload r25 — audio_tick clobbers it.
    lds   r25, frame_counter
    cpi   r25, 128
    brne  no_spawn_check
    rcall try_spawn_lander
no_spawn_check:

    rcall input_read                     ; r24 = button bitmask

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
    rcall audio_play
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
    rcall audio_play
    clr   r24
    sts   pending_sfx, r24
no_pending_sfx:

    ; --- Render this frame ---
    rcall clear_fb
    rcall render_horizon
    rcall render_radar
    rcall render_projectiles
    rcall render_entities

    ; --- Pick sprite by facing: Z = ship_right + facing*8 ---
    lds   r25, ship_facing
    ldi   r30, lo8(ship_right)
    ldi   r31, hi8(ship_right)
    lsl   r25
    lsl   r25
    lsl   r25
    add   r30, r25
    ldi   r25, 0
    adc   r31, r25

    ; --- Blit ship: x locked at 60, y from sprite_y (reload — entity
    ; render may have overwritten r17 with another sprite's y).
    ldi   r16, 60
    lds   r17, sprite_y
    rcall blit_8x8

    ; --- Death flash: if active, invert the entire framebuffer ---
    rcall invert_if_dead

    ; --- Render ---
    ldi   r30, lo8(framebuffer)
    ldi   r31, hi8(framebuffer)
    rcall oled_render

    ; --- Frame cap: sleep until Timer1 Compare-A interrupt fires ---
    ; SMCR is already armed (SE=1, SLEEP_IDLE). The ISR is a bare reti;
    ; it exists only so the CPU wakes back up here.
    sleep

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
    brne  rr_ent_next                    ; only Landers plot on radar (for now)

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
try_spawn_lander:
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
    ret                                  ; no free slots
tsl_have_slot:
    ; world_x = spawn_pos_idx * 32 + 16
    lds   r25, spawn_pos_idx
    lsl   r25
    lsl   r25
    lsl   r25
    lsl   r25
    lsl   r25                            ; r25 = idx * 32 (mod 256)
    subi  r25, -16                       ; +16
    ldi   r24, (1<<7) | (TYPE_LANDER<<4)
    st    Y, r24
    std   Y+1, r25
    ldi   r24, 10                        ; y
    std   Y+2, r24
    ; advance spawn_pos_idx
    lds   r24, spawn_pos_idx
    inc   r24
    sts   spawn_pos_idx, r24
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
;   * carrying Lander, beam in lower half [y+8, y+15] → kill BOTH (the
;     humanoid was the one shot; the lander goes down with it)
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
    ; Carrying Lander hit. If the beam was in the lower 8 rows, the
    ; humanoid took the shot — kill both. Otherwise, free the humanoid.
    ldd   r26, Y+2
    andi  r26, 0x3F                      ; entity_y
    mov   r27, r17                       ; beam_y
    sub   r27, r26                       ; r27 = beam_y - entity_y
    cpi   r27, 8
    brsh  chh_kill                       ; lower half → kill both
    ; Upper half — free the humanoid as a falling one.
    ldi   r25, 0x80 | (TYPE_HUMANOID << 4) | 0x01
    st    Y, r25
    ret
chh_kill:
    clr   r25
    st    Y, r25
    ret

; ============================================================================
; ship_collision_check: scan active Landers; on overlap with the ship's 8×8
; box (cols 60..67, rows sprite_y..sprite_y+7), reset ship to y=28 and arm
; the death-flash counter. Lander survives (Defender behavior).
;
; Clobbers: r17, r18, r24, r25, r26, r27, Y
; ============================================================================
ship_collision_check:
    ; Skip while we're already dying — death_flash counts the death animation
    ; down; re-arming each frame would freeze the SFX at sound_frame=0.
    lds   r24, death_flash
    cpi   r24, 0
    breq  sc_alive
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
    brne  sc_next
    ; lander_screen_x in r26
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
    ; HIT — reset ship, arm flash, queue SFX_DEATH; main_loop drains after.
    ldi   r25, 28
    sts   sprite_y, r25
    ldi   r25, 8
    sts   death_flash, r25
    ldi   r25, 3                          ; SFX_DEATH (overrides any pending HIT)
    sts   pending_sfx, r25
    ret
sc_next:
    adiw  r28, ENTITY_SIZE
    dec   r18
    brne  sc_loop
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
    rjmp  update_noop                    ; type 2 = (Mutant)
    rjmp  update_noop                    ; type 3 = (Bomber)
    rjmp  update_noop                    ; type 4 = (Pod)
    rjmp  update_noop                    ; type 5 = (Swarmer)
    rjmp  update_noop                    ; type 6 = (Baiter)
    rjmp  update_humanoid                ; type 7 = Humanoid

update_noop:
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
    brlo  ul_done                        ; y <= 8 → pinned below radar bar
    dec   r25
    or    r25, r24
    std   Y+2, r25
ul_done:
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
    rjmp  render_noop                    ; type 2 = (Mutant)  — Phase 3
    rjmp  render_noop                    ; type 3 = (Bomber)
    rjmp  render_noop                    ; type 4 = (Pod)
    rjmp  render_noop                    ; type 5 = (Swarmer)
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
    rcall blit_8x8
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
    rcall blit_8x8
rl_offscreen:
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
    rcall blit_8x8
rh_offscreen:
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
