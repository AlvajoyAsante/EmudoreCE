# JIT Game Crash Investigation

## Status

**10K fixed as of 2026-09-15; Parallax input validation remains in progress.**
This note records the resolved target-only native-cache corruption and the
remaining input-sensitive Parallax investigation. It is not evidence that
either game is fully compatible or that the JIT should be disabled.

The release uses the assembly CPU core, the native eZ80 JIT, and fast JIT
re-entry. Keep those paths enabled while investigating. A host emitter test
or a successful boot cannot establish target-game correctness.

## Historical 10K Failure and Root Cause

The linked CEmu game harness reproduces this with the first program in
`tests/10K.d64`, JIT enabled, no input, and a bounded 180-frame run:

```powershell
$env:PATH = 'C:/msys64/ucrt64/bin;' + $env:PATH
& 'C:/Users/alvaj/CEdev/bin/make.exe' -C tests/jit -f game.mk game-check `
    TARGET_IMAGE=../../bin/EMUDORE.obj FRAMES=180 JIT=on FILE_INDEX=0 `
    OUTPUT=build/10k-jit-on-repro INPUT=none RESUME=none DISK=../10K.d64 `
    EXEEXT=.10k-jit-on-repro.exe CC=C:/msys64/ucrt64/bin/gcc.exe
```

The now-fixed release failed deterministically at completed frame 87. It
escaped from linked application/native RAM after guest PC `$01A5` and reached
target PC `$000038`:

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

The root cause was a byte-array index treated as a two-byte stride in assembly.
`jit_page_generation` has 256 one-byte entries, but `cpu_core.s::write_de`
computed `jit_page_generation + 2 * page`. An ordinary assembly-core write to
C64 page `$AF` therefore incremented address `jit_page_generation + $15E`.
On the failing layout that is `$D063A8`, exactly native byte 19 of slot 0:

```text
expected native byte: B7
interpreter write value: C2
actual native byte: C2
```

The overwritten `B7` was an `OR A` in the compiled `$01A5` loop. Replacing it
with `$C2` (`JP NZ`) redirected execution and eventually caused the `$0038`
runaway. An isolated CEmu write watch confirmed the writer was the assembly
interpreter's page-generation `INC (HL)` after a C64 `$AF36` store, not the
generated block, dispatcher call stack, or the cache compiler.

`jit_core.s` used the same erroneous doubled index for its fast-dispatch
generation check and snapshot. The fix removes the two `add hl,hl` operations
and changes its adjacent-page snapshots from two increments to one. The
target regression compiles slot 0, executes an assembly-core `$AF36` store,
checks `jit_page_generation[$AF]`, and compares every native byte before and
after the write. It now passes. A fresh JIT-on 10K replay completed 120 frames
without a native-cache difference.

## What Has Been Tested

The following rejected hypotheses are retained for future investigations:

- Bypassing `jit_run_batch_fast()` did not prevent the frame-87 10K failure.
- Bypassing both fast JIT entry functions so the C dispatcher executes the
  block also did not prevent it. The bug is therefore not proven to be only
  the low-PC-byte fast lookup path.
- Declining compilation of the nearby `$91` (`STA ($zp),Y`) block did not
  prevent it.
- A target regression starts at `$0198`, heats `$01A5`, enters through
  the assembly CPU re-entry path, sweeps batch budgets 1 through 64, and
  compares full state against vrEmu6502. It passes. The generic backward
  branch shape alone is insufficient to reproduce the title failure.
- Target edge validation passes the native execution
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

In the pre-fix current-image startup run, the exported
`jit_native_mismatches` counter became `1` around frame 450, before the first
scripted input at frame 1501. The dispatcher detected a changed native guard,
blacklisted that block, and execution continued. This is evidence of a
guarded native-cache overwrite before gameplay input. The same page-generation
alias is a plausible shared cause, but the fixed current-image input replay
must complete before treating Parallax as resolved.

For a fresh current-image Parallax test, run the startup task first, then the
controls task that resumes its newly written checkpoint. A crash, target-PC
escape, watchdog timeout, JIT mismatch count, missing input-mask transition,
or unexpected joystick byte is a failure. Preserve the CSV, `.state`, BMPs,
and task log under a unique `OUTPUT` prefix.

## Next Validation Step

Run a fresh current-image Parallax startup replay with the established
menu/trainer script through and past the old frame-3,621 escape. Then run the
released and right-held continuations from its newly generated checkpoint.
The harness diagnostics remain available if a new issue appears, but no
additional hot-path tracing is required for the fixed 10K failure.

## Validation Rules

1. Rebuild `bin/EMUDORE.obj` and `bin/EMUDORE.8xp` after every production
   JIT/assembly change before running a linked target harness.
2. Use a genuinely fresh `EXEEXT` for target executables. This workspace has
   exhibited stale execution for reused harness paths.
3. Run the focused target edge/re-entry test after changing dispatch, bridges,
   cache layout, or generated-code entry conventions.
4. Re-run 10K JIT-on/JIT-off and Parallax released/held input paths after a
  candidate fix. Then run the broader D64/T64 campaign before calling the
  remaining compatibility investigation resolved.
5. The harness uses CEmu at a requested 135% throttle with throttling enabled.
   It validates target execution, not GUI gameplay speed. Real playability
   needs a separate CEmu GUI run at 100% throttle with the same controls.

Related implementation and test surfaces: `src/jit.c`, `src/jit_core.s`,
`src/jit_emit.c`, `src/cpu_core.s`, `tests/jit/target_test.c`, and
`tests/jit/game_test.c`.