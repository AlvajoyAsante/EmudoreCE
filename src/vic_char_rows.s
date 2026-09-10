.assume adl=1

.section .text,"ax",@progbits

.extern _vic_char_dst
.extern _vic_char_screen
.extern _vic_char_colram
.extern _vic_char_glyphs
.extern _vic_char_cells
.extern _vic_char_rows
.extern _vic_char_multicolor

.global _vic_char_expand_rows
.type _vic_char_expand_rows, @function
_vic_char_expand_rows:
    ld a, (_vic_char_cells)
    or a, a
    ret z
    cp a, 41
    ret nc
    ld a, (_vic_char_rows)
    or a, a
    ret z
    cp a, 9
    ret nc

    push ix
    push iy
    ld hl, (_vic_char_screen)
    ld (.Lchar_screen), hl
    ld hl, (_vic_char_colram)
    ld (.Lchar_colram), hl
    ld hl, (_vic_char_dst)
    ld (.Lchar_dst), hl
    ld a, (_vic_char_cells)
    ld (.Lchar_cells), a

.Lchar_cell:
    ld hl, (.Lchar_screen)
    ld a, (hl)
    inc hl
    ld (.Lchar_screen), hl
    ld hl, 0x0800
    ld l, a
    mlt hl
    ld de, (_vic_char_glyphs)
    add hl, de
    push hl
    pop ix

    ld hl, (.Lchar_colram)
    ld a, (hl)
    inc hl
    ld (.Lchar_colram), hl
    and a, 0x0f
    ld c, a
    ld a, (_vic_char_multicolor)
    or a, a
    jr z, .Lchar_hires
    bit 3, c
    jr z, .Lchar_hires
    ld a, c
    and a, 7
    ld de, _vic_char_mc_lut
    jr .Lchar_color

.Lchar_hires:
    ld a, c
    ld de, _vic_char_hires_lut

.Lchar_color:
    ld hl, 0x4000
    ld l, a
    mlt hl
    add hl, de
    push hl
    pop iy
    ld de, (.Lchar_dst)
    ld a, (_vic_char_rows)
    ld (.Lchar_rows), a

.Lchar_row:
    ld a, (ix)
    inc ix
    push af
    rrca
    rrca
    and a, 0x3c
    lea hl, iy+0
    or a, l
    ld l, a
    ld bc, 8
    ldi
    ldi
    ldi
    ldi
    pop af
    add a, a
    add a, a
    and a, 0x3c
    lea hl, iy+0
    or a, l
    ld l, a
    ldi
    ldi
    ldi
    ldi

    ld a, (.Lchar_rows)
    dec a
    jr z, .Lchar_next_cell
    ld (.Lchar_rows), a
    ld hl, 312
    add hl, de
    ex de, hl
    jr .Lchar_row

.Lchar_next_cell:
    ld hl, (.Lchar_dst)
    ld de, 8
    add hl, de
    ld (.Lchar_dst), hl
    ld a, (.Lchar_cells)
    dec a
    ld (.Lchar_cells), a
    jp nz, .Lchar_cell
    pop iy
    pop ix
    ret
.size _vic_char_expand_rows, .-_vic_char_expand_rows

.section .bss,"aw",@nobits
.balign 256
.global _vic_char_hires_lut
.type _vic_char_hires_lut, @object
_vic_char_hires_lut:
    .space 1024
.size _vic_char_hires_lut, .-_vic_char_hires_lut
.global _vic_char_mc_lut
.type _vic_char_mc_lut, @object
_vic_char_mc_lut:
    .space 512
.size _vic_char_mc_lut, .-_vic_char_mc_lut
.Lchar_screen:
    .space 3
.Lchar_colram:
    .space 3
.Lchar_dst:
    .space 3
.Lchar_cells:
    .space 1
.Lchar_rows:
    .space 1

.section ".note.GNU-stack","",@progbits
