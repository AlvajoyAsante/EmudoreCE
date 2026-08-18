# emudore-CE (C64 on TI-84+CE) port notes

## What was done
- Pivoted the repo from MiniMSX (MSX/fMSX) to emudore (Commodore 64).
- Deleted: MiniMSX.c/.h, src/Z80/, src/fMSX/. Kept CEPackage.{c,h} -> moved to src/ root.
- Ported the C++ core (emudore/src) to pure C99 for ez80-clang (user wants C, not C++).
  Classes -> structs + module functions; single global state per chip (calculator = one machine).
- src/ now: main.c, c64.c/.h, cpu.c/.h (6510), memory.c/.h, cia1.c/.h, cia2.c/.h,
  vic.c/.h, sid.h (empty stub), io.c/.h, util.h, CEPackage.c/.h.

## Removed (not needed on calculator)
- SDL2 (IO backend) -> replaced by graphx + keypadc in io.c.
- loader (BASIC/PRG via <fstream>) -> ROMs loaded from AppVars via CEPackage in c64.c.
- debugger / debugger.cpp, test_cpu, dump_regs, memory dump/print helpers.
- <iostream>/<string>/<functional>/<thread>/<chrono>/<queue>/<unordered_map> all gone.
- sid.cpp -> sid.h is now an empty documented stub (no audio on CE yet).

## Hardware facts (verified)
- LCD is 320x240 8bpp via graphx. gfx_vbuffer (**(uint8_t(**)[240][320])0xE30014) = current
  draw buffer (back buffer after gfx_SetDrawBuffer()); gfx_BlitBuffer() copies it to LCD.
- C64 visible area 403x284 is BIGGER than the 320x240 LCD -> must clip/crop or scale.
  Current io.c clips to top-left 1:1 (palette indices 0-15 direct into gfx palette).
- ez80 int = 24-bit; VIC raster arithmetic is int-based -> watch truncation at 24 bits.
- C64 ROMs: BASIC 8K@$A000, KERNAL 8K@$E000, CHARGEN 4K@$D000 -> AppVars via CEPackage.
  Package ids used: "BASIC", "KERN", "CHAR" (CEPackage names must be <=4 chars A-Z0-9).
- Makefile: NAME=EMUDORE, BSSHEAP_HIGH=0xD2C000 (RAM+ROM are 128K in BSS/heap).

## Screen fit (current: full-screen 1:1, done in io.c)
- C64 text area is exactly 320x200 = perfect LCD width match. CROP_X=43 (draw_char
  emits first pixel at x+1, so 42+1), CROP_Y=22 (centres text rows 42..241 onto the
  240-line LCD; the 20-line bands above/below show real border colour). No squeeze
  table, no rescale; every LCD row is memset each frame so no stale pixels remain.
- Rendering fast path: static uint8_t *row_ptr[240] built in io_init (avoids 24-bit
  multiply per pixel); io_screen_draw_rect/draw_border are memset-based;
  io_screen_refresh is a no-op (single buffer, palette loaded once in io_init -
  C64 palette entries 0/1 are black/white so boot text stays readable).

## Keyboard (done in io.c)
- JOYSTICK PORT 2 (io.c/cia1.c): arrows + [2nd]=fire, active-low byte from
  io_joystick2(); CIA1 $DC00 read returns pra & joy2 (real-HW AND semantics).
  Games poll joysticks, NOT the keyboard - FINAL BLOW's menu was dead until
  this existed. NOTE: reading $DC00 previously returned 0 = "all directions
  + fire jammed". Joystick 1 ($DC01 side) not mapped.
- Full CE->C64 matrix using original emudore (row,col) positions.
- Letters = TI-84 ALPHA labels; digits = face value; top row Yequ/Window/Zoom/Trace = F1/F3/F5/F7.
- 2nd = LSHIFT(1,7) + shift_map for symbols (SPACE,./-+*@;=). ALPHA=RSHIFT(6,4).
- MODE=Commodore(7,5), GRAPHVAR=Control(7,2), ENTER=RETURN(0,1), DEL=backspace(0,0). CLEAR=quit.

## ROM packaging (done in tools/rom_to_appvar.py)
- Rewritten for C64: --kind basic|kernal|chars. AppVar magic "EM64", names EC<id>NN
  (id=BAS/KRN/CHR). Sizes: basic/kernal 0x2000, chars 0x1000. LZSS compress kept,
  MSX BIOS-patch/mapper logic removed. CEPackage.c reads kind from header[5]=packageIndex.

## ROMs (working)
- Linked repo erin100280/Emunator has VALID basic.901226-01.bin and characters.901225-01.bin
  but its Kernal.bin/.rom are actually the 1541 disk-drive DOS (RESET=$00E2) - do NOT use.
- Valid KERNAL: bmc64 repo third_party/vice-3.3/data/C64/kernal (MD5 39065497630802346bce17963f13c092,
  RESET=$FCE2). Saved as assets/roms/kernal.bin.
- AppVar ids shortened to BAS/KRN/CHR (TI var name limit 8 = "EC"+id+2hex). Generated:
  ECBAS00.8xv, ECKRN00.8xv, ECCHR00.8xv via tools/rom_to_appvar.py --kind basic|kernal|chars.

- Cartridge packaging added via tools/rom_to_appvar.py --kind cart -> ECCRTnn AppVars; cart images are variable-size and validated like DSK packages.

- Startup menu added in src/main.c: shows ROM/file counts and lets you choose which DSK entry autoloads; drive loader treats selected wildcard load as that entry.
  Entry 0 = "BASIC (no disk)": show_file_menu returns 2 -> main zeroes fileCount so
  drive_patch_kernal/autoload are skipped (plain BASIC boot even with a DSK present).
  File entries are menu index-1 into drive_select_file.

## MEMORY LAYOUT (current, working design)
- Old AppVar-backed approach is DEAD: an AppVar cannot hold 64KB (16-bit size field,
  max ~65.5KB minus header, and 2x64KB doesn't fit user RAM anyway); default heap is
  ~60KB so malloc(64K) fails too (mem_init FAILED, graceful).
- C64 64KB RAM = second half of VRAM at 0xD52C00 (76800 bytes free because graphx runs
  SINGLE-buffered: no gfx_SetDrawBuffer, no gfx_BlitBuffer anywhere; io.c draws direct
  to visible screen via gfx_vbuffer, io_screen_refresh only reloads the palette).
- ROMs are NOT a 64KB image anymore: three static BSS arrays rom_basic[0x2000],
  rom_kernal[0x2000], rom_chars[0x1000] (20KB total fits default BSS). Accessors:
  mem_basic_rom_ptr()/mem_kernal_rom_ptr()/mem_chars_rom_ptr(); reads index with
  addr-ADDR_BASIC / addr-ADDR_KERNAL / (vic_addr&0xfff). mem_rom_ptr/mem_read_byte_rom
  removed.


## Tear-free compact VRAM staging (current)
- Direct VIC line rendering into visible VRAM causes visible split-frame tearing because
  io_screen_refresh used to be a no-op and the LCD scanout sees rows while they are painted.
- A full 320x240 staging frame in normal RAM needs 76,800 bytes and DOES NOT fit with the
  emulator's existing BSS. Do not move BSS/stack/code to force it; that causes startup resets.
- VRAM layout: visible page 0xD40000..0xD52BFF, C64 64 KiB RAM 0xD52C00..0xD62BFF,
  spare tail 0xD65400..0xD67FFF (11,264 bytes). IO stages rows at 0xD65400 as 240 rows x
  32 bytes, one byte per 16 horizontally packed LCD pixels (line-ident tag).
- vic.c renderers write 40 C64 cells into 20/32-byte compact rows; hires/MC bitmap asm emits
  one byte per C64 cell. io_screen_refresh() expands every staged byte to a 16-pixel run in
  visible VRAM only after a rendered frame completes, preventing row-by-row tearing.
- Tradeoff: display and framebuffer tag collisions are 16-pixel granular, not true 1:1 pixels.
  Row writes go through checked io_stage_row() so corrupted row pointers cannot write outside
  the spare VRAM tail.


## Disk loading (D64 -> AppVar, done)
- tools/d64_to_appvar.py packs all CLOSED PRG files from a .d64 into "DSK" package
  AppVars ECDSK00/01... (EM64 v2, kind index 3, UNCOMPRESSED on purpose: drive.c reads
  file bytes straight out of the archived AppVar via CEPackageChunkPtr; no RAM copy).
  Payload: u8 count, then 22-byte entries {name[16] PETSCII $A0-pad, u24 off, u24 size},
  then raw PRG blobs (incl. 2-byte load addr). `make disk DISK=path.d64`.
- src/drive.c = virtual 1541 fastload: drive_patch_kernal() plants opcode 0x02 (JAM) at
  KERNAL $F4A5 (default ILOAD target, orig bytes 85 93 verified first); cpu.c
  `case DRIVE_TRAP_OPCODE` calls drive_trap_load(): checks pc==$F4A6 + kernal page
  mapped (mem_rd_pg[0xF4].ptr[0xF4A5]==0x02), reads $B7/$BB filename ('*'/'?' wildcards),
  SA=$B9 (0 -> relocate to $C3/$C4), copies via disk_copy_to_ram: memcpy
  (LDIR) block spans split at chunk boundary, $1000 mem_low/mem_ram seam and
  16-bit wrap (was per-byte), sets $AE/$AF+X/Y=end, $90=0x40, A=0, C=0,
  emulates RTS.
  Not found: A=4, C=1. drive_tick() (called with the CIA block in c64_run) auto-types
  `lO"*",8,1`+CR (10 PETSCII bytes = exact kbd buffer size; L+shift-O abbreviation!)
  once frames>=60 && $CC==0 (cursor at prompt) && $C6==0.
- CEPackage now has 4 package ids (CE_PACKAGE_COUNT); DSK id "DSK".
- Rally Driver.d64: boot file loads at $02C6 overwriting the BASIC warm-start vector
  $0302 -> autostarts itself after LOAD"*",8,1; then chain-loads 1..5.RALLY DRIVER via
  KERNAL LOAD (SA=1). NOTE: games with custom IEC serial fastloaders will NOT work
  (no real drive/IEC emulation) - only KERNAL-LOAD-based multiloaders.
- drive.c has ZERO dbg_printf (bool-return IX lore). tests/test_rom_to_appvar.py
  rewritten for C64 kinds + D64 packer (was stale MSX cart/bios; run with
  `python -m unittest discover -s tests`). tools/d64_list.py = disk dir lister.

## Debug tracing (CEmu)
- Build with: make CC_DEBUG="" LD_DEBUG=""   (clears NDEBUG so dbg_printf prints to
  CEmu View->Console; plain `make` sets NDEBUG=1 and compiles it out). Do NOT use
  `make debug` here (it sets -DDEBUG=1, only matters for old Z80 core).
- dbg_Debugger() opens CEmu's debugger; dbg_WatchpointSet(addr,size,mask) sets watchpoints.
- main.c shows an ON-SCREEN boot screen (palette + gfx_PrintStringXY) listing which ROM
  AppVars were found (ECBAS00/ECKRN00/ECCHR00 OK or MISSING) and waits for a key, so you
  don't need the console to see boot status.

## BUILD GOTCHAS (critical)
- ROOT CAUSE OF ALL THE IX MISCOMPILES: CFLAGS had -O2. ez80-clang's register
  allocator at -O2 assigns IX/IXL/IXH as scratch INSIDE functions that use IX as
  their frame pointer (io_init built its table through a corrupted frame; io_emulate
  loaded the keypad MMIO address 0xF5001C into IX and wrote frame locals through it,
  spraying ports -> "Unknown debug Command" + NMI flash-write resets; vic_emulate had
  dozens of clobbers). FIX: use -Oz (CEdev standard, well-tested). CFLAGS now
  `-Wall -Wextra -Oz -fno-strict-aliasing`. After ANY toolchain/flag change, re-scan:
  every `ld ix,`/`ld ixl/ixh`/`lea ix` between `add ix, sp` and `ld sp, ix`/`pop ix`
  in obj/src/*.c.s is a red flag (PowerShell scan snippet in session history).
- NO dbg_printf IN EMULATION HOT PATHS: cpu_emulate/vic_emulate store stop reasons in
  globals (cpu_illegal_op/cpu_illegal_pc, vic_bad_mode); main() prints them after
  c64_run returns. main also prints the KERNAL reset vector after load_roms
  ("[boot] resetvec=fce2" expected) to validate ROM placement.
- CEdev source glob `*.c` is CASE-SENSITIVE. A file named MAIN.C (uppercase) is silently
  SKIPPED -> no _main -> linker produces a broken program that jumps to garbage and
  crashes immediately (we chased this as a "0x003b null deref" for a while). ALWAYS
  check src filenames are lowercase: look for `[cc] src/main.c` in the build output and
  `undefined reference to _main` at link time.
- Use ONLY CEdev's dbg_printf (sprintf into 0xFB0000). Do NOT write custom debug writers
  that poke 0xFB0000 directly byte-by-byte - writing raw to the debug port wrongly causes
  NMI "writing to flash at 0x003b" faults. dbg_printf/sprintf is heavy on stack but correct.
- IX-REGISTER TRAP (the c64_init crash): ez80-clang at -O2 keeps a loop/local accumulator
  in the 24-bit IX register across CALLS to functions that don't preserve IX (graphx,
  CEPackageValidate, printf/div helpers). IX's upper byte gets clobbered, and later pointer
  math uses a garbage 24-bit IX -> wild write (we saw 0x7b4c82, 0x0017). FIX: make such
  accumulators `static` (memory-backed) so the compiler can't keep them in IX. In emudore
  this was main()'s `int romMask` -> changed to `static int romMask`. Check the generated
  .s for `inc ix`/`add ix,de`/`ld a,ixl` used as a variable across a `call`.
- SPRINTF-IN-INIT TRAP (the mem_init crash): dbg_printf() -> sprintf(0xFB0000) makes the
  compiler load the debug-port constant -327680 (0xFFFB0000) into IX. If the calling
  function ALSO uses IX as its frame pointer (mem_init does), the sprintf clobbers the
  frame -> `ld sp,ix; pop ix; ret` returns to garbage. SYMPTOM in .s: `ld ix,-327680`
  before a `call _sprintf` inside a function that does `push ix; ld ix,0; add ix,sp`.
  FIX: do NOT call dbg_printf/sprintf inside mem_init / mem_alloc_var or any IX-frame
  function. Keep debug prints at the caller (c64_init) after the IX work is done.
- SPRINTF-IN-RETURN-PATH (the c64_init crash): c64_init returns bool in A via IX
  (`ld ixl,0/1` then `ld a,ixl; pop ix; ret`). Any sprintf between the work and the
  return does `ld ix,-327680` and clobbers the return value AND the saved IX -> crash.
  FIX: c64_init/load_roms must have ZERO dbg_printf/sprintf. Verify by grepping the
  generated obj/src/c64.c.s for `sprintf` (must be empty) after every build.
- main.c stores ROM-status as a static bitmask (romMask) since sprintf stack use can
  clobber plain locals across deep validate calls.
- For init-reset crashes with logs stopping at "[boot] c64_init", temporarily stage boot in main with explicit step logs (`init_mem`, `load_roms`, `reset cia1`, `reset cia2`, `reset vic`, `reset cpu`) to identify the exact crashing boundary without adding dbg_printf inside c64.c return paths.
- IXL-FRAME-POINTER MISCOMPILE (the io_init crash): ez80-clang -O2 can allocate an 8-bit
  loop temp to IXL (or even reload IX wholesale, e.g. `ld ix,_keyboard_matrix`) INSIDE a
  function that uses IX as its frame pointer (`push ix; ld ix,0; add ix,sp` ... `ld sp,ix`).
  The epilogue `ld sp,ix` then loads SP from loop data -> instant reset. Rewriting the loop
  (statics, no multiply) did NOT help; the only reliable fix was REMOVING the runtime loop
  entirely (src_column became a compile-time const uint16_t[344] table). Symptom to grep in
  the .s: any `ld ix,`/`ld ixl,`/`lea ix,` between `add ix, sp` and `ld sp, ix`.

## Performance (run loop)
- PAGE-TABLE MEMORY (the big CPU win): MemPage mem_rd_pg[256]/mem_wr_pg[256] biased
  host pointers (p[addr] direct; NULL = IO -> mem_read_io/mem_write_io slow paths).
  CRITICAL EZ80 DETAIL: table entries are a union padded to 4 BYTES (a 3-byte pointer
  stride makes clang call __imulu PER ACCESS for the *3 indexing!), and the page index
  is taken via a U16B union (.b.hi) because `addr >> 8` lowers to a __ishru helper
  CALL. Fixing both removed 230 __imulu + 224 __ishru calls from cpu.c (2-4 helper
  calls per emulated instruction - dominant cost). mem_read_word composes via U16B
  byte stores, not `| << 8`. Check with:
  Select-String obj\src\cpu.c.s -Pattern 'call\s+(__\w+)' after every hot-path change.
- -Oz OUTLINES `static inline` functions! Must use __attribute__((__always_inline__))
  on hot helpers. Applied to: memory.h accessors, cpu.c load_byte/push/pop/fetch_op/
  fetch_opw and all 8 addr_* addressing modes. Verify with:
  Select-String obj\src\cpu.c.s -Pattern 'call\s+_mem_|_addr_|_fetch' -> only
  mem_read_io/mem_write_io calls should remain (slow path). cpu_emulate switch
  compiles to a jump table (jp (hl)) at -Oz - verified.
- vic_emulate re-raise check is `if (irq_status & 0x0f)` inline, not vic_read_register.
- c64_run batches: inner `while ((int)(cpu.cycles - vic_raster_due) < 0) cpu_emulate()`
  then ONE vic_emulate per raster line; cia1/cia2 per line; io_emulate every 8 lines.
  vic_raster_due is the exported (non-static) next_raster_at from vic.c.
- WRAP-SAFE cycle compares are REQUIRED: cpu.cycles is 24-bit, wraps every ~17
  emulated seconds. Use `(int)(a - b) >= 0`, never `a >= b`.
- KERNAL RAM-test skip: load_roms patches $FD6C..$FD76 (LDX#0/LDY#$A0/CLC/JSR $FE2D/
  JMP $FD90) after verifying original bytes (INC $C2 at 0x1d6c, B1 C1 at 0x1d6e,
  20 2D FE at 0x1d8d). Saves ~1.3M emulated cycles of boot. Different KERNAL -> no-op.
- io_emulate rescans the keypad at REAL-TIME 60 Hz (546 ticks of the shared 32 kHz
  timer 1), matching the KERNAL's now-real-time IRQ scan rate; frame-based throttling
  missed short taps when emulated frames took >>16 ms real.
  2026-08-15: kb_Scan() MEASURED at ~1.3ms/call (busy-waits a full hardware
  scan) = 8% of wall at 60Hz. Replaced with kb_SetMode(MODE_3_CONTINUOUS) set
  LAZILY on first io_emulate call (boot/menu still kb_Scan after io_init,
  which would reset the mode) + raw kb_IsDown register reads; io_shutdown
  restores MODE_1_INDISCRIMINATE for TI-OS.
- CIA1 REAL-TIME PACING (the blink fix): cia1.c timers decrement by ELAPSED WALL
  TIME, not emulated cycles - hardware timer 1 at 32768 Hz (sys/timers.h,
  timer_Enable(1,TIMER_32K,TIMER_NOINT,TIMER_UP) in cia1_reset), elapsed C64 cycles
  = dticks * 30. Jiffy IRQ/cursor blink/key repeat run at true 60 Hz regardless of
  CPU-core speed. CIA2 stays cycle-based. Timer 1 is therefore RESERVED.
  CLAMP ADDED 2026-08-15: elapsed = min(dticks*30, cpu.cycles executed since last
  call) (static prev_cpu_cycles in cia1_emulate). UNCAPPED wall pacing LIVELOCKED
  slow scenes: measured 87k cyc/s (8% PAL) at READY = 750 real-time jiffy IRQs per
  emulated second; the KERNAL handler (~2500 cyc of slow-path IO reads) outran the
  16.7ms IRQ period -> foreground starved (the "3-4 fps" report). Games on VIC
  raster IRQs were unaffected (FINAL BLOW playable while BASIC crawled).
- CPU fast paths in cpu.c: fetch_op skips the IO NULL check (PC never in IO);
  push/pop/zp_read/zpx_read/zpy_read/zp_word/zp_write hit mem_low directly
  (zp_write keeps the $0001 bank-switch check); addr_indx/indy use zp_word.
- HOT-PAGE SPLIT: C64 pages $00-$0F (ZP, stack, OS workspace, default screen) live in
  static BSS ram_low[0x1000] (exported as mem_low), NOT VRAM - VRAM pays LCD-DMA
  contention per byte. Page tables map pages 0-15 -> mem_low, 16+ -> mem_ram (VRAM;
  its low 4KB unused). ALL no_io accessors + VIC renderer go through
  mem_host_ptr(addr) = (addr<0x1000 ? mem_low : mem_ram)+addr. NEVER index mem_ram
  directly for addresses that can be < $1000.
- c64_run cadence: cpu_run per raster line; CIA every 4 lines; io_emulate every line
  (internally wall-clock throttled), so slow startup cannot starve keypad scans.
- cpu.h exports cpu_run(until_cycles) (NOT cpu_emulate): cpu_step is a static
  always_inline single-caller inside cpu_run, so the whole raster-line batch runs in
  ONE function frame (no per-instruction call). c64_run calls cpu_run(vic_raster_due).
- VIC adaptive frame skip: frames < 300 render 1-in-16, but ALSO render immediately
  when io_emulate requests it (key matrix changed). After warm-up: render on request
  or every other frame. This avoids "slow at start, better later" typing lag.
- io_screen_draw_border paints BLACK (letterbox blends into bezel); draw_rect +1
  to match glyph x+1 emission. main clears screen before c64_run; boot text
  centred. NOTE 2026-08-15: a border-colour version + SX-64 squeeze experiment
  were both discarded by another workspace revert; user then said keep black
  ("don't worry about the purple parts"). Do NOT reintroduce.
- BORDER/OFF-LCD SKIP (2026-08-15, vic.c): process_line does pixel work ONLY
  for rasters in the text window (VIC_G_FIRST_LINE..VIC_G_LAST_LINE); the
  letterbox bands + off-LCD lines are never repainted (static black from
  main's gfx_FillScreen(0) before c64_run — nothing may write to LCD rows
  0-19/220-239 during the run). io_screen_draw_border now only called for
  screen-off (DEN=0) window rows. draw_sprite/draw_mcsprite return early on
  y_in_border instead of writing a border_colour overlay (border covers
  sprites; the old overlay leaked game border colour into the black bands).
  Collision semantics unchanged (border rows never latched collisions).
- FLUSH SPAN SKIP (2026-08-15, vic.c flush_lines): when sprites are idle
  (sprite_enabled==0 AND all spr_row==0xff -> sprite_tick provably no-op),
  whole line spans are skipped: unrendered frames set pending_line=upto
  immediately; rendered frames jump over the border bands (<G_FIRST_LINE,
  >=G_LAST_LINE) and only walk window lines. SAFE because every VIC register
  write calls vic_render_catchup() BEFORE the write lands, so sprite state
  cannot change mid-span. The emulated 6502 still runs all 312 lines (border
  lines are real game time; waitloop warp handles idle polls).
- FAST CHAR RENDERER (vic.c draw_raster_char_mode): resolves all source pointers ONCE
  per line (screen row + colour RAM = contiguous 40-byte runs in mem_ram; 2KB char-gen
  block is entirely in or out of the char-ROM window -> one check) and writes pixels
  directly into the framebuffer row via io_text_row(y) (io.c accessor returning
  row_ptr[dy] or NULL). memset(row, bg, 320) then unrolled 8-pixel bit tests per char.
  Old per-pixel draw_char/draw_mcchar + get_char_data are DELETED. vic_emulate only
  calls io_screen_draw_border for non-text rows (text rows repaint fully themselves).
  Bitmap + sprite paths still use per-pixel io_screen_update_pixel (optimize when
  games need it).
- 6510 cycles: INC/DEC now tick correctly; branches +1 taken / +2 page-cross.

## Banking bug fixes (memory.c mem_setup_banks) - REAL C64 PLA RULES
- $D000 area: RAM whenever LORAM=0 && HIRAM=0 (regardless of CHAREN)!
  Only when (loram||hiram): charen ? IO : char ROM. Old code mapped char ROM
  for $01=$30/$38 -> corrupted FINAL BLOW's depacker stream reads at $D000.
- Char ROM window covers ALL of $D000-$DFFF (pages 0xd0-0xdf), not just $D3FF.
- VIC colour regs ($D020-$D02E) latch only low 4 bits: vic_write_register
  masks & 0x0f (games write $F6/$F7 -> were rendering invalid palette >15).

## VIC modes (vic.c)
- YSCROLL SUPPORT (char + bitmap renderers): line = rstr -
  (VIC_G_FIRST_LINE - 3) - vertical_scroll(); out-of-window lines render
  bg0. Default yscroll=3 reproduces the old math exactly. The renderer
  previously ignored $D011 bits 0-2 -> games positioning text via yscroll
  got sliced glyph rows (FINAL BLOW select-screen names under black bands).
- RASTER IRQ TIMING: vic_emulate delivers the raster IRQ at the START of the
  target line (checked after incrementing the counter, via noinline
  raster_irq_check()), so handler writes (sprite multiplexers, scroll/HUD
  splits) run during that line's CPU batch BEFORE the line is drawn. The old
  pre-draw check fired every raster IRQ one line late -> FINAL BLOW's
  multiplexed boxer sprites scattered.
- EZ80-CLANG CRASH LORE: "unable to legalize G_ADD (s14)" in vic_emulate =
  optimizer range-narrowing the raster counter into the renderer's masked
  14-bit address adds. FIX: get_raster_counter() is __attribute__((noinline))
  so its result stays opaque. Reshuffling code between the counter read and
  the address math can resurface this crash.
- 38-COLUMN MODE (cr2 bit3=0): render ALL 40 columns anyway - real VIC still
  fetches them and only overlays border on ~7 edge px, which the LCD crop
  hides. The old c0=1/c1=39 narrowing chopped visible edge columns (FINAL
  BLOW showed "YNAMITE JOE"). Sprite side-border strip removed too; only the
  24-row top/bottom border overlay remains.
- GM_EXT_BG (ECM text) implemented in draw_raster_char_mode: glyph = code&0x3f,
  cell bg = bgcolor[code>>6], all 8 px written per cell.
- ILLEGAL modes (ECM+BMM etc) render BLACK rows and CONTINUE (real VIC does
  black; never return false - FINAL BLOW passes through mode 5 in its raster
  IRQ and used to hard-exit the emulator). vic_bad_mode is now informational
  only ("[vic] illegal gfx mode N seen").

## Palette-tag pixel scheme (io.c + vic.c)
- Palette entries 16-63 MIRROR colors 0-15 (gfx_palette[i]=c64_palette[i&15])
  -> framebuffer bytes carry invisible tags: IO_TAG_FOREGROUND=0x20,
  IO_TAG_SPRITE=0x10 (io.h). All renderers tag fg pixels (char/ECM color,
  MC-char px 10/11 incl bg2, MC-bitmap lut[2]/[3], hires fg; asm row has
  `or a,0x20`; self-test expects the tag). MC pixel-pair 01 = bg priority,
  untagged. io_screen_update_pixel masks 0x3f; io_screen_peek added.
- sprite_pixel(): under&0x20 -> $D01F latch + hides behind-priority sprites;
  per-line sprite_line_mask[] gives both-bits $D01E sprite-sprite collisions.
- $D01E/$D01F are real latched regs cleared on read (previously fell through
  to default 0xff = "all sprites always colliding" - game logic breaker).
- Frame skip: collisions only latch on rendered frames, but latch until read.
- BLINK ALIASING FIX: wall-clock pacing commits each render on ALTERNATING
  emulated frame parity (render_pending + render_parity toggle in
  vic_emulate). A fixed-phase ~12fps sampler can lock onto the "off" phase
  of sprites that games blink every frame (menu cursors etc.), making them
  permanently invisible on the LCD.
- Sprite fast path: draw_sprite/draw_mcsprite resolve io_text_row(y) ONCE per
  sprite line, write dst[fx - IO_CROP_X] directly (sprite_pixel_at inline,
  no io calls), and SKIP zero data bytes (8 transparent px at once). Border
  clip tests hoisted out of the pixel loop. IO_CROP_X/Y moved to io.h.
- MC SPRITE X-EXPANSION: draw_mcsprite ignored $D01D double-width (upstream
  emudore limitation carried into the port) -> X-expanded multicolor sprites
  (FINAL BLOW's boxers) drew at HALF width = "missing chunks". Fixed: pair
  position * swid, 2*swid px per pair; also gained the top/bottom border
  overlay the hires sprite path already had.
- FINAL BLOW in-game facts (runtime [spr]/[vic] dump): mode=1 MC CHAR
  (playfield is char gfx, charset $4800 in bank $4000, screen $4000,
  cr2=d7 -> xscroll=7 + 38col), 8 MC sprites UNexpanded, prio=00 (palette
  tag/priority theory DISPROVEN; pal entries verified healthy), rirq caught
  mid-chain (=176) -> boxers use a sprite MULTIPLEXER walking down the
  frame; snapshot: 2 sprites y=64 (heads) + 6 parked y=159. Scatter =
  multiplex schedule vs our line-batch timing.
- VIC WRITE TRACER: vic_wtrace[64] ring ({raster line, reg, val}) records
  sprite/control/d018 writes; main dumps tail at exit as "[wtrace]". Use to
  reconstruct a game's multiplexer schedule.
- STATEFUL SPRITES (the multiplexer fix): wtrace proved FINAL BLOW rewrites
  sprite Y at line 110 while the y=96 strip displays until 117 -> stateless
  per-line Y comparison chopped strip tails and displaced them (the
  "scattered boxers"). sprite_tick(rstr, draw): Y match ARMS the sprite
  (spr_row=0), it then displays all 21 rows regardless of later Y writes,
  X read live per line (real VIC semantics), dh via spr_phase, disable
  clears. State advances EVERY line incl. unrendered frames (arming
  continuity); pixels drawn only when rendering. Replaced
  draw_raster_sprites.
- FINAL BLOW multiplex schedule (wtrace): heads y=64 @46-47; body strips
  y=69 @67, y=96 @90-94, y=117 @110-117; d018 charset splits @45 (=04) and
  @106 (=02); raster irq chain via $D012 @88,110,120.
- SELECT SCREEN schedule: text/portrait sprite strips (sprites 2-7) chained
  Y=138@136, 159@157, 180@176, 201@199 (21 apart, seamless re-arm);
  bottom rows Y=222/230 @221 with dw=ff@222 + dh=f0@223, cleared @45.
  d018 alternates 02/04 (charset flip). Game hammers $D011=1b every other
  line -> tracer now EXCLUDES reg 0x11 and INCLUDES 0x1c + 0x25-0x2e
  (sprite colours/MC are rewritten per strip - needed to diagnose the
  still-missing name text). In-game boxers CONFIRMED assembled after the
  stateful sprite engine.
- SELECT SCREEN root cause (found via colour-inclusive wtrace): screen is
  MC TEXT mode (cr2=d0), portraits = 8 sprites, name text = custom charset
  at $D000-$D7FF RAM UNDER I/O (bank 3, d018 flips 02<->04 mid-frame at
  lines 113/178). Game never rewrites sprite colours per frame. Two memory
  bugs fixed: (1) colour RAM was mem_ram[0xd800] shared with RAM-under-IO ->
  now dedicated mem_color_ram[0x400] array, pages d8-db map to it when I/O
  banked in, VIC reads it directly (both renderers); (2) SID writes
  ($D400-$D7FF) wrote through to RAM underneath, corrupting the charset ->
  pages d4-d7 now NULL/IO, mem_write_io discards SID, mem_read_io returns
  LCG for OSC3/ENV3 ($D41B/1C, RNG source) else 0. Asm CPU core inherits
  both via shared page tables.
- GEOMETRY made hardware-true (was +5 lines late): VIC_G_FIRST_LINE 56->51,
  G_LAST 256->251, FIRST_VISIBLE 14->9, LAST_VISIBLE 298->293,
  SPRITES_FIRST_LINE 6->1, SPRITES_FIRST_COL 18->19. Text top = raster 51
  (yscroll=3), sprite Y=50/X=24 = text top-left, LCD centring preserved
  (text at LCD y 20-219). Motivation: game $D012 splits (charset flips @113
  /178) hit the picture 5 lines off -> garbage band across boxers.
- Sprite X pixel math fixed: hires leftmost bit was emitted at +1
  ((8-j)*swid), MC leftmost pair at +2 ((8-j*2)) -> now (7-j)*swid and
  ((3-j)*2)*swid, leftmost at offset 0, consistent across modes.
- FIGHT screen fully decoded (colour+1b/1c trace): sprites 0,1 = green HUD
  counters y=64 x=63/247 (MC, 1c=03 @45); sprites 2-4 = green score digits
  strip y=69 x=144-200 HIRES colour 5; boxers = sprites 2-7 multiplexed in
  strips y=96,117,138,159,180,201 (1c=ff @96, colours rewritten per strip
  @94/117/138/159/181/201, X rewritten only for 96+117 strips - lower strips
  reuse X); bottom HUD y=222/230 (1c=0f, 1d=ff, 17=f0 @222-223, cleared
  @45). $D01B prio NEVER written (stays 0). Game writes colour regs with
  junk upper bits (2c 37) - we mask &0x0f on store, OK. No MC-flag timing
  bug found; left-boxer striping still unexplained -> added [sptr] (C3F8
  pointer table) + [pix] (framebuffer rows LCD y=90/110/130, x0-95, hex with
  tag bits) probes to main.c exit dump to get pixel ground truth.
- vic.c now has DEFERRED renderer: flush_lines/process_line/pending_line,
  8-line batched draw_char_row + sprite_tick x8 fast path; register writes
  call vic_render_catchup() only when value actually changes (idempotent
  writes skip flush); collision reads flush first. vic_bad_mode is
  informational only (renders black, never exits) - exit msg fixed to say so.

## Hybrid eZ80 CPU core (src/cpu_core.s)
- DISPATCH SURGERY (2026-08-15, measured by [bench] 650k-cycle asm loop):
  round 1: push/pop reg moves -> lea (main_loop expiry `lea hl,ix+0`,
  FETCH_A `lea de,iy+0`, CYC `lea ix,ix-n` [preserves flags+DE now],
  HL16_TO_DE reg moves, do_branch de-RAM'd): 219.8k -> 250.1k cyc/s (+14%).
  round 2: LAZY N/Z FLAGS (250.1k -> 270.2k cyc/s, +8%; READY 148k = 15%):
  SET_NZ = 2 stores to last_z/last_n bss (clobbers nothing); zf/nf
  materialized only in sync_out; seed_lazy (last_z=zf^1, last_n=nf ror 1)
  at batch entry + fallback re-entry; flags_pack/unpack use lazy bytes;
  BEQ/BNE = BR_Z, BMI/BPL = BR_N (jp m/p); TWO bytes because BIT takes Z
  from a&m but N from m (bit_core rewritten). br_old deleted.
  round 3: FETCH_DE16 fast path - hi operand byte
  reuses the lo fetch's host pointer (FETCH_A ends with hl=host ptr,
  de=6502 addr of fetched byte; e==0xFF -> page cross -> full second walk
  via `7f` label); jsr_h return addr via `lea hl,iy-1`. Round 3 bench:
  282,340 cyc/s (+4.5%); READY flat at ~148k (its idle loop is zp-heavy;
  abs-heavy game/KERNAL code benefits more). SURGERY CONCLUDED: dispatch
  is now ~60 code bytes/instruction at ~9.4 clk/byte fetch cost = fetch-
  bandwidth-bound; remaining tricks (mlt dispatch, code-page cache w/ bank
  invalidation) are <5% each. Next structural step would be JIT. Totals
  2026-08-15: core bench 219.8k -> 282.3k (+28%), READY 87k -> 148k (+70%).
  Self-test+probe must validate every round (BIT $16 added to selftest prog
  at $0462 for the lazy bit_core; spin moved to $0464).
- FALLBACK DIAGNOSTICS (2026-08-15): _cpu_fallbacks (u24) + _cpu_fallback_op
  in cpu_core.s bss, printed by main as "[cpu] fallbacks=N last-op=XX".
  Opcode capture lives in op_fallback (COLD path, re-reads the byte after
  the pc rewind) - first version stashed it per-instruction in dispatch and
  cost -4.5% on the bench. MEASURED: FINAL BLOW load+run = 3129 fallbacks
  over 37M cycles -> game slowness is honest raw compute, NOT fallbacks;
  1 fps during depack is expected at interpreter speed.
- ROM JIT (2026-08-15, blocks #1-#2): architecture = control-transfer hook:
  do_branch/jmp_h/jsr_h all end with `cp page,0xE0 / jp nc, jit_dispatch`
  (branch: a=scr3+1; jmp/jsr: `ld a,iyh`). jit_dispatch chains page compares
  (E5 -> e5cd, F6 -> udtim); blocks match `ld a,iyl`, check their bit in
  _cpu_jit_kernal (BITMASK now: bit0=e5cd, bit1=udtim; c64.c
  detect_jit_blocks verifies exact KERNAL bytes at load) and HIRAM in
  mem_low[1]. Block $E5CD (wait-for-key): if NDX==0 do one iteration's side
  effects (A=0, lazy z=1/n=0, BLNSW=0, AUTODN=0), `ld ix,0/lea ix,ix-1` =
  batch over; NDX!=0 -> interpret. Block $F69B UDTIM: X=0, native 24-bit
  jiffy inc at mem_low $A2/$A1/$A0 with exact path cycles 10/17/21, lazy NZ
  from last INC, exit pc=$F6A7 (`ld iy,0xF6A7`), rollover compare
  interpreted. RTS hook + batch-entry hooks NOT added yet (no block needs
  them). cpu_jit_kernal=0 during selftest (set in load_roms, selftest runs
  earlier); boot probe (30k cyc, no IRQ delivery) reaches neither address so
  asm-only warp can't diverge it. iyl/iyh access verified in objdump (fd 7d).
  NEXT: build-time AOT translator for IRQ chain/SCNKEY/CHROUT; RAM-code JIT
  needs write invalidation (games depack = RAM code, untouched by ROM JIT).
- OFFSET BUG CAUGHT BY [jit] HIT COUNTERS (2026-08-15): $E5CD - $E000 =
  offset 0x05CD, NOT 0x15cd (anchored on $F4A5->0x14A5 and nibble-shifted).
  detect_jit_blocks AND drive.c ready_pc_detect both had 0x15cd -> the e5cd
  block never armed ([jit] mask=02 e5cd=0) and the READY fast-LOAD pc-detect
  NEVER worked (silently used the 60-frame fallback). UDTIM's 0x169b was
  correct ($F69B-$E000). Both fixed to 0x05cd. LESSON: KERNAL offsets =
  addr-0xE000; confirm feature ENGAGEMENT with a hit counter, not absence
  of errors. [jit] line prints mask + per-block hit counts (_cpu_jit_hits/
  _cpu_jit_hits2).
- "READY at 2fps" solved: exit pc=$EEAC = manual LOAD with no device hung
  in SEI'd IEC timing loops polling $DD00. Fix: cia2 PRA read ->
  vic_poll_check(3) (poll_key[] grown to 4). Safe: no drive attached, IEC
  polls only await timeouts. Also explains 64 UDTIM hits in 264s (SEI
  blocks jiffy IRQs during the hang). In-game FINAL BLOW measured 12-15
  emulated fps (24-30% PAL) with the warp stack. READY with e5cd block LIVE
  ~47 fps (14579 warps/~1s sit; fallbacks 110). DISKLESS LOAD FIX: the
  $F4A5 trap is now planted in EVERY boot mode (main calls
  drive_patch_kernal unconditionally; disk_size gate removed from patch);
  drive_trap_load with !disk_size returns A=5 DEVICE NOT PRESENT + carry +
  RTS instantly (was: return false = treated as illegal opcode - old code
  simply never planted diskless). NEW drive_disable() (drive.c/h): "BASIC
  (no disk)" menu choice zeroes disk_size - previously with a DSK present
  that choice still auto-typed LOAD into an UNPATCHED kernal -> IEC hang.
  NOTE drive.h got reformatted to doxygen comments (user edit) - re-read
  before editing.
- FINAL VALIDATION (2026-08-15): READY = 232,460 cyc/s (23% PAL) - ABOVE
  the 220k bare-interpreter bench from the morning, in the full system.
  e5cd warps = 87% of raster lines; udtim tracks jiffy correctly (54/emu-
  sec, wall-clamped); fallbacks ~64/run; per-line cpu cost 8.5 -> 4.1
  ticks. READY is now RENDER-BOUND: renders ~107ms each at 1-in-4 pacing =
  ~34% of wall (draw path expected ~20ms from VRAM bench: possible 5x waste
  in flush/draw_char_row worth investigating someday). DAY TOTAL: READY
  87k -> 232k cyc/s (2.7x). Release built 25,504 bytes. NEXT CANDIDATES:
  render-path audit; AOT KERNAL translator; RAM JIT (needs invalidation).
- ~70 hottest 6510 opcodes in asm; everything else bridges to C via
  cpu_step_slow() (exported non-inline wrapper of cpu_step). cpu_run picks
  the asm core only when cpu_asm_ok (set by cpu_asm_selftest).
- Register plan: IY = 6502 pc (top byte 0), IX = cpu.cycles. Both callee-
  saved so C helper calls preserve them; op_fallback rewinds pc by 1, syncs
  pc/cycles to CpuState, calls cpu_step_slow, re-syncs back.
- CpuState offsets pinned by _Static_asserts in cpu.c: pc0 sp2 a3 x4 y5 cf6
  zf7 idf8 dmf9 bcf10 of11 nf12 cycles13. Bool flags stored as 0/1 bytes.
- Mem access replicates page tables: 24-bit NULL test = add hl,de/or a/
  sbc hl,de (Z if null) -> C io call; $0001 writes call mem_setup_banks;
  zp/stack via (_mem_low). BCD ADC/SBC falls back to C (dmf checked first).
- cpu_asm_selftest(): same 6502 program ($0400: JSR/RTS, branches, (zp),Y,
  abs,X/Y, INC abs = fallback-bridge test, JMP-self spin) through BOTH cores,
  compares regs+flags+cycles+memory; asm enabled only on exact match.
  Boot line: "[boot] cpu core: asm (self-test OK)".
- BUG CAUGHT BY OBJDUMP REVIEW: jsr_h read target into DE then called
  push6502 which clobbers DE -> fixed with push de / pop iy. Clobber lists
  matter: push6502/pop6502 kill de+hl, CYC macro kills DE.
- pc wrap $FFFF->$0000 unhandled in asm fetch (never happens in sane code).
- ON-CALC FAILURE RESOLVED: the boot-divergence probe (budget 44, states
  identical EXCEPT pc: C=fcf2 ASM=0000, cycles identical) proved the FIRST
  IO WRITE (STX $D016 -> call _mem_write_io) CLOBBERED IY. **CEdev ez80-clang
  C functions preserve IX but NOT IY** (IX-held cycles survived the same
  call). Fix: push iy/pop iy around ALL C calls in cpu_core.s (read_de io,
  write_de io + bank, ST_ZP_C bank, inc/dec_zp bank). op_fallback needs none
  (reloads IY from cpu.pc). Self-test extended with STA $01 / STA $D020 /
  LDA $D020 so the C-call paths are always covered.
- OPCODE COVERAGE ROUND 2 (depack/ROM-processing speed): ~45 new handlers
  via shared cores (adc_core/sbc_core/cmp*_core/shift cores/bit_core/
  rmw_inc_de+rmw_dec_de double-write) + loader macros (LOAD_B_ZP/ABS/ABSI/
  INDY, LD_ZPI): INC/DEC abs+absx, shifts zp+abs, BIT zp/abs, ADC/SBC
  zp/abs/absx/absy/(zp)y, AND/ORA/EOR abs/absx/absy/(zp)y, CMP absx/absy/
  (zp)y, CPX/CPY zp/abs, LDX absy/zpy, LDY absx/zpx. FINAL BLOW's depacker
  (INC abs per byte) is now fallback-free. Still fallback: BRK, RTI,
  PHP/PLP, JMP ind, (zp,X) ops, RMW zpx, illegals, BCD ADC/SBC.
- OPCODE COVERAGE ROUND 3 (2026-08-14): PHP/PLP/RTI (flags_pack/flags_unpack
  helpers: P byte = cf|zf<<1|idf<<2|dmf<<3|0x30|of<<6|nf<<7, unpack leaves
  bcf alone, exactly mirroring C cpu_flags/set_flags), JMP indirect (16-bit
  pointer wrap via scr3 DEU clear), all 8 (zp,X) ops (LOAD_ADDR_INDX macro,
  zp+1 wraps via 8-bit inc c), RMW zp,X shifts (RMW_ZPX: push bc/hl around
  core call - cores clobber b&c via SET_NZ; keeps $0001 bank latch),
  INC/DEC zp,X, shifts abs,X (SHIFT_ABSI, single-write like SHIFT_ABS).
  Self-test program extended (sub moved $0450->$04A0; new coverage at
  $0444-$0465: SEC/PHP/CLC/PLP, LDA/STA (zp,X) via $12 ptr, ASL $16 zp,X,
  ASL $0506 abs,X, JMP ($0018)->$0458, PHA/PHA/PHP/RTI->$0462 spin; ZP
  inits $16=0x41, $18/$19=$0458 ptr; ref_mem[8] adds $16). STILL FALLBACK
  (intentional): BRK, illegals, BCD ADC/SBC, and opcodes 0x02/0x12 = DRIVE
  TRAP opcodes - those table slots must NEVER get asm handlers.
- The debug-build boot probe validates all handlers against the C core over
  30000 cycles of real KERNAL boot on every CEmu run.
- cpu_run's C fallback calls cpu_step_slow() (NOT inline cpu_step) so the
  huge opcode switch exists ONCE in flash (-10 KiB, 41.5k -> 27.5k binary).

## Palette slots (io.c/io.h) [FULLY DELETED 2026-08-14]
- Slot code (IO_SLOT_* defines, io_set_slot_color, slot_shadow) removed from
  io.c/io.h after entries 64-68 showed as black holes in the pattern.
  io_reload_palette = one loop: gfx_palette[i] = c64_palette[i & 15] for all
  256 entries (every column matches). io_palette_probe kept for [pal] dump.
- Historical (superseded) slot usage notes:
- Entries 0x40-0x44 = IO_SLOT_BG0..BG3, IO_SLOT_BORDER. Background pixels are
  written as SLOT indices, not colour values; $D020-$D024 writes call
  io_set_slot_color -> ONE palette write recolours the whole screen instantly
  (even on skipped frames). Used by: char-row memset, MC-char 00/01, ECM cell
  bg (0x40+(sc>>6)), MC-bitmap lut[0], vic_row_bg for the MC asm row, sprite
  top/bottom border overlay. Hires bitmap bg is per-cell -> NOT slotted.
  io_screen_update_pixel masks 0x7f. vic_reset syncs all 5 slots.
- Palette robustness: io_reload_palette() fills ALL 256 entries with
  c64_palette[i&15] (any stray framebuffer byte shows a legal C64 colour),
  then re-applies the 5 slot_shadow[] live colours; main calls it right
  before c64_run. io_palette_probe() + "[pal]" exit dump diagnose runtime
  palette clobbering (user saw gray mirrors on-calc while io_init source was
  correct - suspected stale binary or runtime overwrite).

## WORKSPACE REVERT INCIDENT (2026-08-14)
- vic.c, io.c, io.h, vic_hires_row.s reverted to old versions and
  vic_mc_row.s became EMPTY (discarded editor chat diffs?); cpu_core.s,
  cpu.c, drive.c, memory.c survived. All re-applied + rebuilt.
  LESSON: if a boot line goes missing from user logs, grep feature markers
  across src/ to detect reverts BEFORE layering new edits.

## TIBoyce techniques ported (2026-08-14, vic.c/cpu.c/cpu_core.s)
Studied github.com/calc84maniac/tiboyce render.asm/waitloop.asm. JIT + write-
time tile decode NOT portable (per-cell colour RAM, unhookable RAM writes).
Ported: lazy catch-up rendering + waitloop time-warp.
- LAZY RENDER (vic.c): lines are NOT drawn per raster line anymore.
  pending_line = first undrawn line; flush_lines(upto) draws stripes.
  Triggers: any display-affecting VIC reg write (vic_render_catchup(),
  exported - also called from cia2.c on PRA bank-bit changes), $D01E/$D01F
  collision reads, and frame wrap (flush(312) before io_screen_refresh,
  then pending_line=0). IDEMPOTENT writes skip the flush (FINAL BLOW hammers
  $D011 with same value every other line - would chop stripes); comparison
  via vic_read_register EXCEPT 0x1e/0x1f (read clears latches!) which
  ignore writes. Writes to 0x12/13/14/19/1a never flush (no pixel effect).
- BULK CHAR ROW (draw_char_row, noinline): register-stable stripes covering
  a full aligned glyph row (line&7==0, all 8 in text window+stripe) render
  8 scanlines per cell fetch (screen code/colour/mode branch 1x not 8x);
  supports CHAR/MC_CHAR/ECM; data==0 rows skipped (spaces!). Partial
  stripes + bitmap modes use old per-line path via process_line().
  Renderers now read `cur_line` static instead of get_raster_counter()
  (keeps the s14-legalize crash barrier; draw_char_row takes line param).
  sprite_tick still runs per line inside flush (order-correct: sprite regs
  stable within a stripe by construction).
- WAITLOOP SKIP (cpu_warp, cpu.h/cpu.c/cpu_core.s/vic.c): 2nd read of
  $D011/$D012 on the SAME raster line (key = line | frame<<9) sets
  cpu_warp=1; consumed at BOTH cores' IO-read choke points (C load_byte
  after mem_read_byte; asm read_de after call _mem_read_io) -> cycle
  counter warps FORWARD-ONLY to batch end (C: run_until static set in
  cpu_run/cpu_run_c; asm: until_cyc). Also JMP-to-self detection in both
  jmp handlers (C jmp(): t==pc-3; asm jmp_h: lea hl,iy-3/sbc hl,de) warps
  to batch end - games idle in `jmp *` waiting for IRQ. Poll loops now cost
  ~2 iterations/line instead of ~10. run_until set in cpu_run BEFORE asm
  dispatch so op_fallback's C reads use the right target. Self-test's
  JMP $0444 spin exercises warp in both cores -> both land at exactly
  until+3 cycles (asymmetry would fail the boot self-test).
- Known coarseness: screen-RAM writes mid-frame (racing the beam without
  touching VIC regs) now render at flush granularity, not line granularity.
  CIA2-timer-driven mid-frame effects without VIC writes would also miss
  splits. No game seen doing either.
- SID WRITE BIT-BUCKET (memory.c mem_setup_banks): write pages d4-d7 point
  at a 256-byte static BSS bucket (biased sid_bitbucket-(i<<8)), NOT NULL ->
  music-player SID writes ride the FAST page path (no mem_write_io C call,
  no IY save in asm core). READ pages stay NULL (OSC3/ENV3 LCG RNG readouts
  need mem_read_io). mem_write_io's SID discard branch is now dead code but
  kept defensively. Sound was never emulated; this removes the dispatch cost.
- BUILD SIZE LORE: an incremental make once reported 30947 bytes (stale
  partial link); `make clean; make` returned 23212. Distrust size jumps
  from incremental links - clean-rebuild before investigating.
- COUNTDOWN CYCLE COUNTER (cpu_core.s, 2026-08-14): IX is now
  REMAINING-1 countdown (until - cycles - 1), NOT absolute cycles.
  main_loop expiry check = push ix/pop hl/add hl,hl/jp c (carry = sign
  bit) instead of the old 7-instr 24-bit compare (~8-10 F saved per
  emulated instruction). CYC n does `ld de,-n; add ix,de`; do_branch
  extras are -1/-2. Absolute cycles reconstructed ONLY at sync_out and
  op_fallback re-entry (until_cyc - 1 - IX). Warp sites simply `ld ix,0`
  (batch ends after current op). C-side warp_to_batch_end targets
  run_until - 1 so both cores land at exactly until-1+n after a warped
  instruction (self-test parity). RULE: any new asm handler must never
  treat IX as absolute cycles; C helpers called mid-batch must not read
  cpu.cycles (it is stale during asm runs - always was).
- TIBoyce ROM answer: romgen does NO code transformation - it splits into
  <=16KB pages (trailing-filler trimmed), subset-sum bin-packs pages into
  appvars + title metadata, all for IN-PLACE archive mapping; speed comes
  from the on-calc JIT. emudoreCE already reads D64 in place and keeps
  ROMs in RAM (faster than flash) - nothing to gain ROM-side.

## Game loader forensics (tools/verify_finalblow.py, disasm_part1.py)
- FINAL BLOW CONFIRMED WORKING on-calc after the banking fix + auto-RUN.
- eZ80 hires bitmap row asm (src/vic_hires_row.s, self-tested at boot via
  vic_row_asm_selftest) re-enabled (hires_asm_enabled=true in vic.c) after
  the bisection proved the garble was banking, not the asm.
- MC bitmap row asm (src/vic_mc_row.s, _vic_expand_mc_row): per-cell 4-colour
  LUT in a 256-ALIGNED .bss scratch so each 2-bit code indexes with `ld e,a`
  (D/DEU keep the page). NO SHADOW REGISTERS (first version used exx and
  FAILED the on-calc self-test - OS ISRs are not guaranteed to preserve
  shadows): IX=screen, IY=bitmap, colram self-increments through mc_cr BSS.
  Extra globals vic_row_colram/vic_row_bg. vic_row_asm_selftest() returns a
  bitmask (bit0 hires, bit1 mc); each row enabled independently; boot prints
  "[boot] asm rows: hires=OK mc=OK".
- Frame pacing: after 300 boot frames, render on wall-clock interval
  (timer_Get(1) 32kHz, ~12 fps) instead of every-other-frame; key activity
  still forces immediate render.
- disasm_part1.py now takes --file N --disk path + hex ranges (any PRG entry).
- FINAL BLOW (FINALBLW.D64, 1 PRG $0801-$A598): BASIC stub SYS 2074=$081A ->
  copies depack routines from $07BC+X to stack $0106+X (source $0801-$08B5
  overlap = real code; TXS=$F9; RTS-into-stack trick; end falls through
  JSR-clobbered bytes into JSR $C000 at $01FD). Init loop at $082B is a
  RELOCATOR: moves packed stream from $08B6-$A597 UP to $63BE-$FFFF
  (backwards copy, dst hi skips page 0). Then RLE-depacks ('M'=$4D escape:
  t>=3 RLE run, t==2 literal M, t<2 new dest lo/hi, hi=0 end) from $63BE
  through $D000+ (why the banking fix matters) down to $080B-$C634.
  Expected $c000 head: a9 93 20 d2 ff. NO drive access after load ->
  no 1541 emulation needed for this game.
- drive_tick auto-types RUN (stage 2, autostart_done==1 && trace_count==1)
  after single-file loads; multi-loaders self-start via $0302 overwrite.
- HIDDEN BOOT (2026-08-15): choosing a disk file skips showing the KERNAL
  boot/LOAD/RUN screens. vic.c boot_hidden flag (vic_set_boot_hidden) forces
  render_frame=false; main sets it + prints "loading..." when fileCount>0.
  drive.c unhide_frame = vic_frames()+12 after a served LOAD (+1 on FILE NOT
  FOUND), lifted in drive_tick (also vic_request_render). Safety: vic_emulate
  auto-unhides at frame_count>=900. Cannot truly skip KERNAL boot: programs
  need KERNAL-initialized ZP/vectors; boot already runs unthrottled.
- FAST AUTOLOAD (2026-08-15): drive_tick types LOAD the moment READY is
  reached: ready_pc_detect (set in drive_patch_kernal after verifying KERNAL
  bytes A5 C6 85 CC @0x15cd + F0 F7 @0x15d4 = $E5CD wait-for-key loop)
  accepts cpu.pc in $E5CD-$E5D5 with NDX/BLNSW==0. Replaced the fixed
  vic_frames()<60 gate (~1.2M wasted cycles = ~half of boot). Fallbacks:
  frame 120 if pc sampling misses, frame 60 if KERNAL bytes don't match.
  NOTE: more asm opcode coverage would NOT speed boot - the asm core already
  covers boot-hot opcodes; boot was dominated by this fixed wait.
- SPEED REPORT + WIDER WARP (2026-08-15): c64_total_cycles (unsigned long,
  c64.c/h) accumulates wrap-safe per-line deltas of cpu.cycles in c64_run;
  main times c64_run with timer_Get(1) (static speed_ticks, IX lore) and
  prints "[speed] N cyc in T ticks = C cyc/s (P% of PAL)" at exit
  (cps = cycles<<15/ticks; percent = cps/9852; PAL = 985248 cyc/s).
  vic.c raster_poll_check generalized to exported vic_poll_check(id) with
  per-stream keys: 0=$D011/$D012, 1=$DC00 (cia1 PRA read), 2=$DC01 (cia1 PRB
  read) - joystick/keyboard busy-polls now warp to batch end like raster
  polls. Separate streams so an IRQ handler touching CIA+VIC once each never
  false-triggers. cia1.c includes vic.h. $DC0D/$DD00 deliberately NOT warped
  (ICR read-clears + CIA1 is wall-clock paced; IEC polls unserved anyway).
- FPS OVERLAY (2026-08-15): io_fps_overlay(can_draw) in io.c (decl io.h)
  draws emulated-frames/sec (50 = full PAL) top-right in the black letterbox
  band (y=2), white on opaque black, manual digit formatting (no sprintf).
  Called at vic_emulate frame wrap with was_rendered && !boot_hidden
  (was_rendered = render_frame captured BEFORE the pacing block reassigns
  it); must redraw after every rendered frame since repaints erase it.
- PROFILER (2026-08-15): #ifndef NDEBUG c64_prof[4]/c64_prof_lines in c64.c
  (exported c64.h): 32kHz-tick buckets around cpu_run/vic_emulate/io_emulate/
  cia-block in c64_run; main prints "[prof] lines= cpu= vic= io= cia= ticks".
  Added because READY-screen throughput measured only ~90k cyc/s (9% PAL)
  EVEN AFTER the CIA livelock clamp - ~0.68ms host per raster line, ~30x
  slower than asm-core expectations; hog not yet identified. User also
  REVERTED the pre-c64_run key-drain loop in main.c (do not re-add).
- MEASURED GROUND TRUTH (2026-08-15, on-CEmu benches in main.c debug boot):
  timer 1 timebase VERIFIED honest ([cal] 16502/0.5s vs 16384 expected, via
  timer 2 TIMER_CPU counting 24M clocks; CPU confirmed 48MHz, plain RAM
  loops full speed). timer_Get(1) costs ~12us/call (~590 clk, peripheral
  bus). kb_Scan ~1.3ms/call (busy-waits hardware scan). ASM CORE CEILING =
  219,800 cyc/s = 22% PAL measured on bare INX/DEX/JMP loop with ZERO glue
  (~580 clk per 6502 instruction) - the old "50-100 F per instruction"
  belief was WRONG. In-loop cpu_run ≈ ceiling+10%. VRAM memset = 11.5ms per
  76800B frame (~7 clk/byte; VRAM is NOT slow). Full-frame char render ≈
  62ms = legit C-code cost (platform executes roughly 1-3M compiled C ops/s
  effective, ~13 clk/instr from wait-stated RAM - PC intuition 10x off).
  "Empty" per-line vic_emulate ≈ 37us = ~100 C ops, also legit.
  PROGRESSION at READY: 87k (CIA livelock) -> 126k (clamp + render cap +
  glue cadence) -> 133k cyc/s debug (13% PAL); release ≈ +8% (no profiler
  timer_Gets). FIXES SHIPPED: render needs >=4 emulated frames spacing
  (vic.c; render_requested bypasses; at 50fps = 12.5fps unchanged); c64_run
  cadence io every 8 lines / cia1 every 16 / cia2+drive every 4; keypad
  MODE_3_CONTINUOUS. FUTURE LEVERS (all measured): cpu_core.s dispatch
  surgery (expiry check push ix/pop hl RAM roundtrips per instr; target
  ~580->~400 clk = 1.4x), wider warp coverage, or restricted JIT; full PAL
  is IMPOSSIBLE for an interpreter on this hardware.
- AppVar 2-chunk split VERIFIED byte-perfect host-side (verify_finalblow.py
  parses .8xv, reassembles, compares vs build_payload; also sims the packer).
- main.c exit dump includes [mem] c000=.. d000=.. bytes for depack checks.
