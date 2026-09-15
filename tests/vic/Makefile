CC ?= cc
CHAR_INTEGRATION ?= 1
FRAME_SKIP ?= 0
CPPFLAGS += -Iinclude -DNDEBUG -DVIC_TEST_CHAR_INTEGRATION=$(CHAR_INTEGRATION) -DEMUDORE_FRAME_SKIP=$(FRAME_SKIP)
CFLAGS ?= -std=c11 -O1 -g -Wall -Wextra
TESTS ?=
EXTRA_SOURCES ?= char_stub.c ../../src/vic_char.c ../../src/vblank.c

ifeq ($(OS),Windows_NT)
EXEEXT := .exe
endif

PROGRAM := vic_tests$(EXEEXT)

.PHONY: all check vblank-check list clean
.DELETE_ON_ERROR:

all: $(PROGRAM)

$(PROGRAM): Makefile vic_test.c ../../src/vic.c $(wildcard ../../src/*.h) include/sys/timers.h include/debug.h $(EXTRA_SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP vic_test.c $(EXTRA_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

check: $(PROGRAM)
	./$(PROGRAM) $(TESTS)

vblank_tests$(EXEEXT): vblank_test.c ../../src/vblank.c ../../src/vblank.h
	$(CC) $(CFLAGS) vblank_test.c ../../src/vblank.c $(LDFLAGS) $(LDLIBS) -o $@

vblank-check: vblank_tests$(EXEEXT)
	./vblank_tests$(EXEEXT)

list: $(PROGRAM)
	./$(PROGRAM) --list

clean:
	$(RM) $(PROGRAM) vic_tests.d

-include vic_tests.d