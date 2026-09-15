CC ?= cc
CPPFLAGS += -Iinclude -I../../../src
CFLAGS ?= -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror
TESTS ?=

ifeq ($(OS),Windows_NT)
EXEEXT := .exe
endif

PROGRAM := drive_tests$(EXEEXT)
SOURCES := drive_test.c mocks.c ../../../src/drive.c ../../../src/crc32.c ../../../src/browser_model.c
HEADERS := include/mocks.h $(addprefix ../../../src/,drive.h crc32.h cpu.h memory.h vic.h CEPackage.h browser_model.h)

.PHONY: all check list
.DELETE_ON_ERROR:

all: $(PROGRAM)

$(PROGRAM): Makefile $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

check: $(PROGRAM)
	./$(PROGRAM) $(TESTS)

list: $(PROGRAM)
	./$(PROGRAM) --list