; Input subsystem for the original Arduboy.
;
; Six buttons, all active-low with internal pull-ups:
;   UP    = PF7    DOWN  = PF4    LEFT  = PF5    RIGHT = PF6
;   A     = PE6    B     = PB4
;
; Public API:
;   input_init  Configure DDR for input + enable pull-ups. Call once.
;               Clobbers: r24
;   input_read  Read all six buttons; return bitmask in r24:
;                 bit 0 = UP, bit 1 = DOWN, bit 2 = LEFT, bit 3 = RIGHT,
;                 bit 4 = A,  bit 5 = B.
;               1 = pressed, 0 = released.
;               Clobbers: r24

#include <avr/io.h>

.section .text, "ax", @progbits

; ============================================================================
.global input_init
input_init:
    ; DDRF: clear bits 4..7 (input mode)
    in    r24, _SFR_IO_ADDR(DDRF)
    andi  r24, 0x0F
    out   _SFR_IO_ADDR(DDRF), r24

    ; DDRE: clear bit 6 (input mode)
    cbi   _SFR_IO_ADDR(DDRE), 6

    ; DDRB: clear bit 4 (input mode); leave PB0-2 (SPI) alone
    cbi   _SFR_IO_ADDR(DDRB), 4

    ; PORTF: enable pull-ups on bits 4..7
    in    r24, _SFR_IO_ADDR(PORTF)
    ori   r24, 0xF0
    out   _SFR_IO_ADDR(PORTF), r24

    ; PORTE bit 6, PORTB bit 4: pull-ups
    sbi   _SFR_IO_ADDR(PORTE), 6
    sbi   _SFR_IO_ADDR(PORTB), 4

    ret

; ============================================================================
; SBIS = "Skip if I/O Bit Set". Buttons are active-low, so a bit set in PINx
; means the button is *not* pressed. Each test is 2 cycles regardless of
; outcome; total ~17 cycles for all six buttons.
.global input_read
input_read:
    clr   r24

    sbis  _SFR_IO_ADDR(PINF), 7        ; UP
    ori   r24, 0x01
    sbis  _SFR_IO_ADDR(PINF), 4        ; DOWN
    ori   r24, 0x02
    sbis  _SFR_IO_ADDR(PINF), 5        ; LEFT
    ori   r24, 0x04
    sbis  _SFR_IO_ADDR(PINF), 6        ; RIGHT
    ori   r24, 0x08
    sbis  _SFR_IO_ADDR(PINE), 6        ; A
    ori   r24, 0x10
    sbis  _SFR_IO_ADDR(PINB), 4        ; B
    ori   r24, 0x20

    ret
