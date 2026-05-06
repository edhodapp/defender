// Virtual SSD1306 display model for simavr-based Arduboy tests.
//
// Two interfaces:
//   - Direct (ssd1306_command/data): drive the model from C, no AVR needed.
//     Use for unit-testing the model itself.
//   - Attached (ssd1306_attach): hook into a running avr_t's SPI peripheral
//     and the Arduboy CS/DC GPIO pins (PD6, PD4). Use for integration tests
//     where firmware drives the display.
//
// Pin map (Arduboy original):
//   CS  = PD6 (active low)
//   DC  = PD4 (0 = command, 1 = data)
//   RES = PD7 (active low; reset not yet modeled)
//   SPI = USART/SPI peripheral 0 (PB1=SCK, PB2=MOSI)

#ifndef SSD1306_H
#define SSD1306_H

#include <stdint.h>
#include <stdio.h>

struct avr_t;

#define SSD1306_WIDTH     128
#define SSD1306_HEIGHT    64
#define SSD1306_PAGES     8
#define SSD1306_FB_BYTES  (SSD1306_WIDTH * SSD1306_PAGES)  // 1024

typedef enum {
    SSD1306_AM_HORIZONTAL = 0,
    SSD1306_AM_VERTICAL   = 1,
    SSD1306_AM_PAGE       = 2,
} ssd1306_addr_mode_t;

typedef struct ssd1306_t {
    // Framebuffer: 8 pages × 128 columns. Bit n of fb[page*128+col] is the
    // pixel at (col, page*8 + n) — i.e. bit 0 is the top pixel of the page.
    uint8_t fb[SSD1306_FB_BYTES];

    // Current write cursor and addressing window
    ssd1306_addr_mode_t addr_mode;
    uint8_t col, page;
    uint8_t col_start, col_end;
    uint8_t page_start, page_end;

    // Display state
    uint8_t display_on;
    uint8_t inverse;
    uint8_t contrast;

    // Multi-byte command latch
    uint8_t pending_cmd;
    uint8_t pending_args_left;

    // Pin state (gated by attach; ignored in direct mode)
    uint8_t cs;        // 1 = inactive
    uint8_t dc;        // 0 = command, 1 = data

    // Stats
    uint32_t cmd_count;
    uint32_t data_count;
} ssd1306_t;

void ssd1306_init_model(ssd1306_t *d);
void ssd1306_attach(ssd1306_t *d, struct avr_t *avr);

void ssd1306_command(ssd1306_t *d, uint8_t b);
void ssd1306_data(ssd1306_t *d, uint8_t b);

int  ssd1306_pixel(const ssd1306_t *d, int x, int y);
void ssd1306_dump_pbm(const ssd1306_t *d, FILE *out);

#endif
