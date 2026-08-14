.assume adl=1

.section .text,"ax",@progbits

/*
 * Expand 40 C64 high-resolution bitmap cells into 320 8bpp framebuffer
 * pixels. C sets the four globals before calling this routine:
 *   dst, bitmap bytes (one byte every eight), screen colour bytes, count.
 *
 * IX/IY are callee-saved in the ez80 C ABI, so retain them even though this
 * routine otherwise needs no C stack arguments.
 */
.extern _vic_row_dst
.extern _vic_row_bitmap
.extern _vic_row_screen
.extern _vic_row_cells

.global _vic_expand_hires_row
.type _vic_expand_hires_row, @function
_vic_expand_hires_row:
    push ix
    push iy
    ld hl, (_vic_row_dst)
    ld ix, (_vic_row_screen)
    ld iy, (_vic_row_bitmap)
    ld a, (_vic_row_cells)
    ld b, a

.Lcell:
    ld c, (iy)
    inc iy
    ld a, (ix)
    inc ix
    ld d, a
    and a, 0x0f
    ld e, a
    ld a, d
    rrca
    rrca
    rrca
    rrca
    and a, 0x0f
    or a, 0x20    ; IO_TAG_FOREGROUND: palette 32-47 mirrors 0-15
    ld d, a

    rlc c
    jr nc, .Lbg0
    ld a, d
    jr .Lwrite0
.Lbg0:
    ld a, e
.Lwrite0:
    ld (hl), a
    inc hl

    rlc c
    jr nc, .Lbg1
    ld a, d
    jr .Lwrite1
.Lbg1:
    ld a, e
.Lwrite1:
    ld (hl), a
    inc hl

    rlc c
    jr nc, .Lbg2
    ld a, d
    jr .Lwrite2
.Lbg2:
    ld a, e
.Lwrite2:
    ld (hl), a
    inc hl

    rlc c
    jr nc, .Lbg3
    ld a, d
    jr .Lwrite3
.Lbg3:
    ld a, e
.Lwrite3:
    ld (hl), a
    inc hl

    rlc c
    jr nc, .Lbg4
    ld a, d
    jr .Lwrite4
.Lbg4:
    ld a, e
.Lwrite4:
    ld (hl), a
    inc hl

    rlc c
    jr nc, .Lbg5
    ld a, d
    jr .Lwrite5
.Lbg5:
    ld a, e
.Lwrite5:
    ld (hl), a
    inc hl

    rlc c
    jr nc, .Lbg6
    ld a, d
    jr .Lwrite6
.Lbg6:
    ld a, e
.Lwrite6:
    ld (hl), a
    inc hl

    rlc c
    jr nc, .Lbg7
    ld a, d
    jr .Lwrite7
.Lbg7:
    ld a, e
.Lwrite7:
    ld (hl), a
    inc hl
    djnz .Lcell

    pop iy
    pop ix
    ret

.section ".note.GNU-stack","",@progbits