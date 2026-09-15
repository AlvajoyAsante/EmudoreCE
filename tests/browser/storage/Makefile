CC ?= gcc
CPPFLAGS += -Iinclude -I../../../src
CFLAGS ?= -O2
CFLAGS += -std=c99 -Wall -Wextra -Wpedantic -Werror

.PHONY: check
check: storage_tests.exe
	./storage_tests.exe

storage_tests.exe: storage_test.o mock_fileioc.o browser_storage.o crc32.o
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

browser_storage.o: ../../../src/browser_storage.c ../../../src/browser_storage.h ../../../src/browser_model.h ../../../src/drive.h ../../../src/crc32.h include/fileioc.h include/allocator.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -include include/allocator.h -c $< -o $@

crc32.o: ../../../src/crc32.c ../../../src/crc32.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

storage_test.o: storage_test.c ../../../src/browser_storage.h ../../../src/browser_model.h ../../../src/drive.h ../../../src/crc32.h include/fileioc.h include/storage_mock.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

mock_fileioc.o: mock_fileioc.c include/fileioc.h include/storage_mock.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@