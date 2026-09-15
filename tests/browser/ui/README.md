# Native Browser UI Tests

This harness compiles the actual [browser.c](../../../src/browser.c),
[browser_model.c](../../../src/browser_model.c), and [crc32.c](../../../src/crc32.c)
as separate translation units. Every UI scenario calls `browser_show()` through
its public API. It does not replace or reconstruct the browser's layout, input
loop, filtering, selection, or save handoff.

All additions, host objects, executables, and generated pictures live here.
Production source, the parent test Makefile, firmware binaries, and memory notes
are not modified. No installation, target build, branch, commit, or clean is
required. Generated artifacts are ignored by this directory's [.gitignore](.gitignore).

## Run

Run these PowerShell commands separately. The PATH prefix supplies GCC's DLLs.

```powershell
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
$uiRoot = 'G:\Other computers\My Laptop\TI Projects\2026\emudore\tests\browser\ui'
& 'C:\Users\alvaj\CEdev\bin\make.exe' -C $uiRoot check CC=C:/msys64/ucrt64/bin/gcc.exe
```

The default build uses C11, `-O1 -g -Wall -Wextra -Wpedantic -Werror`.
The default check includes the allocation-failure regressions below. They are
not marked XFAIL or silently accepted. The runner continues after an assertion
to exercise the other cases, and rejects an empty group or invalid command line.

Select a group:

```powershell
& 'C:\Users\alvaj\CEdev\bin\make.exe' -C $uiRoot check CC=C:/msys64/ucrt64/bin/gcc.exe TEST_ARGS='--group storage'
& 'C:\Users\alvaj\CEdev\bin\make.exe' -C $uiRoot check CC=C:/msys64/ucrt64/bin/gcc.exe TEST_ARGS='--group regression'
```

## Results

Verified on Windows with the specified UCRT64 GCC on 2026-09-10:

| Group | Cases | Outcome |
| --- | ---: | --- |
| lifecycle | 3 | 3 passed |
| navigation | 5 | 5 passed |
| information | 6 | 6 passed |
| input | 5 | 5 passed |
| views | 9 | 9 passed |
| storage | 16 | 16 passed |
| allocation | 2 | 2 passed |
| regression | 2 | 2 passed |
| snapshots | 3 | 3 passed |
| Total | 51 | 51 passed |

The complete run passes 51/51. Snapshot export passes 3/3.
Strict GCC compilation and editor diagnostics report no errors in the new C
sources. The full run records 46 successful browser allocations and 46 frees,
plus four deliberately failed `calloc` attempts. Peak live browser allocation
on this host is 920 bytes, excluding the mock allocator's two 32-byte guards.

## Allocation Regression

The first UI run caught an allocation-failure path that left sprite transparency
at 15 instead of 0. The production browser now restores it on both BASIC and
CLEAR recovery, matching its ordinary finish path. Tests also cover text colors,
scale, text transparency, palette entries 0/1, black screen, input release, and
drive/cache cleanup on both failure exits.

Exact reproduction in [ui_test.c](ui_test.c):

1. Provide 19 files and make the first browser `calloc` return NULL.
2. Queue held ENTER+MODE, then MODE alone to release the consumed menu key.
3. Observe `Browser unavailable` / `BASIC`, with no save or file selection.
4. Press ENTER+MODE for BASIC, or CLEAR+MODE for quit; release to MODE alone.
5. Verify the action, cleanup, and still-asserted raw MODE bit. The sprite
  transparent color must be restored to 0.

The regression cases are `calloc failure BASIC restores sprite transparency` and
`calloc failure CLEAR restores sprite transparency` in group `regression`.

## Coverage And Mocks

- Lifecycle: no files immediately returns BASIC without scans, draws, allocation,
  storage, or mapping changes. This fast path inherits the caller's graphics
  state; its fixture starts black with palette 0/1 black/white. Ordinary BASIC
  and CLEAR returns clean up and make no preference writes.
- Navigation: a synthetic 19-file ACROJet-like directory reaches its final file
  using Down or pages. A synthetic 255-file directory returns original index
  254, with `256/256` displayed. Left/Right and Up/Down clamp at both ends.
- Views: original indices survive filtering and view round-trips; fixture masks
  make both `drive_file_name` and `drive_file_info` assert allowed ORIGINAL
  indices, not filtered row numbers. Hiding preserves launcher assignment,
  All Files shows and unhides hidden entries, removal chooses the nearest row,
  and BASIC ignores mark/hide even when every launcher is hidden. Toggling flags
  or views back to their original values does not write preferences.
- Input: queued keypad scans have a hard capacity and fail immediately when
  input is exhausted, instead of hanging. Marked idle and held-key polls assert
  zero additional draws. Held Down/ALPHA, chord transitions without a full menu
  release, and initially held ENTER are exercised. `kb_IsDown` uses the SDK's
  raw bitmask expression; MODE remains `0x40` after MODE+ENTER and allocation
  recovery. No all-key release requirement is introduced.
- Information: 16 W characters occupy exactly 128 list pixels from x=27 to
  x=155, leaving attribute icons at x=165 and x=177. The information pane's
  wrapping, right-aligned fields, 8-pixel text height, invalid-name fallback,
  invalid-PRG launch rejection, Yes/No/Unknown lock display, read-only lock
  status, inclusive byte count (including a header-only 2-byte PRG), and
  zero-padded hexadecimal LOAD addresses are checked.
- Graphics: a real 320x240 indexed framebuffer is followed by a separately
  guarded 76800-byte second VRAM half. The palette has 256 guarded entries.
  Every GraphX fill, rectangle, text call, and sprite is recorded and rasterized;
  NoClip calls assert screen bounds. Visible text/sprite cells may not overlap,
  and repaint rectangles must fully erase any intersected old cells. The real
  production icon bytes are recognized and checked unchanged after use. Sprite
  headers use a C11 flexible array declaration; pixel access goes through bytes,
  without flexible-array initializers or incompatible struct dereferences.
- Save handoff: storage is mocked against the production header. Every save
  asserts prior `drive_invalidate`, `CEPackageCloseDirect`, and GC callback
  registration. During one or two simulated GC cycles, the before callback must
  be `gfx_End`; the after callback must restart graphics, restore palette and
  transparency, and render the saving notice. Drawing before palette restoration
  or drive reads while invalid are rejected. Callbacks must be cleared only
  after the entire save returns.
- Recovery: OK, FAILED, and RAM_ONLY saves are exercised. FILE exits must remap
  after every save result, validate identity size/CRC and directory count, then
  restore the selected original index. Remap failure, missing identity, and each
  mismatch offer BASIC/CLEAR recovery. Failed/RAM-only saves wait at their notice
  before continuing or quitting. BASIC/quit deliberately disable the drive instead
  of remapping it. Missing initial identity skips optional loads and uses default
  flags/Unknown locks; attempted edits show a notice without an invalid save.
- Heap: only production browser allocation calls are redirected using a forced
  include on its object file. Allocations are zeroed, bounded, aligned, guarded,
  and checked on free; failures and mismatched frees are detectable. No private
  browser static function is exposed or invoked by the harness.

## Native Pictures

The images are rasterizations of actual browser GraphX calls, not a web UI or
CEmu screenshots. [font.h](font.h) is a small hand-authored 5x7 mock font inside
fixed 8x8 cells. The conservative character widths are intentional geometry
stress, not a claim about GraphX's real font metrics or appearance. All icons
come from the actual production browser assets. ACROJet-like fixture names are
synthetic; no disk image is parsed or re-exported.

```powershell
& 'C:\Users\alvaj\CEdev\bin\make.exe' -C $uiRoot snapshots CC=C:/msys64/ucrt64/bin/gcc.exe
```

The harness writes three 77878-byte, 8-bit indexed BMPs, including all 256 palette
entries, into the ignored [snapshots](snapshots/) directory. It asserts at least
eight used colors and more than half the pixels differing from the first pixel.
`--snapshots` selects only these three scenarios; default test runs assert their
geometry and nonblank content without writing images.

Convert with built-in .NET using these direct terminal commands. This works
without changing execution policy even when this machine refuses `.ps1` files:

```powershell
Add-Type -AssemblyName System.Drawing
foreach ($name in 'all-files', 'launchers', 'last-page-255') {
    $source = Join-Path (Join-Path $uiRoot 'snapshots') ($name + '.bmp')
    $image = New-Object System.Drawing.Bitmap $source
    try {
        $image.Save([System.IO.Path]::ChangeExtension($source, '.png'), [System.Drawing.Imaging.ImageFormat]::Png)
    }
    finally {
        $image.Dispose()
    }
}
```

All PNGs were opened and visually inspected at their native 320x240 resolution.

| PNG | Bytes | Used colors | Pixels differing from first | Frame CRC32 |
| --- | ---: | ---: | ---: | --- |
| [snapshots/all-files.png](snapshots/all-files.png) | 3883 | 11 | 64314 | `c5e2046e` |
| [snapshots/launchers.png](snapshots/launchers.png) | 3068 | 10 | 64311 | `d95d79d1` |
| [snapshots/last-page-255.png](snapshots/last-page-255.png) | 3901 | 8 | 64375 | `fab91508` |

PNG SHA-256, in the same order:

```text
034c21fbf8b26acead6c55b02f38b9633015c9a2fd17b4b5ad0352a2a5096a69
ef9701d72bdc012ea90eccaa3ed2d1db065e0ffa5dc7f5b0f2a5406b0e9e4202
7bda74f6354118147ca650144ffc88c7be3099ec7d4c20df0502f23996b8de6d
```

All three captured palettes have CRC32 `5d779acd`. The first picture selects a
16-character W filename with Locked=Yes; the second selects ACROJET in Launchers
with PART1, TITLE.OBJ, and PANEL.OBJ hidden; the third selects index 254 in the
255-file fixture. Export occurs during a queued probe before the real browser
clears the screen on return.

## Limits

SDK declarations and keypad constants were checked against the locally installed
CEdev GraphX, keypadc, and fileioc headers. Host `unsigned int` represents the
SDK's `uint24_t` coordinate/size parameters; this is not a target ABI check.
Real GraphX font metrics, graphx assembly, palette reset behavior, CE allocator
overhead, eZ80 code generation, hardware input timing, and real TI-OS GC prompts
still need native/CEmu verification. No target build or CEmu run was performed.

The 920-byte measurement covers only the host browser screen allocation, not
actual preference-storage scratch space, target stack use, or mock static
framebuffer/log storage. Storage serialization, AppVar corruption/partial writes,
real pointer relocation, D64 parsing, and guest LOAD behavior remain the
responsibility of their separate production-module tests. Here GC invalidates
the mock mapping and scrambles the visible framebuffer/palette; it does not move
real TI variables. The production CRC object is used for snapshot checksums,
not as a substitute for storage-format testing.