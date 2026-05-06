// Smoke test: load blinky.elf into a simulated ATmega32u4, run for 2 simulated
// seconds, and count PB6 transitions. Expect ~9 (initial init high + 8 toggles
// from main_loop iterations).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_avr.h"
#include "sim_elf.h"
#include "avr_ioport.h"

#define F_CPU 16000000UL
#define SIM_SECONDS 2
#define MIN_TRANSITIONS 7
#define MAX_TRANSITIONS 12

static int g_pb6_transitions = 0;

static void pb6_changed(struct avr_irq_t *irq, uint32_t value, void *param) {
    (void)irq; (void)value; (void)param;
    g_pb6_transitions++;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <firmware.elf>\n", argv[0]);
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

    avr_irq_t *pb6 = avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('B'), 6);
    if (!pb6) {
        fprintf(stderr, "avr_io_getirq for PB6 returned NULL\n");
        return 2;
    }
    avr_irq_register_notify(pb6, pb6_changed, NULL);

    const uint64_t target_cycles = (uint64_t)F_CPU * SIM_SECONDS;
    int state = cpu_Running;
    while (avr->cycle < target_cycles) {
        state = avr_run(avr);
        if (state == cpu_Done || state == cpu_Crashed) break;
    }

    printf("blinky: PB6 transitions=%d in %d s (window [%d,%d])\n",
           g_pb6_transitions, SIM_SECONDS, MIN_TRANSITIONS, MAX_TRANSITIONS);

    if (state == cpu_Crashed) {
        fprintf(stderr, "FAIL: simulator reports cpu_Crashed\n");
        return 1;
    }
    if (g_pb6_transitions < MIN_TRANSITIONS || g_pb6_transitions > MAX_TRANSITIONS) {
        fprintf(stderr, "FAIL: transition count out of expected range\n");
        return 1;
    }

    printf("PASS\n");
    return 0;
}
