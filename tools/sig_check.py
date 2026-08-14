#!/usr/bin/env python3
"""Verify the fastloader entry signature appears in each file that overwrites
the loader region, so the on-calc re-plant strategy works."""

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


SIG = bytes.fromhex("a9 00 85 93 78 a6 b9 86 b8 a9 60 85 b9 20 d5 f3".replace(" ", ""))

data = Path("tests/Rally Driver.d64").read_bytes()
FILES = [("RALLY DRIVER", 17, 0), ("1.RALLY DRIVER", 17, 1),
         ("2.RALLY DRIVER", 17, 2), ("3.RALLY DRIVER", 19, 0),
         ("4.RALLY DRIVER", 19, 9), ("5.RALLY DRIVER", 19, 18)]

for name, t, s in FILES:
    body = extract(data, t, s)
    load = body[0] | (body[1] << 8)
    payload = body[2:]
    hits = []
    i = payload.find(SIG)
    while i >= 0:
        hits.append(load + i)
        i = payload.find(SIG, i + 1)
    print(f"{name:16} load=${load:04x} sig at: "
          + (", ".join(f"${a:04x}" for a in hits) if hits else "none"))

# what does part 3 place at $1D90 (the JMP target after parts 2+3 load)?
p3 = extract(data, 19, 0)
load3 = p3[0] | (p3[1] << 8)
o = 0x1D90 - load3 + 2
print(f"\npart3 @$1d90: {p3[o:o+48].hex(' ')}")
