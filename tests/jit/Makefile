CC ?= cc
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror
CPPFLAGS += -DNDEBUG
TESTS ?=

ifeq ($(OS),Windows_NT)
EXEEXT := .exe
endif

PROGRAM := jit_tests$(EXEEXT)
CEMU := build/CEmu/core
CEMU_REVISION := fb10bfea6f5557a9d3696db6f822a72806b60cc8
REFERENCE_REVISION := aae98cb14386d832cb7357c99626520b6590bc24
AR ?= ar
CEMU_SOURCES := $(filter-out $(CEMU)/usb/physical_macos.c,$(wildcard $(CEMU)/*.c $(CEMU)/os/*.c $(CEMU)/usb/*.c $(CEMU)/debug/*.c))
CEMU_OBJECTS := $(patsubst $(CEMU)/%.c,%.o,$(CEMU_SOURCES))

.PHONY: all check clean
.DELETE_ON_ERROR:

all: $(PROGRAM)

$(PROGRAM): Makefile jit_test.c ../../src/jit.c ../../src/jit.h ../../src/jit_emit.c ../../src/jit_emit.h ../../src/cpu.h ../../src/memory.h
	$(CC) $(CPPFLAGS) $(CFLAGS) jit_test.c ../../src/jit_emit.c $(LDFLAGS) $(LDLIBS) -o $@

check: $(PROGRAM)
	"$(CURDIR)/$(PROGRAM)" $(TESTS)

decimal_tests$(EXEEXT): decimal_test.c ../../src/cpu_decimal.c ../../src/cpu.h
	$(CC) $(CPPFLAGS) $(CFLAGS) decimal_test.c ../../src/cpu_decimal.c -o $@

.PHONY: decimal-check
decimal-check: decimal_tests$(EXEEXT)
	"$(CURDIR)/decimal_tests$(EXEEXT)"

.PHONY: bootstrap verify-deps reference-fix
bootstrap: reference-fix
	$(MAKE) verify-deps

build/CEmu/.git/HEAD:
	git clone --no-checkout --depth 1 https://github.com/CE-Programming/CEmu.git build/CEmu
	git -C build/CEmu fetch --depth 1 origin $(CEMU_REVISION)
	git -C build/CEmu checkout --detach $(CEMU_REVISION)

build/vrEmu6502/.git/HEAD:
	git clone --no-checkout --depth 1 https://github.com/visrealm/vrEmu6502.git build/vrEmu6502
	git -C build/vrEmu6502 fetch --depth 1 origin $(REFERENCE_REVISION)
	git -C build/vrEmu6502 checkout --detach $(REFERENCE_REVISION)

build/clibs.8xg: | build/CEmu/.git/HEAD
	curl --fail --location https://github.com/CE-Programming/libraries/releases/download/v15.0/clibs.8xg --output "$@"

reference-fix: build/vrEmu6502/.git/HEAD
	@$(if $(filter $(REFERENCE_REVISION),$(shell git -C build/vrEmu6502 rev-parse HEAD)),echo PASS reference revision,$(error Existing vrEmu6502 checkout has a different revision; it was not reset))
	git -C build/vrEmu6502 diff --cached --exit-code
	git -C build/vrEmu6502 diff --exit-code -- . ":(exclude)src/vrEmu6502.c"
	$(if $(strip $(shell git -C build/vrEmu6502 diff -- src/vrEmu6502.c)),,git -C build/vrEmu6502 apply "$(CURDIR)/vrEmu6502-nmos-jmp.patch")
	git -C build/vrEmu6502 apply --reverse --check "$(CURDIR)/vrEmu6502-nmos-jmp.patch"

verify-deps: build/CEmu/.git/HEAD build/vrEmu6502/.git/HEAD
	@$(if $(filter $(CEMU_REVISION),$(shell git -C build/CEmu rev-parse HEAD)),echo PASS CEmu $(CEMU_REVISION),$(error Existing CEmu checkout has a different revision; it was not reset))
	@$(if $(filter $(REFERENCE_REVISION),$(shell git -C build/vrEmu6502 rev-parse HEAD)),echo PASS vrEmu6502 $(REFERENCE_REVISION),$(error Existing vrEmu6502 checkout has a different revision; it was not reset))
	git -C build/CEmu diff --exit-code HEAD
	git -C build/vrEmu6502 diff --cached --exit-code
	git -C build/vrEmu6502 diff --exit-code -- . ":(exclude)src/vrEmu6502.c"
	git -C build/vrEmu6502 apply --reverse --check "$(CURDIR)/vrEmu6502-nmos-jmp.patch"
	@echo PASS pinned JIT dependencies and NMOS JMP reference correction

$(CEMU)/libcemucore.a:
	$(MAKE) -C $(CEMU) CC="$(CC)" AR="$(AR)" CFLAGS="-O2 -std=gnu11 -w" OBJS="$(CEMU_OBJECTS)" lib

native_probe$(EXEEXT): native_probe.c cemu_throttle.c cemu_throttle.h $(CEMU)/libcemucore.a
	$(CC) -O2 -std=gnu11 -Wall -Wextra -Werror -I$(CEMU) native_probe.c cemu_throttle.c $(CEMU)/libcemucore.a -o $@

throttle_tests$(EXEEXT): throttle_test.c cemu_throttle.c cemu_throttle.h $(CEMU)/libcemucore.a
	$(CC) -O2 -std=gnu11 -Wall -Wextra -Werror -I$(CEMU) throttle_test.c cemu_throttle.c $(CEMU)/libcemucore.a -o $@

.PHONY: throttle-check
throttle-check: throttle_tests$(EXEEXT)
	"$(CURDIR)/throttle_tests$(EXEEXT)"

.PHONY: native-probe
native-probe: native_probe$(EXEEXT)
	"$(CURDIR)/native_probe$(EXEEXT)"

build/vrEmu6502.o: build/vrEmu6502/src/vrEmu6502.c
	$(CC) -std=c99 -O2 -DVR_EMU_6502_STATIC -c $< -o $@

native_tests$(EXEEXT): native_test.c cemu_throttle.c cemu_throttle.h ../../src/jit_emit.c ../../src/jit_emit.h $(CEMU)/libcemucore.a build/vrEmu6502.o
	$(CC) -O2 -std=gnu11 -Wall -Wextra -Werror -DVR_EMU_6502_STATIC -I$(CEMU) -Ibuild/vrEmu6502/src native_test.c cemu_throttle.c ../../src/jit_emit.c $(CEMU)/libcemucore.a build/vrEmu6502.o -o $@

.PHONY: native-check native-exhaustive
native-check: native_tests$(EXEEXT)
	"$(CURDIR)/native_tests$(EXEEXT)"

native-exhaustive: native_tests$(EXEEXT)
	"$(CURDIR)/native_tests$(EXEEXT)" --exhaustive

target_tests$(EXEEXT): target_test.c cemu_throttle.c cemu_throttle.h ../../src/jit_emit.c ../../src/jit_emit.h $(CEMU)/libcemucore.a build/vrEmu6502.o
	$(CC) -O2 -std=gnu11 -Wall -Wextra -Werror -DVR_EMU_6502_STATIC -I$(CEMU) -Ibuild/vrEmu6502/src target_test.c cemu_throttle.c ../../src/jit_emit.c $(CEMU)/libcemucore.a build/vrEmu6502.o -o $@

.PHONY: target-check target-edge-check target-benchmark target-copy-check target-reentry-check
TARGET_IMAGE ?= ../../bin/EMUDORE.obj

.PHONY: cpu-check
cpu-check: target_tests$(EXEEXT)
	"$(CURDIR)/target_tests$(EXEEXT)" "$(TARGET_IMAGE)" --cpu-only

target-check: target_tests$(EXEEXT)
	"$(CURDIR)/target_tests$(EXEEXT)" "$(TARGET_IMAGE)"

target-edge-check: target_tests$(EXEEXT)
	"$(CURDIR)/target_tests$(EXEEXT)" "$(TARGET_IMAGE)" --edge-only

target-benchmark: target_tests$(EXEEXT)
	"$(CURDIR)/target_tests$(EXEEXT)" "$(TARGET_IMAGE)" --benchmark-only

target-copy-check: target_tests$(EXEEXT)
	"$(CURDIR)/target_tests$(EXEEXT)" "$(TARGET_IMAGE)" --copy-only

target-reentry-check: target_tests$(EXEEXT)
	"$(CURDIR)/target_tests$(EXEEXT)" "$(TARGET_IMAGE)" --reentry-only

.PHONY: target-extensions-check
target-extensions-check: target_tests$(EXEEXT)
	"$(CURDIR)/target_tests$(EXEEXT)" "$(TARGET_IMAGE)" --extensions-only

.PHONY: target-decimal-exhaustive
target-decimal-exhaustive: target_tests$(EXEEXT)
	"$(CURDIR)/target_tests$(EXEEXT)" "$(TARGET_IMAGE)" --decimal-exhaustive

.PHONY: startup-check startup-check-hot
TARGET_PROGRAM ?= ../../bin/EMUDORE.8xp
CE_LIBRARIES ?= build/clibs.8xg
startup-check: target_tests$(EXEEXT) $(CE_LIBRARIES)
	"$(CURDIR)/target_tests$(EXEEXT)" "$(TARGET_IMAGE)" "$(TARGET_PROGRAM)" "$(CE_LIBRARIES)"

startup-check-hot: target_tests$(EXEEXT) $(CE_LIBRARIES)
	$(MAKE) startup-check TARGET_IMAGE=../../bin/cpu-hot/EMUDORE.obj TARGET_PROGRAM=../../bin/cpu-hot/EMUDORE.8xp

clean:
	$(RM) $(PROGRAM) native_probe$(EXEEXT) native_tests$(EXEEXT) target_tests$(EXEEXT) throttle_tests$(EXEEXT)