# JIT Game Crash Investigation

## Status

**Open as of 2026-09-15.** This note records an unresolved target-only JIT
failure seen in 10K and the input-sensitive Parallax investigation. It is a
debugging handoff, not evidence that either game is fully compatible or that
the JIT should be disabled.

The release uses the assembly CPU core, the native eZ80 JIT, and fast JIT
re-entry. Keep those paths enabled while investigating. A host emitter test
or a successful boot cannot establish target-game correctness.

## Reproducible 10K Failure

The linked CEmu game harness reproduces this with the first program in
`tests/10K.d64`, JIT enabled, no input, and a bounded 180-frame run:

```powershell
$env:PATH = 'C:/msys64/ucrt64/bin;' + $env:PATH
& 'C:/Users/alvaj/CEdev/bin/make.exe' -C tests/jit -f game.mk game-check `
    TARGET_IMAGE=../../bin/EMUDORE.obj FRAMES=180 JIT=on FILE_INDEX=0 `
    OUTPUT=build/10k-jit-on-repro INPUT=none RESUME=none DISK=../10K.d64 `
    EXEEXT=.10k-jit-on-repro.exe CC=C:/msys64/ucrt64/bin/gcc.exe
```

The failure is deterministic at completed frame 87 in the known failing
release. It escapes from linked application/native RAM after guest PC `$01A5`
and reaches target PC `$000038`:

```text
ESCAPE target_pc=000038 ... guest_pc=01a5
JIT_BLOCK slot=0 pc=01a5 guest=88cad0ef native_size=100
JIT_BLOCK slot=14 pc=019c guest=91fe native_size=69
FAIL target image: execution left linked application/native RAM
```

The first block is the guest loop `DEY; DEX; BNE $0198`. The raw stack at
`$0038` contains repeated return address `$000039`; this is a later recursive
runaway, not proof that `$0038` is the original bad return address.

With JIT disabled, the same disk completed 120 frames, including 117 nonblank
frames. This localizes the observed failure to native JIT execution or its
handoff, not the disk parser, LOAD copy, or baseline assembly CPU path.

## What Has Been Tested

These observations narrow the search but do not identify a root cause:

- Bypassing `jit_run_batch_fast()` did not prevent the frame-87 10K failure.
- Bypassing both fast JIT entry functions so the C dispatcher executes the
  block also did not prevent it. The bug is therefore not proven to be only
  the low-PC-byte fast lookup path.
- Declining compilation of the nearby `$91` (`STA ($zp),Y`) block did not
  prevent it.
- A target regression now starts at `$0198`, heats `$01A5`, enters through
  the assembly CPU re-entry path, sweeps batch budgets 1 through 64, and
  compares full state against vrEmu6502. It passes. The generic backward
  branch shape alone is insufficient to reproduce the title failure.
- Target edge validation passes 7,744 cases, including the native execution
  bridge, CPU assembly self-test, re-entry test, BRK cases, and decimal RRA/
  ISC cases. Passing this suite does not clear 10K.
- Interrupt masking around the fast dispatcher did not remove the escape.
  It was reverted.
- A speculative range check on the fast-cache entry pointer changed the
  frame-87 escape into a no-frame stall. It was reverted rather than left as
  a behavior regression.

The C fallback invokes `jit_execute_block(block)` so target code derives the
native entry from a stable block pointer instead of relying on fragile C
address-expression code generation. This ABI hardening remains, but it did
not fix 10K.

## Parallax and Input Coverage

The reported Parallax problem occurs after entering the game and applying
controls, so a no-input startup replay is not enough to clear it. The game
harness accepts deterministic input scripts in the form
`frame:key:duration`, for example `7421:right:1000`. It logs each input mask
and the sampled active-low joystick byte in the per-frame CSV.

The established route is encoded in the **Test current Parallax startup** and
**Test current Parallax controls** VS Code tasks. It drives the menu/trainer
path, fires, moves right and left, then runs paired continuations:

- released: fire at frame 7401, then no direction held;
- held: the same history plus right held from frame 7421 for 1,000 frames.

Historical target runs with an earlier matching image reached frame 8,400 for
both continuations and recorded different framebuffer/RAM state after the
right press. That is useful input-delivery evidence, not a guarantee for the
current release or a full-game compatibility claim. Checkpoint restore
requires exact ELF, disk, JIT mode, selected file, and prior input identity;
never force an old checkpoint to load after rebuilding.

In the fresh current-image startup run begun on 2026-09-15, the exported
`jit_native_mismatches` counter became `1` around frame 450, before the first
scripted input at frame 1501. The dispatcher detected a changed native guard,
blacklisted that block, and execution continued. This is evidence of a
guarded native-cache overwrite before gameplay input, not yet proof that it
causes a later Parallax crash. Retain the full run log and identify the
affected slot before changing cache lifetime or input code.

For a fresh current-image Parallax test, run the startup task first, then the
controls task that resumes its newly written checkpoint. A crash, target-PC
escape, watchdog timeout, JIT mismatch count, missing input-mask transition,
or unexpected joystick byte is a failure. Preserve the CSV, `.state`, BMPs,
and task log under a unique `OUTPUT` prefix.

## Next Diagnostic Step

If 10K still fails on the current release, add temporary test-only tracing at
the native dispatch boundary. Capture each event only for guest PCs `$0198`,
`$019C`, and `$01A5`:

1. selected `JitBlock *`, its `start`, readiness, and native guard bytes;
2. native-entry address and stack pointer immediately before and after the
   generated-code call;
3. returned boolean, guest PC/cycle state, and IY/IX values at the return;
4. whether the call came from C dispatch, fast dispatch, or assembly re-entry.

Store a bounded ring in target test memory and dump it only after an escape or
watchdog. Do not use `dbg_printf` in the JIT hot path: it changes timing and
can expose CEdev frame-register hazards. If trapping `$0038` is needed again,
use it only to preserve the first bad stack frame, then remove the trap.

## Validation Rules

1. Rebuild `bin/EMUDORE.obj` and `bin/EMUDORE.8xp` after every production
   JIT/assembly change before running a linked target harness.
2. Use a genuinely fresh `EXEEXT` for target executables. This workspace has
   exhibited stale execution for reused harness paths.
3. Run the focused target edge/re-entry test after changing dispatch, bridges,
   cache layout, or generated-code entry conventions.
4. Re-run both 10K JIT-on/JIT-off and Parallax released/held input paths after
   a candidate fix. Then run the broader D64/T64 campaign before calling the
   issue resolved.
5. The harness uses CEmu at a requested 135% throttle with throttling enabled.
   It validates target execution, not GUI gameplay speed. Real playability
   needs a separate CEmu GUI run at 100% throttle with the same controls.

Related implementation and test surfaces: `src/jit.c`, `src/jit_core.s`,
`src/jit_emit.c`, `src/cpu_core.s`, `tests/jit/target_test.c`, and
`tests/jit/game_test.c`.