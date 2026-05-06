// Unit test for the virtual SSD1306 model. Drives the model directly
// (no AVR, no SPI traffic) to validate command handling, addressing modes,
// framebuffer accumulation, and pixel inspection.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ssd1306.h"

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        return 1; \
    } \
} while (0)

// Arduboy-style init, condensed to the bytes that affect model state.
static const uint8_t init_seq[] = {
    0xAE,             // display off
    0xD5, 0xF0,       // clock divide
    0xA8, 0x3F,       // multiplex 64
    0xD3, 0x00,       // display offset 0
    0x40,             // start line 0
    0x8D, 0x14,       // charge pump on
    0x20, 0x00,       // horizontal addressing
    0xA1,             // segment remap
    0xC8,             // COM scan reverse
    0xDA, 0x12,       // COM pins
    0x81, 0xCF,       // contrast 0xCF
    0xD9, 0xF1,       // precharge
    0xDB, 0x40,       // VCOM detect
    0xA4,             // resume display from RAM
    0xA6,             // normal (non-inverse)
    0xAF,             // display on
};

static int test_init_sequence(void) {
    ssd1306_t d;
    ssd1306_init_model(&d);

    for (size_t i = 0; i < sizeof(init_seq); i++) {
        ssd1306_command(&d, init_seq[i]);
    }

    CHECK(d.display_on == 1);
    CHECK(d.inverse    == 0);
    CHECK(d.contrast   == 0xCF);
    CHECK(d.addr_mode  == SSD1306_AM_HORIZONTAL);
    CHECK(d.cmd_count  == sizeof(init_seq));
    CHECK(d.data_count == 0);
    return 0;
}

static int test_horizontal_fill(void) {
    ssd1306_t d;
    ssd1306_init_model(&d);

    // Set horizontal mode, full-screen window
    ssd1306_command(&d, 0x20); ssd1306_command(&d, 0x00);
    ssd1306_command(&d, 0x21); ssd1306_command(&d, 0); ssd1306_command(&d, 127);
    ssd1306_command(&d, 0x22); ssd1306_command(&d, 0); ssd1306_command(&d, 7);

    // Fill: page p gets byte 0xA0 | p in every column
    for (int p = 0; p < 8; p++) {
        for (int c = 0; c < 128; c++) {
            ssd1306_data(&d, 0xA0 | p);
        }
    }

    CHECK(d.data_count == SSD1306_FB_BYTES);
    for (int p = 0; p < 8; p++) {
        for (int c = 0; c < 128; c++) {
            CHECK(d.fb[p * 128 + c] == (0xA0 | p));
        }
    }
    return 0;
}

static int test_pixel_inspection(void) {
    ssd1306_t d;
    ssd1306_init_model(&d);

    // Place known bytes
    d.fb[0 * 128 + 0]  = 0x01;  // (0,0) on; (0,1..7) off
    d.fb[0 * 128 + 1]  = 0x80;  // (1,0..6) off; (1,7) on
    d.fb[7 * 128 + 64] = 0xFF;  // column 64, page 7 = rows 56..63 all on

    CHECK(ssd1306_pixel(&d, 0, 0) == 1);
    CHECK(ssd1306_pixel(&d, 0, 1) == 0);
    CHECK(ssd1306_pixel(&d, 1, 7) == 1);
    CHECK(ssd1306_pixel(&d, 1, 6) == 0);
    for (int y = 56; y < 64; y++) CHECK(ssd1306_pixel(&d, 64, y) == 1);
    CHECK(ssd1306_pixel(&d, 64, 55) == 0);

    // Out-of-range
    CHECK(ssd1306_pixel(&d, -1, 0) == 0);
    CHECK(ssd1306_pixel(&d, 128, 0) == 0);
    CHECK(ssd1306_pixel(&d, 0, 64) == 0);
    return 0;
}

static int test_page_mode_column_addr(void) {
    ssd1306_t d;
    ssd1306_init_model(&d);
    // page mode is the default; set page 3, column = 0x35
    ssd1306_command(&d, 0xB3);          // page 3
    ssd1306_command(&d, 0x05);          // col low nibble = 0x5
    ssd1306_command(&d, 0x13);          // col high nibble = 0x3 → col = 0x35
    CHECK(d.page == 3);
    CHECK(d.col  == 0x35);

    ssd1306_data(&d, 0xDE);
    CHECK(d.fb[3 * 128 + 0x35] == 0xDE);
    CHECK(d.col == 0x36);  // page mode advances column
    CHECK(d.page == 3);
    return 0;
}

int main(void) {
    int rc = 0;
    if (test_init_sequence())        { rc = 1; printf("init_sequence FAIL\n"); }
    else                              printf("init_sequence PASS\n");
    if (test_horizontal_fill())      { rc = 1; printf("horizontal_fill FAIL\n"); }
    else                              printf("horizontal_fill PASS\n");
    if (test_pixel_inspection())     { rc = 1; printf("pixel_inspection FAIL\n"); }
    else                              printf("pixel_inspection PASS\n");
    if (test_page_mode_column_addr()){ rc = 1; printf("page_mode_column_addr FAIL\n"); }
    else                              printf("page_mode_column_addr PASS\n");
    return rc;
}
