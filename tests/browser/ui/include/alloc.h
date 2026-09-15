#ifndef UI_ALLOC_H
#define UI_ALLOC_H

#include <stdlib.h>

void *ui_calloc(size_t count, size_t size);
void ui_free(void *pointer);

#define calloc ui_calloc
#define free ui_free

#endif