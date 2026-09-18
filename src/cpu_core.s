.assume adl=1

.section .text,"ax",@progbits

/*
 * Native 6510 core. Supported official and stable-undocumented instructions
 * execute here; JAM encodings stop explicitly.
 *
 * Register plan while the core runs:
 *   IY = 6502 PC (upper byte 0), IX = cycles REMAINING in the batch minus 1
 *   (countdown: negative = batch done, so the per-instruction check is a
 *   sign test instead of a 24-bit compare; cpu.cycles = until - 1 - IX).
 * C helpers preserve IX but not IY; callers save IY or reload both from
 * CpuState at a synchronized boundary.
 *
 * CpuState offsets are pinned by _Static_asserts in cpu.c:
 *   pc=0 sp=2 a=3 x=4 y=5 cf=6 zf=7 idf=8 dmf=9 bcf=10 of=11 nf=12 cycles=13
 */
.extern _cpu
.extern _mem_rd_pg
.extern _mem_wr_pg
.extern _jit_page_generation
.extern _jit_page_generation_wrapped
.extern _mem_low
.extern _mem_read_io
.extern _mem_write_io
.extern _mem_setup_banks
.extern _cpu_decimal_adc
.extern _cpu_decimal_sbc
.extern _cpu_warp
.extern _cpu_jit_kernal
.extern _cpu_jit_reentry
.extern _jit_entry_points
.extern _jit_run_batch
.extern _cpu_illegal_op
.extern _cpu_illegal_pc
.extern _drive_trap_load
.extern _drive_trap_fastload
.extern _drive_trap_geos
.extern _cpu_brk_native

.macro INC_PC
    inc iyl
    jr nz, 9f
    inc iyh
9:
.endm

/* a = code byte at pc, pc++ (no IO check: pc never points at IO) */
.macro FETCH_A
    lea de, iy + 0
    INC_PC
    ld hl, 0
    ld l, d
    add hl, hl
    add hl, hl
    ld bc, _mem_rd_pg
    add hl, bc
    ld hl, (hl)
    add hl, de
    ld a, (hl)
.endm

/* de = 16-bit operand (lo, hi), DEU = 0. The hi byte reuses the lo fetch's
   host pointer (same 6502 page => same table entry => contiguous); operands
   crossing a page boundary take the full second walk. */
.macro FETCH_DE16
    FETCH_A
    ld (op_lo), a
    ld a, e              ; low byte of the lo-operand's 6502 address
    inc a
    jr z, 7f
    INC_PC
    inc hl
    ld a, (hl)
    jr 8f
7:  FETCH_A
8:  ld de, 0
    ld d, a
    ld a, (op_lo)
    ld e, a
.endm

/* Lazy N/Z: store the result byte; cpu.zf/cpu.nf are materialized only at
   sync_out / flags_pack (last_z: zero <=> zf set; last_n: bit7 = nf).
   Two bytes because BIT derives Z and N from different values. Preserves
   a and all flags; clobbers nothing. */
.macro SET_NZ
    ld (last_z), a
    ld (last_n), a
.endm

.macro CYC n
    lea ix, ix - \n
.endm

/* hl (16-bit result, upper byte dirty) -> de with DEU = 0 */
.macro HL16_TO_DE
    ld de, 0
    ld e, l
    ld d, h
.endm

/* ---- shared helpers ---------------------------------------------------- */

/* a = mem[de]; preserves de; clobbers hl, bc.
   C calls clobber IY (CEdev ABI: only IX is callee-saved) - save it. */
read_de:
    ld hl, 0
    ld l, d
    add hl, hl
    add hl, hl
    ld bc, _mem_rd_pg
    add hl, bc
    ld hl, (hl)
    add hl, de
    or a, a
    sbc hl, de
    jr z, 1f
    add hl, de
    ld a, (hl)
    ret
1:  push iy
    push de
    call _mem_read_io
    pop de
    pop iy
    ld c, a              ; result; check the waitloop-skip flag from vic
    ld a, (_cpu_warp)
    or a, a
    ld a, c
    ret z
    xor a, a
    ld (_cpu_warp), a
    ld ix, 0             ; raster busy poll: batch ends after this op
    ld a, c
    ret

/* mem[de] = (wr_val); preserves de; clobbers hl, bc, a */
write_de:
    ld hl, 0
    ld l, d
    add hl, hl
    add hl, hl
    ld bc, _mem_wr_pg
    add hl, bc
    ld hl, (hl)
    add hl, de
    or a, a
    sbc hl, de
    jr z, 2f
    add hl, de
    ld a, (wr_val)
    ld (hl), a
    ld hl, 0
    ld l, d
    ld bc, _jit_page_generation
    add hl, bc
    inc (hl)
    jr nz, 3f
    ld a, 1
    ld (_jit_page_generation_wrapped), a
3:
    ld hl, 1
    or a, a
    sbc hl, de
    ret nz
    ld a, (wr_val)
    ld bc, 0
    ld c, a
    push de
    push iy
    push bc
    call _mem_setup_banks
    pop bc
    pop iy
    pop de
    ret
2:  ld a, (wr_val)
    ld bc, 0
    ld c, a
    push iy
    push bc
    push de
    call _mem_write_io
    pop de
    pop bc
    pop iy
    ret

/* b -> 6502 stack; clobbers a, de, hl */
push6502:
    ld a, (_cpu + 2)
    ld de, 0
    ld e, a
    ld hl, (_mem_low)
    add hl, de
    ld de, 0x100
    add hl, de
    ld (hl), b
    ld a, (_cpu + 2)
    dec a
    ld (_cpu + 2), a
    ret

/* 6502 stack -> a; clobbers de, hl */
pop6502:
    ld a, (_cpu + 2)
    inc a
    ld (_cpu + 2), a
    ld de, 0
    ld e, a
    ld hl, (_mem_low)
    add hl, de
    ld de, 0x100
    add hl, de
    ld a, (hl)
    ret

/* ---- shared op cores (operand in b) ------------------------------------ */

adc_core:
    ld a, (_cpu + 9)
    or a, a
    jp nz, adc_decimal
    ld a, (_cpu + 6)
    rra
    ld a, (_cpu + 3)
    adc a, b
    ld (_cpu + 3), a
    ld c, a
    push af
    ld a, 0
    jr nc, 9f
    inc a
9:  ld (_cpu + 6), a
    pop af
    ld a, 0
    jp po, 8f
    inc a
8:  ld (_cpu + 11), a
    ld a, c
    SET_NZ
    ret

sbc_core:
    ld a, (_cpu + 9)
    or a, a
    jp nz, sbc_decimal
    ld a, (_cpu + 6)
    rra
    ccf
    ld a, (_cpu + 3)
    sbc a, b
    ld (_cpu + 3), a
    ld c, a
    push af
    ld a, 0
    jr c, 9f
    inc a
9:  ld (_cpu + 6), a
    pop af
    ld a, 0
    jp po, 8f
    inc a
8:  ld (_cpu + 11), a
    ld a, c
    SET_NZ
    ret

adc_decimal:
    push de
    ld hl, 0
    ld l, b
    push hl
    call _cpu_decimal_adc
    pop hl
    pop de
    jp seed_lazy

sbc_decimal:
    push de
    ld hl, 0
    ld l, b
    push hl
    call _cpu_decimal_sbc
    pop hl
    pop de
    jp seed_lazy

cmpa_core:
    ld a, (_cpu + 3)
    jr cmp_flags
cmpx_core:
    ld a, (_cpu + 4)
    jr cmp_flags
cmpy_core:
    ld a, (_cpu + 5)
cmp_flags:
    sub a, b
    ld c, a
    ld a, 0
    jr c, 9f
    inc a
9:  ld (_cpu + 6), a
    ld a, c
    SET_NZ
    ret

/* shift cores: value in a -> result in a, cf/zf/nf set */
asl_core:
    sla a
    jr shift_flags
lsr_core:
    srl a
    jr shift_flags
rol_core:
    ld c, a
    ld a, (_cpu + 6)
    rra
    ld a, c
    rl a
    jr shift_flags
ror_core:
    ld c, a
    ld a, (_cpu + 6)
    rra
    ld a, c
    rr a
shift_flags:
    ld c, a
    ld a, 0
    jr nc, 9f
    inc a
9:  ld (_cpu + 6), a
    ld a, c
    SET_NZ
    ret

bit_core:
    ld a, b
    ld (last_n), a       ; nf = operand bit7 (lazy)
    rlca
    rlca
    and a, 1
    ld (_cpu + 11), a
    ld a, (_cpu + 3)
    and a, b
    ld (last_z), a       ; zf = (a & operand) == 0 (lazy)
    ret

/* RMW inc/dec at de (double write, matching the C core's IO semantics) */
rmw_inc_de:
    call read_de
    ld (wr_val), a
    call write_de
    ld a, (wr_val)
    inc a
    ld (wr_val), a
    call write_de
    ld a, (wr_val)
    SET_NZ
    ret
rmw_dec_de:
    call read_de
    ld (wr_val), a
    call write_de
    ld a, (wr_val)
    dec a
    ld (wr_val), a
    call write_de
    ld a, (wr_val)
    SET_NZ
    ret

/* ---- main loop ---------------------------------------------------------- */

.global _cpu_run_asm
.type _cpu_run_asm, @function
_cpu_run_asm:
    push ix
    push iy
    ld iy, 0
    add iy, sp
    ld hl, (iy + 9)          ; until_cycles arg
    ld (until_cyc), hl
    dec hl
    ld de, (_cpu + 13)       ; ix = until - cycles - 1 (countdown)
    or a, a
    sbc hl, de
    push hl
    pop ix
    ld a, (_cpu + 0)         ; pc -> iy (top byte 0)
    ld (scr3), a
    ld a, (_cpu + 1)
    ld (scr3 + 1), a
    xor a, a
    ld (scr3 + 2), a
    ld iy, (scr3)
    call seed_lazy

main_loop:
    lea hl, ix + 0
    add hl, hl               ; carry = countdown sign: batch expired
    jp c, run_done
    FETCH_A
    ld hl, 0
    ld l, a
    add hl, hl
    add hl, hl
    ld de, op_table
    add hl, de
    ld hl, (hl)
    jp (hl)

run_done:
    call sync_out
    ld a, 1
    pop iy
    pop ix
    ret

run_fail:
    xor a, a
    pop iy
    pop ix
    ret

sync_out:                    ; iy/ix -> cpu.pc/cpu.cycles, lazy NZ -> flags
    ld (scr3), iy
    ld a, (scr3)
    ld (_cpu + 0), a
    ld a, (scr3 + 1)
    ld (_cpu + 1), a
    ld hl, (until_cyc)       ; cycles = until - 1 - countdown
    dec hl
    push ix
    pop de
    or a, a
    sbc hl, de
    ld (_cpu + 13), hl
    ld a, (last_n)
    rlca
    and a, 1
    ld (_cpu + 12), a
    ld a, (last_z)
    or a, a
    ld a, 0
    jr nz, 4f
    inc a
4:  ld (_cpu + 7), a
    ret

seed_lazy:                   ; struct zf/nf -> lazy bytes; clobbers a
    ld a, (_cpu + 7)
    xor a, 1
    ld (last_z), a
    ld a, (_cpu + 12)
    rrca                     ; bit0 -> bit7
    ld (last_n), a
    ret

/* C device traps are stateful helpers, not opcode interpretation. */
trap_resume:
    or a, a
    jp z, run_fail
    call restore_cpu_state
    jp main_loop

restore_cpu_state:
    ld hl, (until_cyc)
    dec hl
    ld de, (_cpu + 13)
    or a, a
    sbc hl, de
    push hl
    pop ix
    ld a, (_cpu + 0)
    ld (scr3), a
    ld a, (_cpu + 1)
    ld (scr3 + 1), a
    xor a, a
    ld (scr3 + 2), a
    ld iy, (scr3)
    call seed_lazy
    ret

try_jit_reentry:
    ld a, (_cpu_jit_reentry)
    or a, a
    ret z
    lea hl, ix + 0
    add hl, hl
    jr c, jit_reentry_miss
    ld bc, 0x300
    ld c, iyl
    mlt bc
    ld hl, _jit_entry_points
    add hl, bc
    ld hl, (hl)
    ld de, 0
    or a, a
    sbc hl, de
    jr z, jit_reentry_miss
    ld a, iyl
    cp a, (hl)
    jr nz, jit_reentry_miss
    inc hl
    ld a, iyh
    cp a, (hl)
    jr nz, jit_reentry_miss
    ld bc, 16
    add hl, bc
    ld e, (hl)
    lea hl, ix + 1
    or a, a
    sbc hl, de
    jr c, jit_reentry_miss
    call sync_out
    ld hl, (until_cyc)
    push hl
    call _jit_run_batch
    pop hl
    call restore_cpu_state
    ld a, 1
    ret
jit_reentry_miss:
    xor a, a
    ret

.macro JAM name, opcode
jam_\name\()_h:
    ld a, \opcode
    ld (_cpu_illegal_op), a
    ld (scr3), iy
    ld a, (scr3)
    ld (_cpu + 0), a
    ld (_cpu_illegal_pc), a
    ld a, (scr3 + 1)
    ld (_cpu + 1), a
    ld (_cpu_illegal_pc + 1), a
    jp run_fail
.endm

trap_02_h:
    ld a, iyh
    cp a, 0xf4
    jp nz, jam_02_h
    ld a, iyl
    cp a, 0xa6
    jp nz, jam_02_h
    call sync_out
    push iy
    call _drive_trap_load
    pop iy
    jp trap_resume

trap_12_h:
    ld a, iyh
    cp a, 0x1a
    jp nz, jam_12_h
    ld a, iyl
    cp a, 0x4f
    jp nz, jam_12_h
    call sync_out
    push iy
    call _drive_trap_fastload
    pop iy
    jp trap_resume

trap_22_h:
    call sync_out
    push iy
    call _drive_trap_geos
    pop iy
    or a, a
    jp z, jam_22_h
    jp trap_resume

brk_h:
    call sync_out
    push iy
    call _cpu_brk_native
    pop iy
    ld a, 1
    jp trap_resume

JAM 02, 0x02
JAM 12, 0x12
JAM 22, 0x22
JAM 32, 0x32
JAM 42, 0x42
JAM 52, 0x52
JAM 62, 0x62
JAM 72, 0x72
JAM 92, 0x92
JAM b2, 0xb2
JAM d2, 0xd2
JAM f2, 0xf2

/* taken branch: c = offset, iy = pc after operand */
do_branch:
    lea hl, iy + 0
    ld b, h                  ; old page byte
    ld de, 0
    ld e, c
    bit 7, c
    jr z, 1f
    ld d, 0xff
1:  add hl, de
    ld (scr3), hl
    xor a, a
    ld (scr3 + 2), a
    ld iy, (scr3)
    ld a, (scr3 + 1)
    cp a, b
    lea ix, ix - 1           ; taken +1, page cross +2 (countdown)
    jr z, 2f
    lea ix, ix - 1
2:
    call try_jit_reentry
    or a, a
    jp nz, main_loop
    ld a, (scr3 + 1)
    cp a, 0x01               ; byte-guarded Swirler RAM-loop translations
    jp z, jit_try_swirler
    cp a, 0x02               ; dynamic RAM loops with no guest writes
    jp c, 3f
    cp a, 0xA0
    jp nc, 3f
    jp jit_try_ram_loop
3:
    cp a, 0xE0               ; a = new page: KERNAL JIT blocks
    jp nc, jit_dispatch
    jp main_loop

/* ---- ROM JIT blocks ------------------------------------------------------
   Native translations of verified KERNAL code, entered whenever a control
   transfer (branch/jmp/jsr) lands on a block address. Gates: exact pc
   match, the block's bit in _cpu_jit_kernal (ROM bytes verified at boot by
   c64.c), HIRAM set (KERNAL banked in). Idle-loop blocks follow the warp
   convention: one iteration's architectural side effects, then end the
   batch (state cannot change until an IRQ, and IRQs only fire at batch
   boundaries). */
jit_dispatch:                ; iy = new pc, a = its page byte (>= 0xE0)
    cp a, 0xE5
    jr z, jit_try_e5cd
    cp a, 0xF6
    jr z, jit_try_udtim
    jp main_loop

jit_try_e5cd:                ; $E5CD: screen editor wait-for-key loop
    ld a, iyl
    cp a, 0xCD
    jp nz, main_loop
    ld a, (_cpu_jit_kernal)
    and a, 1
    jp z, main_loop
    ld hl, (_mem_low)
    inc hl
    ld a, (hl)               ; $01 bank latch
    and a, 2                 ; HIRAM: KERNAL actually mapped
    jp z, main_loop
    ld hl, (_mem_low)
    ld de, 0x00C6
    add hl, de
    ld a, (hl)               ; NDX (keyboard buffer count)
    or a, a
    jp nz, main_loop         ; key waiting: interpret the exit normally
    ; LDA $C6 / STA $CC / STA $0292 with A=0, then spin: emulate one
    ; iteration and consume the batch
    ld (last_z), a
    ld (last_n), a
    ld (_cpu + 3), a
    ld hl, (_mem_low)
    ld de, 0x00CC
    add hl, de
    ld (hl), a               ; BLNSW: cursor blink on
    ld hl, (_mem_low)
    ld de, 0x0292
    add hl, de
    ld (hl), a               ; AUTODN: scroll direction
    ld hl, (_cpu_jit_hits)
    inc hl
    ld (_cpu_jit_hits), hl
    ld ix, 0
    lea ix, ix - 1           ; countdown -1: batch ends at the next check
    jp main_loop

jit_try_udtim:               ; $F69B UDTIM: LDX #0, 24-bit jiffy increment
    ld a, iyl
    cp a, 0x9B
    jp nz, main_loop
    ld a, (_cpu_jit_kernal)
    and a, 2
    jp z, main_loop
    ld hl, (_mem_low)
    inc hl
    ld a, (hl)
    and a, 2                 ; HIRAM
    jp z, main_loop
    xor a, a
    ld (_cpu + 4), a         ; X = 0 (INC flags overwrite LDX's below)
    ld hl, (_mem_low)
    ld de, 0x00A2
    add hl, de
    inc (hl)                 ; INC $A2 (jiffy lo)
    ld a, (hl)
    ld (last_z), a
    ld (last_n), a
    jr nz, 5f                ; LDX 2 + INC 5 + BNE taken 3
    dec hl
    inc (hl)                 ; INC $A1
    ld a, (hl)
    ld (last_z), a
    ld (last_n), a
    jr nz, 6f                ; 2+5+2 + 5+3
    dec hl
    inc (hl)                 ; INC $A0
    ld a, (hl)
    ld (last_z), a
    ld (last_n), a
    lea ix, ix - 21          ; 2+5+2+5+2+5, falls through to $F6A7
    jr 7f
6:  lea ix, ix - 17
    jr 7f
5:  lea ix, ix - 10
7:  ld hl, (_cpu_jit_hits2)
    inc hl
    ld (_cpu_jit_hits2), hl
    ld iy, 0xF6A7            ; SEC / day-rollover compare: interpreted
    jp main_loop

/* Swirler relocates this copy loop from $5D89 to $012D:
     DEY / BCC $010D / LDA ($AE),Y / STA ($FE),Y / TYA / BNE $012D.
   It is entered only from its taken BNE, verifies all bytes on every entry,
   and processes complete iterations only when they fit the raster batch. */
jit_try_swirler:
    ld a, iyl
    cp a, 0x2D
    jp nz, main_loop
    ld hl, (_mem_low)
    ld de, 0x012D
    add hl, de
    ld a, (hl)
    cp a, 0x88
    jp nz, main_loop
    inc hl
    ld a, (hl)
    cp a, 0x90
    jp nz, main_loop
    inc hl
    ld a, (hl)
    cp a, 0xDD
    jp nz, main_loop
    inc hl
    ld a, (hl)
    cp a, 0xB1
    jp nz, main_loop
    inc hl
    ld a, (hl)
    cp a, 0xAE
    jp nz, main_loop
    inc hl
    ld a, (hl)
    cp a, 0x91
    jp nz, main_loop
    inc hl
    ld a, (hl)
    cp a, 0xFE
    jp nz, main_loop
    inc hl
    ld a, (hl)
    cp a, 0x98
    jp nz, main_loop
    inc hl
    ld a, (hl)
    cp a, 0xD0
    jp nz, main_loop
    inc hl
    ld a, (hl)
    cp a, 0xF6
    jp nz, main_loop
    ld hl, (_cpu_jit_hits3)
    inc hl
    ld (_cpu_jit_hits3), hl

jit_swirler_copy:
    lea hl, ix - 20
    add hl, hl
    jp c, main_loop           ; preserve ordinary instruction-boundary exit
    ld a, (_cpu + 5)
    dec a                     ; DEY
    ld (_cpu + 5), a
    ld a, (_cpu + 6)
    or a, a
    jp z, jit_swirler_bcc     ; carry clear: BCC's unusual loop exit

    ld hl, (_mem_low)         ; LDA ($AE),Y
    ld de, 0x00AE
    add hl, de
    ld e, (hl)
    inc hl
    ld d, (hl)
    ld a, (_cpu + 5)
    ld hl, 0
    ld l, a
    add hl, de
    HL16_TO_DE
    call read_de
    ld (wr_val), a

    ld hl, (_mem_low)         ; STA ($FE),Y
    ld de, 0x00FE
    add hl, de
    ld e, (hl)
    inc hl
    ld d, (hl)
    ld a, (_cpu + 5)
    ld hl, 0
    ld l, a
    add hl, de
    HL16_TO_DE
    call write_de
    ld a, (_cpu + 5)          ; TYA
    ld (_cpu + 3), a
    SET_NZ
    or a, a
    jr z, jit_swirler_done    ; BNE not taken
    CYC 20
    jp jit_swirler_copy

jit_swirler_done:
    CYC 19
    ld iy, 0x0137             ; TXA after the final BNE
    jp main_loop

jit_swirler_bcc:
    ld a, (_cpu + 5)
    SET_NZ
    CYC 5
    ld iy, 0x010D
    jp main_loop

/* First dynamic block class: DEX / BNE -3 in ordinary RAM. This is selected
   only after the branch lands at its loop head, verifies all three guest
   bytes, and remains valid for its uninterrupted CPU batch. It never caches
   host pointers or code, so self-modifying code falls back on the next entry.
   The final DEX leaves PC at the instruction following BNE, as on 6510. */
jit_try_ram_loop:
    ld (scr3), iy
    ld a, (scr3 + 1)
    ld hl, 0
    ld l, a
    add hl, hl
    add hl, hl
    ld de, _mem_rd_pg
    add hl, de
    ld hl, (hl)
    lea de, iy + 0
    add hl, de
    ld a, (hl)
    cp a, 0xCA               ; DEX
    jp nz, main_loop
    inc hl
    ld a, (hl)
    cp a, 0xD0               ; BNE
    jp nz, main_loop
    inc hl
    ld a, (hl)
    cp a, 0xFD               ; branch back three bytes
    jp nz, main_loop
    ld hl, (_cpu_jit_hits4)
    inc hl
    ld (_cpu_jit_hits4), hl

jit_ram_dex_bne_loop:
    lea hl, ix - 5           ; DEX 2 + taken BNE 3 guest cycles
    add hl, hl
    jp c, main_loop           ; retain normal instruction-boundary exit
    lea ix, ix - 5
    ld a, (_cpu + 4)
    dec a
    ld (_cpu + 4), a
    SET_NZ
    jp nz, jit_ram_dex_bne_loop
    inc ix                   ; non-taken BNE costs 2, not 3
    lea iy, iy + 3
    jp main_loop

/* ---- opcode macro bodies ------------------------------------------------ */

.macro RMW_EXEC core, cycles, after=none
    call read_de
    ld (wr_val), a
    call write_de
    ld a, (wr_val)
    call \core
    ld (wr_val), a
    call write_de
.ifnc \after,none
    ld a, (wr_val)
    ld b, a
    call \after
.endif
    CYC \cycles
    jp main_loop
.endm

.macro BR_IF flagoff, taken_if_set
    FETCH_A
    ld c, a
    CYC 2
    ld a, (_cpu + \flagoff)
    or a, a
.if \taken_if_set
    jp nz, do_branch
.else
    jp z, do_branch
.endif
    jp main_loop
.endm

.macro BR_Z taken_if_set     ; zf lazy: set <=> last_z == 0
    FETCH_A
    ld c, a
    CYC 2
    ld a, (last_z)
    or a, a
.if \taken_if_set
    jp z, do_branch
.else
    jp nz, do_branch
.endif
    jp main_loop
.endm

.macro BR_N taken_if_set     ; nf lazy: set <=> last_n bit7
    FETCH_A
    ld c, a
    CYC 2
    ld a, (last_n)
    or a, a
.if \taken_if_set
    jp m, do_branch
.else
    jp p, do_branch
.endif
    jp main_loop
.endm

.macro LD_IMM regoff
    FETCH_A
    ld (_cpu + \regoff), a
    SET_NZ
    CYC 2
    jp main_loop
.endm

.macro LD_ZP regoff
    FETCH_A
    ld de, 0
    ld e, a
    ld hl, (_mem_low)
    add hl, de
    ld a, (hl)
    ld (_cpu + \regoff), a
    SET_NZ
    CYC 3
    jp main_loop
.endm

.macro LD_ABS regoff
    FETCH_DE16
    call read_de
    ld (_cpu + \regoff), a
    SET_NZ
    CYC 4
    jp main_loop
.endm

.macro LD_ABSI regoff, idxoff
    FETCH_DE16
    ld a, (_cpu + \idxoff)
    add a, e
    ld e, a
    jr nc, 9f
    inc d
    CYC 1
9:  call read_de
    ld (_cpu + \regoff), a
    SET_NZ
    CYC 4
    jp main_loop
.endm

.macro ST_ABS regoff
    FETCH_DE16
    ld a, (_cpu + \regoff)
    ld (wr_val), a
    call write_de
    CYC 4
    jp main_loop
.endm

.macro ST_ABSI regoff, idxoff
    FETCH_DE16
    ld a, (_cpu + \idxoff)
    add a, e
    ld e, a
    jr nc, 9f
    inc d
9:
    ld a, (_cpu + \regoff)
    ld (wr_val), a
    call write_de
    CYC 5
    jp main_loop
.endm

/* zp store incl. the $0001 bank latch; c = effective zp address */
.macro ST_ZP_C regoff
    ld de, 0
    ld e, c
    ld hl, (_mem_low)
    add hl, de
    ld a, (_cpu + \regoff)
    ld (hl), a
    CYC 3
    ld a, c
    dec a
    jp nz, main_loop
    ld a, (_cpu + \regoff)
    ld bc, 0
    ld c, a
    push iy
    push bc
    call _mem_setup_banks
    pop bc
    pop iy
    jp main_loop
.endm

.macro XFER srcoff, dstoff, donz
    ld a, (_cpu + \srcoff)
    ld (_cpu + \dstoff), a
.if \donz
    SET_NZ
.endif
    CYC 2
    jp main_loop
.endm

.macro FLAGSET flagoff, val
    ld a, \val
    ld (_cpu + \flagoff), a
    CYC 2
    jp main_loop
.endm

.macro INCDEC regoff, isinc
    ld a, (_cpu + \regoff)
.if \isinc
    inc a
.else
    dec a
.endif
    ld (_cpu + \regoff), a
    SET_NZ
    CYC 2
    jp main_loop
.endm

/* compare register with value in b: cf = reg >= v, nz from diff */
.macro CMP_B regoff
    ld a, (_cpu + \regoff)
    sub a, b
    ld c, a
    ld a, 0
    jr c, 9f
    inc a
9:  ld (_cpu + 6), a
    ld a, c
    SET_NZ
.endm

.macro LOGIC_IMM op
    FETCH_A
    ld b, a
    ld a, (_cpu + 3)
    \op a, b
    ld (_cpu + 3), a
    SET_NZ
    CYC 2
    jp main_loop
.endm

.macro LOGIC_ZP op
    FETCH_A
    ld de, 0
    ld e, a
    ld hl, (_mem_low)
    add hl, de
    ld b, (hl)
    ld a, (_cpu + 3)
    \op a, b
    ld (_cpu + 3), a
    SET_NZ
    CYC 3
    jp main_loop
.endm

/* ---- operand loaders (result in b) -------------------------------------- */

.macro LOAD_B_ZP
    FETCH_A
    ld de, 0
    ld e, a
    ld hl, (_mem_low)
    add hl, de
    ld b, (hl)
.endm

.macro LOAD_B_ZPI idxoff
    FETCH_A
    ld c, a
    ld a, (_cpu + \idxoff)
    add a, c
    ld de, 0
    ld e, a
    ld hl, (_mem_low)
    add hl, de
    ld b, (hl)
.endm

.macro LOAD_B_ABS
    FETCH_DE16
    call read_de
    ld b, a
.endm

.macro LOAD_B_ABSI idxoff
    FETCH_DE16
    ld a, (_cpu + \idxoff)
    add a, e
    ld e, a
    jr nc, 9f
    inc d
    CYC 1
9:  call read_de
    ld b, a
.endm

.macro LOAD_ADDR_INDY pagecost=0
    FETCH_A
    ld de, 0
    ld e, a
    ld hl, (_mem_low)
    add hl, de
    ld c, (hl)
    inc e
    ld hl, (_mem_low)
    add hl, de
    ld b, (hl)
    ld d, b
    ld a, (_cpu + 5)
    add a, c
    ld e, a
    jr nc, 9f
    inc d
.if \pagecost
    CYC 1
.endif
9:
.endm

.macro LOAD_B_INDY
    LOAD_ADDR_INDY 1
    call read_de
    ld b, a
.endm

.macro LOGIC_ABS op
    LOAD_B_ABS
    ld a, (_cpu + 3)
    \op a, b
    ld (_cpu + 3), a
    SET_NZ
    CYC 4
    jp main_loop
.endm

.macro LOGIC_ABSI op, idxoff
    LOAD_B_ABSI \idxoff
    ld a, (_cpu + 3)
    \op a, b
    ld (_cpu + 3), a
    SET_NZ
    CYC 4
    jp main_loop
.endm

.macro LOGIC_ZPI op, idxoff
    LOAD_B_ZPI \idxoff
    ld a, (_cpu + 3)
    \op a, b
    ld (_cpu + 3), a
    SET_NZ
    CYC 4
    jp main_loop
.endm

.macro LOGIC_INDY op
    LOAD_B_INDY
    ld a, (_cpu + 3)
    \op a, b
    ld (_cpu + 3), a
    SET_NZ
    CYC 5
    jp main_loop
.endm

.macro LD_ZPI regoff, idxoff
    FETCH_A
    ld b, a
    ld a, (_cpu + \idxoff)
    add a, b
    ld de, 0
    ld e, a
    ld hl, (_mem_low)
    add hl, de
    ld a, (hl)
    ld (_cpu + \regoff), a
    SET_NZ
    CYC 4
    jp main_loop
.endm

.macro LOAD_ADDR_ABSI idxoff
    FETCH_DE16
    ld a, (_cpu + \idxoff)
    add a, e
    ld e, a
    jr nc, 9f
    inc d
9:
.endm

/* ---- handlers ----------------------------------------------------------- */

lda_imm_h:  LD_IMM 3
ldx_imm_h:  LD_IMM 4
ldy_imm_h:  LD_IMM 5
lda_zp_h:   LD_ZP 3
ldx_zp_h:   LD_ZP 4
ldy_zp_h:   LD_ZP 5
lda_abs_h:  LD_ABS 3
ldx_abs_h:  LD_ABS 4
ldy_abs_h:  LD_ABS 5
lda_absx_h: LD_ABSI 3, 4
lda_absy_h: LD_ABSI 3, 5

lda_zpx_h:
    FETCH_A
    ld b, a
    ld a, (_cpu + 4)
    add a, b
    ld de, 0
    ld e, a
    ld hl, (_mem_low)
    add hl, de
    ld a, (hl)
    ld (_cpu + 3), a
    SET_NZ
    CYC 4
    jp main_loop

lda_indy_h:
    LOAD_B_INDY
    ld a, b
    ld (_cpu + 3), a
    SET_NZ
    CYC 5
    jp main_loop

sta_zp_h:
    FETCH_A
    ld c, a
    ST_ZP_C 3
stx_zp_h:
    FETCH_A
    ld c, a
    ST_ZP_C 4
sty_zp_h:
    FETCH_A
    ld c, a
    ST_ZP_C 5

sta_zpx_h:
    FETCH_A
    ld b, a
    ld a, (_cpu + 4)
    add a, b
    ld c, a
    ; zp,x costs 4 cycles, ST_ZP_C adds 3: pre-add the extra 1
    CYC 1
    ST_ZP_C 3

sty_zpx_h:
    FETCH_A
    ld b, a
    ld a, (_cpu + 4)
    add a, b
    ld c, a
    CYC 1
    ST_ZP_C 5

stx_zpy_h:
    FETCH_A
    ld b, a
    ld a, (_cpu + 5)
    add a, b
    ld c, a
    CYC 1
    ST_ZP_C 4

sta_abs_h:  ST_ABS 3
stx_abs_h:  ST_ABS 4
sty_abs_h:  ST_ABS 5
sta_absx_h: ST_ABSI 3, 4
sta_absy_h: ST_ABSI 3, 5

sta_indy_h:
    LOAD_ADDR_INDY
    ld a, (_cpu + 3)
    ld (wr_val), a
    call write_de
    CYC 6
    jp main_loop

ahx_indy_h:
    LOAD_ADDR_INDY
    ld a, (_cpu + 3)
    ld b, a
    ld a, (_cpu + 4)
    and a, b
    ld b, d
    inc b
    and a, b
    ld (wr_val), a
    call write_de
    CYC 6
    jp main_loop

tas_absy_h:
    LOAD_ADDR_ABSI 5
    ld a, (_cpu + 3)
    ld b, a
    ld a, (_cpu + 4)
    and a, b
    ld (_cpu + 2), a
    ld b, d
    inc b
    and a, b
    ld (wr_val), a
    call write_de
    CYC 5
    jp main_loop

shy_absx_h:
    LOAD_ADDR_ABSI 4
    ld a, (_cpu + 5)
    ld b, d
    inc b
    and a, b
    ld (wr_val), a
    call write_de
    CYC 5
    jp main_loop

shx_absy_h:
    LOAD_ADDR_ABSI 5
    ld a, (_cpu + 4)
    ld b, d
    inc b
    and a, b
    ld (wr_val), a
    call write_de
    CYC 5
    jp main_loop

ahx_absy_h:
    LOAD_ADDR_ABSI 5
    ld a, (_cpu + 3)
    ld b, a
    ld a, (_cpu + 4)
    and a, b
    ld b, d
    inc b
    and a, b
    ld (wr_val), a
    call write_de
    CYC 5
    jp main_loop

tax_h: XFER 3, 4, 1
txa_h: XFER 4, 3, 1
tay_h: XFER 3, 5, 1
tya_h: XFER 5, 3, 1
tsx_h: XFER 2, 4, 1
txs_h: XFER 4, 2, 0

inx_h: INCDEC 4, 1
iny_h: INCDEC 5, 1
dex_h: INCDEC 4, 0
dey_h: INCDEC 5, 0

clc_h: FLAGSET 6, 0
sec_h: FLAGSET 6, 1
cli_h: FLAGSET 8, 0
sei_h: FLAGSET 8, 1
cld_h: FLAGSET 9, 0
sed_h: FLAGSET 9, 1
clv_h: FLAGSET 11, 0

bne_h: BR_Z 0
beq_h: BR_Z 1
bcc_h: BR_IF 6, 0
bcs_h: BR_IF 6, 1
bpl_h: BR_N 0
bmi_h: BR_N 1
bvc_h: BR_IF 11, 0
bvs_h: BR_IF 11, 1

cmp_imm_h:
    FETCH_A
    ld b, a
    CMP_B 3
    CYC 2
    jp main_loop
cpx_imm_h:
    FETCH_A
    ld b, a
    CMP_B 4
    CYC 2
    jp main_loop
cpy_imm_h:
    FETCH_A
    ld b, a
    CMP_B 5
    CYC 2
    jp main_loop
cmp_zp_h:
    FETCH_A
    ld de, 0
    ld e, a
    ld hl, (_mem_low)
    add hl, de
    ld b, (hl)
    CMP_B 3
    CYC 3
    jp main_loop
cmp_zpx_h:
    LOAD_B_ZPI 4
    CMP_B 3
    CYC 4
    jp main_loop
cmp_abs_h:
    FETCH_DE16
    call read_de
    ld b, a
    CMP_B 3
    CYC 4
    jp main_loop

and_imm_h: LOGIC_IMM and
ora_imm_h: LOGIC_IMM or
eor_imm_h: LOGIC_IMM xor
and_zp_h:  LOGIC_ZP and
ora_zp_h:  LOGIC_ZP or
eor_zp_h:  LOGIC_ZP xor
and_zpx_h: LOGIC_ZPI and, 4
ora_zpx_h: LOGIC_ZPI or, 4
eor_zpx_h: LOGIC_ZPI xor, 4
and_abs_h:  LOGIC_ABS and
ora_abs_h:  LOGIC_ABS or
eor_abs_h:  LOGIC_ABS xor
and_absx_h: LOGIC_ABSI and, 4
ora_absx_h: LOGIC_ABSI or, 4
eor_absx_h: LOGIC_ABSI xor, 4
and_absy_h: LOGIC_ABSI and, 5
ora_absy_h: LOGIC_ABSI or, 5
eor_absy_h: LOGIC_ABSI xor, 5
and_indy_h: LOGIC_INDY and
ora_indy_h: LOGIC_INDY or
eor_indy_h: LOGIC_INDY xor

ldy_zpx_h: LD_ZPI 5, 4
ldx_zpy_h: LD_ZPI 4, 5
ldy_absx_h: LD_ABSI 5, 4
ldx_absy_h: LD_ABSI 4, 5

adc_zp_h:
    LOAD_B_ZP
    call adc_core
    CYC 3
    jp main_loop
adc_zpx_h:
    LOAD_B_ZPI 4
    call adc_core
    CYC 4
    jp main_loop
adc_abs_h:
    LOAD_B_ABS
    call adc_core
    CYC 4
    jp main_loop
adc_absx_h:
    LOAD_B_ABSI 4
    call adc_core
    CYC 4
    jp main_loop
adc_absy_h:
    LOAD_B_ABSI 5
    call adc_core
    CYC 4
    jp main_loop
adc_indy_h:
    LOAD_B_INDY
    call adc_core
    CYC 5
    jp main_loop

sbc_zp_h:
    LOAD_B_ZP
    call sbc_core
    CYC 3
    jp main_loop
sbc_zpx_h:
    LOAD_B_ZPI 4
    call sbc_core
    CYC 4
    jp main_loop
sbc_abs_h:
    LOAD_B_ABS
    call sbc_core
    CYC 4
    jp main_loop
sbc_absx_h:
    LOAD_B_ABSI 4
    call sbc_core
    CYC 4
    jp main_loop
sbc_absy_h:
    LOAD_B_ABSI 5
    call sbc_core
    CYC 4
    jp main_loop
sbc_indy_h:
    LOAD_B_INDY
    call sbc_core
    CYC 5
    jp main_loop

cmp_absx_h:
    LOAD_B_ABSI 4
    call cmpa_core
    CYC 4
    jp main_loop
cmp_absy_h:
    LOAD_B_ABSI 5
    call cmpa_core
    CYC 4
    jp main_loop
cmp_indy_h:
    LOAD_B_INDY
    call cmpa_core
    CYC 5
    jp main_loop
cpx_zp_h:
    LOAD_B_ZP
    call cmpx_core
    CYC 3
    jp main_loop
cpx_abs_h:
    LOAD_B_ABS
    call cmpx_core
    CYC 4
    jp main_loop
cpy_zp_h:
    LOAD_B_ZP
    call cmpy_core
    CYC 3
    jp main_loop
cpy_abs_h:
    LOAD_B_ABS
    call cmpy_core
    CYC 4
    jp main_loop

bit_zp_h:
    LOAD_B_ZP
    call bit_core
    CYC 3
    jp main_loop
bit_abs_h:
    LOAD_B_ABS
    call bit_core
    CYC 4
    jp main_loop

.macro SHIFT_ZP core
    FETCH_A
    ld de, 0
    ld e, a
    call read_de
    ld (wr_val), a
    call write_de
    ld a, (wr_val)
    call \core
    ld (wr_val), a
    call write_de
    CYC 5
    jp main_loop
.endm

.macro SHIFT_ABS core
    FETCH_DE16
    call read_de
    ld (wr_val), a
    call write_de
    ld a, (wr_val)
    call \core
    ld (wr_val), a
    call write_de
    CYC 6
    jp main_loop
.endm

asl_zp_h:  SHIFT_ZP asl_core
lsr_zp_h:  SHIFT_ZP lsr_core
rol_zp_h:  SHIFT_ZP rol_core
ror_zp_h:  SHIFT_ZP ror_core
asl_abs_h: SHIFT_ABS asl_core
lsr_abs_h: SHIFT_ABS lsr_core
rol_abs_h: SHIFT_ABS rol_core
ror_abs_h: SHIFT_ABS ror_core

inc_abs_h:
    FETCH_DE16
    call rmw_inc_de
    CYC 6
    jp main_loop
dec_abs_h:
    FETCH_DE16
    call rmw_dec_de
    CYC 6
    jp main_loop
inc_absx_h:
    FETCH_DE16
    ld a, (_cpu + 4)
    ld hl, 0
    ld l, a
    add hl, de
    HL16_TO_DE
    call rmw_inc_de
    CYC 7
    jp main_loop
dec_absx_h:
    FETCH_DE16
    ld a, (_cpu + 4)
    ld hl, 0
    ld l, a
    add hl, de
    HL16_TO_DE
    call rmw_dec_de
    CYC 7
    jp main_loop

adc_imm_h:
    FETCH_A
    ld b, a
    call adc_core
    CYC 2
    jp main_loop

sbc_imm_h:
    FETCH_A
    ld b, a
    call sbc_core
    CYC 2
    jp main_loop

asl_a_h:
    ld a, (_cpu + 3)
    sla a
    jr .sh_done
lsr_a_h:
    ld a, (_cpu + 3)
    srl a
    jr .sh_done
rol_a_h:
    ld a, (_cpu + 6)
    rra
    ld a, (_cpu + 3)
    rl a
    jr .sh_done
ror_a_h:
    ld a, (_cpu + 6)
    rra
    ld a, (_cpu + 3)
    rr a
.sh_done:
    ld (_cpu + 3), a
    ld c, a
    ld a, 0
    jr nc, 9f
    inc a
9:  ld (_cpu + 6), a
    ld a, c
    SET_NZ
    CYC 2
    jp main_loop

inc_zp_h:
    FETCH_A
    ld de, 0
    ld e, a
    RMW_EXEC inc_core, 5
dec_zp_h:
    FETCH_A
    ld de, 0
    ld e, a
    RMW_EXEC dec_core, 5

pha_h:
    ld a, (_cpu + 3)
    ld b, a
    call push6502
    CYC 3
    jp main_loop
pla_h:
    call pop6502
    ld (_cpu + 3), a
    SET_NZ
    CYC 4
    jp main_loop

jmp_h:
    FETCH_DE16
    lea hl, iy - 3       ; instruction start
    or a, a
    sbc hl, de
    push de
    pop iy
    jr nz, 9f
    ld ix, 0             ; jmp-to-self: only an IRQ exits, end the batch
9:  CYC 3
    call try_jit_reentry
    or a, a
    jp nz, main_loop
    ld a, iyh
    cp a, 0xE0
    jp nc, jit_dispatch
    jp main_loop

jsr_h:
    FETCH_DE16
    lea hl, iy - 1       ; return address = next instruction - 1
    push de              ; push6502 clobbers de: save the jump target
    ld b, h
    push hl
    call push6502
    pop hl
    ld b, l
    call push6502
    pop iy               ; saved target -> pc
    CYC 6
    call try_jit_reentry
    or a, a
    jp nz, main_loop
    ld a, iyh
    cp a, 0xE0
    jp nc, jit_dispatch
    jp main_loop

rts_h:
    call pop6502
    ld (op_lo), a
    call pop6502
    ld de, 0
    ld d, a
    ld a, (op_lo)
    ld e, a
    inc de
    ld (scr3), de
    xor a, a
    ld (scr3 + 2), a
    ld iy, (scr3)
    CYC 6
    call try_jit_reentry
    jp main_loop

nop_h:
    CYC 2
    jp main_loop

/* 6502 P byte from flag bytes -> a; clobbers b */
flags_pack:
    ld a, (last_n)
    and a, 0x80          ; nf -> bit7
    ld b, a
    ld a, (_cpu + 11)
    rrca
    rrca                 ; of -> bit6
    or a, b
    or a, 0x30           ; brk + unused always set on push
    ld b, a
    ld a, (_cpu + 9)
    add a, a
    add a, a
    add a, a             ; dmf -> bit3
    or a, b
    ld b, a
    ld a, (_cpu + 8)
    add a, a
    add a, a             ; idf -> bit2
    or a, b
    ld b, a
    ld a, (last_z)
    or a, a
    ld a, 0
    jr nz, 3f
    ld a, 2              ; zf -> bit1
3:  or a, b
    ld b, a
    ld a, (_cpu + 6)     ; cf -> bit0
    or a, b
    ret

/* P byte in c -> flag bytes (bcf untouched, like the C core); clobbers a */
flags_unpack:
    ld a, c
    and a, 1
    ld (_cpu + 6), a
    ld a, c
    rrca
    and a, 1
    xor a, 1             ; last_z = zf ? 0 : nonzero
    ld (last_z), a
    ld a, c
    rrca
    rrca
    and a, 1
    ld (_cpu + 8), a
    ld a, c
    rrca
    rrca
    rrca
    and a, 1
    ld (_cpu + 9), a
    ld a, c
    rlca
    rlca
    and a, 1
    ld (_cpu + 11), a
    ld a, c
    and a, 0x80          ; last_n bit7 = nf
    ld (last_n), a
    ret

php_h:
    call flags_pack
    ld b, a
    call push6502
    CYC 3
    jp main_loop

plp_h:
    call pop6502
    ld c, a
    call flags_unpack
    CYC 4
    jp main_loop

rti_h:                   ; pull P, then pc lo/hi (no +1, unlike RTS)
    call pop6502
    ld c, a
    call flags_unpack
    call pop6502
    ld (op_lo), a
    call pop6502
    ld de, 0
    ld d, a
    ld a, (op_lo)
    ld e, a
    ld (scr3), de
    xor a, a
    ld (scr3 + 2), a
    ld iy, (scr3)
    CYC 6
    jp main_loop

jmp_ind_h:
    FETCH_DE16
    call read_de         ; pointer lo
    ld (op_lo), a
    inc e                ; NMOS JMP ($xxFF) wraps within the same page
    ld (scr3), de
    xor a, a
    ld (scr3 + 2), a
    ld de, (scr3)
    call read_de         ; pointer hi
    ld d, a
    ld a, (op_lo)
    ld e, a
    push de
    pop iy
    CYC 5
    jp main_loop

/* de = word at zp (op + X); both zp accesses wrap; clobbers a, b, c, hl */
.macro LOAD_ADDR_INDX
    FETCH_A
    ld b, a
    ld a, (_cpu + 4)
    add a, b
    ld c, a
    ld de, 0
    ld e, a
    ld hl, (_mem_low)
    add hl, de
    ld b, (hl)           ; pointer lo
    inc c                ; 8-bit: (zp+1) & 0xff
    ld de, 0
    ld e, c
    ld hl, (_mem_low)
    add hl, de
    ld d, (hl)           ; pointer hi (DEU still 0)
    ld e, b
.endm

lda_indx_h:
    LOAD_ADDR_INDX
    call read_de
    ld (_cpu + 3), a
    SET_NZ
    CYC 6
    jp main_loop

sta_indx_h:
    LOAD_ADDR_INDX
    ld a, (_cpu + 3)
    ld (wr_val), a
    call write_de
    CYC 6
    jp main_loop

.macro LOGIC_INDX op
    LOAD_ADDR_INDX
    call read_de
    ld b, a
    ld a, (_cpu + 3)
    \op a, b
    ld (_cpu + 3), a
    SET_NZ
    CYC 6
    jp main_loop
.endm

ora_indx_h: LOGIC_INDX or
and_indx_h: LOGIC_INDX and
eor_indx_h: LOGIC_INDX xor

adc_indx_h:
    LOAD_ADDR_INDX
    call read_de
    ld b, a
    call adc_core
    CYC 6
    jp main_loop
sbc_indx_h:
    LOAD_ADDR_INDX
    call read_de
    ld b, a
    call sbc_core
    CYC 6
    jp main_loop
cmp_indx_h:
    LOAD_ADDR_INDX
    call read_de
    ld b, a
    call cmpa_core
    CYC 6
    jp main_loop

/* zp,X shift: direct mem_low RMW, keeps the $0001 bank latch */
.macro RMW_ZPX core
    FETCH_A
    ld b, a
    ld a, (_cpu + 4)
    add a, b
    ld de, 0
    ld e, a
    call read_de
    ld (wr_val), a
    call write_de
    ld a, (wr_val)
    call \core
    ld (wr_val), a
    call write_de
    CYC 6
    jp main_loop
.endm

asl_zpx_h: RMW_ZPX asl_core
lsr_zpx_h: RMW_ZPX lsr_core
rol_zpx_h: RMW_ZPX rol_core
ror_zpx_h: RMW_ZPX ror_core

.macro SHIFT_ABSI core
    FETCH_DE16
    ld a, (_cpu + 4)
    ld hl, 0
    ld l, a
    add hl, de
    HL16_TO_DE
    call read_de
    ld (wr_val), a
    call write_de
    ld a, (wr_val)
    call \core
    ld (wr_val), a
    call write_de
    CYC 7
    jp main_loop
.endm

asl_absx_h: SHIFT_ABSI asl_core
lsr_absx_h: SHIFT_ABSI lsr_core
rol_absx_h: SHIFT_ABSI rol_core
ror_absx_h: SHIFT_ABSI ror_core

inc_zpx_h:
    FETCH_A
    ld b, a
    ld a, (_cpu + 4)
    add a, b
    ld de, 0
    ld e, a
    RMW_EXEC inc_core, 6
dec_zpx_h:
    FETCH_A
    ld b, a
    ld a, (_cpu + 4)
    add a, b
    ld de, 0
    ld e, a
    RMW_EXEC dec_core, 6

inc_core:
    inc a
    SET_NZ
    ret
dec_core:
    dec a
    SET_NZ
    ret

.macro LOGIC_CORE name, operation
\name:
    ld a, (_cpu + 3)
    \operation a, b
    ld (_cpu + 3), a
    SET_NZ
    ret
.endm

LOGIC_CORE ora_core, or
LOGIC_CORE and_core, and
LOGIC_CORE eor_core, xor

.macro RMW_FAMILY name, core, after
\name\()_zp_h:
    FETCH_A
    ld de, 0
    ld e, a
    RMW_EXEC \core, 5, \after
\name\()_zpx_h:
    FETCH_A
    ld b, a
    ld a, (_cpu + 4)
    add a, b
    ld de, 0
    ld e, a
    RMW_EXEC \core, 6, \after
\name\()_abs_h:
    FETCH_DE16
    RMW_EXEC \core, 6, \after
\name\()_absx_h:
    LOAD_ADDR_ABSI 4
    RMW_EXEC \core, 7, \after
\name\()_absy_h:
    LOAD_ADDR_ABSI 5
    RMW_EXEC \core, 7, \after
\name\()_indx_h:
    LOAD_ADDR_INDX
    RMW_EXEC \core, 8, \after
\name\()_indy_h:
    LOAD_ADDR_INDY
    RMW_EXEC \core, 8, \after
.endm

RMW_FAMILY slo, asl_core, ora_core
RMW_FAMILY rla, rol_core, and_core
RMW_FAMILY sre, lsr_core, eor_core
RMW_FAMILY rra, ror_core, adc_core
RMW_FAMILY dcp, dec_core, cmpa_core
RMW_FAMILY isc, inc_core, sbc_core

.macro LAX_DONE cycles
    ld a, b
    ld (_cpu + 3), a
    ld (_cpu + 4), a
    SET_NZ
    CYC \cycles
    jp main_loop
.endm

lax_imm_h:
    FETCH_A
    ld b, a
    LAX_DONE 2
xaa_h:
    FETCH_A
    ld b, a
    ld a, (_cpu + 4)
    and a, b
    ld (_cpu + 3), a
    SET_NZ
    CYC 2
    jp main_loop
lax_zp_h:
    LOAD_B_ZP
    LAX_DONE 3
lax_zpy_h:
    LOAD_B_ZPI 5
    LAX_DONE 4
lax_abs_h:
    LOAD_B_ABS
    LAX_DONE 4
lax_absy_h:
    LOAD_B_ABSI 5
    LAX_DONE 4
las_absy_h:
    LOAD_B_ABSI 5
    ld a, (_cpu + 2)
    and a, b
    ld b, a
    ld (_cpu + 2), a
    LAX_DONE 4
lax_indx_h:
    LOAD_ADDR_INDX
    call read_de
    ld b, a
    LAX_DONE 6
lax_indy_h:
    LOAD_B_INDY
    LAX_DONE 5

.macro SAX_DONE cycles
    ld a, (_cpu + 4)
    ld b, a
    ld a, (_cpu + 3)
    and a, b
    ld (wr_val), a
    call write_de
    CYC \cycles
    jp main_loop
.endm

sax_zp_h:
    FETCH_A
    ld de, 0
    ld e, a
    SAX_DONE 3
sax_zpy_h:
    FETCH_A
    ld b, a
    ld a, (_cpu + 5)
    add a, b
    ld de, 0
    ld e, a
    SAX_DONE 4
sax_abs_h:
    FETCH_DE16
    SAX_DONE 4
sax_indx_h:
    LOAD_ADDR_INDX
    SAX_DONE 6

anc_h:
    FETCH_A
    ld b, a
    call and_core
    rlca
    and a, 1
    ld (_cpu + 6), a
    CYC 2
    jp main_loop

alr_h:
    FETCH_A
    ld b, a
    ld a, (_cpu + 3)
    and a, b
    call lsr_core
    ld (_cpu + 3), a
    CYC 2
    jp main_loop

arr_h:
    FETCH_A
    ld b, a
    ld a, (_cpu + 3)
    and a, b
    ld b, a
    ld a, (_cpu + 6)
    rra
    ld a, b
    rra
    ld (_cpu + 3), a
    SET_NZ
    ld c, a
    rlca
    rlca
    and a, 1
    ld (_cpu + 6), a
    ld a, c
    add a, a
    xor a, c
    rlca
    rlca
    and a, 1
    ld (_cpu + 11), a
    CYC 2
    jp main_loop

sbx_h:
    FETCH_A
    ld b, a
    ld a, (_cpu + 4)
    ld c, a
    ld a, (_cpu + 3)
    and a, c
    sub a, b
    ld (_cpu + 4), a
    SET_NZ
    ld a, 0
    jr c, 9f
    inc a
9:  ld (_cpu + 6), a
    CYC 2
    jp main_loop

nop_imm_h:
    FETCH_A
    CYC 2
    jp main_loop
nop_zp_h:
    LOAD_B_ZP
    CYC 3
    jp main_loop
nop_zpx_h:
    LOAD_B_ZPI 4
    CYC 4
    jp main_loop
nop_abs_h:
    LOAD_B_ABS
    CYC 4
    jp main_loop
nop_absx_h:
    LOAD_B_ABSI 4
    CYC 4
    jp main_loop

/* ---- dispatch table (4-byte entries: op << 2) --------------------------- */

.macro OPS a, b, c, d
    .long \a, \b, \c, \d
.endm

op_table:
    OPS brk_h,     ora_indx_h, trap_02_h, slo_indx_h
    OPS nop_zp_h,    ora_zp_h,   asl_zp_h,    slo_zp_h
    OPS php_h,       ora_imm_h,  asl_a_h,     anc_h
    OPS nop_abs_h,   ora_abs_h,  asl_abs_h,   slo_abs_h
    OPS bpl_h,       ora_indy_h, trap_12_h, slo_indy_h
    OPS nop_zpx_h,   ora_zpx_h,  asl_zpx_h,   slo_zpx_h
    OPS clc_h,       ora_absy_h, nop_h,       slo_absy_h
    OPS nop_absx_h,  ora_absx_h, asl_absx_h,  slo_absx_h
    OPS jsr_h,       and_indx_h, trap_22_h,   rla_indx_h
    OPS bit_zp_h,    and_zp_h,   rol_zp_h,    rla_zp_h
    OPS plp_h,       and_imm_h,  rol_a_h,     anc_h
    OPS bit_abs_h,   and_abs_h,  rol_abs_h,   rla_abs_h
    OPS bmi_h,       and_indy_h, jam_32_h, rla_indy_h
    OPS nop_zpx_h,   and_zpx_h,  rol_zpx_h,   rla_zpx_h
    OPS sec_h,       and_absy_h, nop_h,       rla_absy_h
    OPS nop_absx_h,  and_absx_h, rol_absx_h,  rla_absx_h
    OPS rti_h,       eor_indx_h, jam_42_h, sre_indx_h
    OPS nop_zp_h,    eor_zp_h,   lsr_zp_h,    sre_zp_h
    OPS pha_h,       eor_imm_h,  lsr_a_h,     alr_h
    OPS jmp_h,       eor_abs_h,  lsr_abs_h,   sre_abs_h
    OPS bvc_h,       eor_indy_h, jam_52_h, sre_indy_h
    OPS nop_zpx_h,   eor_zpx_h,  lsr_zpx_h,   sre_zpx_h
    OPS cli_h,       eor_absy_h, nop_h,       sre_absy_h
    OPS nop_absx_h,  eor_absx_h, lsr_absx_h,  sre_absx_h
    OPS rts_h,       adc_indx_h, jam_62_h, rra_indx_h
    OPS nop_zp_h,    adc_zp_h,   ror_zp_h,    rra_zp_h
    OPS pla_h,       adc_imm_h,  ror_a_h,     arr_h
    OPS jmp_ind_h,   adc_abs_h,  ror_abs_h,   rra_abs_h
    OPS bvs_h,       adc_indy_h, jam_72_h, rra_indy_h
    OPS nop_zpx_h,   adc_zpx_h,  ror_zpx_h,   rra_zpx_h
    OPS sei_h,       adc_absy_h, nop_h,       rra_absy_h
    OPS nop_absx_h,  adc_absx_h, ror_absx_h,  rra_absx_h
    OPS nop_imm_h,   sta_indx_h, nop_imm_h,   sax_indx_h
    OPS sty_zp_h,    sta_zp_h,   stx_zp_h,    sax_zp_h
    OPS dey_h,       nop_imm_h,  txa_h,       xaa_h
    OPS sty_abs_h,   sta_abs_h,  stx_abs_h,   sax_abs_h
    OPS bcc_h,       sta_indy_h, jam_92_h, ahx_indy_h
    OPS sty_zpx_h,   sta_zpx_h,  stx_zpy_h,   sax_zpy_h
    OPS tya_h,       sta_absy_h, txs_h,       tas_absy_h
    OPS shy_absx_h,  sta_absx_h, shx_absy_h,  ahx_absy_h
    OPS ldy_imm_h,   lda_indx_h, ldx_imm_h,   lax_indx_h
    OPS ldy_zp_h,    lda_zp_h,   ldx_zp_h,    lax_zp_h
    OPS tay_h,       lda_imm_h,  tax_h,       lax_imm_h
    OPS ldy_abs_h,   lda_abs_h,  ldx_abs_h,   lax_abs_h
    OPS bcs_h,       lda_indy_h, jam_b2_h, lax_indy_h
    OPS ldy_zpx_h,   lda_zpx_h,  ldx_zpy_h,   lax_zpy_h
    OPS clv_h,       lda_absy_h, tsx_h,       las_absy_h
    OPS ldy_absx_h,  lda_absx_h, ldx_absy_h,  lax_absy_h
    OPS cpy_imm_h,   cmp_indx_h, nop_imm_h,   dcp_indx_h
    OPS cpy_zp_h,    cmp_zp_h,   dec_zp_h,    dcp_zp_h
    OPS iny_h,       cmp_imm_h,  dex_h,       sbx_h
    OPS cpy_abs_h,   cmp_abs_h,  dec_abs_h,   dcp_abs_h
    OPS bne_h,       cmp_indy_h, jam_d2_h, dcp_indy_h
    OPS nop_zpx_h,   cmp_zpx_h,  dec_zpx_h,   dcp_zpx_h
    OPS cld_h,       cmp_absy_h, nop_h,       dcp_absy_h
    OPS nop_absx_h,  cmp_absx_h, dec_absx_h,  dcp_absx_h
    OPS cpx_imm_h,   sbc_indx_h, nop_imm_h,   isc_indx_h
    OPS cpx_zp_h,    sbc_zp_h,   inc_zp_h,    isc_zp_h
    OPS inx_h,       sbc_imm_h,  nop_h,       sbc_imm_h
    OPS cpx_abs_h,   sbc_abs_h,  inc_abs_h,   isc_abs_h
    OPS beq_h,       sbc_indy_h, jam_f2_h, isc_indy_h
    OPS nop_zpx_h,   sbc_zpx_h,  inc_zpx_h,   isc_zpx_h
    OPS sed_h,       sbc_absy_h, nop_h,       isc_absy_h
    OPS nop_absx_h,  sbc_absx_h, inc_absx_h,  isc_absx_h

.section .bss,"aw",@nobits
until_cyc: .space 3
scr3:      .space 3
op_lo:     .space 1
wr_val:    .space 1
last_z:    .space 1
last_n:    .space 1
.global _cpu_fallbacks
_cpu_fallbacks:   .space 3
.global _cpu_fallback_op
_cpu_fallback_op: .space 1
.global _cpu_fallback_counts
_cpu_fallback_counts: .space 512
.global _cpu_jit_hits
_cpu_jit_hits:    .space 3
.global _cpu_jit_hits2
_cpu_jit_hits2:   .space 3
.global _cpu_jit_hits3
_cpu_jit_hits3:   .space 3
.global _cpu_jit_hits4
_cpu_jit_hits4:   .space 3
.global _cpu_jit_hits5
_cpu_jit_hits5:   .space 3
.global _cpu_jit_copy_attempts
_cpu_jit_copy_attempts: .space 3
.global _cpu_jit_copy_stage
_cpu_jit_copy_stage: .space 1

.section ".note.GNU-stack","",@progbits
