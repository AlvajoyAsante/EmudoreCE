.assume adl=1

.extern _cpu

.equ CPU_A, 3
.equ CPU_CF, 6
.equ CPU_ZF, 7
.equ CPU_OF, 11
.equ CPU_NF, 12

.section .text.cpu_decimal_adc,"ax",@progbits
.global _cpu_decimal_adc
.type _cpu_decimal_adc, @function
_cpu_decimal_adc:
    push ix
    ld hl, 6
    add hl, sp
    ld bc, 0
    ld c, (hl)
    ld ix, _cpu
    ld b, (ix + CPU_A)
    ld de, 0
    ld a, (ix + CPU_CF)
    or a, a
    jr z, .Ldecimal_adc_carry_ready
    inc e
.Ldecimal_adc_carry_ready:
    ld a, b
    add a, c
    add a, e
    cp a, 1
    ld a, 0
    adc a, 0
    ld (ix + CPU_ZF), a

    ld a, b
    and a, 15
    ld d, a
    ld a, c
    and a, 15
    add a, d
    add a, e
    cp a, 10
    jr c, .Ldecimal_adc_low_ready
    add a, 6
    and a, 15
    add a, 16
.Ldecimal_adc_low_ready:
    ld hl, 0
    ld l, a
    ld de, 0
    ld a, b
    and a, 240
    ld e, a
    add hl, de
    ld a, c
    and a, 240
    ld e, a
    add hl, de

    ld a, l
    rlca
    and a, 1
    ld (ix + CPU_NF), a
    ld a, b
    xor a, c
    cpl
    ld e, a
    ld a, b
    xor a, l
    and a, e
    rlca
    and a, 1
    ld (ix + CPU_OF), a

    ld a, h
    or a, a
    jr nz, .Ldecimal_adc_adjust
    ld a, l
    cp a, 160
    jr c, .Ldecimal_adc_result
.Ldecimal_adc_adjust:
    ld de, 96
    add hl, de
.Ldecimal_adc_result:
    ld a, h
    or a, a
    ld a, 0
    jr z, .Ldecimal_adc_carry_out
    inc a
.Ldecimal_adc_carry_out:
    ld (ix + CPU_CF), a
    ld (ix + CPU_A), l
    pop ix
    ret
.size _cpu_decimal_adc, . - _cpu_decimal_adc

.section .text.cpu_decimal_sbc,"ax",@progbits
.global _cpu_decimal_sbc
.type _cpu_decimal_sbc, @function
_cpu_decimal_sbc:
    push ix
    ld hl, 6
    add hl, sp
    ld bc, 0
    ld c, (hl)
    ld ix, _cpu
    ld b, (ix + CPU_A)
    ld de, 0
    ld a, (ix + CPU_CF)
    cp a, 1
    ld a, 0
    adc a, 0
    ld d, a
    rra
    ld a, b
    sbc a, c
    ld e, a
    ld a, 0
    adc a, 0
    xor a, 1
    ld (ix + CPU_CF), a

    ld a, e
    cp a, 1
    ld a, 0
    adc a, 0
    ld (ix + CPU_ZF), a
    ld a, e
    rlca
    and a, 1
    ld (ix + CPU_NF), a
    ld a, b
    xor a, e
    ld e, a
    ld a, b
    xor a, c
    and a, e
    rlca
    and a, 1
    ld (ix + CPU_OF), a

    ld hl, 0
    ld a, c
    and a, 15
    add a, d
    ld e, a
    ld a, b
    and a, 15
    sub a, e
    jr nc, .Ldecimal_sbc_low_ready
    sub a, 6
    and a, 15
    or a, 240
    ld hl, -1
.Ldecimal_sbc_low_ready:
    ld l, a
    ld de, 0
    ld a, b
    and a, 240
    ld e, a
    add hl, de
    ld a, c
    and a, 240
    ld e, a
    or a, a
    sbc hl, de
    bit 7, h
    jr z, .Ldecimal_sbc_result
    ld de, -96
    add hl, de
.Ldecimal_sbc_result:
    ld (ix + CPU_A), l
    pop ix
    ret
.size _cpu_decimal_sbc, . - _cpu_decimal_sbc

.section .note.GNU-stack,"",@progbits
