# =======================================================================
# C64 PORT: EMUDORE
# This build system is configured for the Commodore 64 (C64) target.
#
# IMPORTANT: All development, builds, and release procedures must follow
# the instructions in the README.md file, which has been updated for C64.
#
# Build flags (-O2, -fno-strict-aliasing) are specifically tailored
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

CFLAGS ?= -Wall -Wextra -O2 -fno-strict-aliasing

PROFILE ?= 0
CPU_HOT_PROFILE ?= 0
# Rendering every emulated frame can dominate gameplay; VIC pacing advances
# emulation continuously while sampling display frames at a bounded rate.
FRAME_SKIP ?= 1
CHAR_ASM ?= 1
# Reserve IX: nightly 19.1.0 otherwise reuses live stack frames as data pointers.
override CFLAGS += -fno-omit-frame-pointer -DEMUDORE_PROFILE=$(PROFILE) -DEMUDORE_CPU_HOT_PROFILE=$(CPU_HOT_PROFILE) -DEMUDORE_FRAME_SKIP=$(FRAME_SKIP) -DEMUDORE_CHAR_ASM=$(CHAR_ASM)

ifeq ($(PROFILE),1)
OBJDIR ?= obj/profile
BINDIR ?= bin/profile
endif

# ----------------------------

ifndef CEDEV
$(error CEDEV environment path variable is not set)
endif

EXTRA_LDFLAGS += -T tools/program_memory.ld
DEPS += tools/program_memory.ld

include $(CEDEV)/meta/makefile.mk

SIZE_OPTIMIZED_SOURCES := src/jit_emit.c src/jit.c src/cpu_decimal.c src/main.c src/CEPackage.c \
						  src/browser.c src/browser_model.c src/browser_storage.c src/drive.c src/vblank.c
$(addprefix $(OBJDIR)/,$(addsuffix .s,$(SIZE_OPTIMIZED_SOURCES))): override CFLAGS += -Oz

# ----------------------------
# ROM AppVars
# ----------------------------
# Package the C64 ROM dumps in assets/roms/ into AppVars under bin/rom/.
# Run `make roms` after changing the ROM dumps or the packaging tool.
.PHONY: roms
roms:
	python tools/appvar_builder.py assets/roms/basic.901226-01.bin --kind basic --output bin/rom
	python tools/appvar_builder.py assets/roms/kernal.bin --kind kernal --output bin/rom
	python tools/appvar_builder.py assets/roms/characters.901225-01.bin --kind chars --output bin/rom

# Package a cartridge image into ECCRTnn AppVars.
CART ?=
.PHONY: cart
cart:
	python tools/appvar_builder.py "$(CART)" --kind cart --output bin/rom

# Package BASIC listings / PRGs into ECDSKnn AppVars.
FILES ?=
.PHONY: files
files:
	python tools/files_to_dsk_appvar.py $(FILES) --output bin/rom

# Package a .d64 disk image or .t64 tape image's PRG files into ECDSKnn AppVars (see src/drive.c).
DISK ?= tests/4SOCCSIM.T64
.PHONY: disk
disk:
	python tools/rom_to_appvar.py "$(DISK)" --output bin/rom

PYTHON ?= python
OPTIMIZED_OUTPUT ?= bin/optimized
OPT_PROFILE ?=
OPT_PROFILE_ARG = $(if $(strip $(OPT_PROFILE)),--profile "$(OPT_PROFILE)")

.PHONY: roms-optimized cart-optimized files-optimized disk-optimized
roms-optimized:
	"$(PYTHON)" tools/optimize_appvar.py assets/roms/basic.901226-01.bin --kind basic --output "$(OPTIMIZED_OUTPUT)" $(OPT_PROFILE_ARG)
	"$(PYTHON)" tools/optimize_appvar.py assets/roms/kernal.bin --kind kernal --output "$(OPTIMIZED_OUTPUT)" $(OPT_PROFILE_ARG)
	"$(PYTHON)" tools/optimize_appvar.py assets/roms/characters.901225-01.bin --kind chars --output "$(OPTIMIZED_OUTPUT)" $(OPT_PROFILE_ARG)

cart-optimized:
	"$(PYTHON)" tools/optimize_appvar.py "$(CART)" --kind cart --output "$(OPTIMIZED_OUTPUT)" $(OPT_PROFILE_ARG)

files-optimized:
	"$(PYTHON)" tools/optimize_appvar.py $(FILES) --output "$(OPTIMIZED_OUTPUT)" $(OPT_PROFILE_ARG)

disk-optimized:
	"$(PYTHON)" tools/optimize_appvar.py "$(DISK)" --output "$(OPTIMIZED_OUTPUT)" --require-code-change $(OPT_PROFILE_ARG)
