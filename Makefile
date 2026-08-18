# =======================================================================
# C64 PORT: EMUDORE
# This build system is configured for the Commodore 64 (C64) target.
#
# IMPORTANT: All development, builds, and release procedures must follow
# the instructions in the README.md file, which has been updated for C64.
#
# Build flags (-Oz, -fno-strict-aliasing) are specifically tailored
# for the eZ80-clang compiler environment used for C64 emulation.
# =======================================================================

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

# Package a cartridge image into ECCRTnn AppVars.
CART ?=
.PHONY: cart
cart:
	python tools/rom_to_appvar.py "$(CART)" --kind cart --output bin/rom

# Package BASIC listings / PRGs into ECDSKnn AppVars.
FILES ?= emudore/assets/bas/10print.bas emudore/assets/bas/lowercase.bas emudore/assets/bas/bouncing.bas emudore/assets/prg/monopole.prg
.PHONY: files
files:
	python tools/files_to_dsk_appvar.py $(FILES) --output bin/rom

# Package a .d64 disk image's PRG files into ECDSKnn AppVars (see src/drive.c).
DISK ?= tests/FINALBLW.D64
.PHONY: disk
disk:
	python tools/d64_to_appvar.py "$(DISK)" --output bin/rom
