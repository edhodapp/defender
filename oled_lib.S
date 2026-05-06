; SSD1306 driver subroutines for the original Arduboy.
;
; Public API (all routines preserve nothing beyond what's documented):
;   oled_init    Configure pins, reset OLED, enable SPI, send init sequence.
;                Clobbers: r16, r24, r25, r26, Z (r30:r31).
;   oled_clear   Blast 1024 zeros into the OLED display RAM.
;                Clobbers: r24, r25, r26, r27.
;   oled_render  Push 1024 bytes from RAM[Z..Z+1023] to the OLED.
;                Input:    Z = RAM pointer.
;                Clobbers: r24, r25, r26, r27, Z (advanced past end).
;
; Pin map (Arduboy original):
;   PB0 = SS  (output, kept high; required for SPI master mode)
;   PB1 = SCK
;   PB2 = MOSI
;   PD4 = DC  (0 = command, 1 = data)
;   PD6 = CS  (active low)
;   PD7 = RES (active low)

#include <avr/io.h>

#define DDRB_MASK   ((1<<DDB0)|(1<<DDB1)|(1<<DDB2))
#define DDRD_MASK   ((1<<DDD4)|(1<<DDD6)|(1<<DDD7))

.section .text, "ax", @progbits

; ============================================================================
.global oled_init
oled_init:
    ; --- DDR setup ---
    in    r16, _SFR_IO_ADDR(DDRB)
    ori   r16, DDRB_MASK
    out   _SFR_IO_ADDR(DDRB), r16

    in    r16, _SFR_IO_ADDR(DDRD)
    ori   r16, DDRD_MASK
    out   _SFR_IO_ADDR(DDRD), r16

    ; --- Initial pin levels: CS=1 (idle), RES=1, DC=0 ---
    sbi   _SFR_IO_ADDR(PORTD), 6
    sbi   _SFR_IO_ADDR(PORTD), 7
    cbi   _SFR_IO_ADDR(PORTD), 4

    ; --- Reset pulse: RES low ~3 µs, RES high ~3 µs ---
    cbi   _SFR_IO_ADDR(PORTD), 7
    ldi   r16, 60
1:  dec   r16
    brne  1b
    sbi   _SFR_IO_ADDR(PORTD), 7
    ldi   r16, 60
2:  dec   r16
    brne  2b

    ; --- SPI: master, MSB first, F_CPU/2 = 8 MHz ---
    ldi   r16, (1<<SPE)|(1<<MSTR)
    out   _SFR_IO_ADDR(SPCR), r16
    ldi   r16, (1<<SPI2X)
    out   _SFR_IO_ADDR(SPSR), r16

    ; --- Send init sequence: CS=0, DC=0 ---
    cbi   _SFR_IO_ADDR(PORTD), 6
    cbi   _SFR_IO_ADDR(PORTD), 4

    ldi   r30, lo8(_oled_init_seq)
    ldi   r31, hi8(_oled_init_seq)
    ldi   r24, _oled_init_seq_end - _oled_init_seq
3:  lpm   r25, Z+
    out   _SFR_IO_ADDR(SPDR), r25
4:  in    r26, _SFR_IO_ADDR(SPSR)
    sbrs  r26, SPIF
    rjmp  4b
    in    r26, _SFR_IO_ADDR(SPDR)
    dec   r24
    brne  3b

    sbi   _SFR_IO_ADDR(PORTD), 6        ; CS=1
    ret

; ============================================================================
.global oled_clear
oled_clear:
    cbi   _SFR_IO_ADDR(PORTD), 6        ; CS=0
    sbi   _SFR_IO_ADDR(PORTD), 4        ; DC=1 (data)
    clr   r25                            ; payload byte = 0
    ldi   r24, 4                         ; outer = 4
1:  clr   r26                            ; inner = 256 (wraps from 0)
2:  out   _SFR_IO_ADDR(SPDR), r25
3:  in    r27, _SFR_IO_ADDR(SPSR)
    sbrs  r27, SPIF
    rjmp  3b
    in    r27, _SFR_IO_ADDR(SPDR)
    dec   r26
    brne  2b
    dec   r24
    brne  1b
    sbi   _SFR_IO_ADDR(PORTD), 6        ; CS=1
    ret

; ============================================================================
.global oled_render
oled_render:
    cbi   _SFR_IO_ADDR(PORTD), 6        ; CS=0
    sbi   _SFR_IO_ADDR(PORTD), 4        ; DC=1 (data)
    ldi   r24, 4
1:  clr   r26
2:  ld    r25, Z+
    out   _SFR_IO_ADDR(SPDR), r25
3:  in    r27, _SFR_IO_ADDR(SPSR)
    sbrs  r27, SPIF
    rjmp  3b
    in    r27, _SFR_IO_ADDR(SPDR)
    dec   r26
    brne  2b
    dec   r24
    brne  1b
    sbi   _SFR_IO_ADDR(PORTD), 6        ; CS=1
    ret

; ============================================================================
_oled_init_seq:
    .byte 0xAE
    .byte 0xD5, 0xF0
    .byte 0xA8, 0x3F
    .byte 0xD3, 0x00
    .byte 0x40
    .byte 0x8D, 0x14
    .byte 0x20, 0x00
    .byte 0xA1
    .byte 0xC8
    .byte 0xDA, 0x12
    .byte 0x81, 0xCF
    .byte 0xD9, 0xF1
    .byte 0xDB, 0x40
    .byte 0xA4
    .byte 0xA6
    .byte 0xAF
_oled_init_seq_end:
    .balign 2                            ; keep subsequent code word-aligned
