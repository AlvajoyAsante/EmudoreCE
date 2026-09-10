# Graphics and performance validation

## Current behavior

- All eight sprite X registers are independent, including sprite 5 at `$D00A`.
- Sprite-number priority is resolved before foreground priority. Original
  foreground occupancy remains available to every overlapping sprite's
  collision test, even after another sprite paints that pixel.
- Active and newly starting sprite lines consume their pointers and bitmap
  bytes at the end of each raster, before the next CPU slice can rewrite RAM. Previously
  only register writes forced catchup, so later RAM writes changed earlier
  rows of multiplexed sprites. Enabled sprites outside their active strips
  no longer force per-raster drawing; those spans can use bulk character rows.
- Visible emulation renders every frame by default, without warm-up sampling
  or a minimum skipped-frame gap. Hidden disk autoload is outside this policy.
- Hires and multicolor character modes have an optional assembly expander.
  It uses 1,536 bytes of color-dependent nibble tables, reads glyph/screen RAM
  live, and handles full cells in partial lines and eight-line batches.
  The C path handles ECM, clipped cell edges and noncontiguous output rows.
- Startup checks the character kernel against an independent pixel oracle.
  Allocation failure or an incorrect result leaves the C renderer selected.
- Sprite expansion uses fixed bit masks and forward pixel positions instead
  of variable shifts and per-pixel coordinate multiplication. Repeated-color
  runs (two pixels in multicolor mode, up to four with X expansion) are clipped
  once, then walked with framebuffer and coverage pointers. Unexpanded hires
  sprites keep the single-pixel path without run setup. Collision latches
  remain byte-sized updates; local OR accumulators caused this compiler to emit
  24-bit helper calls per pixel and are not used. Sprite row timing is unchanged.
- A 2,560-byte, eight-row composition buffer keeps background erase and sprite
  drawing off the visible LCD. Only completed rows are copied to VRAM, with
  foreground and collision tags intact. There is no full-frame allocation or
  pixel compression, and guest RAM stays in the existing second VRAM region.
- Composition does not read the previous LCD row back into scratch memory:
  each renderer completely replaces its row. This removes 64,000 bytes of
  VRAM reads and the corresponding scratch writes per full 320x200 repaint.
  Completed rows still require 64,000 bytes of VRAM writes. This is a memory
  traffic reduction, not a measured FPS improvement.
- Row composition reduces exposed intermediate painting, but it is not an
  atomic full-frame presentation. LCD scanout can still mix old and new rows,
  especially at low FPS. This work does not claim tear-free presentation.

Full-speed PAL, approximately 50 complete rendered frames per second, remains
an acceptance target, not a measured result. Native host test speed cannot
establish calculator speed. Increasing rendering frequency without reducing
execution cost can lower emulated throughput; compare both rates.

## ACROJet bitmap correction (2026-09-10)

ACROJet showed persistent scrambled cockpit graphics, initially reported as
sprite or VRAM corruption. The cause was the bitmap source stride in both
[hires](../src/vic_hires_row.s) and [multicolor](../src/vic_mc_row.s) assembly
renderers: they advanced one byte per horizontal cell, but the caller passes
direct C64 bitmap memory with eight bytes per cell. The correct fetch offsets
are 0, 8, 16, through 312, not consecutive bytes. Both kernels now advance
with `lea iy, iy + 8`, matching the existing C fallback.

The startup test in [vic.c](../src/vic.c) had also used 40 packed bytes, so it
incorrectly approved the same faulty layout. It now supplies a 320-byte
bitmap block, tests all eight scanline offsets, uses distinct screen and
color RAM values, and checks every tagged output pixel plus buffer guards.
Hires and multicolor kernels still fall back to C independently on failure.
Sprite timing, framebuffer allocation, ROMs, and game data were unchanged.

The [VIC regression harness](../tests/vic/README.md) passed all 50 tests.
New checks reject the old stride, verify independent startup-test failures,
cover 384 bitmap-addressing cases across all four VIC banks, and exercise
64 bitmap/sprite compositions with priority and collision checks. Release,
diagnostic, and profile builds succeeded; generated IX frame checks passed.
Host adapters validate the contract, not native eZ80 instruction execution.

On September 10, 2026, the user confirmed that the fix resolved most of the
ACROJet display corruption. Further testing showed a persistently corrupted
lower-right RIB CUT indicator, including with the saved C-bitmap reference
build. The broad bitmap-stride fix remains valid, but complete visual
acceptance is still open. This is user-observed evidence, not a local
automated CEmu run or a new FPS measurement.

### Remaining RIB CUT indicator

The same corruption with C bitmap decoding rules out an assembly-only
bitmap-expansion fault; it does not yet distinguish sprite rendering from
background data or guest execution. No second rendering fix is claimed.

The diagnostic build `bin/indicator-debug/EMUDORE.8xp` captures the active
VIC bank, sprite pointer bytes and colors, enabled sprites' 63-byte data
blocks, and tagged framebuffer samples over LCD X=224..319, Y=156..219.
The pointer snapshot uses the VIC read path, including character-ROM windows,
instead of the old hardcoded `$C3F8` diagnostic address. The pixel samples
describe the displayed image; register and data snapshots describe exit
state, so mid-frame changes must be interpreted with the VIC write trace.
All extra panel/data logging occurs after emulation stops, not per pixel.

Reproduce the panel corruption, exit with CLEAR, and capture CEmu's console
sections `[vic]`, `[spr]`, `[sptr]`, `[sprdata]`, `[panel]`, and `[wtrace]`.
Pixel tag `0x10` identifies a sprite-painted pixel; `0x20` preserves bitmap or
character foreground occupancy. A 64-configuration bank/screen regression
checks the new snapshot. The full VIC suite now has 51 passing tests.

## Observed CEmu runs

The user's September 9, 2026 screenshots show FINAL BLOW running after the
frame-pointer compiler fix, with frame skipping OFF and 32 recent intervals:

| Character renderer | Emulated/rendered FPS | Average | 95th percentile | Maximum |
| --- | --- | --- | --- | --- |
| C | 1.0 / 1.0 | 934 ms | 940 ms | 942 ms |
| ASM | 4.2 / 4.2 | 232 ms | 233 ms | 233 ms |

The ASM status confirms that its startup test passed in that run. The reported
average frame times differ by about fourfold; these are user-provided CEmu
measurements, not hardware results or a controlled CPU/render attribution.
The sprite raster-order and row-composition changes came afterward. A later
gameplay screenshot reports ASM at 1.2 emulated/rendered FPS, average 798 ms,
95th percentile 825 ms, and maximum 826 ms, with 32 recent intervals and frame
skipping OFF. Whole-run sampled time was 39% CPU and 55% rendering, including
loading. These shares are not a breakdown of the 798 ms recent-frame average.

The per-raster sprite fix prevented batching even before sprites started and
after they finished. A new regression reproduced eight one-row character
kernel calls instead of one eight-row call for these inactive spans. The
current build restores that batching while keeping active and newly starting
sprites synchronized at every raster. The correctness regressions still pass;
the new target FPS and visual result require another run. Active strips retain
per-raster work, so this is not a claim of recovering all of the earlier speed.

The next report, identified by the user as Punch Out during a fight, shows
ASM, frame skipping OFF, 0.9 emulated/rendered FPS, 32 recent intervals,
average 1,023 ms, 95th percentile 1,026 ms, and maximum 1,033 ms. Whole-run
sample shares are CPU 29% and rendering 65%, including loading. The precise
game build and comparison conditions are not established, so this is not a
controlled regression comparison with the earlier FINAL BLOW runs. Changing
firmware package compression affects startup, not the steady-state renderer.

The subsequent sprite-run change removes repeated coordinate and clipping
work within expanded pixels, while preserving priority, foreground tags,
collision latches, and per-raster memory ordering. A 10,240-case pixel oracle
covers all sprite bits/colors, priority settings, clipped positions and run
widths. Host equivalence and target code-generation checks do not establish
an FPS gain; the new release and profile builds need another comparable fight.

## Builds

Use the nightly CEdev compiler with `-O2` and `-fno-omit-frame-pointer`.
The Makefile always appends the frame-pointer flag, including when `CFLAGS`
is overridden. Without it, nightly 19.1.0 reuses IX for data while still
accessing stack locals through IX in `vic_char_selftest` and `c64_run`.
This corrupts memory at startup or when entering emulation; host C tests
cannot detect this target code-generation defect. Keep the release
optimization flags identical across comparisons, and force a rebuild with
`make -B` after changing compiler flags.

```sh
make
make PROFILE=1
make PROFILE=1 CHAR_ASM=0 OBJDIR=obj/profile-c BINDIR=bin/profile-c
```

Outputs are respectively `bin/EMUDORE.8xp`, `bin/profile/EMUDORE.8xp`, and
`bin/profile-c/EMUDORE.8xp`. They use separate object directories so reference
and optimized builds do not share stale compiler options. When changing other
flags in an existing configuration, use `make -B` with the same arguments to
force recompilation. Do not install both calculator programs simultaneously:
they intentionally have the same calculator program name.

`CHAR_ASM=0` retains the corrected C character renderer for a direct comparison.
`FRAME_SKIP=1` explicitly restores the legacy sampled-render policy for
diagnosis only; it does not satisfy the no-skip acceptance criterion. Keep
different comparison options in separate object/output directories.

`PROFILE=1` enables counters independently of `NDEBUG`. It does not turn on
the old per-line debug profiler or VIC write ring by default. For detailed
VIC/CPU boot traces, use a separate diagnostic build with `CC_DEBUG=""`
and `LD_DEBUG=""`; do not use that heavily instrumented run as the release
performance measurement.

## Capture a run

1. Record the compiler version, artifact SHA-256, calculator model, CEmu
   version/speed setting, and installed ROM/disk packages.
2. Load FINAL BLOW and enter an active fight using the same input sequence.
3. Run a repeatable 60-second scene at normal calculator speed, then use CLEAR
   to exit emulation. The profile build keeps a report on screen; ENTER closes it.
4. Capture that report and CEmu's console. Repeat three times on both CEmu
   and physical hardware, comparing with the reference C-renderer build.
5. Check complete boxer strips during movement and multiplexing, HUD/text,
   overlap behavior, inputs, and stability across the 24-bit CPU cycle wrap.

The report's rolling window contains up to the most recent 32 visible frame
intervals. Hidden frames clear the window; the first subsequent visible
boundary establishes the clock. It reports separate completed emulated and
rendered frame rates, average, nearest-rank 95th percentile and maximum time.
Full PAL requires roughly a 20 ms total frame budget and matching rendered
and emulated frame counts. The old executed-cycle percentage alone is not
sufficient because bad-line CPU budgets and idle warps affect that number.

Console counters:

- `[perf]`: recent frame metrics, whole-run frame totals, renderer calls,
  sprite-line count, frame-skip setting, and row self-test bits. `asm_rows`
  bits 0, 1 and 2 mean hires bitmap, MC bitmap and character kernel enabled.
  `fps10` values are tenths of a frame per second.
- `[perf-sample]`: scope 0 CPU, 1 VIC scheduling, 2 input, 3 CIA/drive.
  `exclusive_ticks` removes rendering nested inside that scope.
- `[perf-render]`: all flush calls and the measured subset, including
  rendering triggered by register writes rather than only frame-end flushes.
- `[perf-cpu]` and `[perf-pc]`: fallback/native-block engagement and sampled
  guest PC pages. Page counters saturate at 65,535.

Subsystem counters cover the whole run, including loading; the recent FPS
window does not. They are raw samples, not exact whole-run durations. One in
17 raster iterations is sampled, so its phase moves relative to PAL frames
and the input/CIA cadences. Any extrapolation from call counts is an estimate.
The on-screen CPU and render percentages divide their sampled exclusive ticks
by the sum of all scope ticks plus render ticks. They include loading, exclude
unmeasured overhead, and are not percentages of the recent frame window.
Do not add the old inclusive debug buckets to these exclusive sampled values.
Always compare a plain release run to assess measurement overhead.

Timer 1 stays at 32,768 Hz and is not reset by the profiler. Timer deltas wrap
at 32 bits. Never use `timer_Get` on a running CPU-clocked timer 2: that routine
waits for stable consecutive reads. No pixel or instruction logging is added.

## Automated checks

With a native GCC or Clang on PATH, separate from `ez80-clang`:

```sh
make -C tests/vic check CC=gcc
make -C tests/perf check CC=gcc
python -m unittest discover -s tests -p "test_*.py"
```

The VIC harness has 51 register, sprite, collision, scrolling, character,
bitmap and row-presentation tests. The profiler has 12 tests with 374 checks plus 18 disabled-build checks.
The converter suites have 43 tests. See `tests/vic/README.md` for the
installed Windows GCC path, strict compiler flags and individual test selection.

The host adapter validates C logic, lookup tables, row contracts and guards,
not eZ80 instructions or ABI behavior. Execute the boot self-test on a real
calculator or CEmu before accepting the assembly path. CEdev's installed
`cemu-autotester` additionally requires a test JSON and a suitable calculator
ROM/state; C64 BASIC/KERNAL dumps are not calculator ROMs. The measurements
above cover successive changes, not one controlled comparison; host validation
alone cannot establish their visual result or throughput on the calculator.

## Interpreting VRAM updates

The visible 320x240 framebuffer starts at `0xD40000`. C64 guest RAM uses the
second VRAM region starting at `0xD52C00`, with the lowest 4 KiB redirected to
ordinary RAM. Changes in that second region can be game loading or CPU data
writes, rather than pixels being rendered.

A slowly appearing screen does not by itself establish a VRAM bandwidth
bottleneck: guest CPU execution and row generation happen between completed
row transfers. The profile's Render share includes composition and copying;
it does not separately measure copy bandwidth. Compare new frame times before
attributing the slow fill to the transfer itself. The current memory layout
does not have a free second full-size framebuffer for a simple page swap.