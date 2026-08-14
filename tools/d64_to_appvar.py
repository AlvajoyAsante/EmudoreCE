#!/usr/bin/env python3
"""Convert a .d64 disk image into an emudore CE disk AppVar set (ECDSKnn).

All closed PRG files on the disk are extracted (following their sector
chains) and packed into one "DSK" package the on-calc drive emulation
(src/drive.c) serves KERNAL LOAD requests from.

Payload layout (little-endian, always stored UNCOMPRESSED so the calculator
can read files straight out of the archived AppVar):
    u8  file count
    file count * 22-byte directory entries:
        name[16]   PETSCII, $A0-padded (exactly as on the disk)
        u24 offset of the file data from the start of the payload
        u24 size   of the file data (includes the 2-byte load address)
    raw file data blobs
"""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path

import rom_to_appvar as rta

SECTORS_PER_TRACK = [21] * 17 + [19] * 7 + [18] * 6 + [17] * 5  # tracks 1..35
DIR_TRACK, DIR_SECTOR = 18, 1
D64_SIZE_35_TRACKS = 174848
FILE_TYPE_PRG = 2
DIRECTORY_ENTRY = 22
MAX_FILES = 144


def sector_offset(track: int, sector: int) -> int:
    if not 1 <= track <= 35 or sector >= SECTORS_PER_TRACK[track - 1]:
        raise ValueError(f"invalid track/sector t{track},s{sector}")
    return (sum(SECTORS_PER_TRACK[: track - 1]) + sector) * 256


def extract_file(image: bytes, track: int, sector: int) -> bytes:
    """Follow a file's sector chain. Last block's 'sector' byte is the index
    of the last used byte in that block."""
    out = bytearray()
    seen = set()
    while track:
        if (track, sector) in seen:
            raise ValueError("sector chain loop")
        seen.add((track, sector))
        block = image[sector_offset(track, sector):][:256]
        next_track, next_sector = block[0], block[1]
        if next_track == 0:
            if next_sector < 2:
                raise ValueError("bad last-block byte count")
            out += block[2: next_sector + 1]
            break
        out += block[2:256]
        track, sector = next_track, next_sector
    return bytes(out)


def read_directory(image: bytes) -> list[tuple[bytes, int, int]]:
    """Return (petscii name[16], start track, start sector) for every closed
    PRG file, in directory order."""
    files = []
    track, sector = DIR_TRACK, DIR_SECTOR
    seen = set()
    while track and (track, sector) not in seen:
        seen.add((track, sector))
        block = image[sector_offset(track, sector):][:256]
        for i in range(8):
            entry = block[2 + i * 32: 2 + i * 32 + 30]
            file_type = entry[0]
            # closed (bit7) PRG files only; skip splat/DEL/SEQ/REL entries
            if (file_type & 0x87) != (0x80 | FILE_TYPE_PRG):
                continue
            files.append((bytes(entry[3:19]), entry[1], entry[2]))
        track, sector = block[0], block[1]
    return files


def build_payload(image: bytes) -> bytes:
    entries = read_directory(image)
    if not entries:
        raise ValueError("no closed PRG files found on the disk")
    if len(entries) > MAX_FILES:
        raise ValueError(f"too many files ({len(entries)} > {MAX_FILES})")

    blobs = []
    for name, track, sector in entries:
        data = extract_file(image, track, sector)
        if len(data) < 2:
            raise ValueError(f"file {name!r} has no load address")
        blobs.append(data)

    directory = bytearray([len(entries)])
    offset = 1 + len(entries) * DIRECTORY_ENTRY
    for (name, _, _), data in zip(entries, blobs):
        directory += name
        directory += offset.to_bytes(3, "little")
        directory += len(data).to_bytes(3, "little")
        offset += len(data)

    return bytes(directory) + b"".join(blobs)


def convert(input_path: Path, output_dir: Path, convbin: str) -> list[Path]:
    image = input_path.read_bytes()
    if len(image) not in (D64_SIZE_35_TRACKS, D64_SIZE_35_TRACKS + 683):
        raise ValueError(
            f"not a 35-track .d64 image ({len(image)} bytes); "
            "40-track and error-byte variants are not supported"
        )

    payload = build_payload(image)
    print(f"packed {payload[0]} PRG files, payload {len(payload)} bytes")

    # Stored uncompressed on purpose: src/drive.c reads file bytes directly
    # out of the archived AppVar (no RAM to decompress a whole disk into).
    chunks = rta.build_chunks(payload, "dsk", len(payload), rta.COMPRESSION_NONE)
    return rta.write_chunks(chunks, rta.KINDS["dsk"][0], output_dir, convbin)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="input .d64 disk image")
    parser.add_argument("--output", type=Path, default=Path("bin/rom"))
    parser.add_argument("--convbin", help="path to the CEdev convbin executable")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        outputs = convert(args.input, args.output, rta.find_convbin(args.convbin))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"error: {error}") from error
    for output in outputs:
        print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
