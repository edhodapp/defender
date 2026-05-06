; Graphics primitives.
;
; blit_8x8 — OR an 8×8 sprite from flash onto the framebuffer at (x, y).
;
; Implementation: dispatch-table architecture. At entry we compute the
; vertical shift `s = y & 7`, then `IJMP` through an 8-entry rjmp table to
; one of `blit_s0..blit_s7`. Each variant has its `LSL`/`ROL` shift chain
; straight-line unrolled with no inner branch — and `blit_s0` skips the
; upper-page write entirely (the high byte is always 0). Trade flash for
; cycles: 8× duplication is tens of bytes of flash.

#include <avr/io.h>

.section .text, "ax", @progbits

; ============================================================================
; blit_8x8
;
; Inputs:
;   r16   x  (0..120; 121..127 will write off-screen — caller must clamp)
;   r17   y  (0..56;  57..63  similarly)
;   Z     Sprite source: 8 bytes in flash, one per column, bit 0 = top pixel.
;
; Effect:
;   framebuffer[(y/8)*128 + x..+7]     |= column-byte << (y&7)        ; lo
;   framebuffer[((y/8)+1)*128 + x..+7] |= column-byte >> (8-(y&7))    ; hi
;   The hi-page write is skipped entirely when (y & 7) == 0.
;
; Clobbers: r0, r1, r18, r19, r20, r22, r24, r25, X, Y, Z, r2:r3 (sprite stash).
; ============================================================================
.global blit_8x8
blit_8x8:
    ; --- Compute s = y & 7, p = y >> 3 ---
    mov   r19, r17
    andi  r19, 0x07                  ; r19 = s
    mov   r20, r17
    lsr   r20
    lsr   r20
    lsr   r20                        ; r20 = p

    ; --- Y = framebuffer + p*128 + x  (uses MUL; clobbers r0:r1) ---
    ldi   r28, lo8(framebuffer)
    ldi   r29, hi8(framebuffer)
    ldi   r24, 128
    mul   r20, r24                   ; r1:r0 = p * 128
    add   r28, r0
    adc   r29, r1
    clr   r1                         ; restore ABI zero
    add   r28, r16
    ldi   r24, 0
    adc   r29, r24

    ; --- X = Y + 128  (next page; only used when s != 0) ---
    movw  r26, r28
    subi  r26, lo8(-128)
    sbci  r27, hi8(-128)

    ; --- Stash sprite Z into r2:r3 before dispatch overwrites it ---
    movw  r2, r30

    ; --- Column counter (shared by every variant) ---
    ldi   r18, 8

    ; --- Compute dispatch Z = pm(blit_dispatch) + s ---
    ldi   r30, lo8(pm(blit_dispatch))
    ldi   r31, hi8(pm(blit_dispatch))
    add   r30, r19
    clr   r24
    adc   r31, r24
    ijmp

blit_dispatch:
    rjmp  blit_s0
    rjmp  blit_s1
    rjmp  blit_s2
    rjmp  blit_s3
    rjmp  blit_s4
    rjmp  blit_s5
    rjmp  blit_s6
    rjmp  blit_s7

; ============================================================================
; Eight unrolled variants, one per (y & 7) shift count.
;
; Each variant:
;   1. Restores sprite Z from r2:r3 (movw).
;   2. Iterates 8 columns: load sprite byte → shift left s times → OR into
;      framebuffer page p, then OR carry-out bits into framebuffer page p+1
;      (only for s>0; s=0 omits the high-page write entirely).
; ============================================================================

blit_s0:
    movw  r30, r2
.Lbs0_loop:
    lpm   r24, Z+
    ld    r22, Y
    or    r22, r24
    st    Y+, r22
    dec   r18
    brne  .Lbs0_loop
    ret

blit_s1:
    movw  r30, r2
.Lbs1_loop:
    lpm   r24, Z+
    clr   r25
    lsl   r24
    rol   r25
    ld    r22, Y
    or    r22, r24
    st    Y+, r22
    ld    r22, X
    or    r22, r25
    st    X+, r22
    dec   r18
    brne  .Lbs1_loop
    ret

blit_s2:
    movw  r30, r2
.Lbs2_loop:
    lpm   r24, Z+
    clr   r25
    lsl   r24
    rol   r25
    lsl   r24
    rol   r25
    ld    r22, Y
    or    r22, r24
    st    Y+, r22
    ld    r22, X
    or    r22, r25
    st    X+, r22
    dec   r18
    brne  .Lbs2_loop
    ret

blit_s3:
    movw  r30, r2
.Lbs3_loop:
    lpm   r24, Z+
    clr   r25
    lsl   r24
    rol   r25
    lsl   r24
    rol   r25
    lsl   r24
    rol   r25
    ld    r22, Y
    or    r22, r24
    st    Y+, r22
    ld    r22, X
    or    r22, r25
    st    X+, r22
    dec   r18
    brne  .Lbs3_loop
    ret

blit_s4:
    movw  r30, r2
.Lbs4_loop:
    lpm   r24, Z+
    clr   r25
    lsl   r24
    rol   r25
    lsl   r24
    rol   r25
    lsl   r24
    rol   r25
    lsl   r24
    rol   r25
    ld    r22, Y
    or    r22, r24
    st    Y+, r22
    ld    r22, X
    or    r22, r25
    st    X+, r22
    dec   r18
    brne  .Lbs4_loop
    ret

blit_s5:
    movw  r30, r2
.Lbs5_loop:
    lpm   r24, Z+
    clr   r25
    lsl   r24
    rol   r25
    lsl   r24
    rol   r25
    lsl   r24
    rol   r25
    lsl   r24
    rol   r25
    lsl   r24
    rol   r25
    ld    r22, Y
    or    r22, r24
    st    Y+, r22
    ld    r22, X
    or    r22, r25
    st    X+, r22
    dec   r18
    brne  .Lbs5_loop
    ret

blit_s6:
    movw  r30, r2
.Lbs6_loop:
    lpm   r24, Z+
    clr   r25
    lsl   r24
    rol   r25
    lsl   r24
    rol   r25
    lsl   r24
    rol   r25
    lsl   r24
    rol   r25
    lsl   r24
    rol   r25
    lsl   r24
    rol   r25
    ld    r22, Y
    or    r22, r24
    st    Y+, r22
    ld    r22, X
    or    r22, r25
    st    X+, r22
    dec   r18
    brne  .Lbs6_loop
    ret

blit_s7:
    movw  r30, r2
.Lbs7_loop:
    lpm   r24, Z+
    clr   r25
    lsl   r24
    rol   r25
    lsl   r24
    rol   r25
    lsl   r24
    rol   r25
    lsl   r24
    rol   r25
    lsl   r24
    rol   r25
    lsl   r24
    rol   r25
    lsl   r24
    rol   r25
    ld    r22, Y
    or    r22, r24
    st    Y+, r22
    ld    r22, X
    or    r22, r25
    st    X+, r22
    dec   r18
    brne  .Lbs7_loop
    ret
