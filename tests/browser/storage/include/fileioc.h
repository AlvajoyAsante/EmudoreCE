#ifndef STORAGE_TEST_FILEIOC_H
#define STORAGE_TEST_FILEIOC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

uint8_t ti_Open(const char *name, const char *mode);
int ti_Close(uint8_t handle);
size_t ti_Read(void *data, size_t size, size_t count, uint8_t handle);
size_t ti_Write(const void *data, size_t size, size_t count, uint8_t handle);
int ti_Seek(int offset, unsigned int origin, uint8_t handle);
uint16_t ti_GetSize(uint8_t handle);
int ti_SetArchiveStatus(uint8_t archive, uint8_t handle);

#define ti_SetArchiveStatus(archive, handle) ti_SetArchiveStatus((bool)(archive), (handle))

#endif