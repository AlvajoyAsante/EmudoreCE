# GEOS Handoff

## Current Status

GEOS is not booting to DeskTop yet.

The raw 35-track `tests/GEOS64.d64` is recognized and can be served sector by
sector. The GEOS bridge installs virtual disk-driver traps at `$9D80-$9DA6` and
handles sector reads, writes, links, and the directory head.

## Confirmed Boot Path

1. The launcher loads `GEOS` from track 17, sector 0.
2. It loads `GEOS BOOT` from track 1, sector 16.
3. EMUDORE loads the first three `GEOS KERNAL` VLIR records into `$9000`,
   `$C000`, and RAM beneath I/O at `$D000`.
4. The direct bootstrap installs the virtual disk driver and redirects `$1000`
   to `JMP $C003`, bypassing GEOS BOOT's physical-1541 path.

The host drive regression passes 14/14 checks, including the real GEOS D64
bootstrap boundary and the KERNAL-tail RAM shadow.

## Current Blocker

The target run reaches resident GEOS code and changes the framebuffer, but it
never calls the virtual disk driver. At 120 and 600 frame checkpoints it cycles
through the resident `$D570-$D59D` range with `drive_geos_call_count == 0`.

This is not a JIT-only problem: JIT-on and JIT-off runs show the same behavior.
It does not prove that DeskTop has started.

## Likely Missing State

The direct handoff skips execution of `GEOS BOOT`. Its entry code begins by
XOR-decoding a 1 KiB region at `$8610-$8A0F`. That setup is a concrete candidate
for missing boot state, but the source and timing of those bytes have not yet
been proven. Do not patch or remove GEOS checks until that contract is traced.

## Next Step

Trace the original GEOS BOOT setup to determine what it places at `$8610-$8A0F`
and any other resident state before its final handoff. Recreate only the proven
state in `drive_bootstrap_geos()`, add a real-D64 host assertion, then require a
real `ReadBlock` call in the target GEOS test before claiming compatibility.

## Constraints

- Keep the one-time `GEOS BOOT` bootstrap boundary.
- Keep `$1000 = JMP $C003`.
- Keep the KERNAL-tail RAM shadow copy.
- Do not restore the unrelated CIA1 experiment or load a fourth VLIR record.
- Do not claim browser or DeskTop compatibility until target disk I/O occurs.
