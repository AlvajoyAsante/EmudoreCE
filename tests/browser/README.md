# C64 Browser Tests

The four host suites compile production browser-model, UI, storage, CRC32, and
drive code. Run these commands separately from the repository root:

```powershell
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
C:\Users\alvaj\CEdev\bin\make.exe -C tests/browser check CC=C:/msys64/ucrt64/bin/gcc.exe
```

| Suite | Tests/groups | Main checks |
| --- | ---: | --- |
| Model | 4 | All 255 entries, BASIC, filtered original indices, paging, CRC32 vectors |
| Drive | 31 | Real LOAD/fastload traps, hidden support files, raw PETSCII, chunk boundaries, identity cache, invalidation |
| Storage | 36 | Original lock metadata, two snapshots, eight disks, corruption, partial writes, failures, no-op saves |
| UI | 51 | Real browser input/drawing, pane bounds, views, MODE handoff, GC callbacks, remapping, allocation failures |

All 122 tests/groups pass. Storage executes 34,419 checks and drive executes
1,035 checks. Fixtures include 19-file and maximum-count directories, high-bit
filenames, hidden launchers, known/unknown lock states, stale metadata, and
simulated AppVar relocation. No game or ROM files are modified by host tests.

Each child Makefile also supports `check`. The UI suite supports `snapshots`;
see [ui/README.md](ui/README.md) for controls and image-export commands. Those
pictures use actual UI draw calls and icons with a conservative mock font,
not the real GraphX font or CEmu output.

The corresponding Python format/export tests are:

```powershell
.venv/Scripts/python.exe -m unittest discover -s tests -p test_*.py
```

Host checks do not execute native GraphX, eZ80 assembly, or TI-OS garbage
collection. Target verification still requires `make`, generated-code inspection,
and CEmu or a device. Test binaries, objects, and snapshots are ignored by Git.