# Native VIC regression harness

This is a unit harness for the live production VIC implementation. The test
translation unit includes `../../src/vic.c` so tests can drive the static
sprite counters and row renderers directly. The build also compiles the real
`../../src/vic_char.c` as a separate translation unit. Its color-table
generator and self-test are not duplicated in the harness. `char_stub.c`
supplies only the assembly-owned aligned tables and table-copy entry point.
The host uses the real public headers and provides the required external
definitions. It does not compile `cpu.c`, `io.c`, `memory.c`, or target
assembly. The target CPU core's packed 24-bit ABI is outside this harness.

## Build and run

From the repository root with a native GCC or Clang on PATH:

```sh
make -C tests/vic check CC=gcc
make -C tests/vic list CC=gcc
make -C tests/vic check CC=gcc TESTS="register-roundtrip snapshot"
make -C tests/vic check CC=gcc TESTS="priority-behind-foreground collision-all-foreground"
make -C tests/vic check CC=gcc TESTS="char-selftest char-tables char-color-invalidation char-mixed char-row-span char-cell-span char-live-glyphs char-invalid-counts"
make -C tests/vic check CC=gcc TESTS="char-scroll-raster-c char-scroll-batch-c"
make -C tests/vic check CC=gcc TESTS="bitmap-selftest-reject-packed bitmap-selftest-layout bitmap-selftest-faults"
make -C tests/vic check CC=gcc TESTS="bitmap-raster-addressing bitmap-sprite-composition"
```

An existing Windows GCC was found at `C:\msys64\ucrt64\bin\gcc.exe`.
Its bin directory must also be on PATH for compiler subprocess DLLs, even
when CC names the executable by its absolute path. In PowerShell:

```powershell
$env:Path = 'C:\msys64\ucrt64\bin;' + $env:Path
& 'C:\Users\alvaj\CEdev\bin\make.exe' -C tests/vic check CC=C:/msys64/ucrt64/bin/gcc.exe

$env:Path = 'C:\msys64\ucrt64\bin;' + $env:Path
& 'C:\Users\alvaj\CEdev\bin\make.exe' -C tests/vic check CC=C:/msys64/ucrt64/bin/gcc.exe "TESTS=char-selftest char-tables char-color-invalidation char-mixed char-row-span char-cell-span char-live-glyphs char-invalid-counts"

$env:Path = 'C:\msys64\ucrt64\bin;' + $env:Path
& 'C:\Users\alvaj\CEdev\bin\make.exe' -C tests/vic check CC=C:/msys64/ucrt64/bin/gcc.exe "TESTS=char-scroll-raster-c char-scroll-batch-c"
```

The same build without make, from the repository root:

```sh
gcc -std=c11 -O1 -g -Wall -Wextra -DNDEBUG -DVIC_TEST_CHAR_INTEGRATION=1 -Itests/vic/include tests/vic/vic_test.c tests/vic/char_stub.c src/vic_char.c -o tests/vic/vic_tests.exe
tests/vic/vic_tests.exe --list
tests/vic/vic_tests.exe register-roundtrip snapshot
```

Both examples include the VIC character integration tests. All builds here
are host-only and write artifacts under `tests/vic`. Do not run the root
calculator build concurrently with its owner.

On a host with sanitizer runtimes installed, an additional check is:

```sh
make -C tests/vic -B check CC=clang CFLAGS="-std=c11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer"
```

`check` runs every test unless TESTS is set to exact space-separated names.
Every selected test runs even when another test fails. The executable exits
0 when all selected tests pass, 1 for assertion failures, and 2 for an unknown
test name. Known production regressions remain real failures; there are no
expected-failure exemptions. Output files stay in this directory.

## Coverage

- `register-roundtrip`, `snapshot`: independent X/Y storage for all eight
  sprites, sprite 5's `$D00A` and `$D00B`, all high-X bits, and every debug
  snapshot field, including decoded memory bases and raster IRQ target.
- `sprite-debug-snapshot`: all 64 combinations of four VIC banks and sixteen
  screen bases, including sprite pointer tables in character-ROM windows.
  Verifies the actual VIC-visible pointer bytes and masked sprite colors,
  with different underlying RAM and ROM data so CPU-only reads fail.
- `bitmap-selftest-reject-packed`: calls the production bitmap startup test
  with adapters that reproduce the old incorrect one-byte source stride.
  Both kernels must be rejected. This regression failed before the startup
  fixture was changed from packed bytes to the real C64 bitmap layout.
- `bitmap-selftest-layout`, `bitmap-selftest-faults`: all nine combinations
  of zero-fill, one-byte, and eight-byte adapters, with independent readiness
  bits. Only the eight-byte layout passes. Both kernels visit all eight
  scanline offsets with 40 cells and distinct screen/color RAM. Prefix and
  suffix guard damage, a wrong final pixel, lost foreground tags, and using
  the screen nibble in place of multicolor color RAM must be rejected.
- `bitmap-raster-addressing`: 384 cases covering hires and multicolor bitmap
  rendering, C fallback and portable native-call adapters, all four VIC
  banks, top/middle/bottom character rows, and all eight scanline offsets.
  Checks every output pixel, actual source and composition pointers, kernel
  engagement, 40-cell spans, and untouched neighboring rows and guards.
- `bitmap-sprite-composition`: 64 cases covering both bitmap modes, both
  rendering paths, sprites in front of or behind foreground, and all eight
  scanline offsets. The display stays unchanged until composition commits;
  foreground tags and sprite/background collision latches remain correct.
- `hires-position`, `hires-high-x`, `hires-x-expand`, `mc-position`,
  `mc-high-x`, `mc-x-expand`: exact tagged pixels across all three sprite
  bytes, hires bit order, MC color selection, two-pixel MC pairs, and
  four-pixel expanded MC pairs.
- `guarded-clipping`, `sprite-border-rows`: left/right crop, maximum X=511
  including expanded output through framebuffer X=577, both 24/25-row
  borders, and off-LCD rows. The existing 579-byte sprite mask fits X=577;
  the tests do not assert a false mask overflow. Each LCD row has separate
  32-byte guards on both sides, checked after every test and clipping case.
- `sprite-21-rows`, `sprite-y-rearm`, `sprite-undrawn-state`,
  `sprite-y-expand`: complete 21-row strips, Y rewrite at raster 110 while
  the strip started at 97, contiguous rearm at 118, counters without pixel
  drawing, and 42-line Y expansion. The rearm test uses real raster advances
  and register-write catchup, not just direct counter calls.
- `sprite-pointer-raster-order`, `sprite-data-raster-order`: a direct RAM
  rewrite between CPU raster slices changes the following sprite row without
  changing the already-emulated row, even with no VIC register write.
- `sprite-inactive-batching`: an enabled sprite outside its active strip
  permits one eight-row character kernel call instead of eight one-row calls.
  The first active sprite row is still drawn on time, and batching resumes
  after the 21-row strip ends.
- `composed-sprite-presentation`, `composed-character-presentation`: the LCD
  retains its previous pixels during background and sprite drawing, and only
  receives the finished composition. Eight-row composition preserves the ASM
  character path even when the real output rows have noncontiguous guards.
  Scratch rows start with invalid palette/tag bytes; setup leaves them intact
  without LCD readback, and rendering must replace the entire output row.
- `transparent-color-zero`, `foreground-color-zero`,
  `foreground-mc-priority`: zero data is transparent but palette color zero
  is opaque; black foreground retains priority. Real C character rendering
  supplies the foreground tags, including MC background pairs 00/01 versus
  foreground pairs 10/11.
- `priority-lowest-number`, `priority-transparent-winner`,
  `priority-behind-foreground`, `foreground-tag-preserved`: all eight sprite
  numbers, transparent holes, the hidden lowest-numbered opaque winner, and
  retention of the original background-occupancy tag after sprite writes.
- `sprite-pixel-runs`: 10,240 comparisons against independent per-pixel logic
  for all eight sprite bits, all 16 colors, foreground priority on/off, ten
  clipped or visible positions, and widths 0/1/2/4. Full output and coverage
  arrays are compared, including untouched pixels and previously set latches.
- `collision-all-foreground`, `collision-clear-on-read`,
  `collision-read-catchup`: every overlapping sprite collides with original
  foreground, sprite/sprite masks include both participants, independent
  clear-on-read latches, ignored writes, relatching, and raster-boundary timing.
- `pal-raster-timing`, `pal-every-frame`: 312-line raster wrap, no advance
  before a CPU deadline, and an independently colored full 320x200 image
  every visible frame. The latter intentionally rejects warm-up sampling
  without using render requests or advancing the wall clock to force it.
- `char-selftest`: calls the production `vic_char_selftest()` through the
  portable adapter and verifies restoration of all public inputs and the
  previously selected color tables.
- `char-tables`, `char-color-invalidation`: check every pattern nibble and
  every cell color, all 16 values of each background component, masking of
  upper color bits, repeated palettes, independent background changes, and
  return to an earlier palette. MC pairs 00/01 select untagged bg0/bg1;
  10 selects bg2 with `0x20`; 11 selects `(cell & 7) | 0x20`.
- `char-mixed`, `char-row-span`, `char-cell-span`: compare adapter output
  against an independent per-pixel oracle, with mixed hires/MC color RAM,
  multicolor values 1 and 255, every row count 1-8, nonzero glyph-row starts,
  glyph code 255, column 312, every cell count 1-40, and a 320-byte stride.
  The whole guarded output is compared, including every untouched prefix,
  row gap, unused row, and suffix. Screen, color, glyph, and table bytes and
  all pointer/count globals must remain unchanged.
- `char-live-glyphs`, `char-invalid-counts`: mutate glyph bytes, screen codes,
  and color RAM between expansions without resetting the color cache;
  invalid counts are no-ops with both guarded and null pointers.
- `char-scroll-raster-c`, `char-scroll-batch-c`: production C rendering in
  hires, mixed MC, and ECM modes, every horizontal scroll 0-7, a nonzero
  glyph row in the raster path, and all eight rows in the batch path. Each
  expected row includes 39 full cells plus the remaining 1-7 pixels of the
  final cell when scrolled. Entirely dropping the final cell is a failure.

The geometry assertions use IO_CROP_X=43, IO_CROP_Y=22 and framebuffer
Y=raster-9. Sprite X=24/Y=50 starts at LCD X=0/Y=20. Border comparisons in
the production sprite renderer receive framebuffer Y, not LCD Y.

## Host boundaries and integration

The stubs provide deterministic timer ticks, `CpuState cpu`, `cpu_warp`, an
IRQ counter, a selected CIA2 VIC bank, split low/high RAM, dedicated color
RAM, the VIC character-ROM windows, and cropped tagged framebuffer rows.
The border stub matches the production black fill. Refresh counts frame
publication calls; it does not imply a frame was actually repainted.

The two `vic_expand_*_row` adapters default to zero-fill, with bitmap
acceleration disabled in each fixture. The bitmap tests explicitly select
one-byte or eight-byte source strides and record calls and input pointers.
Fault injection writes within the production self-test's guard allocation
or corrupts selected pixels/colors. These adapters validate the production
self-test and renderer contract, not eZ80 instructions, pointer width, or
register preservation. Native wall-clock speed is not evidence of 50 FPS
on the CE.

`char_stub.c` defines 256-byte-aligned `vic_char_hires_lut[16][16][4]` and
`vic_char_mc_lut[8][16][4]`. It reads live glyph bytes and copies the selected
high/low nibble table entries to the destination at stride 320. It performs
no table generation, input advancement, or global mutation. The real C
generator populates the tables. This validates the portable adapter contract
and production C logic, NOT eZ80 instructions, register preservation, target
pointer arithmetic, CPU ABI, or performance. Guard comparisons detect writes;
they are not a substitute for a sanitizer when detecting out-of-bounds reads.

## Native bitmap validation

The native bitmap kernels receive direct C64 memory, not packed scanline
bytes. Successive horizontal cells are at offsets 0, 8, 16, through 312.
The production startup test now supplies all eight scanline offsets from a
320-byte bitmap block and checks 320 tagged output pixels plus guards for
each kernel. A failed kernel independently falls back to C.

Run the strengthened startup test on CEmu or a calculator before accepting
the native path; the diagnostic boot line should report `hires=OK mc=OK`.
Then compare several completed ACROJet cockpit frames and sprite overlays
against the reference image. Host test success is not visual acceptance.
`CHAR_ASM=0` disables only character acceleration, not bitmap acceleration.

## Character renderer integration

The makefile enables `VIC_TEST_CHAR_INTEGRATION=1` explicitly, including on
older GNU make versions without the read-only `file` function:

- `char-kernel-raster`, `char-kernel-batch`: explicitly enable
  `char_asm_ready`, route eight LCD rows to a local contiguous guarded buffer,
  assert their 320-byte spacing, and count actual kernel calls from the VIC
  source. Check the destination, screen/color/glyph pointers, row/cell counts,
  and mixed-mode flag at entry, then compare the complete guarded result.
  All scroll offsets are checked against the same pixel oracle as the C
  regressions, including the clipped last cell. ECM must use C instead.
- `char-kernel-row-fallback`: keep the original separately guarded rows
  (384-byte stride), enable the kernel, and require the eight-row renderer
  to fall back to C without calling the adapter. Compare every output row,
  including scrolling, and retain every original row guard.

The original fixture remains row-guarded. Contiguous rows are used only
inside the explicit integration checks, with outer guards and full checks
of untouched pixels and neighboring rows. Other tests explicitly disable
character acceleration when the integration is available.

To run only the integration tests:

```powershell
$env:Path = 'C:\msys64\ucrt64\bin;' + $env:Path
& 'C:\Users\alvaj\CEdev\bin\make.exe' -C tests/vic -B check CC=C:/msys64/ucrt64/bin/gcc.exe CHAR_INTEGRATION=1 "TESTS=char-kernel-raster char-kernel-batch char-kernel-row-fallback"
```

`EXTRA_SOURCES` defaults to `char_stub.c ../../src/vic_char.c`; paths are
relative to this directory. Do not compile the VIC source a second time,
replace the real table generator with a test copy, or add `cpu.c`.

The scroll regressions reject the old whole-final-cell skip. Both the
production self-test and independent `char-row-span` check use a wide column
offset and genuinely check column 312. The harness does not weaken expected
results or suppress compiler warnings.