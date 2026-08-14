#!/usr/bin/env python3
"""Convert a C64 ROM (BASIC / KERNAL / character generator) into emudore CE AppVars.

Each ROM is split into <= 32 KiB AppVars with a small header and an optional
LZSS-compressed payload. The on-calc CEPackage reader (src/CEPackage.c)
understands this exact format: magic "EM64", version, package index, chunk
numbering and a per-chunk checksum.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

CHUNK_SIZE = 0x8000
HEADER = struct.Struct("<4sBBBBBBIIHH")
PACKAGE_VERSION = 2
COMPRESSION_NONE = 0
COMPRESSION_LZSS = 1
LZSS_MIN_MATCH = 3
LZSS_MAX_MATCH = 18
LZSS_MAX_DISTANCE = 4096
LZSS_MATCH_CANDIDATES = 64

# package kind -> (package id, expected ROM size, C64 load address)
# ids must be <= 4 chars: the AppVar name is "EC" + id + 2 hex digits (<=8).
# "dsk" is a variable-size disk-file package built by tools/d64_to_appvar.py.
KINDS = {
    "basic":  ("BAS", 0x2000, 0xA000),
    "kernal": ("KRN", 0x2000, 0xE000),
    "chars":  ("CHR", 0x1000, 0xD000),
    "dsk":    ("DSK", None, 0),
}
KIND_INDEX = {"basic": 0, "kernal": 1, "chars": 2, "dsk": 3}


def validate_rom(data: bytes, kind: str) -> None:
    expected = KINDS[kind][1]
    if expected is None:
        return
    if len(data) != expected:
        raise ValueError(
            f"a C64 {kind} ROM must be exactly {expected:#x} bytes "
            f"({expected // 1024} KiB), got {len(data):#x}"
        )


def compress_lzss(data: bytes) -> bytes:
    """Simple LZSS variant whose matches copy out of the already-decoded
    output, so the decoder needs no separate sliding window."""
    size = len(data)
    out = bytearray()
    positions: dict[bytes, list[int]] = {}
    tokens: list[tuple] = []

    def flush() -> None:
        control = 0
        body = bytearray()
        for slot, token in enumerate(tokens):
            bit = 7 - slot
            if token[0] == "literal":
                control |= 1 << bit
                body.append(token[1])
            else:
                distance_minus1 = token[1] - 1
                length_minus3 = token[2] - LZSS_MIN_MATCH
                body.append(distance_minus1 & 0xFF)
                body.append(((distance_minus1 >> 8) & 0x0F) | ((length_minus3 & 0x0F) << 4))
        out.append(control)
        out.extend(body)
        tokens.clear()

    index = 0
    while index < size:
        best_length = 0
        best_distance = 0

        if index + LZSS_MIN_MATCH <= size:
            key = bytes(data[index : index + 3])
            for candidate in reversed(positions.get(key, ())):
                distance = index - candidate
                if distance > LZSS_MAX_DISTANCE:
                    break
                max_length = min(LZSS_MAX_MATCH, size - index)
                length = 0
                while length < max_length and data[candidate + length] == data[index + length]:
                    length += 1
                if length > best_length:
                    best_length = length
                    best_distance = distance
                    if best_length == LZSS_MAX_MATCH:
                        break

        if best_length >= LZSS_MIN_MATCH:
            tokens.append(("match", best_distance, best_length))
            advance = best_length
        else:
            tokens.append(("literal", data[index]))
            advance = 1

        end = index + advance
        while index < end:
            if index + 3 <= size:
                key = bytes(data[index : index + 3])
                bucket = positions.setdefault(key, [])
                bucket.append(index)
                if len(bucket) > LZSS_MATCH_CANDIDATES:
                    del bucket[0]
            index += 1

        if len(tokens) == 8:
            flush()

    if tokens:
        flush()

    return bytes(out)


def decompress_lzss(data: bytes, original_size: int) -> bytes:
    out = bytearray(original_size)
    read_pos = 0
    write_pos = 0

    while write_pos < original_size:
        control = data[read_pos]
        read_pos += 1
        for slot in range(8):
            if write_pos >= original_size:
                break
            bit = 7 - slot
            if control & (1 << bit):
                out[write_pos] = data[read_pos]
                read_pos += 1
                write_pos += 1
            else:
                low, high = data[read_pos], data[read_pos + 1]
                read_pos += 2
                distance = (low | ((high & 0x0F) << 8)) + 1
                length = min((high >> 4) + LZSS_MIN_MATCH, original_size - write_pos)
                for _ in range(length):
                    out[write_pos] = out[write_pos - distance]
                    write_pos += 1

    return bytes(out)


def maybe_compress(data: bytes, enabled: bool) -> tuple[bytes, int]:
    if not enabled or not data:
        return data, COMPRESSION_NONE

    compressed = compress_lzss(data)
    if decompress_lzss(compressed, len(data)) != data:
        raise ValueError("internal LZSS compressor self-check failed")
    if len(compressed) < len(data):
        return compressed, COMPRESSION_LZSS
    return data, COMPRESSION_NONE


def build_chunks(data: bytes, kind: str, original_size: int, compression: int) -> list[bytes]:
    chunk_count = max((len(data) + CHUNK_SIZE - 1) // CHUNK_SIZE, 1)
    if chunk_count > 255:
        raise ValueError("ROM requires more than 255 AppVar chunks")

    chunks = []
    for index in range(chunk_count):
        payload = data[index * CHUNK_SIZE : (index + 1) * CHUNK_SIZE]
        header = HEADER.pack(
            b"EM64",
            PACKAGE_VERSION,
            KIND_INDEX[kind],
            index,
            chunk_count,
            0,  # mapper field unused for C64 ROMs
            compression,
            len(data),
            original_size,
            len(payload),
            sum(payload) & 0xFFFF,
        )
        chunks.append(header + payload)
    return chunks


def find_convbin(explicit: str | None) -> str:
    if explicit:
        return explicit

    executable = shutil.which("convbin")
    if executable:
        return executable

    cedev = os.environ.get("CEDEV")
    if cedev:
        candidate = Path(cedev) / "bin" / "convbin.exe"
        if candidate.is_file():
            return str(candidate)

    raise FileNotFoundError("convbin was not found; pass --convbin or set CEDEV")


def write_chunks(
    chunks: list[bytes],
    package_id: str,
    output_dir: Path,
    convbin: str,
) -> list[Path]:
    output_dir.mkdir(parents=True, exist_ok=True)
    outputs = []

    with tempfile.TemporaryDirectory() as temporary_directory:
        for index, chunk in enumerate(chunks):
            variable_name = f"EC{package_id}{index:02X}"
            raw_path = Path(temporary_directory) / f"{variable_name}.bin"
            output_path = output_dir / f"{variable_name}.8xv"
            raw_path.write_bytes(chunk)
            subprocess.run(
                [
                    convbin,
                    "-j", "bin",
                    "-k", "8xv",
                    "-i", str(raw_path),
                    "-o", str(output_path),
                    "-n", variable_name,
                    "-r",
                ],
                check=True,
            )
            outputs.append(output_path)

    return outputs


def convert(
    input_path: Path,
    output_dir: Path,
    kind: str,
    convbin: str,
    compress: bool,
) -> list[Path]:
    data = input_path.read_bytes()
    validate_rom(data, kind)
    package_id = KINDS[kind][0]

    stored, compression = maybe_compress(data, compress)
    chunks = build_chunks(stored, kind, len(data), compression)

    if compression != COMPRESSION_NONE:
        print(f"compressed {len(data)} bytes down to {len(stored)} bytes")

    return write_chunks(chunks, package_id, output_dir, convbin)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="input ROM file")
    parser.add_argument("--kind", required=True, choices=tuple(KINDS),
                        help="which C64 ROM this file is")
    parser.add_argument("--output", type=Path, default=Path("bin/rom"))
    parser.add_argument("--convbin", help="path to the CEdev convbin executable")
    parser.add_argument(
        "--compress",
        dest="compress",
        action="store_true",
        default=True,
        help="LZSS-compress the AppVar payload to save archive space (default)",
    )
    parser.add_argument(
        "--no-compress",
        dest="compress",
        action="store_false",
        help="store the ROM uncompressed",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    try:
        outputs = convert(
            args.input,
            args.output,
            args.kind,
            find_convbin(args.convbin),
            args.compress,
        )
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"error: {error}") from error

    for output in outputs:
        print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
