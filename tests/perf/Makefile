CC = gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic
CPPFLAGS += -I../../src -I../vic/include -DNDEBUG

ifeq ($(OS),Windows_NT)
EXEEXT := .exe
endif

PROGRAM := test_perf$(EXEEXT)
DISABLED := disabled$(EXEEXT)
.DEFAULT_GOAL := check

.PHONY: check clean
.DELETE_ON_ERROR:

$(PROGRAM): Makefile test_perf.c ../../src/perf.c ../../src/perf.h ../vic/include/sys/timers.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -DEMUDORE_PROFILE=1 test_perf.c ../../src/perf.c $(LDFLAGS) $(LDLIBS) -o $@

$(DISABLED): Makefile disabled.c ../../src/perf.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -DEMUDORE_PROFILE=0 disabled.c $(LDFLAGS) $(LDLIBS) -o $@

scheduler_tests$(EXEEXT): scheduler_test.c ../../src/c64.c $(wildcard ../../src/*.h) ../vic/include/sys/timers.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -DEMUDORE_PROFILE=0 scheduler_test.c ../../src/c64.c $(LDFLAGS) $(LDLIBS) -o $@

check: $(PROGRAM) $(DISABLED) scheduler_tests$(EXEEXT)
	./$(PROGRAM)
	./$(DISABLED)
	./scheduler_tests$(EXEEXT)

clean:
	$(RM) $(PROGRAM) $(DISABLED) scheduler_tests$(EXEEXT)