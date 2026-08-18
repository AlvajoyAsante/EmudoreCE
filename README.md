# EMUDORE

EMUDORE is a Commodore 64 emulator project built for the TI-84 Plus CE platform. The focus is to recreate the feel and behavior of classic C64 software while running in a compact calculator environment.

This project is centered around the original C64 machine architecture: its CPU, memory map, peripheral behavior, and data formats. It is designed to be modular and portable, with source files split across the main emulator core and a small set of tools for preparing C64 assets.

Learn more about the Commodore 64: [C64 Wiki](https://www.c64-wiki.com/)

## Features

- Accurate C64-style CPU and memory emulation
- Modular emulator structure for the core subsystems
- Support for ROM dumps, cartridge media, BASIC programs, and disk images
- C64 asset packaging utilities for calculator-friendly AppVars
- Build system tailored to the CE toolchain and emulator runtime

## Controls

Controls are not always identical across all games and applications. The C64 ecosystem had a large variety of titles, and many games use different mappings, input conventions, or keyboard-based controls depending on the software.

Because of that, the control scheme may differ slightly depending on the game being loaded. In practice:

- Some titles rely on keyboard input rather than a joystick-style layout
- Some games use different fire, jump, or action bindings
- Some programs expect different key mappings for movement and commands
- A game may use a custom control scheme even if it runs on the same emulator core

Use the game’s instructions or on-screen prompts when available. The emulator should be treated as a flexible C64-compatible environment, not a single hard-coded control profile for every title.

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