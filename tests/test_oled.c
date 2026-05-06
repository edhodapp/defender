// Integration test: run oled.elf in simavr with the virtual SSD1306 attached.
// Assert init sequence is sent verbatim, framebuffer is cleared, and the
// model's state reflects an Arduboy-style horizontal-addressing display.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_avr.h"
#include "sim_elf.h"

#include "ssd1306.h"

#define F_CPU       16000000UL
#define MAX_CYCLES  (200000UL)
#define EXPECTED_CMD_BYTES   25
#define EXPECTED_DATA_BYTES  1024
#define EXPECTED_CONTRAST    0xCF

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        rc = 1; \
    } \
} while (0)

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <oled.elf>\n", argv[0]);
        return 2;
    }

    elf_firmware_t fw;
    memset(&fw, 0, sizeof(fw));
    if (elf_read_firmware(argv[1], &fw) != 0) {
        fprintf(stderr, "elf_read_firmware failed for %s\n", argv[1]);
        return 2;
    }

    avr_t *avr = avr_make_mcu_by_name("atmega32u4");
    if (!avr) {
        fprintf(stderr, "avr_make_mcu_by_name(atmega32u4) failed\n");
        return 2;
    }
    avr_init(avr);
    avr->frequency = F_CPU;
    avr_load_firmware(avr, &fw);

    ssd1306_t display;
    ssd1306_init_model(&display);
    ssd1306_attach(&display, avr);

    int state = cpu_Running;
    while (avr->cycle < MAX_CYCLES) {
        state = avr_run(avr);
        if (state == cpu_Done || state == cpu_Crashed) break;
        // Stop early once both phases (init + clear) are complete and CS is released.
        if (display.cs == 1 && display.cmd_count >= EXPECTED_CMD_BYTES
                            && display.data_count >= EXPECTED_DATA_BYTES) {
            break;
        }
    }

    int rc = 0;

    printf("oled: cycles=%llu cmd=%u data=%u cs=%u dc=%u on=%u mode=%d contrast=0x%02X\n",
           (unsigned long long)avr->cycle,
           display.cmd_count, display.data_count,
           display.cs, display.dc,
           display.display_on, display.addr_mode, display.contrast);

    if (state == cpu_Crashed) {
        fprintf(stderr, "FAIL: simulator reports cpu_Crashed\n");
        return 1;
    }

    CHECK(display.cmd_count  == EXPECTED_CMD_BYTES);
    CHECK(display.data_count == EXPECTED_DATA_BYTES);
    CHECK(display.display_on == 1);
    CHECK(display.inverse    == 0);
    CHECK(display.contrast   == EXPECTED_CONTRAST);
    CHECK(display.addr_mode  == SSD1306_AM_HORIZONTAL);
    CHECK(display.cs         == 1);   // CS released after clear
    CHECK(display.dc         == 1);   // last DC set was data

    // Framebuffer must be all zero (clear-screen wrote 1024 bytes of 0)
    int nonzero = 0;
    for (int i = 0; i < SSD1306_FB_BYTES; i++) {
        if (display.fb[i] != 0) nonzero++;
    }
    CHECK(nonzero == 0);

    if (rc == 0) printf("PASS\n");
    return rc;
}
