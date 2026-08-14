.assume adl=1

.section .text,"ax",@progbits

/*
 * Hybrid 6510 core: the ~70 most executed opcodes run in eZ80 assembly,
 * everything else (illegals, BCD, RMW combos, drive traps, IRQ paths)
 * falls back to the C core one instruction at a time via _cpu_step_slow.
 *
 * Register plan while the core runs:
 *   IY = 6502 PC (upper byte 0), IX = cycles REMAINING in the batch minus 1
 *   (countdown: negative = batch done, so the per-instruction check is a
 *   sign test instead of a 24-bit compare; cpu.cycles = until - 1 - IX).
 * Both are callee-saved, so C helpers called from here preserve them; the
 * fallback path re-syncs both with the CpuState struct around the call.
 *
 * CpuState offsets are pinned by _Static_asserts in cpu.c:
 *   pc=0 sp=2 a=3 x=4 y=5 cf=6 zf=7 idf=8 dmf=9 bcf=10 of=11 nf=12 cycles=13
 */
.extern _cpu
.extern _mem_rd_pg
.extern _mem_wr_pg
.extern _mem_low
.extern _mem_read_io
.extern _mem_write_io
.extern _mem_setup_banks
.extern _cpu_step_slow
.extern _cpu_warp

/* a = code byte at pc, pc++ (no IO check: pc never points at IO) */
.macro FETCH_A
    push iy
    pop de
    inc iy
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

/* de = 16-bit operand (lo, hi), DEU = 0 */
.macro FETCH_DE16
    FETCH_A
    ld (op_lo), a
    FETCH_A
    ld de, 0
    ld d, a
    ld a, (op_lo)
    ld e, a
.endm

/* zf/nf from a; preserves a (via b) */
.macro SET_NZ
    ld b, a
    rlca
    and a, 1
    ld (_cpu + 12), a
    ld a, b
    or a, a
    ld a, 0
    jr nz, 8f
    inc a
8:  ld (_cpu + 7), a
    ld a, b
.endm

.macro CYC n
    ld de, -\n
    add ix, de
.endm

/* hl (16-bit result, upper byte dirty) -> de with DEU = 0 */
.macro HL16_TO_DE
    ld (scr3), hl
    xor a, a
    ld (scr3 + 2), a
    ld de, (scr3)
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
    ld hl, 1
    or a, a
    sbc hl, de
    ret nz
    ld a, (wr_val)
    ld bc, 0
    ld c, a
    push iy
    push bc
    call _mem_setup_banks
    pop bc
    pop iy
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
    rlca
    and a, 1
    ld (_cpu + 12), a
    ld a, b
    rlca
    rlca
    and a, 1
    ld (_cpu + 11), a
    ld a, (_cpu + 3)
    and a, b
    ld a, 0
    jr nz, 9f
    inc a
9:  ld (_cpu + 7), a
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

main_loop:
    push ix
    pop hl
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

sync_out:                    ; iy/ix -> cpu.pc/cpu.cycles
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
    ret

/* fallback: rewind the consumed opcode and run one C step */
op_fallback:
    ld de, 0xffffff
    add iy, de
    call sync_out
    call _cpu_step_slow
    or a, a
    jp z, run_fail
    ld hl, (until_cyc)       ; ix = until - cycles - 1 (main_loop re-checks)
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
    jp main_loop

/* taken branch: c = offset, iy = pc after operand */
do_branch:
    push iy
    pop hl
    ld a, h
    ld (br_old), a
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
    ld b, a
    ld a, (br_old)
    cp a, b
    ld de, -1                ; taken +1, page cross +2 (countdown)
    jr z, 2f
    dec de
2:  add ix, de
    jp main_loop

/* ---- opcode macro bodies ------------------------------------------------ */

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
    ld hl, 0
    ld l, a
    add hl, de
    HL16_TO_DE
    call read_de
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
    ld hl, 0
    ld l, a
    add hl, de
    HL16_TO_DE
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

.macro LOAD_B_ABS
    FETCH_DE16
    call read_de
    ld b, a
.endm

.macro LOAD_B_ABSI idxoff
    FETCH_DE16
    ld a, (_cpu + \idxoff)
    ld hl, 0
    ld l, a
    add hl, de
    HL16_TO_DE
    call read_de
    ld b, a
.endm

.macro LOAD_B_INDY
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
    ld hl, 0
    ld l, c
    ld h, b
    ld a, (_cpu + 5)
    ld de, 0
    ld e, a
    add hl, de
    HL16_TO_DE
    call read_de
    ld b, a
.endm

.macro ADCSBC_PRE
    ld a, (_cpu + 9)
    or a, a
    jp nz, op_fallback
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
    ld a, (_cpu + 5)
    ld hl, 0
    ld l, c
    ld h, b
    ld de, 0
    ld e, a
    add hl, de
    HL16_TO_DE
    call read_de
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

sta_abs_h:  ST_ABS 3
stx_abs_h:  ST_ABS 4
sty_abs_h:  ST_ABS 5
sta_absx_h: ST_ABSI 3, 4
sta_absy_h: ST_ABSI 3, 5

sta_indy_h:
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
    ld a, (_cpu + 5)
    ld hl, 0
    ld l, c
    ld h, b
    ld de, 0
    ld e, a
    add hl, de
    HL16_TO_DE
    ld a, (_cpu + 3)
    ld (wr_val), a
    call write_de
    CYC 6
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

bne_h: BR_IF 7, 0
beq_h: BR_IF 7, 1
bcc_h: BR_IF 6, 0
bcs_h: BR_IF 6, 1
bpl_h: BR_IF 12, 0
bmi_h: BR_IF 12, 1
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
    ADCSBC_PRE
    LOAD_B_ZP
    call adc_core
    CYC 3
    jp main_loop
adc_abs_h:
    ADCSBC_PRE
    LOAD_B_ABS
    call adc_core
    CYC 4
    jp main_loop
adc_absx_h:
    ADCSBC_PRE
    LOAD_B_ABSI 4
    call adc_core
    CYC 4
    jp main_loop
adc_absy_h:
    ADCSBC_PRE
    LOAD_B_ABSI 5
    call adc_core
    CYC 4
    jp main_loop
adc_indy_h:
    ADCSBC_PRE
    LOAD_B_INDY
    call adc_core
    CYC 5
    jp main_loop

sbc_zp_h:
    ADCSBC_PRE
    LOAD_B_ZP
    call sbc_core
    CYC 3
    jp main_loop
sbc_abs_h:
    ADCSBC_PRE
    LOAD_B_ABS
    call sbc_core
    CYC 4
    jp main_loop
sbc_absx_h:
    ADCSBC_PRE
    LOAD_B_ABSI 4
    call sbc_core
    CYC 4
    jp main_loop
sbc_absy_h:
    ADCSBC_PRE
    LOAD_B_ABSI 5
    call sbc_core
    CYC 4
    jp main_loop
sbc_indy_h:
    ADCSBC_PRE
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
    call \core
    ld (wr_val), a
    call write_de
    CYC 5
    jp main_loop
.endm

.macro SHIFT_ABS core
    FETCH_DE16
    call read_de
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
    ld a, (_cpu + 9)
    or a, a
    jp nz, op_fallback       ; BCD: C core handles it
    FETCH_A
    ld b, a
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
    CYC 2
    jp main_loop

sbc_imm_h:
    ld a, (_cpu + 9)
    or a, a
    jp nz, op_fallback
    FETCH_A
    ld b, a
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
    ld c, a
    ld de, 0
    ld e, a
    ld hl, (_mem_low)
    add hl, de
    inc (hl)
    jr .idzp_done
dec_zp_h:
    FETCH_A
    ld c, a
    ld de, 0
    ld e, a
    ld hl, (_mem_low)
    add hl, de
    dec (hl)
.idzp_done:
    ld a, (hl)
    SET_NZ
    CYC 5
    ld a, c
    dec a
    jp nz, main_loop
    ld a, (hl)
    ld bc, 0
    ld c, a
    push iy
    push bc
    call _mem_setup_banks
    pop bc
    pop iy
    jp main_loop

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
    jp main_loop

jsr_h:
    FETCH_DE16
    push iy
    pop hl
    dec hl
    push de              ; push6502 clobbers de: save the jump target
    ld b, h
    push hl
    call push6502
    pop hl
    ld b, l
    call push6502
    pop iy               ; saved target -> pc
    CYC 6
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
    jp main_loop

nop_h:
    CYC 2
    jp main_loop

/* 6502 P byte from flag bytes -> a; clobbers b */
flags_pack:
    ld a, (_cpu + 12)
    rrca                 ; nf -> bit7
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
    ld a, (_cpu + 7)
    add a, a             ; zf -> bit1
    or a, b
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
    ld (_cpu + 7), a
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
    rlca
    and a, 1
    ld (_cpu + 12), a
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
    CYC 7
    jp main_loop

jmp_ind_h:
    FETCH_DE16
    call read_de         ; pointer lo
    ld (op_lo), a
    inc de               ; 16-bit wrap: drop any carry out of bit 15
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
    CYC 3
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
    ADCSBC_PRE
    LOAD_ADDR_INDX
    call read_de
    ld b, a
    call adc_core
    CYC 6
    jp main_loop
sbc_indx_h:
    ADCSBC_PRE
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
    ld c, a
    ld de, 0
    ld e, a
    ld hl, (_mem_low)
    add hl, de
    ld a, (hl)
    push bc
    push hl
    call \core
    pop hl
    pop bc
    ld (hl), a
    CYC 6
    ld a, c
    dec a
    jp nz, main_loop
    ld a, (hl)
    ld bc, 0
    ld c, a
    push iy
    push bc
    call _mem_setup_banks
    pop bc
    pop iy
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
    ld c, a
    ld de, 0
    ld e, a
    ld hl, (_mem_low)
    add hl, de
    inc (hl)
    jr .idzpx_done
dec_zpx_h:
    FETCH_A
    ld b, a
    ld a, (_cpu + 4)
    add a, b
    ld c, a
    ld de, 0
    ld e, a
    ld hl, (_mem_low)
    add hl, de
    dec (hl)
.idzpx_done:
    ld a, (hl)
    SET_NZ
    CYC 6
    ld a, c
    dec a
    jp nz, main_loop
    ld a, (hl)
    ld bc, 0
    ld c, a
    push iy
    push bc
    call _mem_setup_banks
    pop bc
    pop iy
    jp main_loop

/* ---- dispatch table (4-byte entries: op << 2) --------------------------- */

.macro OPS a, b, c, d
    .long \a, \b, \c, \d
.endm

op_table:
    OPS op_fallback, ora_indx_h,  op_fallback, op_fallback  ; 00
    OPS op_fallback, ora_zp_h,    asl_zp_h,    op_fallback  ; 04
    OPS php_h,       ora_imm_h,   asl_a_h,     op_fallback  ; 08
    OPS op_fallback, ora_abs_h,   asl_abs_h,   op_fallback  ; 0C
    OPS bpl_h,       ora_indy_h,  op_fallback, op_fallback  ; 10
    OPS op_fallback, op_fallback, asl_zpx_h,   op_fallback  ; 14
    OPS clc_h,       ora_absy_h,  op_fallback, op_fallback  ; 18
    OPS op_fallback, ora_absx_h,  asl_absx_h,  op_fallback  ; 1C
    OPS jsr_h,       and_indx_h,  op_fallback, op_fallback  ; 20
    OPS bit_zp_h,    and_zp_h,    rol_zp_h,    op_fallback  ; 24
    OPS plp_h,       and_imm_h,   rol_a_h,     op_fallback  ; 28
    OPS bit_abs_h,   and_abs_h,   rol_abs_h,   op_fallback  ; 2C
    OPS bmi_h,       and_indy_h,  op_fallback, op_fallback  ; 30
    OPS op_fallback, op_fallback, rol_zpx_h,   op_fallback  ; 34
    OPS sec_h,       and_absy_h,  op_fallback, op_fallback  ; 38
    OPS op_fallback, and_absx_h,  rol_absx_h,  op_fallback  ; 3C
    OPS rti_h,       eor_indx_h,  op_fallback, op_fallback  ; 40
    OPS op_fallback, eor_zp_h,    lsr_zp_h,    op_fallback  ; 44
    OPS pha_h,       eor_imm_h,   lsr_a_h,     op_fallback  ; 48
    OPS jmp_h,       eor_abs_h,   lsr_abs_h,   op_fallback  ; 4C
    OPS bvc_h,       eor_indy_h,  op_fallback, op_fallback  ; 50
    OPS op_fallback, op_fallback, lsr_zpx_h,   op_fallback  ; 54
    OPS cli_h,       eor_absy_h,  op_fallback, op_fallback  ; 58
    OPS op_fallback, eor_absx_h,  lsr_absx_h,  op_fallback  ; 5C
    OPS rts_h,       adc_indx_h,  op_fallback, op_fallback  ; 60
    OPS op_fallback, adc_zp_h,    ror_zp_h,    op_fallback  ; 64
    OPS pla_h,       adc_imm_h,   ror_a_h,     op_fallback  ; 68
    OPS jmp_ind_h,   adc_abs_h,   ror_abs_h,   op_fallback  ; 6C
    OPS bvs_h,       adc_indy_h,  op_fallback, op_fallback  ; 70
    OPS op_fallback, op_fallback, ror_zpx_h,   op_fallback  ; 74
    OPS sei_h,       adc_absy_h,  op_fallback, op_fallback  ; 78
    OPS op_fallback, adc_absx_h,  ror_absx_h,  op_fallback  ; 7C
    OPS op_fallback, sta_indx_h,  op_fallback, op_fallback  ; 80
    OPS sty_zp_h,    sta_zp_h,    stx_zp_h,    op_fallback  ; 84
    OPS dey_h,       op_fallback, txa_h,       op_fallback  ; 88
    OPS sty_abs_h,   sta_abs_h,   stx_abs_h,   op_fallback  ; 8C
    OPS bcc_h,       sta_indy_h,  op_fallback, op_fallback  ; 90
    OPS op_fallback, sta_zpx_h,   op_fallback, op_fallback  ; 94
    OPS tya_h,       sta_absy_h,  txs_h,       op_fallback  ; 98
    OPS op_fallback, sta_absx_h,  op_fallback, op_fallback  ; 9C
    OPS ldy_imm_h,   lda_indx_h,  ldx_imm_h,   op_fallback  ; A0
    OPS ldy_zp_h,    lda_zp_h,    ldx_zp_h,    op_fallback  ; A4
    OPS tay_h,       lda_imm_h,   tax_h,       op_fallback  ; A8
    OPS ldy_abs_h,   lda_abs_h,   ldx_abs_h,   op_fallback  ; AC
    OPS bcs_h,       lda_indy_h,  op_fallback, op_fallback  ; B0
    OPS ldy_zpx_h,   lda_zpx_h,   ldx_zpy_h,   op_fallback  ; B4
    OPS clv_h,       lda_absy_h,  tsx_h,       op_fallback  ; B8
    OPS ldy_absx_h,  lda_absx_h,  ldx_absy_h,  op_fallback  ; BC
    OPS cpy_imm_h,   cmp_indx_h,  op_fallback, op_fallback  ; C0
    OPS cpy_zp_h,    cmp_zp_h,    dec_zp_h,    op_fallback  ; C4
    OPS iny_h,       cmp_imm_h,   dex_h,       op_fallback  ; C8
    OPS cpy_abs_h,   cmp_abs_h,   dec_abs_h,   op_fallback  ; CC
    OPS bne_h,       cmp_indy_h,  op_fallback, op_fallback  ; D0
    OPS op_fallback, op_fallback, dec_zpx_h,   op_fallback  ; D4
    OPS cld_h,       cmp_absy_h,  op_fallback, op_fallback  ; D8
    OPS op_fallback, cmp_absx_h,  dec_absx_h,  op_fallback  ; DC
    OPS cpx_imm_h,   sbc_indx_h,  op_fallback, op_fallback  ; E0
    OPS cpx_zp_h,    sbc_zp_h,    inc_zp_h,    op_fallback  ; E4
    OPS inx_h,       sbc_imm_h,   nop_h,       op_fallback  ; E8
    OPS cpx_abs_h,   sbc_abs_h,   inc_abs_h,   op_fallback  ; EC
    OPS beq_h,       sbc_indy_h,  op_fallback, op_fallback  ; F0
    OPS op_fallback, op_fallback, inc_zpx_h,   op_fallback  ; F4
    OPS sed_h,       sbc_absy_h,  op_fallback, op_fallback  ; F8
    OPS op_fallback, sbc_absx_h,  inc_absx_h,  op_fallback  ; FC

.section .bss,"aw",@nobits
until_cyc: .space 3
scr3:      .space 3
op_lo:     .space 1
wr_val:    .space 1
br_old:    .space 1

.section ".note.GNU-stack","",@progbits
