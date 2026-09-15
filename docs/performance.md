# Graphics and performance validation

This document describes the active CE build, its rendering model, and the validation workflow used to assess emulator throughput, compatibility, and regressions. It is meant to reflect the current project status rather than a historical timeline of earlier experiments.

The current release is the TI-84+ CE emulator with the active JIT, browser, ROM/disk packaging, and startup-memory constraints described elsewhere in the project. The measurements here should be interpreted as compatibility and performance evidence for the current build, not as a generic statement about all possible C64 workloads or older branches.

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
- The emulator starts at Turbo 1x with frame skipping Adaptive. Adaptive frame
  skipping uses a 32,768 Hz timer and a 50 Hz deadline, skipping at most two
  consecutive display frames. CPU/VIC timing, IRQs and sprite state still
  advance on skipped frames, including rendering needed for collisions.
- **Optimization -> Turbo CPU/RAM** selects the original 1x, 50x, 100x,
  200x or Adaptive processing. The multiplier applies to CPU budgets for
  normal and bad raster lines while the VIC advances one line. Adaptive
  adjusts the multiplier toward six completed frames per calculator second,
  bounded to 1..200. Fast-forward bypasses the real-time frame wait and changes
  CPU/device timing ratios; use 1x for normal compatibility. These rates are
  not measured gameplay speedups or calculator clock multipliers. The host
  shortcut toggles 1x/200x. Frame skipping stays independently selectable.
- Compact mode has been removed: presentation is 320x200 with full-width row
  composition and eight-row character batching. The JIT changes are retained.
- CPU interpretation uses the assembly core without a C interpreter fallback.
  Its startup self-test executes `INC $01` and `DEC $01` and checks registers,
  flags, cycles and the resulting ROM/RAM page mappings. C device helpers
  still use the CEdev ABI; native callers must preserve every live register
  that those helpers may clobber.
- For CPU profiling, use the compact debug build rather
  than `PROFILE=1`. Its `[cpu-hot]` exit line samples the dominant PCs in RAM
  pages `$01` and `$08` with 12 bytes of state; the full profile binary may
  exceed available calculator program memory.
- If debug builds do not fit, build `make -B CPU_HOT_PROFILE=1
  OBJDIR=obj/cpu-hot BINDIR=bin/cpu-hot`. This release-sized diagnostic pauses
  after CLEAR and displays the sampled `$01xx` and `$08xx` PCs on screen; send
  their decimal values for the next block-translation step.
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

## Native CPU regression checks

After rebuilding the release, the VS Code task **Test current native CPU**
runs the linked eZ80 code in CEmu against an independent 6502 reference.
It checks 244 opcode encodings in 15,616 register, flag, memory and wrap
scenarios, 12 explicit JAM stops, and 384 read-modify-write cases on `$0001`.
The latter compare both RAM and the banking tables, including the dummy write.
XAA/LAX use the project's deterministic convention; unstable undocumented
instructions are not a claim of transistor-level NMOS accuracy.

The banking cases reproduce a Parallax regression missed by flat-RAM opcode
tests: `write_de` did not preserve DE across `mem_setup_banks`. An RMW's first
write clobbered the address of its second write. `INC $01` then left BASIC ROM
mapped over the game's depacker source. The release startup self-test now
also guards this failure without running a C interpreter.

The Parallax harness checks successful LOAD copies byte-for-byte at the
device-trap return and rejects bounded runs that never reach RAM code after
loading. Game runs retain production acceleration and the mandatory 135%
CEmu cap. Use a fresh `EXEEXT` when rebuilding a Windows harness.
Changing/nonblank frames alone do not establish gameplay: inspect the actual
intro, trainer, second load, gameplay screenshots and input responses.

The pinned reference patch also corrects NMOS indirect JMP page wrapping and
AHX `$93` addressing (indirect,Y, not zero-page,Y). Run `verify-deps` in
`tests/jit` to check the pinned revisions and recorded patch.

## JIT dispatch and re-entry

### Opcode Coverage

The emitter includes all 151 official and 93 undocumented non-JAM encodings.
The final additions are XAA, LAS, AHX (two addressing modes), TAS, SHY and SHX.
The remaining 12 encodings intentionally stop the CPU; they are not missing
translations. Undocumented instructions follow the project's deterministic
CPU conventions. Cycle budgets, mappings and self-modification can still force
interpreter fallback; opcode coverage does not mean every instruction executes
through the JIT, nor does it establish a game-speed improvement.

After building the release, run **Test JIT extensions** or
`make -C tests/jit target-extensions-check` for the 244-encoding matrix and
3,584 focused load/masked-store cases. This retains JIT/CPU initialization
self-tests but skips the full startup, benchmark and game regression suites.
On Windows, use the configured task for the native compiler and DLL path.

### Cache and Dispatch

The ready-block cache now has a 256-entry index keyed by the low PC byte.
Exact-PC hits avoid the 16-slot scan; collisions retain the scan and empty
index entries reject immediately. Eviction, invalidation, blacklisting and
reset maintain the index. It costs 768 bytes of target RAM plus a one-byte
re-entry flag. Visible instruction-byte checks, mapping checks, native guards,
decimal policy and cycle-budget admission remain active.

After an initial cache miss, the assembly interpreter can enter cached code
at taken branches, absolute JMP, JSR and RTS targets in the same CPU batch.
Previously the rest of that batch stayed in the interpreter. Direct
`cpu_run_asm` calls remain interpreter-only. Cold-PC observation is deferred
until the batch finishes and skipped when that call already used native code;
this avoids expensive observation/copy work, but can change cache discovery.
This is guarded re-entry, not direct native branch linking or a new emitter.

### Matched measurements

The preserved release and candidate used the same standalone target harness,
release flags and seeded workloads. Times below are modeled eZ80 clocks,
not host execution time. Warm cases exclude compilation and run 32 batches;
the cold-entry case runs one 63-guest-cycle batch with only its destination hot.

| Workload | Before | After | Change in CPU time |
| --- | ---: | ---: | ---: |
| Warm arithmetic (63 guest cycles/batch) | 188064 | 192384 | +2.3% |
| Warm direct RAM reads (75) | 221824 | 226144 | +1.9% |
| Warm indexed RAM copy (63) | 395584 | 360896 | -8.8% |
| Warm RAM increment (64) | 279072 | 283392 | +1.5% |
| Cold entry into cached arithmetic | 73075 | 12113 | -83.4% |

The cold-entry case executes two native blocks after the change instead of
zero. Direct interpretation of that case costs 14162 clocks, up from 13898:
the new control-transfer hooks have a cost even when re-entry is disabled.
These are workload-specific tradeoffs, not a universal speedup.

The matched Parallax test used local disk entry 2, no input or checkpoint,
JIT On, Turbo 1x, frame skip Off and 300 completed frames. The harness executes
the linked program in CEmu with TI-library hooks, not the TI-OS GUI. Both runs
loaded the same PRG and reached the animated intro. All 300 framebuffer hashes,
color masks, load counts and input records match; both have 297 nonblank frames
and 209 frame changes. Gameplay and whole-game completion remain unconfirmed.

| Full startup/intro run | Before | After |
| --- | ---: | ---: |
| Modeled calculator seconds | 187.668550 | 167.537629 |
| Host wall seconds | 142.957398 | 127.965957 |
| Requested CEmu cap | 135% | 135% |
| Achieved cap | 131.276% | 130.924% |

This sequence uses 10.7% less modeled calculator time (12.0% higher throughput),
with unchanged resolution and presentation settings. These totals include
startup and harness overhead; they are not isolated gameplay FPS or evidence
of full-speed PAL or TI-Boy parity. Logs and all frame records are retained as
`tests/jit/build/parallax-reentry-{before,after}.{log,csv}`. The original linked
image is preserved at `bin/jit-reentry/EMUDORE-before.obj`.

### Re-entry checks

- Cache/index lifecycle: 9 host tests, 26658 checks, including collisions,
  eviction, invalidation, blacklist and reset.
- Linked JIT integration: 26942 cases, including 520 JMP/BNE/JSR/RTS re-entry
  batches with budgets 0..64, decimal/carry flags, split RAM and 24-bit wrap.
- Native CPU regression: 16012 cases, including 384 RMW banking cases.
- Shipped ZX7 decompression, ELF parity and CE-library startup memory pass;
  the candidate leaves 39415 bytes spare under the existing memory model.

After **Build EMUDORE release**, the **Test JIT re-entry** VS Code task runs
host, compressed-startup/full-JIT and native CPU checks. Focused commands:

```sh
make -C tests/jit target-reentry-check
make -C tests/jit target-benchmark
make -C tests/jit check startup-check cpu-check
make -C tests/jit -f game.mk game-check FRAMES=300 FILE_INDEX=2 JIT=on INPUT=none RESUME=none
```

Supply native GCC `CC`/`AR`, its DLL directory on `PATH`, and a fresh `EXEEXT`
on Windows. The new re-entry assertion intentionally fails on the preserved
old image after printing its warm baseline results. The **Compare Parallax
JIT re-entry** task uses the preserved baseline and isolated candidate images.

## Earlier indexed-access tuning

The measurements in this section predate the dispatch/re-entry change and
subsequent assembly-core migration; they are not the current release baseline.

The bounded 6510-to-eZ80 compiler now folds the RAM backing pointer and
absolute operand into one constant for proven flat-RAM indexed accesses.
Adjacent eligible accesses reuse the zero-extended X/Y index in DE and,
for load/store copies, the loaded byte in A. An intervening instruction,
changed index, mapped access or page-crossing read prevents this reuse.
The existing safe RAM cutoffs, exact instruction-byte validation, cycle
budgets and self-modification guards are unchanged.

The dispatcher now distinguishes a matching block rejected by its cycle
budget or decimal-mode policy from an unresolved cache miss. This avoids
redundant cache scans at short batch tails. Already-executed repeating blocks
retain their inexpensive progress return.

The new copy-budget sweep also exposed a pre-existing assembly fallback
timing error: absolute-indexed loads omitted the page-crossing cycle. Their
address helper now adds the index byte with explicit carry handling, charges
the extra cycle and preserves 16-bit address wrap. This also reduces address
calculation work in the interpreter.

### Measured warm CPU cost

These are actual eZ80 clocks in CEmu, using the CEdev-linked release, not host
execution time. Each workload runs 32 times with identical seeded guest state
and checks registers, flags, cycles and memory against an independent 6502.
Compilation is excluded by warming the cache first.

| Workload | Guest cycles per batch | Previous JIT | Tuned JIT | Current interpreter | Interpreter / JIT |
| --- | ---: | ---: | ---: | ---: | ---: |
| Arithmetic | 63 | 190048 | 190048 | 433312 | 2.280 |
| Direct RAM reads | 75 | 223808 | 223808 | 428416 | 1.914 |
| Indexed RAM copy | 63 | 468928 | 395072 | 419584 | 1.062 |
| RAM increment | 64 | 281056 | 281056 | 290494 | 1.034 |

Indexed copies use 15.8% less JIT CPU time than before tuning, and run 6.2%
faster than the current interpreter in this benchmark. The old interpreter
used 423904 clocks for that workload; the fallback helper improvement also
benefits interpreter-only execution. The other three workloads are unchanged.
These results do not establish whole-game FPS, cold-code performance or a
universal speedup.

### Regression coverage

- 420356 emitted-code executions pass, including exhaustive binary ADC/SBC,
  indexed copies, both index registers, invalidation and output-capacity edges.
- All 262144 host NMOS decimal combinations pass, including invalid BCD digits.
- The cache suite passes 8 tests and 282 checks.
- The linked-target copy sweep passes 10240 batches through both assembly and
  JIT paths: budgets 1..64, X/Y extremes, overlapping buffers, page crossings,
  split RAM and 24-bit cycle wrap. It uses the actual unaligned low-RAM backing.
- The warm benchmark passes 256 state/memory comparisons per image.

Run the focused checks with a native GCC toolchain, separately from CEdev:

```sh
make -C tests/jit target-copy-check
make -C tests/jit target-benchmark
make -C tests/jit check decimal-check native-exhaustive
make -C tests/jit startup-check startup-check-hot
```

Build the release and CPU-hot images before the linked-target checks. On
Windows, provide `CC`/`AR` for native GCC and keep its runtime DLL directory on
`PATH`. Use a fresh `EXEEXT` when another copy of a test executable may still
be loaded. The startup-only CPU self-test has a Clang `minsize` attribute to
keep the profiling image within the existing 128 KiB expanded-image limit;
hot CPU/renderer code retains its settings and `-fno-omit-frame-pointer`.

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

### RIB CUT map-loading correction

The follow-up trace identifies bitmap data, not sprite-painted pixels, in
the remaining streaks. The [captured excerpt](../tests/vic/fixtures/acrojet-indicator.log)
has no `0x10` sprite tags. All 80 interior pixels at LCD Y=188 exactly match
ten `$20` bytes decoded with the game's screen/color values `$45`/`$01`:
black, green, black, black doubled pairs. This explains why the same streaks
also appeared with the C-bitmap reference build.

ACROJet's `MODULE3` passes the 12-byte name `0:EVENTA.MAP` at `$4EEE` to
KERNAL LOAD at `$7532`; the event letter at `$4EF5` is selected at runtime.
The disk directory stores `EVENTA.MAP` without the Commodore drive selector.
The shared lookup in [drive.c](../src/drive.c) previously compared the `0:`
literally and returned FILE NOT FOUND. The game does not check that failure
before copying 560 bytes from `$0400` into seven 80-byte bitmap strips at
`$F628`, `$F768`, `$F8A8`, `$F9E8`, `$FB28`, `$FC68`, and `$FDA8`. Stale
screen bytes in the source therefore become the panel's vertical strokes.

Lookup now removes a leading `0:` before matching, for both KERNAL and
resident-fastloader requests. Plain names, wildcards, and the selected-file
autoload behavior are preserved. No renderer, CPU core, disk image, or
AppVar contents were changed for this correction.

The [drive regression harness](../tests/drive/README.md) first reproduced
FILE NOT FOUND and an unchanged `$0400` buffer with the exact map request.
After the fix, all nine tests pass, including the full 560-byte copy, RAM
guards, CPU return state, plain/prefixed wildcards, 16-character names,
failed requests, both load traps, and tracing after eight bootstrap loads.
Calculator/CEmu visual confirmation of the corrected indicator is still
pending; these host checks do not establish target execution or FPS.

The diagnostic build [bin/indicator-debug/EMUDORE.8xp](../bin/indicator-debug/EMUDORE.8xp) captures the active
VIC bank, sprite pointer bytes and colors, enabled sprites' 63-byte data
blocks, and tagged framebuffer samples over LCD X=224..319, Y=156..219.
The pointer snapshot uses the VIC read path, including character-ROM windows,
instead of the old hardcoded `$C3F8` diagnostic address. The pixel samples
describe the displayed image; register and data snapshots describe exit
state, so mid-frame changes must be interpreted with the VIC write trace.
All extra panel/data logging occurs after emulation stops, not per pixel.

For target verification, restart ACROJet with the rebuilt emulator. A
previously running guest still has its failed-load data. If corruption
persists, exit with CLEAR and capture CEmu's console sections `[drive]`,
`[vic]`, `[spr]`, `[sptr]`, `[sprdata]`, `[panel]`, and `[wtrace]`.
The drive trace now keeps the first 16 requests instead of eight, so the
map loads appear after the bootstrap. A successful 560-byte map load shows
`addr=0400 end=0630` and a real directory entry rather than `entry=255`.
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
`FRAME_SKIP=0` builds an every-frame rendering reference for comparisons.
Default `FRAME_SKIP=1` pacing is the gameplay build; it can advance emulation
when painting is the limiting cost, at the intentional tradeoff of a lower
displayed-frame rate. Keep different comparison options in separate
object/output directories.

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
make -C tests/drive check CC=gcc
make -C tests/vic check CC=gcc
make -C tests/perf check CC=gcc
python -m unittest discover -s tests -p "test_*.py"
```

The drive harness has nine filename-lookup and LOAD-trap tests.
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

### Live RAM view (temporary)

`GRAPH -> Optimization -> VRAM view` is a live diagnostic mode. It keeps the
guest running, suppresses normal VIC presentation, and copies the C64 RAM map
into the normal centered 320x200 display area at up to 48 Hz. GRAPH returns
to ordinary VIC rendering. This avoids writing UI into C64 RAM and avoids the
unreliable graphx page swap attempted earlier.

The image is a raw indexed-byte map, not a second decoded C64 display, so it
is expected to appear as colored data rather than game artwork. It is retained
as a temporary inspection tool. Planned work is a more useful VRAM/RAM
inspector that identifies meaningful C64 screen, bitmap, charset, and color
RAM regions without continuously copying the whole 64 KiB map.