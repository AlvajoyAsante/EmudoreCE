# Startup Memory

TI-OS `ERROR: MEMORY` before the emulator screen is different from a missing
C64 ROM. The compressed program must expand in calculator RAM, and LibLoad
then appends relocatable CE library code. Archive file size alone does not
describe this requirement. Missing C64 ROM packages should instead display
`ROM AppVars missing!` after the program starts.

The oversized JIT build occupied 141,258 bytes before its ZX7 loader and
libraries were included. With the standard v15.0 libraries, the combined
154,138-byte requirement exceeded even the 153,473-byte program-area ceiling
between `LOAD_ADDR=0xD1A87F` and the framebuffer at `0xD40000`. Removing disk
or ROM AppVars could not make that build fit on an otherwise empty calculator.

## Build Limits

The [Makefile](../Makefile) uses `-Oz` for startup, package discovery, browser,
and JIT compiler/cache-management C code. CPU, VIC, memory, and peripheral hot
paths retain their existing optimization settings. Every C source retains
`-fno-omit-frame-pointer`; the native emitter's output is tested separately
from the compiler used to build the emitter itself.

[program_memory.ld](../tools/program_memory.ld) rejects expanded images over
128 KiB. With the tested libraries and loader, this leaves more than 8 KiB
for startup overhead. BSS/heap limits remain enforced by the SDK linker script;
they occupy a separate memory region and are not added to the program image.
No framebuffer or C64 RAM allocation was moved.

The release checked during this fix contains 123,668 bytes of initialized
program data and code. The shipped ZX7 loader peaks at 123,823 bytes; GRAPHX
requires 10,419 bytes and FILEIOC 2,306 bytes. KEYPADC executes from archive.
The total is 136,548 bytes, leaving 16,925 bytes below the program-area ceiling.
These are fixture measurements, not a report of free RAM on a particular
calculator. Keep EMUDORE and the CE libraries archived, and allow for other
unarchived variables and launcher/OS overhead.

## Reproduce

Build release and CPU-hot variants with the existing VS Code tasks. The
**Test EMUDORE startup memory** task tests both artifacts. From a shell with a
host GCC, Git, curl, and GNU make available:

```sh
make -C tests/jit bootstrap
make -C tests/jit startup-check CC=gcc
make -C tests/jit startup-check CC=gcc TARGET_IMAGE=../../bin/cpu-hot/EMUDORE.obj TARGET_PROGRAM=../../bin/cpu-hot/EMUDORE.8xp
```

The bootstrap pins CEmu and vrEmu6502 and applies the retained NMOS indirect-JMP
reference correction. `startup-check` downloads the public v15.0 CE library
bundle into the ignored test build directory when absent. It needs no TI-OS
or C64 ROM image. Existing dependency checkouts are not reset.

The startup check validates TI file lengths/checksums, executes the actual
shipped decompressor in CEmu with bounded TI-OS memory-operation stubs, compares
every expanded program section with the linked ELF, and reads the libraries'
real relocation/code metadata. It requires at least 8 KiB spare program RAM.
It also executes the compiled no-ROM main path with library calls stubbed,
checks its exact missing-ROM message and clean return, then runs the existing
native/interpreter integration suite.

This is not a full TI-OS, launcher, or hardware boot test. Library RAM is
accounted for, not a simulation of LibLoad's complete symbol relocation.
Tests fail if a required library gains dependencies that the accounting does
not handle. A real calculator can have less free RAM than the empty-area model.

## References

- [CEdev ZX7 packaging](https://github.com/mateoconlechuga/convbin/blob/master/src/compress.c)
- [Shipped ZX7 loader](https://github.com/mateoconlechuga/convbin/blob/master/src/asm/zx7_decompressor.asm)
- [LibLoad allocation](https://github.com/CE-Programming/toolchain/blob/master/src/libload/libload.asm)