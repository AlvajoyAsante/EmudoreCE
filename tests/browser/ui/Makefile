CC ?= cc
CFLAGS ?= -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror
CPPFLAGS += -Iinclude -I../../../src

ifeq ($(OS),Windows_NT)
EXEEXT := .exe
endif

.PHONY: all check snapshots
.DELETE_ON_ERROR:

all: ui_tests$(EXEEXT)

browser.o: ../../../src/browser.c ../../../src/browser.h ../../../src/browser_model.h ../../../src/browser_storage.h ../../../src/browser_icons.h ../../../src/drive.h ../../../src/CEPackage.h include/alloc.h include/graphx.h include/keypadc.h include/fileioc.h Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) -include include/alloc.h -c $< -o $@

browser_model.o: ../../../src/browser_model.c ../../../src/browser_model.h Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

crc32.o: ../../../src/crc32.c ../../../src/crc32.h Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

ui_tests$(EXEEXT): ui_test.c mock.c mock.h font.h snapshot.c snapshot.h browser.o browser_model.o crc32.o Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) ui_test.c mock.c snapshot.c browser.o browser_model.o crc32.o $(LDFLAGS) $(LDLIBS) -o $@

check: all
	./ui_tests$(EXEEXT) $(TEST_ARGS)

snapshots: all
	./ui_tests$(EXEEXT) --snapshots