#ifndef EMUDORE_CE_PACKAGE_H
#define EMUDORE_CE_PACKAGE_H

#include <stdint.h>

/* AppVar package ids for the C64 ROMs. TI variable names are at most 8 chars
   and CEPackage names them "EC" + id + 2 hex digits, so ids must be <= 4
   chars (A-Z / 0-9). */
#define CE_PACKAGE_BASIC  "BAS"
#define CE_PACKAGE_KERNAL "KRN"
#define CE_PACKAGE_CHARS  "CHR"

uint32_t CEPackageSize(const char *packageId);
int CEPackageValidate(const char *packageId, uint32_t expectedSize);
const uint8_t *CEPackageGetDirect(const char *packageId, uint32_t expectedSize);
const uint8_t *CEPackageGetDirectChunk(const char *packageId, uint8_t chunk,
									   uint32_t expectedSize);
const uint8_t *CEPackageChunkPtr(const char *packageId, uint32_t offset,
								 uint32_t expectedSize);
void CEPackageCloseDirect(void);
int CEPackageRead(const char *packageId, uint8_t *destination, uint32_t expectedSize);
int CEPackageReadAt(const char *packageId, uint32_t offset);

#endif
