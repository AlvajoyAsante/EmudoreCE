.assume adl=1

.section .text,"ax",@progbits

.global _jit_execute
.type _jit_execute, @function
_jit_execute:
    ld hl, 3
    add hl, sp
    ld hl, (hl)
    jp (hl)

    .global _jit_execute_block
    .type _jit_execute_block, @function
    _jit_execute_block:
        ld hl, 3
        add hl, sp
        ld hl, (hl)
        ld de, 73
        add hl, de
        jp (hl)

.global _jit_bytes_equal
.type _jit_bytes_equal, @function
_jit_bytes_equal:
    ld iy, 3
    add iy, sp
    ld hl, (iy + 0)
    ld de, (iy + 3)
    ld b, (iy + 6)
    ld a, b
    or a, a
    jr z, 2f
1:
    ld a, (de)
    cp a, (hl)
    jr nz, 3f
    inc hl
    inc de
    djnz 1b
2:
    ld a, 1
    ret
3:
    xor a, a
    ret

.extern _cpu
.extern _mem_low
.extern _mem_ram
.extern _mem_rd_pg
.extern _mem_wr_pg
.extern _jit_backend_ok
.extern _jit_blocks
.extern _jit_entry_points
.extern _jit_mapping_generation
.extern _jit_page_generation
.extern _jit_page_generation_wrapped
.extern _jit_native_hits
.extern _jit_read_memory
.extern _jit_write_memory

.global _jit_read_bridge
.type _jit_read_bridge, @function
_jit_read_bridge:
    push iy
    push bc
    push de
    push hl
    push af
    ld de, 0
    ld e, l
    ld d, h
    ld l, h
    ld h, 4
    mlt hl
    ld bc, _mem_rd_pg
    add hl, bc
    ld hl, (hl)
    ld bc, 0
    or a, a
    sbc hl, bc
    jr z, jit_read_slow
    add hl, de
    ld a, (hl)
    jr jit_read_result
jit_read_slow:
    ld hl, 9
    add hl, sp
    ld bc, (hl)
    ld hl, 3
    add hl, sp
    ld hl, (hl)
    ld de, 0
    ld e, b
    push de
    push hl
    call _jit_read_memory
    pop hl
    pop de
jit_read_result:
    ld hl, 1
    add hl, sp
    ld (hl), a
    pop af
    pop hl
    pop de
    pop bc
    pop iy
    ret

.global _jit_write_bridge
.type _jit_write_bridge, @function
_jit_write_bridge:
    push iy
    push bc
    push de
    push hl
    push af
    ld a, h
    or a, a
    jr nz, jit_write_page
    ld a, l
    cp a, 2
    jr c, jit_write_slow
jit_write_page:
    ld de, 0
    ld e, l
    ld d, h
    ld l, h
    ld h, 4
    mlt hl
    ld bc, _mem_wr_pg
    add hl, bc
    ld hl, (hl)
    ld bc, 0
    or a, a
    sbc hl, bc
    jr z, jit_write_slow
    add hl, de
    pop af
    ld (hl), a
    jr jit_write_return
jit_write_slow:
    ld hl, 9
    add hl, sp
    ld bc, (hl)
    ld hl, 3
    add hl, sp
    ld hl, (hl)
    pop af
    push af
    ld de, 0
    ld e, b
    push de
    ld e, a
    push de
    push hl
    call _jit_write_memory
    pop hl
    pop de
    pop de
    pop af
jit_write_return:
    pop hl
    pop de
    pop bc
    pop iy
    ret

.global _jit_run_fast
.type _jit_run_fast, @function
_jit_run_fast:
    xor a, a
    jr jit_fast_enter

.global _jit_run_batch_fast
.type _jit_run_batch_fast, @function
_jit_run_batch_fast:
    ld a, 1

jit_fast_enter:
    push ix
    push af
    ld hl, 9
    add hl, sp
    ld ix, (hl)
    ld a, (_jit_backend_ok)
    or a, a
    jp z, jit_fast_miss

jit_fast_lookup:
    ld bc, 0x300
    ld a, (_cpu)
    ld c, a
    mlt bc
    ld hl, _jit_entry_points
    add hl, bc
    ld iy, (hl)
    lea hl, iy + 0
    ld de, 0
    or a, a
    sbc hl, de
    jp z, jit_fast_guard_exit
    ld a, (_cpu)
    cp a, (iy + 0)
    jr nz, jit_fast_lookup_slow
    ld a, (_cpu + 1)
    cp a, (iy + 1)
    jr z, jit_fast_found
jit_fast_lookup_slow:
    ld iy, _jit_blocks
    ld b, 16
jit_fast_slot:
    ld a, (iy + 5)
    or a, a
    jp z, jit_fast_next
    ld a, (_cpu)
    cp a, (iy + 0)
    jp nz, jit_fast_next
    ld a, (_cpu + 1)
    cp a, (iy + 1)
    jp nz, jit_fast_next
jit_fast_found:
    push ix
    pop hl
    ld de, (_cpu + 13)
    or a, a
    sbc hl, de
    jp m, jit_fast_guard_exit
    jp z, jit_fast_guard_exit
    ld de, 0
    ld e, (iy + 17)
    or a, a
    sbc hl, de
    jp c, jit_fast_guard_exit
    ld a, (_cpu + 9)
    or a, a
    jr z, jit_fast_mapping
    ld a, (iy + 19)
    or a, a
    jp nz, jit_fast_guard_exit
jit_fast_mapping:
    ld a, (iy + 72)
    cp a, 0x5a
    jp nz, jit_fast_miss
    lea hl, iy + 73
    ld de, 512
    add hl, de
    ld a, (hl)
    cp a, 0xa5
    jp nz, jit_fast_miss
    ld a, (iy + 0)
    add a, (iy + 4)
    jp c, jit_fast_miss
    ld a, (iy + 21)
    or a, a
    jp nz, jit_fast_pointer
    ld a, (_jit_page_generation_wrapped)
    or a, a
    jp nz, jit_fast_pointer
    ld a, (_jit_mapping_generation)
    cp a, (iy + 7)
    jp nz, jit_fast_pointer
    ld hl, 0
    ld l, (iy + 1)
    add hl, hl
    ld de, _jit_page_generation
    add hl, de
    ld a, (hl)
    cp a, (iy + 2)
    jp nz, jit_fast_pointer
    ld a, (iy + 0)
    add a, (iy + 4)
    dec a
    jp nc, jit_fast_execute
    inc hl
    inc hl
    ld a, (hl)
    cp a, (iy + 3)
    jp nz, jit_fast_pointer
    jp jit_fast_execute
jit_fast_pointer:
    ld hl, 0x400
    ld l, (iy + 1)
    mlt hl
    ld de, _mem_rd_pg
    add hl, de
    ld hl, (hl)
    ld de, 0
    or a, a
    sbc hl, de
    jp z, jit_fast_miss
    ld e, (iy + 0)
    ld d, (iy + 1)
jit_fast_compare:
    add hl, de
    lea de, iy + 24
    ld b, (iy + 4)
jit_fast_byte:
    ld a, (de)
    cp a, (hl)
    jp nz, jit_fast_miss
    inc hl
    inc de
    djnz jit_fast_byte
    ld hl, 0
    ld l, (iy + 1)
    add hl, hl
    ld de, _jit_page_generation
    add hl, de
    ld a, (hl)
    ld (iy + 2), a
    ld a, (iy + 0)
    add a, (iy + 4)
    dec a
    jr nc, jit_fast_snapshot_last
    inc hl
    inc hl
jit_fast_snapshot_last:
    ld a, (hl)
    ld (iy + 3), a
    ld a, (_jit_mapping_generation)
    ld (iy + 7), a
jit_fast_execute:
    push iy
    ld hl, 0
    add hl, sp
    ld hl, (hl)
    ld de, 73
    add hl, de
    push hl
    call _jit_execute
    pop hl
    pop iy
    or a, a
    jr z, jit_fast_guard_exit
    ld hl, (_jit_native_hits)
    inc hl
    ld (_jit_native_hits), hl
    pop af
    or a, 2
    push af
    and a, 1
    jp z, jit_fast_miss
    ld a, (iy + 23)
    or a, a
    jp z, jit_fast_lookup
    ld a, (_cpu)
    cp a, (iy + 0)
    jp nz, jit_fast_lookup
    ld a, (_cpu + 1)
    cp a, (iy + 1)
    jp nz, jit_fast_lookup
    push ix
    pop hl
    ld de, (_cpu + 13)
    or a, a
    sbc hl, de
    jp m, jit_fast_miss
    jp z, jit_fast_miss
    ld de, 0
    ld e, (iy + 17)
    or a, a
    sbc hl, de
    jp c, jit_fast_miss
    ld a, (_cpu + 9)
    or a, a
    jp z, jit_fast_execute
    ld a, (iy + 19)
    or a, a
    jp z, jit_fast_execute
    jr jit_fast_miss

jit_fast_next:
    ld de, 586
    add iy, de
    dec b
    jp nz, jit_fast_slot
    jp jit_fast_guard_exit
jit_fast_miss:
    pop af
    and a, 2
    rrca
    pop ix
    ret
jit_fast_guard_exit:
    pop af
    and a, 2
    ld a, 1
    jr nz, jit_fast_return
    inc a
jit_fast_return:
    pop ix
    ret

.section ".note.GNU-stack","",@progbits
