#!/usr/bin/env python3
"""Analyze 1.RALLY DRIVER (relocated to $18D0, entry $1B80): find which
KERNAL/IEC routines it uses to load the remaining parts."""

from pathlib import Path

SPT = [21] * 17 + [19] * 7 + [18] * 6 + [17] * 5


def off(t, s):
    return (sum(SPT[: t - 1]) + s) * 256


def extract(data, t, s):
    out = bytearray()
    while t:
        b = data[off(t, s):][:256]
        nt, ns = b[0], b[1]
        if nt == 0:
            out += b[2:ns + 1]
            break
        out += b[2:256]
        t, s = nt, ns
    return bytes(out)


data = Path("tests/Rally Driver.d64").read_bytes()
part1 = extract(data, 17, 1)[2:]  # drop load-address header
BASE = 0x18D0                      # boot relocates it here (SA=0)

print(f"part1: {len(part1)} bytes at ${BASE:04x}-${BASE+len(part1)-1:04x}, "
      f"entry $1b80 (offset ${0x1b80-BASE:03x})")

# scan for JSR/JMP absolute targets of interest
NAMES = {
    0xFFD5: "LOAD", 0xFFBA: "SETLFS", 0xFFBD: "SETNAM", 0xFFC0: "OPEN",
    0xFFC3: "CLOSE", 0xFFC6: "CHKIN", 0xFFC9: "CHKOUT", 0xFFCC: "CLRCHN",
    0xFFCF: "CHRIN", 0xFFD2: "CHROUT", 0xFFE4: "GETIN",
    0xFFB4: "TALK", 0xFF96: "TKSA", 0xFFA5: "ACPTR", 0xFFAB: "UNTLK",
    0xFFB1: "LISTEN", 0xFF93: "SECOND", 0xFFA8: "CIOUT", 0xFFAE: "UNLSN",
    0xFF8A: "RESTOR",
}
for i in range(len(part1) - 2):
    op = part1[i]
    if op in (0x20, 0x4C, 0x6C):  # JSR / JMP / JMP()
        target = part1[i + 1] | (part1[i + 2] << 8)
        kind = {0x20: "JSR", 0x4C: "JMP", 0x6C: "JMP()"}[op]
        if target in NAMES:
            print(f"${BASE+i:04x}: {kind} ${target:04x} ({NAMES[target]})")
        elif 0xE000 <= target <= 0xFFFF or 0xD000 <= target <= 0xDFFF:
            print(f"${BASE+i:04x}: {kind} ${target:04x} (KERNAL/IO internal)")

# direct CIA2 / IEC port accesses
print("\n$DD00-$DDFF accesses:")
for i in range(len(part1) - 2):
    if part1[i + 2] == 0xDD and part1[i] in (
            0xAD, 0x8D, 0x2C, 0x0D, 0x2D, 0x4D, 0x6D, 0xEE, 0xCE,
            0xB9, 0x99, 0xBD, 0x9D, 0x0E, 0x4E, 0x2E, 0x6E):
        print(f"${BASE+i:04x}: op {part1[i]:02x} ${part1[i+1] | 0xDD00:04x}")

# entry-point hex dump
e = 0x1B80 - BASE
print("\nentry code:")
for i in range(e, min(e + 96, len(part1)), 16):
    print(f"${BASE+i:04x}: {part1[i:i+16].hex(' ')}")
