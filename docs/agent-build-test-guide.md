# Agent quick reference: build, test, tools, docs

Copy-paste commands for this repo (emudore-CE, a C64 emulator for the TI-84+CE).
Run everything from the workspace root unless noted. All commands assume
Windows PowerShell and this exact toolchain:

- CEdev (eZ80-clang) build: `C:/Users/alvaj/CEdev/bin/make.exe` (needs `CEDEV` env var set; comes from CEdev install)
- Host test builds (C run on Windows, not the calculator) need MinGW GCC:
  `CC=C:/msys64/ucrt64/bin/gcc.exe` and often `AR=C:/msys64/ucrt64/bin/ar.exe`,
  with `C:/msys64/ucrt64/bin` prepended to `PATH` (GCC needs its own DLLs on PATH).
- Python 3 must be on PATH for `tools/*.py`.

Never guess these paths differently — they are the only working toolchain
locations in this environment.

## 1. Building the calculator program (bin/EMUDORE.8xp)

Normal release build (what ships to the calculator):
```
C:/Users/alvaj/CEdev/bin/make.exe
```
Output: `bin/EMUDORE.8xp`. Uses `-O2 -fno-strict-aliasing -fno-omit-frame-pointer`
(the frame-pointer flag is MANDATORY — see `/memories/repo/emudore-ce.md` BUILD GOTCHAS
section for why; never remove it).

Force a clean rebuild (use after any compiler-flag change, or if sizes look wrong):
```
C:/Users/alvaj/CEdev/bin/make.exe -B
```

Profiling build (bigger, adds on-screen FPS/CPU/render report):
```
C:/Users/alvaj/CEdev/bin/make.exe PROFILE=1 OBJDIR=obj/profile BINDIR=bin/profile
```

CPU-hot diagnostic overlay build:
```
C:/Users/alvaj/CEdev/bin/make.exe CPU_HOT_PROFILE=1 OBJDIR=obj/cpu-hot BINDIR=bin/cpu-hot
```

Debug build with `dbg_printf` tracing enabled (prints to CEmu View->Console; do
NOT use `make debug`, that sets an unrelated old DEBUG macro):
```
C:/Users/alvaj/CEdev/bin/make.exe CC_DEBUG="" LD_DEBUG=""
```

## 2. Packaging ROMs / disks / carts into AppVars

```
C:/Users/alvaj/CEdev/bin/make.exe roms
C:/Users/alvaj/CEdev/bin/make.exe disk DISK=tests/Parallax.d64
C:/Users/alvaj/CEdev/bin/make.exe cart CART=path/to/cart.bin
C:/Users/alvaj/CEdev/bin/make.exe files FILES="file1.prg file2.bas"
```
Optimized (code-transforming, only for recognized packers) variants:
```
C:/Users/alvaj/CEdev/bin/make.exe disk-optimized DISK=tests/FINALBLW.D64 OPTIMIZED_OUTPUT=bin/optimized
C:/Users/alvaj/CEdev/bin/make.exe roms-optimized OPTIMIZED_OUTPUT=bin/optimized
```
Output AppVars land in `bin/rom/` (or `--output`/`OPTIMIZED_OUTPUT` dir). Transfer
the `.8xv`/`.8xp` files to the calculator/CEmu to actually run anything.

## 3. Running host test suites (compiled with real GCC, not eZ80)

General pattern — always pass `CC`, and for jit also `AR`, and prepend the
MinGW bin dir to PATH:
```
C:/Users/alvaj/CEdev/bin/make.exe -C tests/io check CC=C:/msys64/ucrt64/bin/gcc.exe PATH=C:/msys64/ucrt64/bin;${env:PATH}
C:/Users/alvaj/CEdev/bin/make.exe -C tests/drive check CC=C:/msys64/ucrt64/bin/gcc.exe
C:/Users/alvaj/CEdev/bin/make.exe -C tests/vic check CC=C:/msys64/ucrt64/bin/gcc.exe
C:/Users/alvaj/CEdev/bin/make.exe -C tests/perf check CC=C:/msys64/ucrt64/bin/gcc.exe
C:/Users/alvaj/CEdev/bin/make.exe -C tests/browser check CC=C:/msys64/ucrt64/bin/gcc.exe
```
List available individual test names (io/vic support this): `make -C tests/<dir> list`.
Run a single named test: `make -C tests/<dir> check TESTS=<name>` (space-separated for multiple).

IMPORTANT: when actually invoking via a terminal (not a VS Code task), set PATH
for real, e.g. in PowerShell:
```
$env:PATH = "C:/msys64/ucrt64/bin;" + $env:PATH
```
then run the `make -C tests/... check CC=... AR=...` command without the
`PATH=...` make-argument form (that form only works inside VS Code tasks that
pass it through `options.env`).

## 4. tests/jit — the complex harness (CPU core + JIT + full game runs)

This directory tests three different things; always pass a fresh `EXEEXT=.<unique>.exe`
per invocation to avoid stale-binary reuse bugs (see repo memory: "Native JIT
validation: same-path stale executable image").

Correctness/CPU-core targets (Makefile):
```
make -C tests/jit check CC=C:/msys64/ucrt64/bin/gcc.exe AR=C:/msys64/ucrt64/bin/ar.exe EXEEXT=.myrun1.exe
make -C tests/jit cpu-check ...
make -C tests/jit target-check ...
make -C tests/jit target-edge-check ...
make -C tests/jit target-copy-check ...
make -C tests/jit target-reentry-check ...
make -C tests/jit target-extensions-check ...
make -C tests/jit target-decimal-exhaustive ...
make -C tests/jit startup-check TARGET_PROGRAM=../../bin/EMUDORE.8xp ...
make -C tests/jit throttle-check ...
```

Real game-disk harness (`game.mk`), for actually running a .d64 against a
built `.obj` for N emulated frames with recorded/replayed input:
```
C:/Users/alvaj/CEdev/bin/make.exe -C tests/jit -f game.mk game-check \
  TARGET_IMAGE=../../bin/EMUDORE.obj \
  FRAMES=60 \
  JIT=on \
  FILE_INDEX=0 \
  OUTPUT=build/test-run \
  INPUT=none \
  RESUME=none \
  DISK=../Parallax.d64 \
  EXEEXT=.myrun2.exe \
  CC=C:/msys64/ucrt64/bin/gcc.exe
```
Key variables: `TARGET_IMAGE` (built `.obj`, NOT the `.8xp`), `FRAMES` (how many
emulated frames to run), `JIT` (`on`/`off`), `FILE_INDEX` (which PRG on the
disk to boot), `OUTPUT` (report/log/CSV prefix under `tests/jit/build/`),
`INPUT` (`none` or a scripted input string like `1501:space:10,2701:stop:10`),
`RESUME` (`none` or a `.state` checkpoint file to continue from), `DISK`
(path to a `.d64`/`.t64`, relative to `tests/jit`).

GEOS-specific variant:
```
C:/Users/alvaj/CEdev/bin/make.exe -C tests/jit -f game.mk geos-check \
  TARGET_IMAGE=../../bin/EMUDORE.obj GEOS_FRAMES=240 GEOS_OUTPUT=build/geos-test \
  EXEEXT=.myrun3.exe CC=C:/msys64/ucrt64/bin/gcc.exe
```

Always give a fresh `OUTPUT`/`GEOS_OUTPUT` name and fresh `EXEEXT` per run so
old logs/CSVs/checkpoints are never silently overwritten or reused.

## 5. tools/*.py CLI reference

```
python tools/appvar_builder.py <input> --kind {basic|kernal|chars|cart} --output <dir>
python tools/rom_to_appvar.py <disk.d64|disk.t64> --output <dir>
python tools/files_to_dsk_appvar.py <file1> [<file2> ...] --output <dir>
python tools/optimize_appvar.py <input> --kind {basic|kernal|chars|cart|dsk} --output <dir> [--profile <json>] [--require-code-change] [--dry-run]
```
`disk_metadata.py` and `disk_optimizations.py` are libraries only (imported by
the scripts above, no CLI). Run the Python unit tests with:
```
python -m unittest discover -s tests -p "test_*.py"
```
(use the project's `.venv/Scripts/python.exe` if one exists in the repo).

## 6. Where to read further documentation (docs/*.md)

Read the SPECIFIC file that matches the task — do not read all of them:
- `docs/emudore-ce.md` — history, hardware facts, memory layout, screen fit.
- `docs/performance.md` — VIC pacing, Turbo/frame-skip settings, char-kernel asm.
- `docs/startup-memory.md` — program-memory (128 KiB) and BSS/heap limits.
- `docs/converter-optimization.md` — disk/ROM code-optimizer (MR.CROSS packer) usage.
- `docs/jit-performance-handoff.md` — JIT design, opcode coverage, cache layout.
- `docs/jit-game-crash-investigation.md` — known JIT cache-guard bugs found/fixed.
- `docs/geos-compatibility-investigation.md` / `docs/geos-handoff.md` — GEOS boot status.
- `README.md` (repo root) — canonical setup/build instructions.
- `/memories/repo/emudore-ce.md` (repo memory tool) — detailed running log of every
  fix/measurement/gotcha in this project; check it BEFORE re-investigating something
  that sounds already-explored (ez80-clang IX miscompiles, JIT bugs, banking rules, etc.).

## 7. Hard rules (do not violate)

- Never drop `-fno-omit-frame-pointer` from CFLAGS.
- Always rebuild with a real `make` after ANY source/flag change before reporting done.
- Use a NEW `EXEEXT=.<unique>.exe` (and new `OUTPUT=`/log path) for every tests/jit
  invocation; reusing the same generated executable name/path can silently run a
  stale cached binary.
- Test/benchmark claims about calculator speed must be based on CEmu 100% speed
  with throttling ON (135% only for the user's own explicit calibration runs) —
  never report uncapped/headless numbers as if they show playability.
