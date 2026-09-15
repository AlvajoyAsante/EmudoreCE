# EMUDORE

EMUDORE is a Commodore 64 emulator project built for the TI-84 Plus CE platform. The focus is to recreate the feel and behavior of classic C64 software while running in a compact calculator environment.

This project is centered around the original C64 machine architecture: its CPU, memory map, peripheral behavior, and data formats. It is designed to be modular and portable, with source files split across the main emulator core and a small set of tools for preparing C64 assets.

Learn more about the Commodore 64: [C64 Wiki](https://www.c64-wiki.com/)

## Features

- Accurate C64-style CPU and memory emulation
- Modular emulator structure for the core subsystems
- Support for ROM dumps, cartridge media, BASIC programs, and disk images
- C64 asset packaging utilities for calculator-friendly AppVars
- Color startup browser with file information, launcher lists, and reversible hiding
- Build system tailored to the CE toolchain and emulator runtime

## Controls

Controls are not always identical across all games and applications. The C64 ecosystem had a large variety of titles, and many games use different mappings, input conventions, or keyboard-based controls depending on the software.

Because of that, the control scheme may differ slightly depending on the game being loaded. In practice:

- Some titles rely on keyboard input rather than a joystick-style layout
- Some games use different fire, jump, or action bindings
- Some programs expect different key mappings for movement and commands
- A game may use a custom control scheme even if it runs on the same emulator core

Use the game’s instructions or on-screen prompts when available. The emulator should be treated as a flexible C64-compatible environment, not a single hard-coded control profile for every title.

### C64 Startup Browser

When a disk package is installed, the startup browser shows a scrolling file
list and a selected-file information pane. It reaches every packaged PRG,
including files beyond the old selector's first eight entries. Small icons
distinguish BASIC, marked launchers, and other PRGs. The interface uses the
existing single visible framebuffer; it does not draw into C64 RAM or run
inside the emulation loop.

| Browser key | Action |
| --- | --- |
| UP / DOWN | Select an entry |
| LEFT / RIGHT | Move one page |
| Y= | Switch Launchers / All Files |
| ALPHA | Mark or unmark the selected file as a launcher |
| TRACE | Hide or unhide the selected file |
| ENTER | Open the selected program, or BASIC without a disk |
| CLEAR | Save changed browser preferences and quit |

An unconfigured disk starts in **All Files** with no automatic classification.
**Launchers** contains only files explicitly marked as launchers and not hidden.
All Files always includes hidden files, so hiding is reversible. A hidden entry
keeps its launcher assignment. BASIC is always reachable and cannot be hidden
or marked. No disk retains the existing direct boot-to-BASIC behavior.

Hiding affects this browser only: support files remain present for exact-name,
wildcard, and supported fastloader requests from the C64. A `.OBJ` suffix does
not establish whether a file is a launcher or a required dependency. No game
files are removed, renamed, or rewritten, and TI-OS variable attributes are
not changed by browser Hide.

The information pane reports **C64 PRG**, user-assigned role, byte size including
the two-byte PRG header, and the hexadecimal **load address**. That address is
not necessarily an execution entry point. The browser does not guess a source
language from filenames or file contents.

**Locked** is read-only: it reports the original C64 DOS file delete-protection
bit, not a launch restriction or physical disk write protection. New normal and
optimized D64 conversions emit an optional `ECDINFO.8xv` beside the existing
`ECDSKnn.8xv` chunks. Transfer that companion with its matching disk to display
Yes/No. Old packages remain compatible without reconversion; missing, stale, or
invalid information displays **Unknown**. Standalone PRG/BASIC conversions do
not have an original disk lock attribute.

Launcher membership, visibility, and the selected view are saved when changed,
in a separate `ECBRSET` AppVar. It retains eight recently configured disk
identities with two checksummed snapshots, rather than changing game data.
Byte-different rebuilt or optimized disks get separate preferences. If a write
fails, the browser reports it and still permits launching; an archive failure
can leave a valid preferences copy in RAM. Two snapshots protect incomplete
writes, not arbitrary power loss during TI-OS garbage collection.

See [tests/browser/README.md](tests/browser/README.md) for host regression tests
and the limits of mock-font screenshots versus actual CEmu/device verification.

### Calculator Keys and Settings

Press **GRAPH** during emulation to pause and open Settings. Press **GRAPH** again
or select **Resume** with **ENTER** to continue. **UP/DOWN** selects an item;
**ENTER** toggles a setting, while **LEFT/RIGHT** selects its value. **CLEAR** or
**Quit emulator** exits. Menu keys are released before gameplay resumes.

| Calculator key | Default C64 action | Even function-key setting |
| --- | --- | --- |
| Y= | F1 | F2 |
| WINDOW | F3 | F4 |
| ZOOM | F5 | F6 |
| TRACE | F7 | F8 |
| GRAPH | Pause / Settings | Unchanged |
| MODE | Commodore C= | Unchanged |
| VARS | RUN/STOP | Unchanged |
| Arrows | Joystick port 2 and cursor keys | Unchanged |
| 2nd | Joystick fire and symbol layer | Unchanged |
| ALPHA | Printed letter layer on digit keys | Unchanged |
| CLEAR | Quit emulator | Unchanged |

The **Function keys** setting selects F1/3/5/7 or F2/4/6/8 for the four mapped
top-row keys. Even function keys send the C64's Shift+function combination.
Holding **2nd** still selects the symbol layer, so it suppresses function keys
in either bank. C64 cursor directions that require Shift can also shift a
simultaneously held function key; the indicator shows the effective function.

The display uses the full 320x200 game area. The GRAPH menu's **Optimization**
settings start with **Turbo CPU/RAM 1x** and **Frame skip Adaptive**. Turbo
offers **1x**, **50x**, **100x**, **200x**, and **Adaptive**. Higher rates give
the CPU more cycles per VIC raster line; Adaptive adjusts that multiplier to
the completed-frame cadence. These modes change CPU/device timing ratios and
can disrupt timing-sensitive games. They do not multiply the calculator's
native speed or guarantee faster gameplay; use 1x for normal C64 timing.
**2nd + WINDOW** toggles 1x/200x, and **2nd + ZOOM** toggles frame skipping.
Automatic skipping omits at most two consecutive display frames while
continuing emulation and collision evaluation.

**Key indicators** defaults to on. The top black band labels the function keys
and GRAPH; the bottom band labels 2nd, ALPHA, MODE, VARS, and CLEAR. Active keys
are highlighted, and unavailable functions display `--` in the symbol layer.
Settings last until the emulator exits; they are not written to an AppVar.

Indicators repaint only changed 64x20 cells at the existing keypad polling
cadence, not once per game frame. The 320x200 game rendering path is unchanged.
The paused menu temporarily saves and restores a 224x88 rectangle (19,712 bytes),
without using the second VRAM half that contains C64 RAM. Timer 1 is stopped
while paused, so menu time is excluded from emulation and profile timing.
The optional legacy FPS overlay is hidden while key indicators are enabled.
Calculator FPS impact still needs a comparable CEmu or hardware measurement.

See [tests/io/README.md](tests/io/README.md) for input and menu regression tests.

## Build

1. Install the CE toolchain and required C libraries.
2. Ensure Python 3 is available.
3. Open a CEdev shell in the project directory.
4. Run the build or asset packaging target you need.
5. Build the emulator with:

```bash
make
```

This produces the final emulator output in the `bin` directory and prepares any required AppVars for calculator execution.

If TI-OS reports `ERROR: MEMORY` before the emulator appears, see
[startup memory limits and checks](docs/startup-memory.md). The build limits
the expanded program size, not only the compressed file transferred to the
calculator. Without C64 ROM packages, a successful launch should display
`ROM AppVars missing!` instead.

## Graphics and Performance Testing

The current build uses nightly eZ80 `-O2` with `-fno-omit-frame-pointer` and
frame pacing by default. CPU/VIC timing, IRQs, and sprite state advance every
emulated frame, while display frames are sampled so expensive painting does
not stall gameplay. Character rendering uses a boot-tested assembly kernel
with a C fallback. Sprite-enabled lines finish before the next CPU raster
slice can change their data. Background and sprites are composed in a small
row buffer before reaching the LCD. This is not full-frame double buffering
or a claim of full-speed PAL on the calculator; verify actual throughput using
the profiling build.

```sh
make
make PROFILE=1
```

The release is `bin/EMUDORE.8xp`. The separately built diagnostic version is
`bin/profile/EMUDORE.8xp`; after leaving emulation it displays recent emulated
and rendered FPS, frame times, the selected character renderer, and CPU/render
shares of sampled time (including loading).

See [the performance procedure](docs/performance.md) for reproducible
comparisons, host regression tests, and interpretation of the sampled counters.

## Build Profiles

The Makefile includes four main asset-generation profiles. These are not general "debug" modes; they are packaging targets for different kinds of C64 content.

### 1. `roms`

This target packages the C64 ROM dumps into AppVars for use by the emulator.

Typical command:

```bash
make roms
```

What it does:

- Reads ROM files from `assets/roms/`
- Converts them into calculator-friendly AppVars under `bin/rom/`
- Commonly includes BASIC, KERNAL, and character ROM images

This is the standard ROM asset preparation target.

### 2. `cart`

This target packages a cartridge image into a cartridge AppVar.

Typical command:

```bash
make cart CART="path/to/cart.bin"
```

What it does:

- Accepts a cartridge image via the `CART` variable
- Produces a cartridge AppVar suitable for the emulator
- Useful when loading C64 cartridge-based games or software

This target is for cartridge media, not standard ROM dumps.

### 3. `files`

This target packages BASIC listings and PRG files into AppVars.

Typical command:

```bash
make files
```

What it does:

- Uses the files listed in the `FILES` variable
- Converts them into AppVars for application loading
- Works with files such as BASIC programs and `.prg` executables

This is the profile for software stored as individual files instead of a full disk image.

### 4. `disk`

This target packages a disk image into AppVars.

Typical command:

```bash
make disk DISK="tests/FINALBLW.D64"
```

What it does:

- Reads a `.d64` disk image
- Extracts the disk contents into AppVars
- Useful for loading disk-based C64 software in the emulator

This is the profile for floppy-disk media workflows.

## Normal and Calculator-Oriented Conversion

The existing `roms`, `cart`, `files`, and `disk` commands retain their package
formats and write to `bin/rom`. D64 conversion also emits optional browser file
information. Separate `roms-optimized`, `cart-optimized`,
`files-optimized`, and `disk-optimized` targets write to `bin/optimized`:

```sh
make disk DISK="tests/FINALBLW.D64"
make disk-optimized DISK="tests/FINALBLW.D64"
```

`disk-optimized` now changes recognized MR.CROSS V2.0 decoder code: it removes
two redundant CPU-bank writes per decoded byte while preserving decoded data
and the screen counter. The selected FINAL BLOW disk saves 484,260 emulated
bootstrap cycles and 96,852 bank writes in the independent CPU test. This is
startup decompression work, not a measured improvement in fight FPS. The
recognizer checks packer code and stream bounds, not the game filename.

The disk target fails without writing new packages if no code transformation
matches. Direct converter use can still make unchanged packages, explicitly
reported as `NO CODE OPTIMIZATION APPLIED`. Normal conversion and source disks
remain unchanged. Reports include per-file hashes, transformations, guest-cycle
savings, and archive savings as separate fields.

`roms-optimized` only changes firmware packaging; it does not optimize a disk
game. Identical PRGs can also share stored bytes. Explicit SHA-256 profiles
remain available for other reviewed patches or prepared replacements.

See [converter optimization](docs/converter-optimization.md) for dry runs,
profile requirements, compatibility limits, and examples. This is not an
automatic arbitrary-game optimizer or a measured gameplay speedup.

## Example workflow

A common flow is:

```bash
make roms
make files
make disk DISK="tests/FINALBLW.D64"
make
```

This prepares the ROMs, software files, and disk assets before building the emulator itself.

## Version

Current version: 0.1.0-alpha

## Credits

- Developed by: Alvajoy "Alvajoy123" Asante
- Platform: TI-84 Plus CE
- Original hardware: Commodore 64 and its software ecosystem
- Project status: emulator development focused on C64 compatibility and portable calculator execution

---
Copyright 2026 Alvajoy "Alvajoy123" Asante. All rights reserved.