.assume adl=1

.section .text,"ax",@progbits

/*
 * Expand 40 C64 multicolor bitmap cells into 320 8bpp framebuffer pixels
 * (160 double-wide pixel pairs). Inputs come from the vic_row_* globals.
 *
 * Per-cell colour table lives in a 256-aligned scratch so each 2-bit pixel
 * code indexes it with a single `ld e,a` (page byte stays in D/DEU):
 *   lut[0]=bg slot  lut[1]=screen>>4  lut[2]=(screen&15)|0x20  lut[3]=(colram&15)|0x20
 * 0x20 = IO_TAG_FOREGROUND (MC pairs 10/11 are foreground).
 *
 * No shadow registers: the OS interrupt handlers are not guaranteed to
 * preserve them. IX = screen, IY = bitmap, colour RAM self-increments
 * through mc_cr in BSS.
 */
.extern _vic_row_dst
.extern _vic_row_bitmap
.extern _vic_row_screen
.extern _vic_row_colram
.extern _vic_row_bg
.extern _vic_row_cells

.global _vic_expand_mc_row
.type _vic_expand_mc_row, @function
_vic_expand_mc_row:
    push ix
    push iy
    ld ix, (_vic_row_screen)
    ld iy, (_vic_row_bitmap)
    ld hl, (_vic_row_colram)
    ld (mc_cr), hl
    ld hl, (_vic_row_dst)
    ld de, _vic_mc_lut
    ld a, (_vic_row_bg)
    ld (_vic_mc_lut), a
    ld a, (_vic_row_cells)
    ld b, a

.Lcell:
    push hl
    ld hl, (mc_cr)
    ld a, (hl)
    inc hl
    ld (mc_cr), hl
    pop hl
    and a, 0x0f
    or a, 0x20
    ld (_vic_mc_lut+3), a

    ld a, (ix)
    inc ix
    ld c, a
    rrca
    rrca
    rrca
    rrca
    and a, 0x0f
    ld (_vic_mc_lut+1), a
    ld a, c
    and a, 0x0f
    or a, 0x20
    ld (_vic_mc_lut+2), a

    ld a, (iy)
    lea iy, iy + 8
    ld c, a

    rlca
    rlca
    ld c, a
    and a, 3
    ld e, a
    ld a, (de)
    ld (hl), a
    inc hl
    ld (hl), a
    inc hl

    ld a, c
    rlca
    rlca
    ld c, a
    and a, 3
    ld e, a
    ld a, (de)
    ld (hl), a
    inc hl
    ld (hl), a
    inc hl

    ld a, c
    rlca
    rlca
    ld c, a
    and a, 3
    ld e, a
    ld a, (de)
    ld (hl), a
    inc hl
    ld (hl), a
    inc hl

    ld a, c
    rlca
    rlca
    and a, 3
    ld e, a
    ld a, (de)
    ld (hl), a
    inc hl
    ld (hl), a
    inc hl

    djnz .Lcell

    pop iy
    pop ix
    ret

.section .bss,"aw",@nobits
.balign 256
_vic_mc_lut:
    .space 4
mc_cr:
    .space 3

.section ".note.GNU-stack","",@progbits