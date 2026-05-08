; EEPROM read/write for ATmega32u4. The 1 KB internal EEPROM is
; addressable via EEAR (16 bit), EEDR, EECR. Reads are immediate;
; writes block ~3.4 ms while the cell programs.
;
; Public API:
;   eeprom_read_byte   r24:r25 = address (only low 10 bits used).
;                      Returns byte in r24. Clobbers r24, r25.
;   eeprom_write_byte  r24:r25 = address, r22 = data.
;                      Busy-waits until any prior write completes, then
;                      programs the cell and busy-waits for that to
;                      finish so the next call sees a quiescent EECR.
;                      Clobbers r24, r25, SREG.
;
; Notes:
;   - The EEMPE→EEPE sequence has a 4-cycle deadline. We disable
;     interrupts around it so a Timer1/Timer3 ISR cannot stretch the
;     window and abort the write.
;   - EEPROM cells degrade after ~100 k writes, so callers (high score
;     persistence) only write on game-over transitions, not per frame.

#include <avr/io.h>

.section .text, "ax", @progbits

.global eeprom_read_byte
eeprom_read_byte:
    ; Wait for any in-flight write to finish.
1:  sbic  _SFR_IO_ADDR(EECR), EEPE
    rjmp  1b
    out   _SFR_IO_ADDR(EEARH), r25
    out   _SFR_IO_ADDR(EEARL), r24
    sbi   _SFR_IO_ADDR(EECR), EERE
    in    r24, _SFR_IO_ADDR(EEDR)
    ret

.global eeprom_write_byte
eeprom_write_byte:
    ; Wait for any prior write to drain.
1:  sbic  _SFR_IO_ADDR(EECR), EEPE
    rjmp  1b
    out   _SFR_IO_ADDR(EEARH), r25
    out   _SFR_IO_ADDR(EEARL), r24
    out   _SFR_IO_ADDR(EEDR), r22
    in    r25, _SFR_IO_ADDR(SREG)
    cli
    sbi   _SFR_IO_ADDR(EECR), EEMPE      ; arm
    sbi   _SFR_IO_ADDR(EECR), EEPE       ; commit (must be within 4 cycles)
    out   _SFR_IO_ADDR(SREG), r25
    ; Block until programming completes so the next call is race-free.
2:  sbic  _SFR_IO_ADDR(EECR), EEPE
    rjmp  2b
    ret
