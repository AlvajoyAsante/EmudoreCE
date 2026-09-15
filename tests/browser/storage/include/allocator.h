#ifndef STORAGE_TEST_ALLOCATOR_H
#define STORAGE_TEST_ALLOCATOR_H

#include <stdlib.h>

void *storage_test_malloc(size_t size);
void storage_test_free(void *pointer);

#define malloc storage_test_malloc
#define free storage_test_free

#endif