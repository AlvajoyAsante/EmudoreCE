# JIT Performance Handoff

## Assignment

Improve emudore's actual execution speed on the TI-84+ CE. Work through phases
1 through 4 in order. This is an implementation handoff, not permission to
rewrite the entire emulator. Complete one measured, tested change at a time.
If evidence does not justify a proposed optimization, report that and choose
the next evidence-supported candidate. Do not claim all four phases succeeded
merely because their code compiles.

## Current State and Constraints

- Workspace: `G:/Other computers/My Laptop/TI Projects/2026/emudore`.
- The user reports unplayable speed at 135% CEmu throttle. Neither compact
  presentation nor Turbo changes produced a perceptible improvement.
- Compact mode was explicitly removed. Preserve full 320x200 presentation,
  automatic frame skipping enabled by default, and the restored original
  1x/50x/100x/200x/Adaptive Turbo settings. Use 1x for performance comparisons.
- Preserve all existing JIT changes. There are translations for 244 non-JAM
  opcode encodings. The remaining 12 are CPU-stop encodings, not missing work.
- General JIT re-entry and the 256-entry low-PC-byte lookup index already
  exist. Do not implement them again. Interpreter fallback is assembly, not C.
- The production cache has 16 slots with 512 native bytes each. The emitter's
  header also defines a 1024-byte capacity; that is NOT the production slot
  size. Guest blocks are capped at 48 bytes and 16 instructions, threshold 8.
- Accumulator reuse, selected indexed-address reuse and deferred N/Z flags
  already exist. Read them before proposing a replacement.
- The worktree contains substantial prior changes. Never use a blanket git
  revert/reset/checkout, delete old checkpoints, create a branch, or commit
  unless the user explicitly asks. Use apply_patch for manual source edits.
- Keep `-O2 -fno-omit-frame-pointer`. CEdev can otherwise reuse a live IX frame.
  Do not relax the 128 KiB initialized-image assertion in
  [tools/program_memory.ld](../tools/program_memory.ld).
- eZ80 ADL pointers/register pairs are 24-bit. Initialize upper bytes before
  using partly constructed registers as pointers or offsets. `MLT HL`
  multiplies H by L, not the full register by L.
- CEdev C preserves IX but may clobber IY. Preserve every live value across
  helpers; do not invent an IX-clobber bug based on another compiler's ABI.
- Preserve bank-port $0001 behavior, RMW dummy/final writes, decimal arithmetic,
  interrupt timing, cycle wrap, self-modifying code and mapped I/O side effects.
- Do not introduce additional agents unless authorized. Parallel research is
  possible; edits to the emitter, bridges and dispatcher must be serialized.

## Phase 1 Result (2026-09-13)

Hypothesis: the existing cache counters can distinguish poor block discovery
from expensive native execution without adding calculator release hot-path
instrumentation. The harness previously recorded only native hits, so it now
records the exported observation, cached, ready, ineligible, and mismatch
counters once per completed frame.

The measured workload is the linked-CEmu Parallax disk entry 2 startup/intro:
180 completed frames, no input, no resume, JIT on/off, the normal full 320x200
presentation, runtime Turbo 1x, and the program's default adaptive frame skip.
It is not gameplay proof. Both runs use the throttle wrapper at its requested
135% cap; the wrapper reported 125.053% for JIT-on and 120.987% for JIT-off,
so neither result is a cap-saturated real-speed claim.

| Mode | Guest cycles | Native hits | Observations | Cached / ready / ineligible | Mismatches | Modeled seconds | Wall seconds |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| JIT on | 3,358,080 | 215,713 | 7,571 | 16 / 7 / 0 | 0 | 51.238785 | 40.973684 |
| JIT off | 3,358,083 | 0 | 0 | 0 / 0 / 0 | 0 | 35.616841 | 29.438478 |

Frame-by-frame framebuffer and guest-RAM hashes match in both modes through
frame 180; each loaded one PRG, yielded 177 nonblank frames, and had 32 frame
changes. The 16-slot cache is full and no observed block was rejected or
invalidated, which rejects a discovery/admission/eviction-first change for
this slice. With matching guest work, JIT-on instead took 43.9% more modeled
calculator time. Phase 2 should therefore inspect native bytes for the hot
ready blocks and measure one within-block emitter improvement before considering
memory specialization or block linking.

Raw frame data: `tests/jit/build/jit-phase1-cache-baseline-20260913.csv` and
`tests/jit/build/jit-phase1-cache-off-20260913.csv`. The exact baseline release
was preserved before candidate work as
`bin/jit-phase1-baseline-20260913/EMUDORE-phase1-baseline.obj` (112,772 bytes,
SHA256 `4BF96960D55076D12FF65B42F3213C27B2DB48537C168739C3C92FEC0F3C98B1`) and
`bin/jit-phase1-baseline-20260913/EMUDORE-phase1-baseline.8xp` (49,502 bytes,
SHA256 `938CAAF74DF29AB5308D6EDD96CF047C026554A6FC8D45E7371F075A7C70F4F8`).

## Phase 2 Investigation (2026-09-13)

The same JIT-on run dumped every ready target block. Seven resident blocks in
the terminal KERNAL loop ranged from 63 to 193 native bytes for 2 to 8 guest
bytes. In particular, `$104E: CMP #$F2; BNE $104B` emitted 99 bytes, and
`$104B: LDA $D012; CMP #$F2; BNE $104B` emitted 132 bytes. The raw block dump
is in the `JIT_BLOCK` output of the `jit-phase2-byte-baseline-20260913` run;
its frame data is `tests/jit/build/jit-phase2-byte-baseline-20260913.csv`.

Tracing the latter block against `emit_memory`, `emit_compare`, and
`emit_nz` found that the mapped load stores A, defers N/Z, and the following
compare reuses the cached accumulator. Therefore removing an assumed A reload
would not change the selected sequence. Its large executed paths instead end
with full guest-PC and 24-bit cycle synchronization. That state contract is
required by the current dispatcher and is an inter-block-ABI/linking concern,
not a safe within-block retention change. No production emitter change is
justified yet, and phase 2 remains open pending a real redundant operation in
a sampled resident block.

## Working Loop

1. Read the owning function and nearest relevant test. State one falsifiable
   hypothesis, one small change, and one cheap check that could reject it.
2. Add or extend an existing focused test. Avoid new test frameworks/files
   when the existing harness is a suitable home.
3. Make the smallest change. Immediately run the focused executable check.
4. If it fails, repair that same slice and rerun before expanding scope.
5. After target code changes, force a release rebuild before linked-image
   tests. Host C tests cannot detect target compiler or assembly ABI failures.
6. Measure the same workload before/after. Preserve raw outputs and provenance.
7. Keep only evidence-supported changes. If removing your unsuccessful probe,
   reverse only that probe, preserving all prior/user edits.
8. Update this document with results and the next phase. End with a built
   release, exact verification results and any unverified requirements.

## 1. Measure Execution and Cache Coverage

Entry points: [cpu_run](../src/cpu.c), [jit_observe_pc](../src/jit.c),
[native dispatcher](../src/jit_core.s), [game harness](../tests/jit/game_test.c),
[throttle wrapper](../tests/jit/cemu_throttle.c), and
[existing performance notes](performance.md).

1. Confirm the current source, build flags and assets. Preserve the baseline
   ELF and 8xp under a new non-overwriting artifact name before candidate
   builds. Record hashes. Old checkpoints require their exact original ELF.
2. Identify the user's slow scene and a reproducible input sequence. If only
   startup/intro is available, label it accordingly; do not call it gameplay.
   Ask for the game/scene or inputs if this is the blocker to useful profiling.
3. Prefer host-side diagnostics in the existing linked CEmu harness, avoiding
   overhead in the calculator release. First expose existing cache counters:
   observations, cached/ready/ineligible blocks, native hits and mismatches.
   Resolve symbol addresses once, not repeatedly in a tight sampling loop.
4. Add bounded target-address sampling using actual ELF symbol ranges and
   native cache ranges. Account separately for halted library hooks, harness
   calls, initialization and unknown addresses. Do not classify every helper
   by whichever function name happens to precede it in the symbol table.
5. Separate CPU interpretation, generated native code, JIT compilation,
   dispatch/validation, rendering and device work where evidence permits.
   Flat PC samples give sampled target residency, not exact guest coverage.
6. Measure guest cycles/instructions executed natively versus interpreted if
   practical. Native block-hit count alone is NOT native coverage. If exact
   coverage needs intrusive hooks/counters, use a diagnostic-only run and
   measure timing separately on an uninstrumented release.
7. Distinguish cache misses, insufficient cycle budgets, decimal rejection,
   byte/mapping changes, admission failures and eviction. Add only counters
   needed to discriminate the leading hypothesis. Do not assume a full cache
   is effective or a partially full cache is necessarily broken.
8. Use bounded runs at the required 135% cap, throttle ON. Record achieved
   cap, modeled calculator time and host elapsed time. Never replace the
   mandatory throttle wrapper with raw unthrottled emu_run.
9. Report emulated and presented frames separately, with measured intervals.
   Exclude loading for a gameplay measurement. Keep Turbo, frame skipping,
   display, ROM/disk, entry point and input sequence identical between runs.
10. Decide from evidence: improve discovery/eviction if native coverage is
    low; improve generated code if native execution is expensive; investigate
    rendering/devices instead if they dominate. Do not force a CPU conclusion.

Exit gate: a reproducible baseline and a ranked, falsifiable bottleneck report.
If the harness only reaches an intro, stop short of claims about gameplay.
No production optimization is required for this phase.

## 2. Improve Register Retention

Entry points: [jit_emit.c](../src/jit_emit.c), especially emit_byte, emit_load,
emit_store, emit_nz, flush_nz, emit_memory and jit_compile.

1. Use a hot sequence identified in phase 1, or explicitly label a synthetic
   sequence as a microbenchmark. Dump its actual generated native bytes.
2. Trace native A/BC/DE/HL and guest A/X/Y/flags through every emitted operation.
   Identify a load/store or flag calculation that really is redundant.
3. Remember emit_store re-establishes accumulator_field after emitting bytes.
   emit_nz can retain that cache when flags are deferred. Do not assume
   emit_byte invalidation means consecutive logical operations always reload.
4. cached_index is deliberately captured before index_field is reset, then
   passed into emit_memory. That mechanism is existing reuse, not a stale-value
   bug. Verify emitted output before changing it.
5. Start with one safe within-block reuse improvement. Do not simultaneously
   redesign the inter-block ABI or stop saving all guest state.
6. If deferring a store, define dirty-state handling at branches, fallback,
   helper calls, exceptions/traps and block exit. Device helpers may observe
   guest state. Preserve values on every exit, not only the common path.
7. Test the chosen sequence, an intervening clobber, overwritten/read flags,
   control-flow exits, mapped helpers and capacity boundaries. Verify target
   state/memory/cycles and host-versus-CEdev emitted-byte parity.
8. Compare native size and target clocks using target-benchmark or a focused
   neighboring benchmark. Reject an optimization that just moves work to
   block boundaries without a net benefit in the selected workload.

Exit gate: exact correctness passes and a measured scoped benefit. No numeric
whole-game prediction from instruction counts or byte-size savings alone.

## 3. Specialize Memory Accesses

Entry points: [jit_core.s](../src/jit_core.s) read/write bridges,
[jit_emit.c](../src/jit_emit.c) indexed-address generation,
[memory.h](../src/memory.h), and [memory.c](../src/memory.c).

1. Establish which bridge/address mode consumes meaningful time. Known-RAM
   indexed folding and selected A/DE reuse already bypass some bridge work.
2. Choose one frequently used addressing case. Document when it is provably
   RAM and when it must use the mapped/device path.
3. Preserve the low-4-KiB SRAM/high-RAM VRAM split, 16-bit guest wrap, zero-page
   wrap, page-cross cycles, ROM overlays, VIC/CIA/SID reads and bank-port writes.
4. Make a register-liveness table for the bridge and each caller. The existing
   fast path changes BC/DE/HL and native flags. It is NOT safe to remove their
   saves simply because the path does not call C. IY must survive slow C calls.
5. Prefer a narrow specialized helper or emitter path over weakening the
   generic bridge contract. Coordinate any new contract with phase 2.
6. Retain timing-bias handling around device calls. Do not treat $0001 as plain
   RAM, or bypass its bank setup to save calls. Preserve RMW write ordering.
7. Test both direct and forced-slow paths, ROM/I/O boundaries, index and cycle
   wrap, self-modifying writes, and original/final register and flag states.
   Use the existing copy, mapped-memory and bank-port fixtures.
8. Measure with the same addressing cases, including the slow-path fallback.
   Retain guards even if removing them produces a faster microbenchmark.

Exit gate: focused direct/mapped/port correctness and a measured improvement
in the chosen workload, with no silent ABI or timing change.

## 4. Add Guarded Block Linking

Entry points: [jit.c](../src/jit.c) cache lifecycle/indexing,
[jit_core.s](../src/jit_core.s) dispatch and self-repeat,
[jit_emit.c](../src/jit_emit.c) block exits, and
[cpu_core.s](../src/cpu_core.s) re-entry.

1. Re-measure transition overhead after phases 2 and 3. Fast self-repeat and
   general interpreter-to-JIT re-entry already exist; do not count them as new.
2. Start with one stable direct-JMP edge to another ready block. Define a
   guarded target-entry path that can skip lookup while retaining validation.
   Do not begin with unrestricted native-to-native jumps.
3. Verify target identity, readiness and slot lifetime before executing it.
   Define invalidation for target/source eviction, reset, blacklist, native
   overwrite and changed successor. A reused slot address is not an identity.
4. Preserve remaining cycle budget, 24-bit wrap, decimal admission, current
   visible code bytes/mapping, native guards and forward progress at every
   linked entry. Return normally at scheduler/device boundaries.
5. Keep native register ownership explicit. If a block assumes IY references
   its metadata, update/preserve it appropriately when taking an edge. Do not
   introduce recursive calls or an unbalanced stack for long linked chains.
6. Do NOT replace byte comparisons with page-generation equality alone.
   Native direct writes do not all update those counters, and counters wrap.
   A generation-only design needs complete write tracking and wrap handling;
   it is a separate project, not a prerequisite for guarded lookup bypass.
7. Test A-to-B, an A/B cycle, insufficient budgets, decimal changes, bank
   changes, self-modification, target eviction/slot reuse, reset and blacklist.
   Compare CPU state, cycles and memory against the existing unlinked path.
8. Benchmark transitions and the matched game segment. Only then consider
   conditional edges, JSR/RTS or cross-block register retention as follow-ups.

Exit gate: linked lifetime/guard tests pass, no stack growth, and measured
benefit. If validation costs dominate and lookup bypass does not help, report
that result instead of deleting guards to force a positive benchmark.

## Commands and Validation

Use Windows PowerShell 5.1. Paths contain spaces. Example setup from the root:

```powershell
$root = 'G:/Other computers/My Laptop/TI Projects/2026/emudore'
$make = 'C:/Users/alvaj/CEdev/bin/make.exe'
$env:PATH = 'C:/msys64/ucrt64/bin;' + $env:PATH
$hostArgs = @('CC=C:/msys64/ucrt64/bin/gcc.exe', 'AR=C:/msys64/ucrt64/bin/ar.exe', 'EXEEXT=.jit-phase1-r1.exe')
```

Use a fresh EXEEXT for each revision to avoid stale harness reuse. Invoke
commands individually; do not run every target after every small edit.

```powershell
# Host cache/emitter checks; TESTS supports names from jit_test.c.
& $make -C "$root/tests/jit" check @hostArgs

# Required after production code changes, before linked-image checks.
& $make -C $root -B

# Choose the target appropriate to the touched slice.
& $make -C "$root/tests/jit" target-copy-check @hostArgs
& $make -C "$root/tests/jit" target-reentry-check @hostArgs
& $make -C "$root/tests/jit" target-extensions-check @hostArgs
& $make -C "$root/tests/jit" target-benchmark @hostArgs
```

Inspect main's CLI routing in target_test.c before assuming a target runs only
its named case. If the new slice lacks a focused route, add one to the existing
harness and Makefile rather than rerunning the entire suite for each opcode.
target-extensions-check covers opcode encoding/extension cases, not every
mapped memory or self-modification scenario. Native-only host-emitter tests
are not substitutes for executing CEdev-generated code from the linked ELF.

For a completed production batch, run the broader required gates once:

```powershell
& $make -C "$root/tests/jit" check startup-check cpu-check @hostArgs
```

startup-check already includes the general linked JIT suite plus compressed
startup and library/memory validation. Do not duplicate it with target-check
unless diagnosing a specific failure. Run decimal exhaustive tests only when
the touched arithmetic/flag behavior warrants them.

Existing bounded game command (startup/intro example, NOT gameplay proof):

```powershell
& $make -C "$root/tests/jit" -f game.mk game-check FRAMES=180 FILE_INDEX=2 JIT=on OUTPUT=build/jit-phase1-baseline INPUT=none RESUME=none @hostArgs
```

Confirm dependencies/assets and choose a unique OUTPUT before running. The
game harness uses game.mk, not the default Makefile. Record actual settings;
Turbo/frame skip are not currently arbitrary game.mk options. Do not pass
invented variables and assume they affected execution. If adding settings,
include them in checkpoint provenance and validate fresh/resumed equivalence.
Never bypass a checkpoint identity mismatch. The harness hooks TI libraries;
it is not a full TI-OS GUI launch. Verify throttle output rather than assuming
emu_set_run_rate(1000) means 1000% speed; it is the scheduler quantum setting.

## Report After Each Phase

- Hypothesis and whether the measurement supported it.
- Files/functions changed, with a short explanation.
- Exact commands, pass/fail totals and any unrelated existing failures.
- Baseline/candidate ELF hashes, inputs/settings and raw log locations.
- Native bytes, modeled target clocks, achieved throttle and relevant frame
  rates; distinguish microbenchmark, startup, intro and actual gameplay.
- Release build result and memory-limit compliance; leave bin/EMUDORE.8xp ready.
- Remaining risks and the next smallest evidence-supported action.

Earlier parallel-agent reports contain unsupported percentages and technical
errors. Do not reuse their estimates. In particular, accumulator/index reuse
is already active; ordinary fast dispatch DOES compare code bytes; compiler
capacity limits can shorten a valid block rather than reject it entirely; and
cache growth must be checked against current memory usage, not old notes.