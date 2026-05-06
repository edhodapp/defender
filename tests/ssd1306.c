#include "ssd1306.h"

#include <string.h>

#include "sim_avr.h"
#include "sim_irq.h"
#include "avr_spi.h"
#include "avr_ioport.h"

void ssd1306_init_model(ssd1306_t *d) {
    memset(d, 0, sizeof(*d));
    d->cs = 1;
    d->dc = 0;
    d->col_end = 127;
    d->page_end = 7;
    d->addr_mode = SSD1306_AM_PAGE;   // datasheet default
    d->contrast = 0x7F;
}

void ssd1306_command(ssd1306_t *d, uint8_t b) {
    d->cmd_count++;

    if (d->pending_args_left) {
        switch (d->pending_cmd) {
        case 0x20:  // Memory addressing mode
            switch (b & 0x03) {
            case 0: d->addr_mode = SSD1306_AM_HORIZONTAL; break;
            case 1: d->addr_mode = SSD1306_AM_VERTICAL; break;
            case 2: d->addr_mode = SSD1306_AM_PAGE; break;
            }
            d->pending_args_left = 0;
            break;
        case 0x21:  // Column address: <start> <end>
            if (d->pending_args_left == 2) {
                d->col_start = b & 0x7F;
            } else {
                d->col_end = b & 0x7F;
                d->col = d->col_start;
            }
            d->pending_args_left--;
            break;
        case 0x22:  // Page address: <start> <end>
            if (d->pending_args_left == 2) {
                d->page_start = b & 0x07;
            } else {
                d->page_end = b & 0x07;
                d->page = d->page_start;
            }
            d->pending_args_left--;
            break;
        case 0x81:  // Contrast
            d->contrast = b;
            d->pending_args_left = 0;
            break;
        default:
            d->pending_args_left--;
            break;
        }
        return;
    }

    if (b >= 0xB0 && b <= 0xB7) { d->page = b - 0xB0; return; }
    if (b <= 0x0F)              { d->col = (d->col & 0xF0) | (b & 0x0F); return; }
    if (b >= 0x10 && b <= 0x1F) { d->col = (d->col & 0x0F) | ((b & 0x0F) << 4); return; }

    switch (b) {
    case 0xAE: d->display_on = 0; break;
    case 0xAF: d->display_on = 1; break;
    case 0xA6: d->inverse = 0; break;
    case 0xA7: d->inverse = 1; break;
    case 0x20: d->pending_cmd = 0x20; d->pending_args_left = 1; break;
    case 0x21: d->pending_cmd = 0x21; d->pending_args_left = 2; break;
    case 0x22: d->pending_cmd = 0x22; d->pending_args_left = 2; break;
    case 0x81: d->pending_cmd = 0x81; d->pending_args_left = 1; break;
    case 0xA8: d->pending_cmd = 0xA8; d->pending_args_left = 1; break;
    case 0xD3: d->pending_cmd = 0xD3; d->pending_args_left = 1; break;
    case 0x8D: d->pending_cmd = 0x8D; d->pending_args_left = 1; break;
    case 0xD5: d->pending_cmd = 0xD5; d->pending_args_left = 1; break;
    case 0xD9: d->pending_cmd = 0xD9; d->pending_args_left = 1; break;
    case 0xDA: d->pending_cmd = 0xDA; d->pending_args_left = 1; break;
    case 0xDB: d->pending_cmd = 0xDB; d->pending_args_left = 1; break;
    default:
        // Other init knobs (segment remap, COM scan dir, start line, etc.)
        // are accepted silently — they don't affect framebuffer semantics.
        break;
    }
}

void ssd1306_data(ssd1306_t *d, uint8_t b) {
    d->data_count++;
    if (d->page > 7 || d->col > 127) return;

    d->fb[d->page * SSD1306_WIDTH + d->col] = b;

    switch (d->addr_mode) {
    case SSD1306_AM_HORIZONTAL:
        if (d->col >= d->col_end) {
            d->col = d->col_start;
            d->page = (d->page >= d->page_end) ? d->page_start : d->page + 1;
        } else {
            d->col++;
        }
        break;
    case SSD1306_AM_VERTICAL:
        if (d->page >= d->page_end) {
            d->page = d->page_start;
            d->col = (d->col >= d->col_end) ? d->col_start : d->col + 1;
        } else {
            d->page++;
        }
        break;
    case SSD1306_AM_PAGE:
        d->col = (d->col < 127) ? d->col + 1 : 0;
        break;
    }
}

int ssd1306_pixel(const ssd1306_t *d, int x, int y) {
    if (x < 0 || x >= SSD1306_WIDTH || y < 0 || y >= SSD1306_HEIGHT) return 0;
    return (d->fb[(y / 8) * SSD1306_WIDTH + x] >> (y % 8)) & 1;
}

void ssd1306_dump_pbm(const ssd1306_t *d, FILE *out) {
    fprintf(out, "P1\n%d %d\n", SSD1306_WIDTH, SSD1306_HEIGHT);
    for (int y = 0; y < SSD1306_HEIGHT; y++) {
        for (int x = 0; x < SSD1306_WIDTH; x++) {
            fputc(ssd1306_pixel(d, x, y) ? '1' : '0', out);
            if ((x & 31) == 31) fputc('\n', out);
        }
    }
}

// --- Attached mode: SPI + GPIO callbacks ---

static void on_spi_byte(struct avr_irq_t *irq, uint32_t value, void *param) {
    ssd1306_t *d = (ssd1306_t *)param;
    (void)irq;
    if (d->cs == 0) {
        if (d->dc) ssd1306_data(d, value & 0xFF);
        else       ssd1306_command(d, value & 0xFF);
    }
}

static void on_cs_change(struct avr_irq_t *irq, uint32_t value, void *param) {
    (void)irq;
    ((ssd1306_t *)param)->cs = value & 1;
}

static void on_dc_change(struct avr_irq_t *irq, uint32_t value, void *param) {
    (void)irq;
    ((ssd1306_t *)param)->dc = value & 1;
}

void ssd1306_attach(ssd1306_t *d, struct avr_t *avr) {
    avr_irq_t *spi_out = avr_io_getirq(avr, AVR_IOCTL_SPI_GETIRQ(0), SPI_IRQ_OUTPUT);
    if (spi_out) avr_irq_register_notify(spi_out, on_spi_byte, d);

    avr_irq_t *cs = avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('D'), 6);
    if (cs) avr_irq_register_notify(cs, on_cs_change, d);

    avr_irq_t *dc = avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('D'), 4);
    if (dc) avr_irq_register_notify(dc, on_dc_change, d);
}
