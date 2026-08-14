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

# -Oz (CEdev standard) on purpose: -O2 triggers ez80-clang register-allocator
# bugs that clobber the IX frame pointer mid-function (io_init/io_emulate/
# vic_emulate all miscompiled -> MMIO corruption and NMI resets).
CFLAGS ?= -Wall -Wextra -Oz -fno-strict-aliasing



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

# Package a .d64 disk image's PRG files into ECDSKnn AppVars (see src/drive.c).
DISK ?= tests/Rally Driver.d64
.PHONY: disk
disk:
	python tools/d64_to_appvar.py "$(DISK)" --output bin/rom
