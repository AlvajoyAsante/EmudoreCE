ifeq ($(origin CC),default)
CC := C:/msys64/ucrt64/bin/gcc.exe
endif
CFLAGS ?= -O2 -std=gnu11 -Wall -Wextra -Werror
CEMU := build/CEmu/core
REFERENCE := build/vrEmu6502
TARGET_IMAGE ?= ../../bin/EMUDORE.obj
FRAMES ?= 60
JIT ?= on
FILE_INDEX ?= 0
OUTPUT ?= build/parallax-$(JIT)
INPUT ?= none
RESUME ?= none
DISK ?= ../Parallax.d64

ifeq ($(OS),Windows_NT)
EXEEXT := .exe
endif
GAME_PROGRAM := game_test$(EXEEXT)

.PHONY: all game-check
all: $(GAME_PROGRAM)

$(GAME_PROGRAM): game_test.c target_test.c cemu_throttle.c cemu_throttle.h ../../src/jit_emit.c ../../src/jit_emit.h $(CEMU)/libcemucore.a build/vrEmu6502.o
	"$(CC)" $(CFLAGS) -DVR_EMU_6502_STATIC -I$(CEMU) -I$(REFERENCE)/src game_test.c cemu_throttle.c ../../src/jit_emit.c $(CEMU)/libcemucore.a build/vrEmu6502.o -o "$@"

game-check: $(GAME_PROGRAM)
	"$(CURDIR)/$(GAME_PROGRAM)" "$(TARGET_IMAGE)" $(FRAMES) $(JIT) $(FILE_INDEX) "$(OUTPUT)" "$(INPUT)" "$(RESUME)" "$(DISK)"

# Run every tests/*.d64 and tests/*.t64 image with one final summary table:
# powershell -ExecutionPolicy Bypass -File run_all_games.ps1