#!/usr/bin/env python3
"""One-off checks for the disk-loader design (kernal trap site + D64 file info)."""

from pathlib import Path

SECTORS_PER_TRACK = [21] * 17 + [19] * 7 + [18] * 6 + [17] * 5


def sector_offset(track: int, sector: int) -> int:
    return (sum(SECTORS_PER_TRACK[: track - 1]) + sector) * 256


def extract_file(data: bytes, track: int, sector: int) -> bytes:
    out = bytearray()
    seen = set()
    while track:
        if (track, sector) in seen:
            raise ValueError("sector chain loop")
        seen.add((track, sector))
        block = data[sector_offset(track, sector):][:256]
        nt, ns = block[0], block[1]
        if nt == 0:
            out += block[2:2 + ns - 1]  # last block: ns = last used byte index
            break
        out += block[2:256]
        track, sector = nt, ns
    return bytes(out)


kernal = Path("assets/roms/kernal.bin").read_bytes()
print("kernal $F4A0..$F4B0:", kernal[0x14A0:0x14B0].hex(" "))

d64 = Path("tests/Rally Driver.d64").read_bytes()
for name, t, s in [("RALLY DRIVER", 17, 0), ("1.RALLY DRIVER", 17, 1),
                   ("2.RALLY DRIVER", 17, 2), ("3.RALLY DRIVER", 19, 0),
                   ("4.RALLY DRIVER", 19, 9), ("5.RALLY DRIVER", 19, 18)]:
    body = extract_file(d64, t, s)
    load = body[0] | (body[1] << 8)
    print(f"{name:16} size={len(body):6} load=${load:04x} "
          f"end=${load + len(body) - 2:04x} first bytes={body[2:12].hex(' ')}")
