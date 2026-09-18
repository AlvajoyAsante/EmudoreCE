# GEOS Browser Launch Investigation

## Status

**GEOS does not currently work when opened from the EMUDORE browser.** The
packaged disk is detected and the early loader gets far enough to issue two
successful KERNAL LOAD requests in the target harness, but it has not reached
the GEOS DeskTop or exercised its normal disk-driver path. The browser launch
observed in CEmu crashes/returns instead of producing a usable GEOS desktop.

Earlier statements that GEOS was working were too broad. They established raw
D64 packaging and a small set of synthetic drive operations, not a successful
browser-to-GEOS boot.

## Evidence

The supplied `tests/GEOS64.d64` is a 174,848-byte 35-track D64. Its BAM names
the disk `GEOS v1.0` and contains the `GEOS` marker. Its directory includes
`GEOS BOOT`, an 84-block VLIR `GEOS KERNAL`, `DESK TOP`, GEOWRITE, and GEOPAINT.
The first sector of `GEOS KERNAL` is a VLIR index block: its first record links
are `1/10`, `1/11`, `1/12`, and so on. It is not a normal PRG data sector.

The most recent bounded target run recorded only two KERNAL loads and remained
in GEOS kernel code around `$D580-$D597`. It neither reached a DeskTop screen
nor performed an observable GEOS block-I/O request. That is progress past the
old `$C5AB` wait, but it is not a successful boot.

## Confirmed Problems

### The test path does not match the browser lifecycle

The actual program calls `drive_init()` before `mem_init()` in `main.c`.
`drive_init()` recognizes the GEOS D64 and immediately writes trap stubs into
guest RAM at `$9D80`. `mem_init()` then clears guest RAM, including those
stubs. The stubs are installed again only after a later successful trapped
KERNAL LOAD.

The linked game harness performs the calls in the opposite order: it calls
`mem_init()` before `drive_init()`. It therefore cannot expose the browser
launch ordering defect. The early install is redundant on a good path and
wrong on the real path; the later reinstall may happen soon enough for this
specific image, but that timing has not been validated.

### The GEOS test fixture is not the real boot format

`tests/drive/drive_test.c` builds a three-entry synthetic disk whose `GEOS`
entry is a plain, loadable PRG. It does not contain a real `GEOS KERNAL` entry,
an 84-block kernel, or the real boot program. As a result, its raw-load test
does not execute `drive_bootstrap_geos()`'s kernel-record loading branch.

The fixture's direct tests of `ReadBlock` and VLIR sectors prove only that a
256-byte sector can be copied into RAM and retrieved from the four-entry cache.
They do not run GEOS code or verify the GEOS driver ABI.

### The target harness bypasses GEOS disk calls

`tests/jit/game_test.c` invokes individual emulator startup functions, not the
calculator application's browser flow. It marks `TI-OS=none` and stubs display
text. Its hook table includes `drive_trap_load` but not `drive_trap_geos`.

For raw GEOS media, the harness intentionally prints `PASS LOAD (RAW GEOS)`
without comparing loaded bytes to an expected GEOS file. Its final pass checks
only require a LOAD plus changing nonblank frames. No assertion requires a
GEOS `ReadBlock`, `WriteBlock`, VLIR record, DeskTop launch, or a recognizable
GEOS framebuffer. A passing run therefore cannot demonstrate GEOS
compatibility.

### The bootstrap is a version-specific, unverified shortcut

After **every** successful raw-D64 KERNAL LOAD, `drive_trap_load()` calls
`drive_bootstrap_geos()`. That routine finds the file named `GEOS KERNAL`,
assumes VLIR records 0, 1, and 2 belong at `$9000`, `$C000`, and `$D000`, then
overwrites `$1000-$1002` with `JMP $C003`.

There is no record-count, chain-length, memory-content, entry-point, or
one-time-bootstrap check. GEOS directory metadata is parsed but not used to
validate this sequence. Even if those three record locations are appropriate
for this particular GEOS v1.0 image, the implementation silently reapplies the
shortcut on every later KERNAL LOAD. It can overwrite live GEOS state and is
not safe for another GEOS release or disk layout.

### The virtual driver is incomplete

The injected `$9D80` table turns GEOS driver calls into emulator shortcuts.
`InitForIO`, `DoneWithIO`, `EnterTurbo`, `ExitTurbo`, `PurgeTurbo`, and
`VerifyBlock` mostly return success without implementing their protocol or
state transitions. `ReadBlock` and `ReadLink` return raw D64 sectors, while
`WriteBlock` only writes to a four-sector in-memory cache; cache eviction loses
earlier writes while still reporting success. This is insufficient for normal
GEOS file, directory, and preference management.

The CIA2 `$DD00` read path returns the port-A latch for bits 0-5 and forces
bits 6-7 high; it does not model the electrical state of the IEC lines. That
avoids the original empty-bus wait, but it is not an IEC electrical or GEOS
turbo-protocol implementation. It must not be treated as proof that the GEOS
bootstrap's serial behavior is correct.

## Why Packaging Is Not the Failure

Raw D64 packaging itself is working as designed: `d64_to_appvar.py` keeps the
entire 35-track image intact, and the disk reader can map sectors from the
chunked AppVar. The failure begins after packaging, when GEOS expects a
consistent boot and disk-device contract that the current shortcuts do not
provide.

## Required Next Work

1. Reproduce the real lifecycle in a regression: call `drive_init()` before
   `mem_init()`, boot the selected `GEOS BOOT` entry, and run until either a
   defined GEOS-ready signature or a bounded failure.
2. Add GEOS-driver observability and target hooks for `drive_trap_geos`.
   The regression must fail unless it sees successful `ReadBlock` calls and
   verifies their destination bytes against the real D64.
3. Replace the three-record/every-LOAD bootstrap with a one-time,
   image-validated loader. It must validate every record chain and preserve
   the guest's expected entry and memory layout before control transfers to
   GEOS.
4. Define and implement the complete virtual GEOS-drive contract, including
   return registers/flags, turbo state, directory behavior, and a coherent
   writable-sector overlay. Alternatively, emulate the 1541 IEC path that
   GEOS expects. A permanent `$DD00` high-bit response is not a substitute.
5. Test the actual browser launch in CEmu at 100% with throttling enabled,
   using a diagnostic build made with `make -B CC_DEBUG="" LD_DEBUG=""`.
   Preserve the console trace and screenshot. Do not call the feature working
   until the DeskTop is visible and an application/file operation performs
   verified disk I/O.

## Current Safe Claim

EMUDORE can package a GEOS D64 as raw AppVar chunks and can perform limited
synthetic sector operations. It does **not** currently provide working GEOS
compatibility from the browser.