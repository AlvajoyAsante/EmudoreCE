#!/usr/bin/env python3
"""Pack BASIC listings and PRG files into emudore CE DSK AppVars (ECDSKnn)."""

from __future__ import annotations

import argparse
import re
import subprocess
from pathlib import Path

import rom_to_appvar as rta

ENTRY_SIZE = 22
MAX_FILES = 144
LOAD_ADDR_BASIC = 0x0801

TOKENS = {
    "END": 0x80,
    "FOR": 0x81,
    "NEXT": 0x82,
    "DATA": 0x83,
    "INPUT": 0x84,
    "DIM": 0x85,
    "READ": 0x86,
    "LET": 0x87,
    "GOTO": 0x89,
    "RUN": 0x8A,
    "IF": 0x8B,
    "RESTORE": 0x8C,
    "GOSUB": 0x8D,
    "RETURN": 0x8E,
    "REM": 0x8F,
    "STOP": 0x90,
    "ON": 0x91,
    "WAIT": 0x92,
    "LOAD": 0x93,
    "SAVE": 0x94,
    "VERIFY": 0x95,
    "DEF": 0x96,
    "POKE": 0x97,
    "PRINT": 0x99,
    "CONT": 0x9A,
    "LIST": 0x9B,
    "CLR": 0x9C,
    "CMD": 0x9D,
    "SYS": 0x9E,
    "OPEN": 0x9F,
    "CLOSE": 0xA0,
    "GET": 0xA1,
    "NEW": 0xA2,
    "TAB(": 0xA3,
    "TO": 0xA4,
    "FN": 0xA5,
    "SPC(": 0xA6,
    "THEN": 0xA7,
    "NOT": 0xA8,
    "STEP": 0xA9,
    "+": 0xAA,
    "-": 0xAB,
    "*": 0xAC,
    "/": 0xAD,
    "^": 0xAE,
    "AND": 0xAF,
    "OR": 0xB0,
    ">": 0xB1,
    "=": 0xB2,
    "<": 0xB3,
    "SGN": 0xB4,
    "INT": 0xB5,
    "ABS": 0xB6,
    "USR": 0xB7,
    "FRE": 0xB8,
    "POS": 0xB9,
    "SQR": 0xBA,
    "RND": 0xBB,
    "LOG": 0xBC,
    "EXP": 0xBD,
    "COS": 0xBE,
    "SIN": 0xBF,
    "TAN": 0xC0,
    "ATN": 0xC1,
    "PEEK": 0xC2,
    "LEN": 0xC3,
    "STR$": 0xC4,
    "VAL": 0xC5,
    "ASC": 0xC6,
    "CHR$": 0xC7,
    "LEFT$": 0xC8,
    "RIGHT$": 0xC9,
    "MID$": 0xCA,
    "GO": 0xCB,
}

TOKEN_ORDER = sorted(TOKENS, key=len, reverse=True)
LINE_RE = re.compile(r"^\s*(\d+)\s*(.*)$")


def sanitize_name(path: Path) -> bytes:
    stem = path.stem.upper().replace("_", " ").replace("-", " ")
    filtered = "".join(ch for ch in stem if ch.isalnum() or ch == " ")
    if not filtered:
        filtered = "PROGRAM"
    return filtered[:16].encode("latin-1", "replace").ljust(16, b"\xA0")


def tokenize_basic_text(text: str) -> bytes:
    upper = text.upper()
    out = bytearray()
    i = 0
    in_quotes = False
    in_rem = False

    while i < len(text):
        ch = text[i]
        if in_rem:
            out.append(ord(ch) & 0xFF)
            i += 1
            continue

        if ch == '"':
            in_quotes = not in_quotes
            out.append(ord(ch))
            i += 1
            continue

        if in_quotes:
            out.append(ord(ch) & 0xFF)
            i += 1
            continue

        if ch == "?":
            out.append(TOKENS["PRINT"])
            i += 1
            continue

        matched = False
        for token in TOKEN_ORDER:
            if upper.startswith(token, i):
                out.append(TOKENS[token])
                i += len(token)
                if token == "REM":
                    in_rem = True
                matched = True
                break
        if matched:
            continue

        out.append(ord(ch) & 0xFF)
        i += 1

    return bytes(out)


def basic_to_prg(path: Path) -> bytes:
    raw_lines = path.read_text(encoding="utf-8").splitlines()
    parsed: list[tuple[int, bytes]] = []
    auto_line = 1

    for line in raw_lines:
        striped = line.strip()
        if not striped:
            continue
        m = LINE_RE.match(line)
        if m:
            line_no = int(m.group(1))
            body = m.group(2)
            auto_line = max(auto_line, line_no + 10)
        else:
            line_no = auto_line
            body = striped
            auto_line += 10
        if not 0 <= line_no <= 63999:
            raise ValueError(f"{path}: invalid BASIC line number {line_no}")
        parsed.append((line_no, tokenize_basic_text(body)))

    if not parsed:
        raise ValueError(f"{path}: no BASIC lines found")

    out = bytearray()
    out += LOAD_ADDR_BASIC.to_bytes(2, "little")

    cur = LOAD_ADDR_BASIC
    for idx, (line_no, body) in enumerate(parsed):
        next_ptr = 0
        if idx != len(parsed) - 1:
            next_ptr = cur + 2 + 2 + len(body) + 1

        out += next_ptr.to_bytes(2, "little")
        out += line_no.to_bytes(2, "little")
        out += body
        out.append(0)
        cur = next_ptr if next_ptr else cur

    return bytes(out)


def read_program(path: Path) -> bytes:
    ext = path.suffix.lower()
    if ext == ".prg":
        data = path.read_bytes()
        if len(data) < 3:
            raise ValueError(f"{path}: PRG is too small")
        return data
    if ext == ".bas":
        return basic_to_prg(path)
    raise ValueError(f"{path}: unsupported extension {path.suffix}")


def build_payload(inputs: list[Path]) -> bytes:
    if not inputs:
        raise ValueError("no input files")
    if len(inputs) > MAX_FILES:
        raise ValueError(f"too many files ({len(inputs)} > {MAX_FILES})")

    files = [(sanitize_name(path), read_program(path)) for path in inputs]

    directory = bytearray([len(files)])
    offset = 1 + len(files) * ENTRY_SIZE
    blobs = bytearray()

    for name, data in files:
        directory += name
        directory += offset.to_bytes(3, "little")
        directory += len(data).to_bytes(3, "little")
        offset += len(data)
        blobs += data

    return bytes(directory + blobs)


def convert(inputs: list[Path], output_dir: Path, convbin: str) -> list[Path]:
    payload = build_payload(inputs)
    chunks = rta.build_chunks(payload, "dsk", len(payload), rta.COMPRESSION_NONE)
    return rta.write_chunks(chunks, rta.KINDS["dsk"][0], output_dir, convbin)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", type=Path, nargs="+", help=".bas and/or .prg inputs")
    parser.add_argument("--output", type=Path, default=Path("bin/rom"))
    parser.add_argument("--convbin", help="path to the CEdev convbin executable")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        outputs = convert(args.inputs, args.output, rta.find_convbin(args.convbin))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"error: {error}") from error

    for output in outputs:
        print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())