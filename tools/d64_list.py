#!/usr/bin/env python3
"""Quick D64 directory lister (debug helper for d64/prg packaging)."""

import sys
from pathlib import Path

SECTORS_PER_TRACK = [21] * 17 + [19] * 7 + [18] * 6 + [17] * 5  # tracks 1..35

FILE_TYPES = {0: "DEL", 1: "SEQ", 2: "PRG", 3: "USR", 4: "REL"}


def sector_offset(track: int, sector: int) -> int:
    return (sum(SECTORS_PER_TRACK[: track - 1]) + sector) * 256


def main() -> None:
    data = Path(sys.argv[1]).read_bytes()
    print(f"image size: {len(data)}")

    track, sector = 18, 1
    seen = set()
    while track and (track, sector) not in seen:
        seen.add((track, sector))
        block = data[sector_offset(track, sector):][:256]
        for i in range(8):
            entry = block[2 + i * 32: 2 + i * 32 + 30]
            ftype = entry[0]
            if ftype == 0:
                continue
            name = entry[3:19].rstrip(b"\xa0").decode("latin1")
            start_t, start_s = entry[1], entry[2]
            blocks = entry[28] | (entry[29] << 8)
            kind = FILE_TYPES.get(ftype & 7, "?")
            closed = "closed" if ftype & 0x80 else "OPEN(splat)"
            print(f"type={ftype:#04x} {kind:3} {closed:6} "
                  f"start=t{start_t},s{start_s} blocks={blocks} name={name!r}")
        track, sector = block[0], block[1]


if __name__ == "__main__":
    main()
