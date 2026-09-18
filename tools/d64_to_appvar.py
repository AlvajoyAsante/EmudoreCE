#!/usr/bin/env python3
"""Convert a .d64 disk image or .t64 tape image into emudore CE disk AppVars.

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
from dataclasses import dataclass
import subprocess
from pathlib import Path

import disk_metadata
import appvar_builder as rta

SECTORS_PER_TRACK = [21] * 17 + [19] * 7 + [18] * 6 + [17] * 5  # tracks 1..35
DIR_TRACK, DIR_SECTOR = 18, 1
D64_SIZE_35_TRACKS = 174848
T64_HEADER_SIZE = 0x40
T64_DIRECTORY_ENTRY = 32
T64_HEADER_SIGNATURES = (b"c64 tape image file", b"c64 tape file",
                         b"c64s tape image file", b"c64s tape file")
FILE_TYPE_PRG = 2
DIRECTORY_ENTRY = 22
MAX_FILES = 144
PACKAGE_ID_MAX = 4


@dataclass(frozen=True)
class DirectoryEntry:
    name: bytes
    track: int
    sector: int
    file_type: int


@dataclass(frozen=True)
class TapeEntry:
    name: bytes
    start: int
    end: int
    offset: int
    file_type: int


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


def read_directory_entries(image: bytes) -> list[DirectoryEntry]:
    """Return closed PRGs in directory order, retaining original type flags."""
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
            files.append(DirectoryEntry(bytes(entry[3:19]), entry[1], entry[2], file_type))
        track, sector = block[0], block[1]
    return files


def read_directory(image: bytes) -> list[tuple[bytes, int, int]]:
    """Return (petscii name[16], start track, start sector) for closed PRGs."""
    return [(entry.name, entry.track, entry.sector) for entry in read_directory_entries(image)]


def is_geos_disk(image: bytes) -> bool:
    """Check if D64 image has GEOS signature in BAM or GEOS file structures."""
    if len(image) < D64_SIZE_35_TRACKS:
        return False
    bam = sector_offset(18, 0)
    if image[bam + 0xAD : bam + 0xB1] == b"GEOS":
        return True
    track, sector = DIR_TRACK, DIR_SECTOR
    seen = set()
    while track and (track, sector) not in seen:
        seen.add((track, sector))
        block = image[sector_offset(track, sector):][:256]
        for i in range(8):
            entry = block[2 + i * 32 : 2 + i * 32 + 30]
            file_type = entry[0]
            if (file_type & 0x80) and (file_type & 0x07) == 3:  # USR file
                return True
            geos_struct = entry[21]
            if (file_type & 0x80) and geos_struct == 1:  # VLIR file
                return True
        track, sector = block[0], block[1]
    return False


def read_all_directory_entries(image: bytes) -> list[DirectoryEntry]:
    """Return all closed files (PRG, USR, SEQ, REL) in directory order."""
    files = []
    track, sector = DIR_TRACK, DIR_SECTOR
    seen = set()
    while track and (track, sector) not in seen:
        seen.add((track, sector))
        block = image[sector_offset(track, sector):][:256]
        for i in range(8):
            entry = block[2 + i * 32 : 2 + i * 32 + 30]
            file_type = entry[0]
            if (file_type & 0x80) and (file_type & 0x07) in (1, 2, 3, 4):
                files.append(DirectoryEntry(bytes(entry[3:19]), entry[1], entry[2], file_type))
        track, sector = block[0], block[1]
    return files


def read_t64_directory_entries(image: bytes) -> list[TapeEntry]:
    if len(image) < T64_HEADER_SIZE:
        raise ValueError("T64 image is shorter than its header")
    if not any(signature in image[:32].lower() for signature in T64_HEADER_SIGNATURES):
        raise ValueError("not a T64 tape image")

    entry_count = int.from_bytes(image[0x22:0x24], "little")
    directory_end = T64_HEADER_SIZE + entry_count * T64_DIRECTORY_ENTRY
    if directory_end > len(image):
        raise ValueError("T64 directory extends past the image")

    entries = []
    for index in range(entry_count):
        offset = T64_HEADER_SIZE + index * T64_DIRECTORY_ENTRY
        entry = image[offset:offset + T64_DIRECTORY_ENTRY]
        entry_type, file_type = entry[0], entry[1]
        if entry_type == 0:
            continue
        if entry_type != 1 or (file_type & 0x07) != FILE_TYPE_PRG:
            continue
        start = int.from_bytes(entry[2:4], "little")
        end = int.from_bytes(entry[4:6], "little")
        data_offset = int.from_bytes(entry[8:12], "little")
        if end < start:
            raise ValueError(f"T64 entry {index} has an invalid address range")
        data_end = data_offset + end - start
        if data_offset < directory_end or data_end > len(image):
            raise ValueError(f"T64 entry {index} data extends past the image")
        entries.append(TapeEntry(bytes(entry[16:32]).rstrip(b" \0").ljust(16, b"\xa0"),
                                 start, end, data_offset, file_type))
    return entries


def normalize_package_id(value: str) -> str:
    package_id = "".join(character for character in value.upper() if character.isalnum())
    if not package_id:
        raise ValueError("disk name does not contain a usable AppVar ID")
    if len(package_id) > PACKAGE_ID_MAX:
        package_id = package_id[:PACKAGE_ID_MAX]
    if package_id in {"BAS", "KRN", "CHR", "CRT"}:
        raise ValueError(f"disk AppVar ID {package_id!r} is reserved")
    return package_id


def disk_package_id(image: bytes, input_path: Path, requested_id: str | None = None) -> str:
    if requested_id is not None:
        return normalize_package_id(requested_id)
    bam = sector_offset(18, 0)
    label = image[bam + 0x90:bam + 0xA0]
    name = "".join(chr(byte - 0x80) if 0xC1 <= byte <= 0xDA else chr(byte)
                   for byte in label if byte != 0xA0)
    return normalize_package_id(name if any(character.isalnum() for character in name)
                                else input_path.stem)


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


def build_t64_payload(image: bytes) -> tuple[bytes, bytes]:
    entries = read_t64_directory_entries(image)
    if not entries:
        raise ValueError("no normal PRG files found in the T64 image")
    if len(entries) > MAX_FILES:
        raise ValueError(f"too many files ({len(entries)} > {MAX_FILES})")

    blobs = [entry.start.to_bytes(2, "little") + image[entry.offset:entry.offset + entry.end - entry.start]
             for entry in entries]
    directory = bytearray([len(entries)])
    offset = 1 + len(entries) * DIRECTORY_ENTRY
    for entry, data in zip(entries, blobs):
        directory += entry.name
        directory += offset.to_bytes(3, "little")
        directory += len(data).to_bytes(3, "little")
        offset += len(data)
    return bytes(directory) + b"".join(blobs), bytes(entry.file_type for entry in entries)


def convert(input_path: Path, output_dir: Path, convbin: str,
            package_id: str | None = None, raw_d64: bool = False) -> list[Path]:
    if package_id is None:
        package_id = "DSK"
    else:
        package_id = normalize_package_id(package_id)

    metadata_name = disk_metadata.variable_name(package_id)
    rta.validate_output_paths([output_dir / f"EC{package_id}00.8xv", output_dir / f"{metadata_name}.8xv"], [input_path])

    image = input_path.read_bytes()
    suffix = input_path.suffix.lower()

    if suffix == ".d64":
        if len(image) not in (D64_SIZE_35_TRACKS, D64_SIZE_35_TRACKS + 683):
            raise ValueError(
                f"not a 35-track .d64 image ({len(image)} bytes); "
                "40-track and error-byte variants are not supported"
            )
        if raw_d64 or is_geos_disk(image):
            payload = image[:D64_SIZE_35_TRACKS]
            file_types = bytes(entry.file_type for entry in read_all_directory_entries(image))
            if not file_types:
                file_types = bytes([0x82])
            print(f"packed raw D64 image, payload {len(payload)} bytes, {len(file_types)} directory entries")
        else:
            payload = build_payload(image)
            file_types = bytes(entry.file_type for entry in read_directory_entries(image))
            print(f"packed {payload[0]} PRG files, payload {len(payload)} bytes")
    elif suffix == ".t64":
        payload, file_types = build_t64_payload(image)
        print(f"packed {payload[0]} PRG files, payload {len(payload)} bytes")
    else:
        raise ValueError(
            f"unsupported input extension {input_path.suffix!r}; expected .d64 or .t64"
        )

    # Stored uncompressed on purpose: src/drive.c reads file bytes directly
    # out of the archived AppVar (no RAM to decompress a whole disk into).
    chunks = rta.build_chunks(payload, "dsk", len(payload), rta.COMPRESSION_NONE)
    metadata = disk_metadata.build_metadata(payload, file_types)
    metadata_path = output_dir / f"{metadata_name}.8xv"
    destinations = [output_dir / f"EC{package_id}{index:02X}.8xv" for index in range(len(chunks))]
    rta.validate_output_paths(destinations + [metadata_path], [input_path])
    outputs = rta.write_chunks(chunks, package_id, output_dir, convbin)
    outputs.append(rta.write_appvar(metadata, metadata_name, output_dir, convbin))
    return outputs


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="input .d64 disk image or .t64 tape image")
    parser.add_argument("--output", type=Path, default=Path("bin/rom"))
    parser.add_argument("--id", dest="package_id", help="1-4 character AppVar package ID")
    parser.add_argument("--convbin", help="path to the CEdev convbin executable")
    parser.add_argument("--raw", action="store_true", help="package raw D64 image (required for GEOS and sector loaders)")
    parser.add_argument("--geos", action="store_true", dest="raw", help="alias for --raw")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        outputs = convert(args.input, args.output, rta.find_convbin(args.convbin), args.package_id, args.raw)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"error: {error}") from error
    for output in outputs:
        print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
