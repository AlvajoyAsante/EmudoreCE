# Calculator-Oriented Conversion

There are two conversion paths. Normal game-data conversion is unchanged;
D64 exports also include optional browser file information. The separate
calculator-oriented converter accepts any input supported by the existing
converters. Its built-in code optimization currently recognizes one verified
MR.CROSS V2.0 bootstrap layout, including the selected FINAL BLOW disk. It is
not a universal optimizer, and matching is based on packer code, not game name.

## Commands

Run these from the repository root in a CEdev shell:

```sh
make disk DISK="tests/FINALBLW.D64"
make disk-optimized DISK="tests/FINALBLW.D64"
make files-optimized FILES="path/to/game.prg"
make roms-optimized
make cart-optimized CART="path/to/cart.bin"
```

Use `disk-optimized`, not `roms-optimized`, to optimize a game on disk. The disk
target passes `--require-code-change`: if no compatible decoder or explicit
profile matches, it fails before writing new AppVars. Existing output files
are left in place on failure and must not be mistaken for a newly converted
disk. The original D64 and normal DSK game payload remain unchanged.

Normal targets write to `bin/rom`. The optimized targets default to
`bin/optimized`, so normal output is not overwritten. Set `OPTIMIZED_OUTPUT`
to keep separate games, and `PYTHON` to choose a Python 3.10+ executable:

```sh
make disk-optimized DISK="tests/FINALBLW.D64" OPTIMIZED_OUTPUT=bin/optimized-finalblow PYTHON=.venv/Scripts/python.exe
```

The converter can also run directly, including without CEdev for a dry run:

```sh
python tools/optimize_appvar.py "tests/FINALBLW.D64" --dry-run --require-code-change
python tools/optimize_appvar.py "path/to/game.prg" --output bin/optimized
python tools/optimize_appvar.py "path/to/basic.bin" --kind basic --output bin/optimized
```

`--dry-run` prints a JSON report and neither writes files nor runs `convbin`.
Actual conversion accepts `--convbin` just like normal conversion. Reports
are named `optimization-dsk.json`, `optimization-basic.json`, and so on, with
one report per package kind in each output directory.

Report version 2 separates `files_changed_automatically` from
`files_changed_by_profile`, and has an `optimization_status` of
`code-transformed`, `storage-only`, or `unchanged`. Per-file
`automatic_optimizations` list the recognized decoder and instruction savings.
Without `--require-code-change`, unchanged conversion is permitted but prints
`NO CODE OPTIMIZATION APPLIED`. Archive-byte savings of zero do not imply zero
execution savings: the built-in rewrite keeps the PRG size unchanged.

Game/ROM AppVar names and the EM64 package format remain unchanged. Transfer either
the normal or optimized set for a package, not both: the calculator uses the
same variable names. BASIC/KERNAL/character packages can coexist with one
disk package. Use a separate output directory per game and transfer all of
that package's chunks together. Emulator rebuilding is not required to use
these packages.

## Browser File Information

Both normal and optimized D64 conversions also write `ECDINFO.8xv`, a small,
standalone AppVar containing the original file-type/lock bytes in directory
order. It is not a disk chunk or a new emulated file. Transfer it with the
matching `ECDSKnn` set to display original C64 lock status in the startup
browser. Existing DSK bytes and ordering are identical to conversion without
this companion; old emulator versions can ignore it.

The new browser treats missing, corrupt, wrong-version, or mismatched ECDINFO
as optional unavailable information: **Locked: Unknown**. It still opens old
packages without reconversion. Locked refers to the C64 DOS delete-protection
bit (`0x40`), not physical disk write protection or permission to launch.
Source `.OBJ` names receive no special treatment; all included closed PRGs
retain their original flags, including duplicate names and shared blobs.

Version 1 is explicitly little-endian:

| Field | Bytes |
| --- | ---: |
| Magic `ECFI`, version 1, file count, two reserved zero bytes | 8 |
| Final logical DSK payload length and IEEE CRC32 | 8 |
| Original directory file-type byte per included PRG | count |
| IEEE CRC32 of all preceding metadata bytes | 4 |

The total is `20 + count` bytes, excluding the TI container. Identity covers
the entire logical payload, including later chunks. The optimized identity is
computed after code transformations and file deduplication, not from the source
D64 or the pre-optimization payload. CRC32 is an accidental-mismatch check,
not authentication. Two source disks differing only in attributes have the
same game payload; transfer the correct companion for the desired source.

D64 optimization reports include `source_file_type` per directory entry and
list the companion among output AppVars. Standalone PRG/BASIC and firmware
conversions stay unchanged. Dry-run, invalid profiles, strict no-code-change
refusals, and output/input collisions write no new metadata. Existing outputs
are not deleted on failure. Browser preferences use a separate calculator-side
`ECBRSET` AppVar and are never embedded in either conversion output.

## Automatic Behavior

| Input | Calculator-oriented behavior | Expected effect |
| --- | --- | --- |
| BASIC/KERNAL/character ROM | Store unchanged bytes without package LZSS | Avoids startup package decompression; uses more archive space when normal compression would help |
| Cartridge | Store unchanged bytes without package LZSS | Packaging change only; existing cartridge support limits remain |
| Recognized MR.CROSS V2.0 disk PRG | Remove redundant bank changes in the bootstrap writer | 10 fewer guest cycles and two fewer bank writes per decoded byte |
| D64 or PRG/BASIC set | Share the stored bytes of exactly identical PRGs, including their load addresses | Archive-space saving only when duplicates exist |
| All inputs | Emit per-file input/output hashes and applied-rule information | Auditable conversion, not a performance measurement |

Disk payloads were already uncompressed and copied into C64 RAM with native
block transfers. Shared blobs retain every filename, duplicate name, directory
position, load address, and byte. Different load addresses never share a blob.
The loader reads each entry's independent offset and size, so it does not need
an emulator change. The converter checks the rebuilt directory and file bytes
against its intended output before packaging.

No pass arbitrarily strips music, intros, padding inside PRGs, or graphics.
Such bytes can be assets, branch targets, self-modifying code, or data loaded
later. An apparent delay can also drive input, random state, or sprite timing.
Unrecognized programs are not rewritten. An explicit profile is needed for a
reviewed change outside the supported packer layout.

These changes do not make the calculator framebuffer render faster. CEmu's
VRAM view includes both visible LCD pixels and C64 RAM backing; a fast fill
can be a data transfer rather than a completed graphics update. Package LZSS,
game-specific decompression, and the VIC pixel renderer are separate costs.

## Built-In Decoder Optimization

The recognized bootstrap selects `$01 = $38` with IRQs disabled and relocates
its RLE decoder into the stack page. Its output helper writes the decoded byte,
decrements `$01`, increments a progress counter at `$0400`, then increments
`$01` again before advancing the output pointer. `$0400` is RAM under both
bank configurations, so the two bank changes are redundant. They also trigger
expensive bank-table updates in the calculator emulator.

The rewrite removes only those two instructions and retains the counter,
decoder entry points, output stores and pointer/flag behavior. Unreachable
padding keeps file size and following addresses unchanged. The converter
requires the known BASIC entry, initializer, relocation and decoder code,
consistent relocation operands, and a bounded, terminated RLE stream. It rejects
streams that write into the CPU port or stack page, wrap a write into zero page,
or overwrite unread packed data. The read-pointer wrap skips zero page into
`$0100`; that remaining packed data is covered by the stack-page exclusion.

An independent py65 execution of the complete selected disk bootstrap produced:

| Check | Original | Optimized |
| --- | --- | --- |
| Guest instructions to game handoff | 1,130,253 | 1,033,401 |
| Guest cycles to game handoff | 4,687,243 | 4,202,983 |
| Decoded-byte writes | 48,426 | 48,426 |
| Redundant bank writes in the writer | 96,852 | 0 |

Both reach `$C000` with identical registers, stack pointer and decoded memory.
The intentionally rewritten decoder bytes at `$01B3-$01C2` differ. The source
disk and packed stream are untouched. This is approximately 10.3% fewer guest
bootstrap cycles, not a measured 10.3% calculator loading-time improvement.
py65 models the 6502 instructions, not C64 peripheral timing or gameplay; a
CEmu or physical-device run is still needed to validate the visible result.

The pass can match another program using the same verified packer layout, but
other MR.CROSS variants and other packers are unsupported until independently
checked. For example, the included Rally Driver disk currently has no matching
built-in transformation. No music or intro bypass is included, and subsequent
game-specific decompression and rendering are unchanged.

## Explicit Profiles

Profiles are an opt-in way to distribute other reviewed transformations for
exact builds. The built-in decoder pass does not need a profile. A matching
profile takes precedence: its result is not additionally rewritten by the
built-in pass. A matching hash establishes identity, not correctness; only use
profiles from a trusted source and retain the normal conversion.

```sh
python tools/optimize_appvar.py "path/to/game.d64" --profile "profiles/reviewed.json" --dry-run
make disk-optimized DISK="path/to/game.d64" OPT_PROFILE="profiles/reviewed.json"
```

The profile is strict JSON with `version: 1`, a nonempty `name`, and a nonempty
`rules` array. Each rule requires:

- `id`: unique, nonempty identifier.
- `description`: what the change does and its intended benefit.
- `input_sha256`: full-file SHA-256, including the two-byte PRG load address.
- `output_sha256`: full-file SHA-256 of the reviewed result.
- Exactly one of `patches` or `replacement`.

`patches` is a nonempty array of objects with integer `offset`, hexadecimal
`expected`, and hexadecimal `replacement` fields. Offsets count from the start
of the PRG file, so offset 2 is its first loaded byte, not C64 address 2.
Patches must be in bounds, nonoverlapping, and equal length. All expected bytes
are checked against the original, and the complete result hash is checked.

`replacement` is a path to a prepared file inside the profile directory,
resolved relative to that directory, not the current working directory.
The result hash is checked. This supports an already-reviewed, pre-unpacked
program without putting a decompressor on the calculator. Replacement PRGs
must retain their original load address; fixed-size firmware ROMs must retain
their required size. Replacement size may otherwise differ.

Rules match file content, not a hard-coded game name. They also work when a
known PRG occurs under a different disk filename. Each original file receives
at most one rule; there is no implicit chaining. Files not matched by a profile
are eligible for the built-in recognizer; files matched by neither remain
unchanged. Unused profile rules appear in `unmatched_rules`.
Malformed profiles and matching rules with incorrect expected bytes or output
hashes abort before any new AppVars are written.

The following illustrates the schema only. The hash placeholders must be
replaced; these synthetic byte changes are not a game optimization:

```json
{
  "version": 1,
  "name": "Reviewed conversion profile",
  "rules": [
    {
      "id": "reviewed-build",
      "description": "Describe the reviewed change and validation here",
      "input_sha256": "<64 hex digits from the original PRG report>",
      "output_sha256": "<64 hex digits from the reviewed result>",
      "patches": [
        {"offset": 2, "expected": "00", "replacement": "01"}
      ]
    }
  ]
}
```

For a replacement rule, replace the entire `patches` field with
`"replacement": "prepared/game.prg"`. No shell commands are executed from
profiles, and replacement paths outside the profile directory are rejected.

## Depacking and Code Optimization Limits

A proven C64 unpacker can help prepare a replacement for a recognized packer,
but a raw memory dump is not necessarily a runnable PRG. Its load address,
RUN/SYS entry point, initialization, memory banking, registers, stack state,
and any subsequent LOAD calls must still be correct. Multiload games can
decompress graphics repeatedly during gameplay; unpacking only the boot file
does not remove that work. The converter intentionally does not invoke a
generic unpacker or remove a detected-looking byte pattern automatically.

A profile author must compare normal and modified behavior, including startup,
input, scene transitions, multiloads, interrupts, collisions, and sprite
multiplexing. Replacing a copy/decompression loop with faster 6502 code or a
native emulator shortcut also needs timing analysis. Skipping an entire IRQ
to remove music can break graphics or game logic.

Universal speedups belong in the emulator's CPU, memory, and rendering paths.
Profiles can provide additional title- or packer-specific improvements once
those transformations are verified. Neither the converter nor its report
claims an FPS improvement without a comparable CEmu or calculator measurement.

## Validation

```sh
python -m pip install -r tools/requirements-optimizer.txt
python -m unittest discover -s tests -p "test_*.py" -v
make
```

py65 is a test-only dependency; conversion itself still uses only the Python
standard library. The decoder tests execute original and patched 6502 code,
compare registers and memory, count bank writes, and check exact cycle savings.
Synthetic programs exercise signatures, relocation bounds, malformed streams,
pointer wrap, profile precedence, and refusal to write no-op output. The
selected-disk test uses the existing local D64; no game bytes are embedded in
the tests or downloaded by them.

Converter tests cover byte-preserving disk packing, shared blobs across AppVar
boundaries, names/order/load addresses, all input kinds, dry-run behavior,
strict profile rejection, expected-byte and result-hash checks, replacements,
and unchanged unknown builds. They do not prove a user-supplied profile's
behavioral correctness or full-machine compatibility. Disk compatibility remains
the existing 35-track closed-PRG extraction, not general raw-sector or custom
1541 drive-code support.