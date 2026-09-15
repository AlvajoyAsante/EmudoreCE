# Drive Regression Tests

This host harness includes the production [drive.c](../../src/drive.c) and
supplies an in-memory disk package, split guest RAM, a KERNAL trap page,
and minimal VIC hooks. The package contains synthetic data, not game assets.

Run from the repository root with a native GCC or Clang:

```sh
make -C tests/drive check CC=gcc
```

On this Windows workspace, use PowerShell with GCC's DLL directory on PATH:

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
& "C:\Users\alvaj\CEdev\bin\make.exe" -C tests/drive check CC=C:/msys64/ucrt64/bin/gcc.exe
```

The nine tests use `-Wall -Wextra -Wpedantic -Werror` and cover:

- ACROJet's `0:EVENTA.MAP` request loading 560 bytes into `$0400`, with
  surrounding RAM guards, the end pointer, and the CPU's LOAD/RTS result.
- Plain names, `?` and `*` patterns, and selected-file wildcard autoloads
  both with and without the `0:` drive selector.
- Unmatched, truncated, and unsupported-drive names returning FILE NOT
  FOUND without modifying the destination.
- A full 16-character directory name preceded by `0:`.
- The same drive-0 request through the resident fastloader trap.
- Recording a map load after eight bootstrap loads in the extended trace.

The initial map-load test failed on the old literal filename matcher:
carry set, accumulator 4 (FILE NOT FOUND), no data copied, and trace entry
255. Removing `0:` in shared lookup makes it pass. This tests actual C
loader behavior, not native eZ80 execution or the game's visual output.
See the [indicator investigation](../../docs/performance.md#rib-cut-map-loading-correction)
for the trace evidence and target verification procedure.