CC ?= cc
CFLAGS ?= -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror

ifeq ($(OS),Windows_NT)
EXEEXT := .exe
endif

.PHONY: all check
.DELETE_ON_ERROR:

all: model_tests$(EXEEXT)

model_tests$(EXEEXT): Makefile model_test.c ../../src/browser_model.c ../../src/browser_model.h ../../src/crc32.c ../../src/crc32.h
	$(CC) $(CPPFLAGS) $(CFLAGS) model_test.c ../../src/browser_model.c ../../src/crc32.c $(LDFLAGS) $(LDLIBS) -o $@

check: all
	./model_tests$(EXEEXT)