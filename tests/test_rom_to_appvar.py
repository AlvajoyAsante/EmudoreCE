import random
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import rom_to_appvar
from appvar_builder import (
    COMPRESSION_LZSS,
    COMPRESSION_NONE,
    HEADER,
    KIND_INDEX,
    PACKAGE_VERSION,
    build_chunks,
    compress_lzss,
    decompress_lzss,
    maybe_compress,
    validate_rom,
)


class RomToAppVarTests(unittest.TestCase):
    def test_rom_sizes_are_enforced(self):
        validate_rom(bytes(0x2000), "basic")
        validate_rom(bytes(0x2000), "kernal")
        validate_rom(bytes(0x1000), "chars")
        for kind in ("basic", "kernal", "chars"):
            with self.assertRaisesRegex(ValueError, "exactly"):
                validate_rom(bytes(100), kind)

    def test_dsk_kind_accepts_any_size(self):
        validate_rom(bytes(12345), "dsk")

    def test_cart_kind_accepts_any_size(self):
        validate_rom(bytes(12345), "cart")

    def test_chunks_round_trip(self):
        data = bytes(i % 251 for i in range(0x14000))  # 2.5 chunks

        chunks = build_chunks(data, "dsk", len(data), COMPRESSION_NONE)

        self.assertEqual(len(chunks), 3)
        rebuilt = bytearray()
        for index, chunk in enumerate(chunks):
            header = HEADER.unpack_from(chunk)
            self.assertEqual(
                header[:5],
                (b"EM64", PACKAGE_VERSION, KIND_INDEX["dsk"], index, 3),
            )
            self.assertEqual(header[6], COMPRESSION_NONE)
            self.assertEqual(header[7], len(data))   # stored total
            self.assertEqual(header[8], len(data))   # original total
            self.assertEqual(header[9], len(chunk) - HEADER.size)
            self.assertEqual(header[10], sum(chunk[HEADER.size:]) & 0xFFFF)
            rebuilt.extend(chunk[HEADER.size:])

        self.assertEqual(bytes(rebuilt), data)

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


def make_d64(files):
    """Build a minimal 35-track d64 with the given (name, type_byte, data)
    files laid out on consecutive sectors of track 1."""
    image = bytearray(rom_to_appvar.D64_SIZE_35_TRACKS)
    dir_offset = rom_to_appvar.sector_offset(18, 1)
    track, sector = 1, 0

    for slot, (name, type_byte, data) in enumerate(files):
        entry = dir_offset + 2 + slot * 32
        image[entry + 0] = type_byte
        image[entry + 1] = track
        image[entry + 2] = sector
        padded = name.ljust(16, b"\xa0")
        image[entry + 3: entry + 19] = padded

        remaining = data
        while True:
            block = rom_to_appvar.sector_offset(track, sector)
            sector += 1
            if len(remaining) <= 254:
                image[block + 0] = 0
                image[block + 1] = len(remaining) + 1
                image[block + 2: block + 2 + len(remaining)] = remaining
                break
            image[block + 0] = track
            image[block + 1] = sector
            image[block + 2: block + 256] = remaining[:254]
            remaining = remaining[254:]

    return bytes(image)


def make_t64(files):
    """Build a minimal T64 from (name, entry_type, file_type, start, data) records."""
    image = bytearray(rom_to_appvar.T64_HEADER_SIZE + len(files) * rom_to_appvar.T64_DIRECTORY_ENTRY)
    signature = b"C64S tape file"
    image[:len(signature)] = signature
    image[0x20:0x22] = (0x0100).to_bytes(2, "little")
    image[0x22:0x24] = len(files).to_bytes(2, "little")
    image[0x24:0x26] = len(files).to_bytes(2, "little")
    data_offset = len(image)
    data = bytearray()
    for index, (name, entry_type, file_type, start, program) in enumerate(files):
        entry = rom_to_appvar.T64_HEADER_SIZE + index * rom_to_appvar.T64_DIRECTORY_ENTRY
        image[entry] = entry_type
        image[entry + 1] = file_type
        image[entry + 2:entry + 4] = start.to_bytes(2, "little")
        image[entry + 4:entry + 6] = (start + len(program)).to_bytes(2, "little")
        image[entry + 8:entry + 12] = data_offset.to_bytes(4, "little")
        image[entry + 16:entry + 32] = name.ljust(16, b" ")
        if entry_type:
            data += program
            data_offset += len(program)
    return bytes(image + data)


class D64ToAppVarTests(unittest.TestCase):
    def test_payload_packs_closed_prg_files_only(self):
        boot = b"\x01\x08" + bytes(50)
        part = b"\x00\x24" + bytes(600)
        image = make_d64([
            (b"BOOT", 0x82, boot),                 # closed PRG
            (b"NOTES", 0x81, b"\x00\x00" + b"x"),  # SEQ: skipped
            (b"SPLAT", 0x02, b"\x00\x00" + b"y"),  # unclosed PRG: skipped
            (b"PART1", 0x82, part),                # closed PRG
        ])

        payload = rom_to_appvar.build_payload(image)

        self.assertEqual(payload[0], 2)
        entry0 = payload[1:23]
        entry1 = payload[23:45]
        self.assertEqual(entry0[:16], b"BOOT".ljust(16, b"\xa0"))
        self.assertEqual(entry1[:16], b"PART1".ljust(16, b"\xa0"))

        offset0 = int.from_bytes(entry0[16:19], "little")
        size0 = int.from_bytes(entry0[19:22], "little")
        offset1 = int.from_bytes(entry1[16:19], "little")
        size1 = int.from_bytes(entry1[19:22], "little")
        self.assertEqual(payload[offset0: offset0 + size0], boot)
        self.assertEqual(payload[offset1: offset1 + size1], part)
        self.assertEqual(offset1, offset0 + size0)
        self.assertEqual(len(payload), offset1 + size1)

    def test_multi_sector_chain_round_trips(self):
        data = b"\x00\x60" + bytes(i & 0xFF for i in range(5000))
        image = make_d64([(b"BIG", 0x82, data)])

        extracted = rom_to_appvar.extract_file(image, 1, 0)

        self.assertEqual(extracted, data)

    def test_rejects_disk_without_prg_files(self):
        image = make_d64([(b"NOTES", 0x81, b"ab")])
        with self.assertRaisesRegex(ValueError, "no closed PRG"):
            rom_to_appvar.build_payload(image)

    def test_t64_payload_packs_normal_prg_files_only(self):
        image = make_t64([
            (b"BOOT", 1, 0x82, 0x0801, b"\x60"),
            (b"NOTES", 1, 0x81, 0x1000, b"ignored"),
            (b"DELETED", 0, 0x82, 0x2000, b"ignored"),
        ])

        payload, file_types = rom_to_appvar.build_t64_payload(image)

        self.assertEqual(payload[0], 1)
        self.assertEqual(payload[1:17], b"BOOT".ljust(16, b"\xa0"))
        offset = int.from_bytes(payload[17:20], "little")
        size = int.from_bytes(payload[20:23], "little")
        self.assertEqual(payload[offset:offset + size], b"\x01\x08\x60")
        self.assertEqual(file_types, b"\x82")

    def test_t64_rejects_entry_data_past_end(self):
        image = bytearray(make_t64([(b"BAD", 1, 0x82, 0x0801, b"\x60")]))
        image[rom_to_appvar.T64_HEADER_SIZE + 8:rom_to_appvar.T64_HEADER_SIZE + 12] = (0xFFFF).to_bytes(4, "little")

        with self.assertRaisesRegex(ValueError, "extends past"):
            rom_to_appvar.build_t64_payload(bytes(image))


if __name__ == "__main__":
    unittest.main()
