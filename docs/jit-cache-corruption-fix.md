# JIT Cache Corruption Fix

## Issue Description

A bug in the JIT cache invalidation mechanism was causing corruption of native code within JIT blocks. This led to incorrect execution and crashes during gameplay, particularly affecting Parallax which would escape at frame 3,621.

The root cause was an incorrect indexing operation when updating `jit_page_generation` array entries that track page write generations for cache coherency validation.

## Root Cause

In the CPU interpreter (`src/cpu_core.s`) and JIT dispatcher (`src/jit_core.s`), there was a faulty calculation:

**Faulty Code (before fix):**
```asm
ld hl, 0
ld l, d          ; Load page byte into L
add hl, hl       ; WRONG: Doubles the index (256*2 = 512)
ld bc, _jit_page_generation
add hl, bc
inc (hl)         ; Increment generation count for page
```

The `add hl,hl` instruction was doubling the byte array index when it should have been using a single-byte offset. Since `jit_page_generation` is a byte array and not a table of 4-byte entries like memory mapping tables, this caused an incorrect address calculation.

For example:
- Page `$AF` would be calculated as `$AF * 2 = $15E`
- This aliased to native code in JIT slot 0 (`$D0624A + $15E = $D063A8`) 
- The byte at that address was being corrupted, leading to bad control flow

## Fix Applied

**Corrected Code:**
```asm
ld hl, 0
ld l, d          ; Load page byte into L  
ld bc, _jit_page_generation
add hl, bc       ; Proper single-byte offset calculation
inc (hl)         ; Increment generation count for page
```

This change ensures that the indexing into `jit_page_generation` correctly uses a single-byte offset rather than an incorrect doubled index.

## Verification

The fix was validated through:
1. Target regression tests confirming no more cache corruption
2. Parallax startup validation reaching 7,400 frames with zero mismatches
3. Paired released/held-right control validation (8,400 frames total) 
4. JIT block native code integrity checks

## Impact

This fix resolves:
- Parallax crashes during gameplay at frame ~3,621  
- Cache corruption issues in the JIT system
- Improves overall JIT correctness and stability