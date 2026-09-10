# Host I/O Tests

The harness compiles the actual [production I/O implementation](../../src/io.c)
with the local graphx, keypadc, timer, allocation, and VIC-request mocks. All 17
`menu-*` tests enter the menu through `io_emulate()` using queued `kb_Scan()`
states. They do not call the menu directly or assign production setting flags.
The original 16 tests and their assertions remain in place.

## Run

From the repository root, run these as separate PowerShell commands:

```powershell
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
C:\Users\alvaj\CEdev\bin\make.exe -C tests/io check CC=C:/msys64/ucrt64/bin/gcc.exe
```

The default flags are `-std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror`.
The GCC directory must be on PATH for its runtime DLLs, even with an absolute
compiler path. No Python, target build, clean, or package installation is needed.

Select one or several tests, or list their names:

```powershell
C:\Users\alvaj\CEdev\bin\make.exe -C tests/io check CC=C:/msys64/ucrt64/bin/gcc.exe TESTS=menu-held-graph-resume
C:\Users\alvaj\CEdev\bin\make.exe -C tests/io check CC=C:/msys64/ucrt64/bin/gcc.exe 'TESTS=menu-timer-freeze-resume menu-disabled-timer'
C:\Users\alvaj\CEdev\bin\make.exe -C tests/io list CC=C:/msys64/ucrt64/bin/gcc.exe
```

Failed assertions print the test name, source line, and condition or diagnostic.
The process returns nonzero for failures or unknown test names. A depleted scan
queue or exceeded key-read budget fails the test instead of hanging in a modal
loop.

## Harness API

- `KEYS(...)` and `NO_KEYS` construct complete keypad register snapshots.
- `mock_set_keys()` changes the current registers without consuming a scan.
- `poll_keys()` advances enabled timers by the 546-tick poll interval and calls
  `io_emulate()`. GRAPH requires a queued script.
- `mock_queue_scan(keys, ticks)` supplies one future blocking scan. Held keys
  must appear in consecutive snapshots; a release is an explicit `NO_KEYS`.
- `queue_key()` supplies one second of simulated scan time; `queue_tap()` adds
  a key and release. `queue_resume()` also drains overlapping and unrelated keys.
- `queue_observed()` runs a test callback after applying a scan, while the menu
  is still on the stack. Snapshot and allocation-failure tests inspect actual
  pixels, live allocations, and guest input at these observation points.
- `mock.draw` records every graphx call, including setters and width queries,
  with timer control/count and scan position. `mock.scan` records all three
  timer counts before/after each scan. `mock.mode` records explicit keypad-mode
  restoration and the number of scans/draws completed at that point.
- `mock_advance_ticks()` respects each timer's enable and direction bits.
  `mock.fail_next_allocation` injects one failed allocation. Allocations track
  live bytes and frees; optional free-time capture verifies the saved payload.
- `save_framebuffer()` captures both VRAM halves, surrounding guards, and the
  guarded palette. `clear_draw_trace()` clears graphics counters, not pixels,
  production state, or timer/keypad history.

`io_init()` does not reset `io_emulate()`'s function-local previous matrix,
joystick, poll timestamp, or continuous-mode flag. `menu_fixture()` therefore
primes an ordinary idle poll before recording render counts. No test reaches
into those inaccessible statics. An initially stopped timer test advances to a
due poll *before* disabling timer 1. If a guest key was held before entry, the
first ordinary idle poll after the menu legitimately requests one release frame;
menu-only activity and subsequent idle polls request none.

Timer assertions cover allocation, every modal graphics call through cleanup,
every entry/menu/exit scan, explicit continuous-mode restoration, and the exact
enable/control/count state on return. The ordinary HUD update before entry and
the update after return are outside the modal interval. Timers 2 and 3 provide
an independent clock-progress check while timer 1 is stopped.

Repeated menu labels can occur with different selection colors in one trace.
Assertions for the final setting inspect its last draw, not the first earlier
unselected copy.

## Added Tests

| Test | Coverage |
| --- | --- |
| `menu-held-graph-resume` | Held GRAPH cannot immediately close; drain all entry/exit keys; fresh GRAPH resumes. |
| `menu-enter-resume` | Enter resumes entry 0; left/right do not; overlapping held Enter is drained. |
| `menu-navigation-edges` | Up/down edges, held and changed-key debounce, unrelated-key debounce, both clamps. |
| `menu-indicator-toggle` | Enter toggles off/on; left/right set idempotently; settings persist after dismissal and reentry. |
| `menu-function-banks` | All four keys and their combined chord in odd, even, and restored odd banks. |
| `menu-second-suppression` | 2nd suppresses every function key and combined chord in both banks; releasing 2nd restores mapping. |
| `menu-even-no-spurious-shift` | Even bank without function keys never adds LSHIFT to idle or unshifted controls. |
| `menu-held-functions-idle` | Every held function key/chord in both banks produces zero repeated graphx calls; idle stays quiet. |
| `menu-snapshot-restoration` | All 256 byte values, exact live 224x88 snapshot and free-time contents, 320x200 restoration, guards and guest VRAM. |
| `menu-layout-bounds` | Fixed rectangle, all real labels/values at 8px width, no text overlap, bank Enter toggles and debounce. |
| `menu-timer-freeze-resume` | Every modal draw/scan is frozen; other timers advance; control bits and 545+1-tick cadence survive. |
| `menu-disabled-timer` | An already-stopped timer stays stopped while another timer advances. |
| `menu-input-isolation` | Every scan leaves matrix/joystick idle and requests no game frame; cached pre-menu guest release is counted correctly. |
| `menu-malloc-failure` | Six dismissal keys with running/stopped timer, visible lower-band error pixels, unchanged bank, successful retry. |
| `menu-malloc-failure-clear` | CLEAR on allocation failure quits with running/stopped timer, restored keypad, and no leak. |
| `menu-explicit-quit` | Entry 3 quits only on Enter, not left/right; drains keys and preserves the snapshot. |
| `menu-clear-exit` | CLEAR quits from all four selections, with cleanup and persistent quit state. |

## Pixel And Target Limits

The mock has a real writable 320x240 indexed framebuffer and a 256-entry palette.
It paints rectangles and deterministic glyph-shaped test pixels at a conservative
8 pixels per character. Tests verify geometry and exact bytes, not the real
graphx font's appearance. The 320x200 game area is LCD rows 20 through 219;
the menu is `(48,76)` with size `224x88`. The second VRAM half and both guards
must remain unchanged.

A bitmap export is possible from the in-memory framebuffer and palette, but the
harness currently has no image exporter or screenshot command; none was added.
An exported image would show mock glyphs. Actual font appearance, LCD behavior,
hardware keypad scanning, and eZ80 code generation still require CEmu or a device.