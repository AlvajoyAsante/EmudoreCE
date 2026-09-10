"""Conservative, recognized-packer transformations for disk PRGs."""

from __future__ import annotations


MRCROSS_LOAD = 0x0801
MRCROSS_ENTRY = 0x081A
MRCROSS_WRITER = 0x0866
MRCROSS_PACKED = 0x08B6
MRCROSS_ORIGINAL_WRITER = bytes.fromhex(
    "8d 0b 08 c6 01 ee 00 04 e6 01 ee b1 01 d0 03 ee b2 01 60"
)
MRCROSS_FAST_WRITER = bytes.fromhex(
    "8d 0b 08 ee 00 04 ee b1 01 d0 03 ee b2 01 60 ea ea ea ea"
)
MRCROSS_SIGNATURES = (
    (0x0801, b"\x18\x08\x00\x00\x9e2074MR.CROSS-V2.0\x00\x00\x00"),
    (0x081A, bytes.fromhex("78 a9 38 85 01 a2 f9 9a bd bc 07 9d 06 01 ca d0 f7")),
    (0x082B, b"\xad"),
    (0x082E, bytes.fromhex(
        "8d 9f 01 ad 2c 08 d0 03 ce 2d 08 ce 2c 08 ad 2f 08 d0 05 "
        "ce 30 08 f0 fb ce 2f 08 ce 54 08 d0 dd ce 55 08 d0 d8 60"
    )),
    (0x0856, b"\xad"),
    (0x0859, bytes.fromhex("49 00 ee a1 01 d0 05 ee a2 01 f0 fb 60")),
    (0x0866, MRCROSS_ORIGINAL_WRITER),
    (0x0879, bytes.fromhex(
        "a9 4d 20 b0 01 20 a0 01 c9 4d d0 f6 20 a0 01 c9 03 90 0c "
        "aa 20 a0 01 20 b0 01 ca d0 fa f0 e6 c9 02 f0 dd "
        "20 a0 01 8d b1 01 20 a0 01 8d b2 01 c9 00 d0 d2 68 85 01 40"
    )),
    (0x08B0, bytes.fromhex("c7 01 37 20")),
)


def inspect_mrcross_stream(stream: bytes, stream_start: int) -> int | None:
    cursor = 0
    destination = 0x080B
    decoded_bytes = 0
    while cursor < len(stream):
        value = stream[cursor]
        cursor += 1
        count = 1
        if value == 0x4D:
            if cursor == len(stream):
                return None
            token = stream[cursor]
            cursor += 1
            if token < 2:
                if cursor + 2 > len(stream):
                    return None
                low, high = stream[cursor:cursor + 2]
                cursor += 2
                if high == 0:
                    return decoded_bytes
                destination = low | (high << 8)
                continue
            if token >= 3:
                if cursor == len(stream):
                    return None
                cursor += 1
                count = token
        end = destination + count
        if destination < 0x0200 or end > 0x10000:
            return None
        unread_start = stream_start + cursor
        if unread_start < 0x10000 and end > unread_start:
            return None
        destination = end & 0xFFFF
        decoded_bytes += count
    return None


def optimize_mrcross(prg: bytes) -> tuple[bytes, dict | None]:
    if int.from_bytes(prg[:2], "little") != MRCROSS_LOAD:
        return prg, None
    if len(prg) < MRCROSS_PACKED - MRCROSS_LOAD + 3:
        return prg, None

    def at(address: int, size: int = 1) -> bytes:
        offset = address - MRCROSS_LOAD + 2
        return prg[offset:offset + size]

    if any(at(address, len(expected)) != expected for address, expected in MRCROSS_SIGNATURES):
        return prg, None
    source_end = int.from_bytes(at(0x082C, 2), "little")
    low_count, high_count = at(0x0854, 2)
    copied_bytes = ((high_count - 1) & 0xFF) * 256 + (low_count or 256)
    packed_size = len(prg) - 2 - (MRCROSS_PACKED - MRCROSS_LOAD)
    if source_end != MRCROSS_LOAD + len(prg) - 3 or copied_bytes != packed_size:
        return prg, None
    if not 0xA0 < copied_bytes <= 0xFEA0:
        return prg, None
    stream_start = (0x00A0 - copied_bytes) & 0xFFFF
    if int.from_bytes(at(0x0857, 2), "little") != stream_start:
        return prg, None
    decoded_bytes = inspect_mrcross_stream(prg[MRCROSS_PACKED - MRCROSS_LOAD + 2:], stream_start)
    if decoded_bytes is None or decoded_bytes == 0:
        return prg, None

    offset = MRCROSS_WRITER - MRCROSS_LOAD + 2
    changed = bytearray(prg)
    changed[offset:offset + len(MRCROSS_FAST_WRITER)] = MRCROSS_FAST_WRITER
    return bytes(changed), {
        "id": "mrcross-v2-redundant-banking",
        "description": "Remove two redundant CPU-bank writes per decoded byte; preserve the screen counter.",
        "scope": "startup-decompression",
        "packer": "MR.CROSS V2.0",
        "decoded_bytes": decoded_bytes,
        "bank_writes_removed": decoded_bytes * 2,
        "guest_cycles_saved": decoded_bytes * 10,
        "bank_writes_removed_per_decoded_byte": 2,
        "guest_cycles_saved_per_decoded_byte": 10,
    }