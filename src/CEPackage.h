/*
 * emudore-CE: AppVar package reader for ROMs, cartridges and D64 payloads.
 *
 * Package chunks are stored as archived TI AppVars named EC<id><nn>. This
 * module validates headers, optionally maps chunk data directly, and can copy
 * or decompress payloads into RAM for the emulator core.
 */
#ifndef EMUDORE_CE_PACKAGE_H
#define EMUDORE_CE_PACKAGE_H

#include <stdint.h>

/* AppVar package ids for the C64 ROMs. TI variable names are at most 8 chars
   and CEPackage names them "EC" + id + 2 hex digits, so ids must be <= 4
   chars (A-Z / 0-9). */
#define CE_PACKAGE_BASIC  "BAS"
#define CE_PACKAGE_KERNAL "KRN"
#define CE_PACKAGE_CHARS  "CHR"
#define CE_PACKAGE_CART   "CRT"  /* cartridge image built by tools/rom_to_appvar.py */
#define CE_PACKAGE_DISK   "DSK"  /* D64 file set built by tools/d64_to_appvar.py */

#define CE_PACKAGE_DISK_ID_MAX 4

typedef struct
{
	char id[CE_PACKAGE_DISK_ID_MAX + 1];
	uint32_t size;
} CEPackageDisk;

/**
 * Enumerate valid EM64 disk packages installed as AppVars.
 *
 * Entries identify package IDs, not individual storage chunks: a multi-chunk
 * package contributes exactly one entry for chunk zero. The caller owns the
 * fixed-capacity output buffer.
 *
 * @param disks Output array, or NULL when capacity is zero.
 * @param capacity Number of entries available in disks.
 * @return Number of valid disk packages found, capped at capacity.
 */
uint8_t CEPackageFindDisks(CEPackageDisk *disks, uint8_t capacity);

/**
 * Query the unpacked payload size of a package.
 *
 * @param packageId Short package identifier such as BAS, KRN, CHR, CRT or DSK.
 * @return Total logical payload size after decompression, or 0 when the
 *         package is missing or invalid.
 */
uint32_t CEPackageSize(const char *packageId);
/**
 * Validate that a package exists and has the expected logical size.
 *
 * @param packageId Short package identifier.
 * @param expectedSize Required unpacked payload size in bytes.
 * @return Non-zero on success, zero on mismatch or missing data.
 */
int CEPackageValidate(const char *packageId, uint32_t expectedSize);
/**
 * Cheaply check that a package's backing AppVar exists, without opening its
 * header or reading any payload bytes.
 *
 * Much faster than CEPackageValidate() but does not catch a corrupt or
 * wrong-size file; intended for release builds where boot time matters more
 * than defending against a hand-corrupted AppVar.
 *
 * @param packageId Short package identifier.
 * @return Non-zero if the package's AppVar is present, zero otherwise.
 */
int CEPackageFileExists(const char *packageId);
/**
 * Map a whole package directly without copying.
 *
 * This is only valid for uncompressed packages whose payload layout is already
 * usable in place.
 *
 * @param packageId Short package identifier.
 * @param expectedSize Required unpacked payload size in bytes.
 * @return Pointer to the package payload, or NULL on failure.
 */
const uint8_t *CEPackageGetDirect(const char *packageId, uint32_t expectedSize);
/**
 * Map one raw storage chunk without copying it into RAM.
 *
 * @param packageId Short package identifier.
 * @param chunk Zero-based 32 KiB storage chunk index.
 * @param expectedSize Required unpacked payload size in bytes.
 * @return Pointer to the chunk payload bytes, or NULL on failure.
 */
const uint8_t *CEPackageGetDirectChunk(const char *packageId, uint8_t chunk,
									   uint32_t expectedSize);
/**
 * Resolve which mapped chunk contains a payload offset.
 *
 * @param packageId Short package identifier.
 * @param offset Byte offset within the logical payload.
 * @param expectedSize Required unpacked payload size in bytes.
 * @return Pointer to the raw byte at that offset, or NULL on failure.
 */
const uint8_t *CEPackageChunkPtr(const char *packageId, uint32_t offset,
								 uint32_t expectedSize);
/** Close any cached direct-mapped AppVar handles held by this module. */
void CEPackageCloseDirect(void);
/**
 * Read a full package into RAM.
 *
 * Compressed packages are decompressed; uncompressed packages are copied.
 *
 * @param packageId Short package identifier.
 * @param destination Output buffer receiving the unpacked payload.
 * @param expectedSize Required unpacked payload size in bytes.
 * @return Non-zero on success, zero on failure.
 */
int CEPackageRead(const char *packageId, uint8_t *destination, uint32_t expectedSize);
/**
 * Read one logical payload byte.
 *
 * @param packageId Short package identifier.
 * @param offset Byte offset within the unpacked payload.
 * @return Byte value 0-255, or a negative value on failure.
 */
int CEPackageReadAt(const char *packageId, uint32_t offset);

#endif /* EMUDORE_CE_PACKAGE_H */
