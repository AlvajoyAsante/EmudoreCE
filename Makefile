# ----------------------------
# Program Options
# ----------------------------

NAME         ?= EMUDORE
ICON         ?= icon.png
DESCRIPTION  ?= "emudore CE - C64 emulator"
COMPRESSED   := YES
ARCHIVED     := YES
LTO          := NO

# ----------------------------

CFLAGS ?= -Wall -Wextra -O2 -fno-strict-aliasing

# Memory layout: all CEdev defaults (BSSHEAP_HIGH / LOAD_ADDR / STACK_HIGH are
# untouched). The 128 KiB C64 RAM+ROM are NOT in BSS; they are allocated at
# runtime as an AppVar (see memory.c), so no layout overrides are needed.

# Sources are auto-globbed from src/ (recursive *.c).

# ----------------------------

ifndef CEDEV
$(error CEDEV environment path variable is not set)
endif

include $(CEDEV)/meta/makefile.mk

# ----------------------------
# ROM AppVars
# ----------------------------
# Package the C64 ROM dumps in assets/roms/ into AppVars under bin/rom/.
# Run `make roms` after changing the ROM dumps or the packaging tool.
.PHONY: roms
roms:
	python tools/rom_to_appvar.py assets/roms/basic.901226-01.bin --kind basic --output bin/rom
	python tools/rom_to_appvar.py assets/roms/kernal.bin --kind kernal --output bin/rom
	python tools/rom_to_appvar.py assets/roms/characters.901225-01.bin --kind chars --output bin/rom
