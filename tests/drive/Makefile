CC ?= cc
CPPFLAGS += -I../vic/include -DNDEBUG
CFLAGS ?= -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror

ifeq ($(OS),Windows_NT)
EXEEXT := .exe
endif

PROGRAM := drive_tests$(EXEEXT)

.PHONY: all check
.DELETE_ON_ERROR:

all: $(PROGRAM)

$(PROGRAM): Makefile drive_test.c ../../src/drive.c ../../src/crc32.c $(wildcard ../../src/*.h) ../vic/include/debug.h
	$(CC) $(CPPFLAGS) $(CFLAGS) drive_test.c ../../src/crc32.c $(LDFLAGS) $(LDLIBS) -o $@

check: $(PROGRAM)
	./$(PROGRAM)