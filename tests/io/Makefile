CC ?= cc
CPPFLAGS += -Iinclude -DNDEBUG
CFLAGS ?= -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror
TESTS ?=

ifeq ($(OS),Windows_NT)
EXEEXT := .exe
endif

PROGRAM := io_tests$(EXEEXT)

.PHONY: all check list
.DELETE_ON_ERROR:

all: $(PROGRAM)

$(PROGRAM): Makefile io_test.c ../../src/io.c $(wildcard ../../src/*.h) $(wildcard include/*.h) include/sys/timers.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -MF io_tests.d io_test.c $(LDFLAGS) $(LDLIBS) -o $@

check: $(PROGRAM)
	./$(PROGRAM) $(TESTS)

list: $(PROGRAM)
	./$(PROGRAM) --list

-include io_tests.d