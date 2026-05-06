; Standalone "OLED dark screen" demo. Calls oled_init + oled_clear from
; oled_lib.S, then halts. This is the minimum-load "device awake" sketch:
; charge pump is on but no pixels are lit.

#include <avr/io.h>

.section .vectors, "ax", @progbits
    rjmp _reset

.section .text, "ax", @progbits

.global _reset
_reset:
    ldi   r16, lo8(RAMEND)
    out   _SFR_IO_ADDR(SPL), r16
    ldi   r16, hi8(RAMEND)
    out   _SFR_IO_ADDR(SPH), r16

    rcall oled_init
    rcall oled_clear

done:
    rjmp  done
