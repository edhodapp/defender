; Arduboy toolchain smoke test.
; Toggles the front-facing RGB red LED (PB6, active-low) at ~2 Hz.
; Pure AVR asm: own reset vector, own stack setup, no C runtime.

#include <avr/io.h>

; ---- Reset vector at flash 0x0000 ----
.section .vectors, "ax", @progbits
    rjmp _reset

; ---- Code ----
.section .text, "ax", @progbits
.global _reset
_reset:
    ; Stack pointer = RAMEND (0x0AFF on ATmega32u4)
    ldi   r16, lo8(RAMEND)
    out   _SFR_IO_ADDR(SPL), r16
    ldi   r16, hi8(RAMEND)
    out   _SFR_IO_ADDR(SPH), r16

    ; PB6 -> output
    sbi   _SFR_IO_ADDR(DDRB), 6
    ; PB6 high (LED off; LED is active-low)
    sbi   _SFR_IO_ADDR(PORTB), 6

main_loop:
    ; Atomic toggle: writing 1 to PINn toggles PORTn on modern AVRs (32u4 supports this)
    sbi   _SFR_IO_ADDR(PINB), 6

    ; ~250 ms delay at 16 MHz: ~4,000,000 cycles.
    ; Inner loop is 4 cycles per iter (subi=1, sbci=1, sbci=1, brcc=1 taken).
    ; Counter = 1,000,000 = 0x0F4240.
    ldi   r24, 0x40
    ldi   r25, 0x42
    ldi   r26, 0x0F
1:  subi  r24, 1
    sbci  r25, 0
    sbci  r26, 0
    brcc  1b

    rjmp  main_loop
