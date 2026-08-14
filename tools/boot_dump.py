#!/usr/bin/env python3
"""Dump the Rally Driver boot file to understand its autostart flow."""

from pathlib import Path

SPT = [21] * 17 + [19] * 7 + [18] * 6 + [17] * 5


def off(t, s):
    return (sum(SPT[: t - 1]) + s) * 256


data = Path("tests/Rally Driver.d64").read_bytes()

# boot file: track 17 sector 0
t, s = 17, 0
out = bytearray()
while t:
    b = data[off(t, s):][:256]
    nt, ns = b[0], b[1]
    if nt == 0:
        out += b[2:ns + 1]
        break
    out += b[2:256]
    t, s = nt, ns

load = out[0] | (out[1] << 8)
body = out[2:]
print(f"load=${load:04x} len={len(body)} end=${load + len(body) - 1:04x}")
for i in range(0, len(body), 16):
    addr = load + i
    print(f"${addr:04x}: {body[i:i+16].hex(' ')}")

# value that lands in the BASIC idle vector $0302/$0303
v = load
idx = 0x0302 - load
print(f"\n$0302 vector after load = ${body[idx] | (body[idx+1] << 8):04x}")
