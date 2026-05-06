# Arduboy / ATmega32u4 build for the defender project.
# Pure AVR asm; no Arduino, no C runtime, no libc.
#
# Each .S file becomes its own firmware target. To flash a specific one:
#     make flash TARGET=oled
# Default TARGET is blinky.

MCU        = atmega32u4
F_CPU      = 16000000UL
PORT      ?= /dev/ttyACM0
PROGRAMMER = avr109
BAUD       = 57600
TARGET    ?= blinky

CC      = avr-gcc
OBJCOPY = avr-objcopy
OBJDUMP = avr-objdump
SIZE    = avr-size
AVRDUDE = avrdude

ASFLAGS = -mmcu=$(MCU) -DF_CPU=$(F_CPU) -Wall -x assembler-with-cpp

BUILD    = build
TARGETS  = blinky oled defender
HEX_ALL  = $(TARGETS:%=$(BUILD)/%.hex)

.PHONY: all clean flash flash-direct flash-wait size dump touch

all: $(HEX_ALL)

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: %.S | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/%.elf: $(BUILD)/%.o
	$(CC) -mmcu=$(MCU) -nostartfiles -nostdlib \
	      -Wl,--gc-sections,-Map,$(BUILD)/$*.map $^ -o $@

# Multi-file targets pull in the oled library, graphics primitives, and input.
$(BUILD)/oled.elf:     $(BUILD)/oled_lib.o
$(BUILD)/defender.elf: $(BUILD)/oled_lib.o $(BUILD)/gfx.o $(BUILD)/input.o $(BUILD)/audio.o

$(BUILD)/%.hex: $(BUILD)/%.elf
	$(OBJCOPY) -O ihex -R .eeprom $< $@
	@$(SIZE) --mcu=$(MCU) --format=avr $<

size: $(BUILD)/$(TARGET).elf
	@$(SIZE) --mcu=$(MCU) --format=avr $<

dump: $(BUILD)/$(TARGET).elf
	$(OBJDUMP) -d $< | less

# 1200-baud touch — works only if the running sketch implements USB-CDC reset.
# Hand-rolled asm sketches don't; use `flash-direct` after a manual double-tap.
touch:
	@echo ">>> 1200-baud touch on $(PORT)"
	@stty -F $(PORT) 1200 raw -echo 2>/dev/null || true
	@sleep 0.05
	@stty -F $(PORT) 9600 2>/dev/null || true
	@sleep 1.5

flash: $(BUILD)/$(TARGET).hex touch
	$(AVRDUDE) -c $(PROGRAMMER) -p $(MCU) -P $(PORT) -b $(BAUD) -D \
	    -U flash:w:$<:i

flash-direct: $(BUILD)/$(TARGET).hex
	$(AVRDUDE) -c $(PROGRAMMER) -p $(MCU) -P $(PORT) -b $(BAUD) -D \
	    -U flash:w:$<:i

# flash-wait: poll for the bootloader port to appear, then flash. Run this
# before double-tapping reset and there's no race against the 8-second
# Caterina window. Times out after 30 s of no port.
flash-wait: $(BUILD)/$(TARGET).hex
	@echo ">>> Waiting up to 30 s for $(PORT) — double-tap reset on the device..."
	@count=0; while [ ! -e $(PORT) ] && [ $$count -lt 30 ]; do \
	    sleep 1; \
	    count=$$((count+1)); \
	done
	@if [ ! -e $(PORT) ]; then echo ">>> Timed out waiting for $(PORT)"; exit 1; fi
	$(AVRDUDE) -c $(PROGRAMMER) -p $(MCU) -P $(PORT) -b $(BAUD) -D \
	    -U flash:w:$<:i

clean:
	rm -rf $(BUILD)
