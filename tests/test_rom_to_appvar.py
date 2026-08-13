import random
import unittest

from tools.rom_to_appvar import (
    COMPRESSION_LZSS,
    COMPRESSION_NONE,
    HEADER,
    PACKAGE_VERSION,
    build_chunks,
    compress_lzss,
    decompress_lzss,
    maybe_compress,
    validate_rom,
)


class RomToAppVarTests(unittest.TestCase):
    def test_cartridge_chunks_round_trip(self):
        rom = bytearray(0x20000)
        rom[:2] = b"AB"

        chunks = build_chunks(bytes(rom), "cart", 4, len(rom), COMPRESSION_NONE)

        self.assertEqual(len(chunks), 4)
        rebuilt = bytearray()
        for index, chunk in enumerate(chunks):
            header = HEADER.unpack_from(chunk)
            self.assertEqual(header[:4], (b"FMSC", PACKAGE_VERSION, 1, index))
            self.assertEqual(header[4], 4)
            self.assertEqual(header[5], 4)
            self.assertEqual(header[6], COMPRESSION_NONE)
            self.assertEqual(header[7], len(rom))
            self.assertEqual(header[8], len(rom))
            self.assertEqual(header[9], len(chunk) - HEADER.size)
            self.assertEqual(header[10], sum(chunk[HEADER.size:]) & 0xFFFF)
            rebuilt.extend(chunk[HEADER.size:])

        self.assertEqual(rebuilt, rom)

    def test_rejects_unaligned_cartridge(self):
        with self.assertRaisesRegex(ValueError, "8 KiB"):
            validate_rom(b"AB" + bytes(100), "cart")

    def test_rejects_cartridge_without_signature(self):
        with self.assertRaisesRegex(ValueError, "AB signature"):
            validate_rom(bytes(0x4000), "cart")

    def test_bios_must_be_32_kib(self):
        with self.assertRaisesRegex(ValueError, "32 KiB"):
            validate_rom(bytes(0x4000), "bios")

    def test_lzss_round_trips_various_payloads(self):
        payloads = [
            bytes(0x8000),
            bytes([0xFF]) * 0x8000,
            bytes(range(256)) * 128,
            bytes(i % 251 for i in range(0x10000)),
            b"AB",
            b"",
        ]
        for data in payloads:
            with self.subTest(size=len(data)):
                compressed = compress_lzss(data)
                self.assertEqual(decompress_lzss(compressed, len(data)), data)

    def test_maybe_compress_shrinks_repetitive_data(self):
        data = bytes(0x8000)

        stored, method = maybe_compress(data, True)

        self.assertEqual(method, COMPRESSION_LZSS)
        self.assertLess(len(stored), len(data))
        self.assertEqual(decompress_lzss(stored, len(data)), data)

    def test_maybe_compress_falls_back_when_not_smaller(self):
        rng = random.Random(42)
        data = bytes(rng.randrange(256) for _ in range(4096))

        stored, method = maybe_compress(data, True)

        self.assertEqual(method, COMPRESSION_NONE)
        self.assertEqual(stored, data)

    def test_maybe_compress_disabled(self):
        data = bytes(0x8000)

        stored, method = maybe_compress(data, False)

        self.assertEqual(method, COMPRESSION_NONE)
        self.assertEqual(stored, data)

    def test_build_chunks_records_original_and_stored_sizes(self):
        data = bytes(0x8000)
        stored, method = maybe_compress(data, True)

        chunks = build_chunks(stored, "bios", 0, len(data), method)

        self.assertEqual(len(chunks), 1)
        header = HEADER.unpack_from(chunks[0])
        self.assertEqual(header[6], method)
        self.assertEqual(header[7], len(stored))
        self.assertEqual(header[8], len(data))


if __name__ == "__main__":
    unittest.main()
