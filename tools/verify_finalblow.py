#!/usr/bin/env python3
"""Host-side verification for the FINAL BLOW investigation.

1. Parse bin/rom/ECDSK00/01.8xv, reassemble the payload, and compare it
   byte-for-byte with a fresh build_payload() of the source D64.
2. Simulate the game's stack-resident RLE depacker to learn the expected
   depacked memory (head of $C000 etc).
"""
import sys
from pathlib import Path

sys.path.insert(0, "tools")
import d64_to_appvar as d64

DISK = Path("tests/FINALBLW.D64")
HEADER = 22
PAYLOAD_MAX = 0x8000


def appvar_payload(path: Path) -> bytes:
    raw = path.read_bytes()
    # 8xv: 55-byte header, then var entry; var data starts at offset 0x4A
    # (0x48 = data section, first two bytes = var data length).
    data_len = raw[0x48] | (raw[0x49] << 8)
    var = raw[0x4A:0x4A + data_len]
    assert var[:4] == b"EM64", f"{path}: bad magic {var[:4]!r}"
    chunk = var[6]
    stored = int.from_bytes(var[10:14], "little")
    payload_size = int.from_bytes(var[18:20], "little")
    payload = var[HEADER:HEADER + payload_size]
    assert len(payload) == payload_size, f"{path}: truncated payload"
    return chunk, stored, payload


def main() -> int:
    image = DISK.read_bytes()
    expected = d64.build_payload(image)

    chunks = {}
    for name in ("ECDSK00.8xv", "ECDSK01.8xv"):
        chunk, stored, payload = appvar_payload(Path("bin/rom") / name)
        chunks[chunk] = payload
        print(f"{name}: chunk={chunk} stored_total={stored} payload={len(payload)}")

    actual = b"".join(chunks[i] for i in sorted(chunks))
    print(f"reassembled={len(actual)} expected={len(expected)} "
          f"match={actual == expected}")
    if actual != expected:
        for i, (a, b) in enumerate(zip(actual, expected)):
            if a != b:
                print(f"first mismatch at payload offset {i:#x}")
                break
        return 1

    # PRG located at payload offset 0x17 (1 count byte + 22-byte entry).
    prg = expected[0x17:]
    base = prg[0] | (prg[1] << 8)
    data = prg[2:]
    print(f"PRG load=${base:04x} len={len(data)}")

    def at(addr):  # C64 address -> PRG byte
        return data[addr - base]

    boundary = base + (PAYLOAD_MAX - 0x19)  # C64 addr of chunk boundary
    print(f"chunk boundary at C64 ${boundary:04x}")
    print("bytes $%04x-$%04x:" % (boundary - 8, boundary + 7),
          bytes(at(a) for a in range(boundary - 8, boundary + 8)).hex(" "))

    print("routines $0854-$0878:",
          bytes(at(a) for a in range(0x0854, 0x0879)).hex(" "))
    print("stream head $63be-$63dd:",
          bytes(at(a) for a in range(0x63BE, 0x63DE)).hex(" "))
    print("stream tail $a588-$a597:",
          bytes(at(a) for a in range(0xA588, 0xA598)).hex(" "))

    # --- exact init-copy simulation ($082b-$0851) --------------------------
    # Relocates the packed stream: LDA $a597 / STA $019f with self-decremented
    # operands; the dest high byte skips page 0 (DEC until nonzero); counter
    # $9de2 in the classic DEC/BNE pair layout.
    mem64 = bytearray(0x10000)
    mem64[base:base + len(data)] = data

    srclo, srchi = 0x97, 0xA5
    dstlo, dsthi = 0x9F, 0x01
    cntlo, cnthi = at(0x0854), at(0x0855)
    iters = 0
    while True:
        iters += 1
        mem64[(dsthi << 8) | dstlo] = mem64[(srchi << 8) | srclo]
        if srclo == 0:
            srchi = (srchi - 1) & 0xFF
        srclo = (srclo - 1) & 0xFF
        if dstlo == 0:
            dsthi = (dsthi - 1) & 0xFF
            while dsthi == 0:
                dsthi = (dsthi - 1) & 0xFF
        dstlo = (dstlo - 1) & 0xFF
        cntlo = (cntlo - 1) & 0xFF
        if cntlo == 0:
            cnthi = (cnthi - 1) & 0xFF
            if cnthi == 0:
                break
    print(f"init copy: {iters} iterations, "
          f"src end ${(srchi << 8) | srclo:04x}, dst end ${(dsthi << 8) | dstlo:04x}")
    print("relocated stream head $63be+:", mem64[0x63BE:0x63BE + 16].hex(" "))

    # --- depacker simulation over the RELOCATED memory ---------------------
    rp = 0x63BE
    wp = 0x080B
    segments = []
    seg_start = wp
    wrapped = False

    def rd():
        nonlocal rp, wrapped
        v = mem64[rp]
        if rp == 0xFFFF:
            wrapped = True
        rp = (rp + 1) & 0xFFFF
        return v

    def wr(v):
        nonlocal wp
        mem64[wp] = v
        wp = (wp + 1) & 0xFFFF

    while True:
        a = rd()
        if a != 0x4D:
            wr(a)
            continue
        t = rd()
        if t >= 3:
            v = rd()
            for _ in range(t):
                wr(v)
        elif t == 2:
            wr(0x4D)
        else:
            lo, hi = rd(), rd()
            segments.append((seg_start, wp))
            if hi == 0:
                break
            wp = lo | (hi << 8)
            seg_start = wp

    print(f"depack ended: rp=${rp:04x} wrapped_through_zp={wrapped}")
    print(f"{len(segments)} segments:")
    for s, e in segments:
        print(f"  ${s:04x}-${(e - 1) & 0xFFFF:04x} ({(e - s) & 0xFFFF} bytes)")
    print("expected $c000-$c00f:", mem64[0xC000:0xC010].hex(" "))
    # bytes the depacker READS from the $d000-$d3ff window (the banking bug
    # made the emulator return char ROM here instead of these RAM bytes)
    print("stream bytes at $d000+:", mem64[0xD000:0xD010].hex(" "))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
