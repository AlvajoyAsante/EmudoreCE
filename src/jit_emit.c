#include <stddef.h>
#include <string.h>
#include "jit_emit.h"

enum
{
    STATE_PC = 0, STATE_SP = 2, STATE_A = 3, STATE_X = 4, STATE_Y = 5,
    STATE_C = 6, STATE_Z = 7, STATE_I = 8, STATE_D = 9,
    STATE_V = 11, STATE_N = 12, STATE_CYCLES = 13
};

typedef struct
{
    uint8_t *output;
    uint16_t capacity;
    uint16_t position;
    uint32_t state;
    uint16_t guest_pc;
    uint8_t guest_size;
    uint8_t accumulator_field;
    uint8_t index_field;
    uint8_t pending_nz;
    uint8_t defer_nz;
    bool overflow;
} Emitter;

uint8_t jit_instruction_size(uint8_t opcode)
{
    static const uint8_t sizes[256] = {
        1,2,0,2,2,2,2,2,1,2,1,2,3,3,3,3,
        2,2,0,2,2,2,2,2,1,3,1,3,3,3,3,3,
        3,2,0,2,2,2,2,2,1,2,1,2,3,3,3,3,
        2,2,0,2,2,2,2,2,1,3,1,3,3,3,3,3,
        1,2,0,2,2,2,2,2,1,2,1,2,3,3,3,3,
        2,2,0,2,2,2,2,2,1,3,1,3,3,3,3,3,
        1,2,0,2,2,2,2,2,1,2,1,2,3,3,3,3,
        2,2,0,2,2,2,2,2,1,3,1,3,3,3,3,3,
        2,2,2,2,2,2,2,2,1,2,1,2,3,3,3,3,
        2,2,0,2,2,2,2,2,1,3,1,3,3,3,3,3,
        2,2,2,2,2,2,2,2,1,2,1,2,3,3,3,3,
        2,2,0,2,2,2,2,2,1,3,1,3,3,3,3,3,
        2,2,2,2,2,2,2,2,1,2,1,2,3,3,3,3,
        2,2,0,2,2,2,2,2,1,3,1,3,3,3,3,3,
        2,2,2,2,2,2,2,2,1,2,1,2,3,3,3,3,
        2,2,0,2,2,2,2,2,1,3,1,3,3,3,3,3
    };
    return sizes[opcode];
}

__attribute__((always_inline)) static inline void emit_byte(Emitter *emitter, uint8_t value)
{
    emitter->accumulator_field = UINT8_MAX;
    if (emitter->position < emitter->capacity)
        emitter->output[emitter->position] = value;
    else
        emitter->overflow = true;
    ++emitter->position;
}

__attribute__((noinline))
static void emit_address(Emitter *emitter, uint32_t address)
{
    emit_byte(emitter, (uint8_t)address);
    emit_byte(emitter, (uint8_t)(address >> 8));
    emit_byte(emitter, (uint8_t)(address >> 16));
}

static void emit_load(Emitter *emitter, uint8_t field)
{
    if (emitter->accumulator_field == field)
        return;
    emit_byte(emitter, 0x3a);
    emit_address(emitter, emitter->state + field);
    emitter->accumulator_field = field;
}

static void emit_store(Emitter *emitter, uint8_t field)
{
    emit_byte(emitter, 0x32);
    emit_address(emitter, emitter->state + field);
    emitter->accumulator_field = field;
}

static void emit_immediate(Emitter *emitter, uint8_t value)
{
    emit_byte(emitter, 0x3e);
    emit_byte(emitter, value);
}

static void emit_constant(Emitter *emitter, uint8_t field, uint8_t value)
{
    emit_immediate(emitter, value);
    emit_store(emitter, field);
}

static uint16_t emit_jump(Emitter *emitter, uint8_t opcode)
{
    uint16_t operand;
    emit_byte(emitter, opcode);
    operand = emitter->position;
    emit_byte(emitter, 0);
    return operand;
}

static void patch_jump(Emitter *emitter, uint16_t operand)
{
    uint16_t distance = emitter->position - operand - 1;
    emitter->accumulator_field = UINT8_MAX;
    if (distance > 127 || operand >= emitter->capacity)
        emitter->overflow = true;
    else
        emitter->output[operand] = (uint8_t)distance;
}

static void emit_zero(Emitter *emitter)
{
    uint16_t skip;
    emit_byte(emitter, 0xb7);
    emit_immediate(emitter, 0);
    skip = emit_jump(emitter, 0x20);
    emit_byte(emitter, 0x3c);
    patch_jump(emitter, skip);
    emit_store(emitter, STATE_Z);
}

static void emit_nz(Emitter *emitter)
{
    if (emitter->defer_nz != UINT8_MAX)
    {
        emitter->pending_nz = emitter->defer_nz;
        emitter->accumulator_field = emitter->defer_nz;
        return;
    }
    emit_byte(emitter, 0x47);
    emit_zero(emitter);
    emit_byte(emitter, 0x78);
    emit_byte(emitter, 0x07);
    emit_byte(emitter, 0xe6);
    emit_byte(emitter, 1);
    emit_store(emitter, STATE_N);
}

static void flush_nz(Emitter *emitter)
{
    if (emitter->pending_nz != UINT8_MAX)
    {
        uint8_t field = emitter->pending_nz;
        emitter->pending_nz = UINT8_MAX;
        emitter->defer_nz = UINT8_MAX;
        emit_load(emitter, field);
        emit_nz(emitter);
    }
}

static void emit_carry(Emitter *emitter, bool invert)
{
    emit_byte(emitter, 0x47);
    if (invert)
        emit_byte(emitter, 0x3f);
    emit_immediate(emitter, 0);
    emit_byte(emitter, 0xce);
    emit_byte(emitter, 0);
    emit_store(emitter, STATE_C);
    emit_byte(emitter, 0x78);
}

static void emit_binary_arithmetic(Emitter *emitter, uint8_t opcode, uint8_t operand, bool from_c)
{
    emit_load(emitter, STATE_C);
    emit_byte(emitter, 0x0f);
    if (opcode == 0xe9)
        emit_byte(emitter, 0x3f);
    emit_load(emitter, STATE_A);
    emit_byte(emitter, from_c ? (opcode == 0x69 ? 0x89 : 0x99) :
                              (opcode == 0x69 ? 0xce : 0xde));
    if (!from_c)
        emit_byte(emitter, operand);
    emit_store(emitter, STATE_A);
    emit_byte(emitter, 0xf5);
    emit_byte(emitter, 0xd1);
    emit_byte(emitter, 0x47);
    emit_byte(emitter, 0x7b);
    emit_byte(emitter, 0xe6);
    emit_byte(emitter, 4);
    emit_byte(emitter, 0x0f);
    emit_byte(emitter, 0x0f);
    emit_store(emitter, STATE_V);
    emit_byte(emitter, 0x7b);
    emit_byte(emitter, 0xe6);
    emit_byte(emitter, 1);
    if (opcode == 0xe9)
    {
        emit_byte(emitter, 0xee);
        emit_byte(emitter, 1);
    }
    emit_store(emitter, STATE_C);
    emit_byte(emitter, 0x78);
    emit_nz(emitter);
}

static void emit_arithmetic(Emitter *emitter, const JitTarget *target,
                            uint8_t opcode, uint8_t operand, bool from_c)
{
    uint32_t helper = opcode == 0x69 ? target->decimal_adc : target->decimal_sbc;
    uint16_t binary = 0, done = 0;
    if (helper)
    {
        emit_load(emitter, STATE_D);
        emit_byte(emitter, 0xb7);
        binary = emit_jump(emitter, 0x28);
        if (!from_c)
        {
            emit_byte(emitter, 0x0e);
            emit_byte(emitter, operand);
        }
        emit_byte(emitter, 0xfd);
        emit_byte(emitter, 0xe5);
        emit_byte(emitter, 0xc5);
        emit_byte(emitter, 0xcd);
        emit_address(emitter, helper);
        emit_byte(emitter, 0xc1);
        emit_byte(emitter, 0xfd);
        emit_byte(emitter, 0xe1);
        done = emit_jump(emitter, 0x18);
        patch_jump(emitter, binary);
    }
    emit_binary_arithmetic(emitter, opcode, operand, from_c);
    if (helper)
        patch_jump(emitter, done);
}

static void emit_finish_cycles(Emitter *emitter, uint8_t cycles)
{
    flush_nz(emitter);
    emit_byte(emitter, 0x2a);
    emit_address(emitter, emitter->state + STATE_CYCLES);
    emit_byte(emitter, 0x11);
    emit_address(emitter, cycles);
    emit_byte(emitter, 0x19);
    emit_byte(emitter, 0x22);
    emit_address(emitter, emitter->state + STATE_CYCLES);
    emit_immediate(emitter, cycles != 0);
    emit_byte(emitter, 0xc9);
}

static void emit_finish(Emitter *emitter, uint16_t pc, uint8_t cycles)
{
    flush_nz(emitter);
    emit_constant(emitter, STATE_PC, (uint8_t)pc);
    emit_constant(emitter, STATE_PC + 1, (uint8_t)(pc >> 8));
    emit_finish_cycles(emitter, cycles);
}

static void emit_compare(Emitter *emitter, uint8_t field, uint8_t value)
{
    emit_load(emitter, field);
    emit_byte(emitter, 0xd6);
    emit_byte(emitter, value);
    emit_carry(emitter, true);
    emit_nz(emitter);
}

static bool emit_simple(Emitter *emitter, const JitTarget *target, uint8_t opcode, uint8_t operand)
{
    uint8_t field;
    switch (opcode)
    {
    case 0x8b:
        emit_load(emitter, STATE_X);
        emit_byte(emitter, 0xe6);
        emit_byte(emitter, operand);
        emit_store(emitter, STATE_A);
        emit_nz(emitter);
        return true;
    case 0xab:
        emit_constant(emitter, STATE_A, operand);
        emit_constant(emitter, STATE_X, operand);
        emit_nz(emitter);
        return true;
    case 0x0b: case 0x2b: case 0x4b:
        emit_load(emitter, STATE_A);
        emit_byte(emitter, 0xe6);
        emit_byte(emitter, operand);
        if (opcode == 0x4b)
        {
            emit_byte(emitter, 0xcb);
            emit_byte(emitter, 0x3f);
            emit_carry(emitter, false);
        }
        emit_store(emitter, STATE_A);
        emit_nz(emitter);
        if (opcode != 0x4b)
        {
            emit_load(emitter, STATE_N);
            emit_store(emitter, STATE_C);
        }
        return true;
    case 0xcb:
        emit_load(emitter, STATE_X);
        emit_byte(emitter, 0x4f);
        emit_load(emitter, STATE_A);
        emit_byte(emitter, 0xa1);
        emit_byte(emitter, 0xd6);
        emit_byte(emitter, operand);
        emit_store(emitter, STATE_X);
        emit_carry(emitter, true);
        emit_nz(emitter);
        return true;
    case 0x6b:
        emit_load(emitter, STATE_A);
        emit_byte(emitter, 0xe6);
        emit_byte(emitter, operand);
        emit_byte(emitter, 0x4f);
        emit_load(emitter, STATE_C);
        emit_byte(emitter, 0x0f);
        emit_byte(emitter, 0x79);
        emit_byte(emitter, 0x1f);
        emit_store(emitter, STATE_A);
        emit_nz(emitter);
        emit_load(emitter, STATE_A);
        emit_byte(emitter, 0x07);
        emit_byte(emitter, 0x07);
        emit_byte(emitter, 0xe6);
        emit_byte(emitter, 1);
        emit_store(emitter, STATE_C);
        emit_byte(emitter, 0x4f);
        emit_load(emitter, STATE_A);
        emit_byte(emitter, 0x07);
        emit_byte(emitter, 0x07);
        emit_byte(emitter, 0x07);
        emit_byte(emitter, 0xe6);
        emit_byte(emitter, 1);
        emit_byte(emitter, 0xa9);
        emit_store(emitter, STATE_V);
        return true;
    case 0xa9: case 0xa2: case 0xa0:
        field = opcode == 0xa9 ? STATE_A : opcode == 0xa2 ? STATE_X : STATE_Y;
        emit_constant(emitter, field, operand);
        emit_nz(emitter);
        return true;
    case 0xaa: case 0xa8: case 0x8a: case 0x98: case 0xba: case 0x9a:
        field = opcode == 0xba ? STATE_SP : opcode == 0x8a || opcode == 0x9a ? STATE_X :
                opcode == 0x98 ? STATE_Y : STATE_A;
        emit_load(emitter, field);
        field = opcode == 0xaa || opcode == 0xba ? STATE_X : opcode == 0xa8 ? STATE_Y :
                opcode == 0x9a ? STATE_SP : STATE_A;
        emit_store(emitter, field);
        if (opcode != 0x9a)
            emit_nz(emitter);
        return true;
    case 0xe8: case 0xc8: case 0xca: case 0x88:
        field = opcode == 0xe8 || opcode == 0xca ? STATE_X : STATE_Y;
        emit_load(emitter, field);
        emit_byte(emitter, opcode == 0xe8 || opcode == 0xc8 ? 0x3c : 0x3d);
        emit_store(emitter, field);
        emit_nz(emitter);
        return true;
    case 0x18: case 0x38: case 0x58: case 0x78: case 0xd8: case 0xf8: case 0xb8:
        field = opcode == 0x18 || opcode == 0x38 ? STATE_C :
                opcode == 0x58 || opcode == 0x78 ? STATE_I :
                opcode == 0xb8 ? STATE_V : STATE_D;
        emit_constant(emitter, field, opcode == 0x38 || opcode == 0x78 || opcode == 0xf8);
        return true;
    case 0x09: case 0x29: case 0x49:
        emit_load(emitter, STATE_A);
        emit_byte(emitter, opcode == 0x09 ? 0xf6 : opcode == 0x29 ? 0xe6 : 0xee);
        emit_byte(emitter, operand);
        emit_store(emitter, STATE_A);
        emit_nz(emitter);
        return true;
    case 0xc9: case 0xe0: case 0xc0:
        emit_compare(emitter, opcode == 0xc9 ? STATE_A : opcode == 0xe0 ? STATE_X : STATE_Y, operand);
        return true;
    case 0x69: case 0xe9: case 0xeb:
        emit_arithmetic(emitter, target, opcode == 0x69 ? 0x69 : 0xe9, operand, false);
        return true;
    case 0x0a: case 0x4a: case 0x2a: case 0x6a:
        if (opcode == 0x2a || opcode == 0x6a)
        {
            emit_load(emitter, STATE_C);
            emit_byte(emitter, 0x0f);
        }
        emit_load(emitter, STATE_A);
        if (opcode == 0x4a)
        {
            emit_byte(emitter, 0xcb);
            emit_byte(emitter, 0x3f);
        }
        else
            emit_byte(emitter, opcode == 0x0a ? 0x87 : opcode == 0x2a ? 0x17 : 0x1f);
        emit_store(emitter, STATE_A);
        emit_carry(emitter, false);
        emit_nz(emitter);
        return true;
    case 0xea: case 0x1a: case 0x3a: case 0x5a: case 0x7a: case 0xda: case 0xfa:
    case 0x80: case 0x82: case 0x89: case 0xc2: case 0xe2:
        return true;
    default:
        return false;
    }
}

static void emit_ram(Emitter *emitter, uint32_t address, bool write)
{
    emit_byte(emitter, write ? 0x32 : 0x3a);
    emit_address(emitter, address);
}

static void emit_word(Emitter *emitter, uint8_t opcode, uint32_t value)
{
    emit_byte(emitter, opcode);
    emit_address(emitter, value);
}

static void emit_address_guard(Emitter *emitter, bool writes, uint16_t pc, uint8_t cycles)
{
    uint16_t low_ram, high_ram, fallback, port_valid;
    emit_byte(emitter, 0x7c);
    emit_byte(emitter, 0xfe);
    emit_byte(emitter, 0xa0);
    low_ram = emit_jump(emitter, 0x38);
    emit_byte(emitter, 0xfe);
    emit_byte(emitter, 0xc0);
    fallback = emit_jump(emitter, 0x38);
    emit_byte(emitter, 0xfe);
    emit_byte(emitter, 0xd0);
    high_ram = emit_jump(emitter, 0x38);
    patch_jump(emitter, fallback);
    emit_finish(emitter, pc, cycles);
    patch_jump(emitter, low_ram);
    patch_jump(emitter, high_ram);
    if (!writes)
        return;
    emit_byte(emitter, 0x7c);
    emit_byte(emitter, 0xb7);
    high_ram = emit_jump(emitter, 0x20);
    emit_byte(emitter, 0x7d);
    emit_byte(emitter, 0xfe);
    emit_byte(emitter, 2);
    port_valid = emit_jump(emitter, 0x30);
    emit_finish(emitter, pc, cycles);
    patch_jump(emitter, port_valid);
    patch_jump(emitter, high_ram);
}

typedef enum { ZERO_PAGE, ABSOLUTE, ZERO_X, ZERO_Y, ABS_X, ABS_Y, IND_X, IND_Y } AddressMode;

static void emit_indexed_address(Emitter *emitter, AddressMode mode, uint16_t address,
                                 const JitTarget *target, bool writes,
                                 uint16_t pc, uint8_t cycles, bool page_cycles, bool mapped, bool known_ram,
                                 bool reuse_index)
{
    uint16_t skip;
    if (known_ram && !page_cycles && (mode == ABS_X || mode == ABS_Y))
    {
        uint32_t backing = address < 0x1000 ? target->low_ram : target->high_ram;
        uint8_t field = mode == ABS_X ? STATE_X : STATE_Y;
        uint8_t accumulator_field = emitter->accumulator_field;
        if (!reuse_index)
            emit_load(emitter, field);
        emit_word(emitter, 0x21, backing + address);
        if (!reuse_index)
        {
            emit_word(emitter, 0x11, 0);
            emit_byte(emitter, 0x5f);
        }
        emit_byte(emitter, 0x19);
        emitter->accumulator_field = reuse_index ? accumulator_field : field;
        return;
    }
    if (mode == IND_X)
    {
        emit_load(emitter, STATE_X);
        emit_byte(emitter, 0xc6);
        emit_byte(emitter, (uint8_t)address);
        emit_word(emitter, 0x01, 0);
        emit_word(emitter, 0x11, 0);
        emit_byte(emitter, 0x4f);
        emit_word(emitter, 0x21, target->low_ram);
        emit_byte(emitter, 0x09);
        emit_byte(emitter, 0x5e);
        emit_byte(emitter, 0x3c);
        emit_byte(emitter, 0x4f);
        emit_word(emitter, 0x21, target->low_ram);
        emit_byte(emitter, 0x09);
        emit_byte(emitter, 0x56);
        emit_byte(emitter, 0xeb);
    }
    else if (mode == ZERO_X || mode == ZERO_Y)
    {
        emit_load(emitter, mode == ZERO_X ? STATE_X : STATE_Y);
        emit_byte(emitter, 0xc6);
        emit_byte(emitter, (uint8_t)address);
        emit_word(emitter, 0x21, 0);
        emit_byte(emitter, 0x6f);
    }
    else
    {
        if (mode == IND_Y)
        {
            emit_word(emitter, 0x21, 0);
            emit_ram(emitter, target->low_ram + (uint8_t)address, false);
            emit_byte(emitter, 0x6f);
            emit_ram(emitter, target->low_ram + (uint8_t)(address + 1), false);
            emit_byte(emitter, 0x67);
        }
        else
            emit_word(emitter, 0x21, address);
        if (page_cycles) emit_byte(emitter, 0x44);
        emit_load(emitter, mode == ABS_X ? STATE_X : STATE_Y);
        emit_word(emitter, 0x11, 0);
        emit_byte(emitter, 0x5f);
        emit_byte(emitter, 0x19);
        if (!known_ram)
        {
            emit_word(emitter, 0x11, 0);
            emit_byte(emitter, 0x5d);
            emit_byte(emitter, 0x54);
            emit_byte(emitter, 0xeb);
        }
        if (page_cycles)
        {
            emit_byte(emitter, 0x7c);
            emit_byte(emitter, 0xb8);
            emit_byte(emitter, 0x0e);
            emit_byte(emitter, 0);
            skip = emit_jump(emitter, 0x28);
            emit_byte(emitter, 0x0c);
            patch_jump(emitter, skip);
        }
    }
    if (!mapped && !known_ram)
        emit_address_guard(emitter, writes, pc, cycles);
    if (page_cycles)
    {
        emit_byte(emitter, 0x79);
        emit_byte(emitter, 0xb7);
        skip = emit_jump(emitter, 0x28);
        emit_byte(emitter, 0xe5);
        emit_word(emitter, 0x2a, emitter->state + STATE_CYCLES);
        emit_byte(emitter, 0x23);
        emit_word(emitter, 0x22, emitter->state + STATE_CYCLES);
        emit_byte(emitter, 0xe1);
        patch_jump(emitter, skip);
    }
    if (mapped)
        return;
    if (known_ram)
    {
        emit_word(emitter, 0x11, address < 0x1000 ? target->low_ram : target->high_ram);
        emit_byte(emitter, 0x19);
        return;
    }
    emit_byte(emitter, 0x7c);
    emit_byte(emitter, 0xfe);
    emit_byte(emitter, 0x10);
    emit_word(emitter, 0x11, target->low_ram);
    skip = emit_jump(emitter, 0x38);
    emit_word(emitter, 0x11, target->high_ram);
    patch_jump(emitter, skip);
    emit_byte(emitter, 0x19);
}

static void emit_access(Emitter *emitter, const JitTarget *target, uint32_t address,
                        bool indirect, bool mapped, bool write, uint8_t bias)
{
    uint8_t accumulator_field = emitter->accumulator_field;
    if (mapped)
    {
        if (!indirect)
            emit_word(emitter, 0x21, address);
        emit_byte(emitter, 0x06);
        emit_byte(emitter, bias);
        emit_word(emitter, 0xcd, write ? target->write_byte : target->read_byte);
    }
    else if (indirect)
        emit_byte(emitter, write ? 0x77 : 0x7e);
    else
        emit_ram(emitter, address, write);
    if (write && !mapped)
        emitter->accumulator_field = accumulator_field;
}

static bool emit_memory(Emitter *emitter, const uint8_t *guest, const JitTarget *target,
                        JitTranslation *translation, uint8_t *cycles, uint8_t *extra_cycles,
                        uint16_t pc, uint8_t prefix_cycles, uint8_t cached_index)
{
    enum { LOAD, STORE, LOGIC, ARITHMETIC, COMPARE, BIT_TEST, SHIFT, INCREMENT, DECREMENT } kind;
    uint8_t opcode = guest[0];
    uint8_t size = jit_instruction_size(opcode);
    uint8_t field = STATE_A;
    uint8_t operation = 0;
    uint8_t combo = 0;
    uint8_t original = opcode;
    bool lax_load = false, sax_store = false, nop_read = false;
    bool las_load = opcode == 0xbb;
    uint16_t address;
    uint32_t host;
    uint8_t index_field;
    bool writes, indirect, page_cycles, mapped, known_ram, indexed_ram;
    AddressMode mode = size == 2 ? ZERO_PAGE : ABSOLUTE;
    if (opcode == 0x93 || opcode == 0x9b || opcode == 0x9c || opcode == 0x9e || opcode == 0x9f)
    {
        if (!target->write_byte || target->low_ram > 0xfff000UL)
            return false;
        flush_nz(emitter);
        address = guest[1];
        if (size == 3)
            address |= (uint16_t)guest[2] << 8;
        mode = opcode == 0x93 ? IND_Y : opcode == 0x9c ? ABS_X : ABS_Y;
        emit_indexed_address(emitter, mode, address, target, true, pc, prefix_cycles,
                             false, true, false, false);
        emit_load(emitter, opcode == 0x9c ? STATE_Y : STATE_X);
        if (opcode == 0x93 || opcode == 0x9b || opcode == 0x9f)
        {
            emit_byte(emitter, 0x4f);
            emit_load(emitter, STATE_A);
            emit_byte(emitter, 0xa1);
            if (opcode == 0x9b)
                emit_store(emitter, STATE_SP);
        }
        emit_byte(emitter, 0x4f);
        emit_byte(emitter, 0x7c);
        emit_byte(emitter, 0x3c);
        emit_byte(emitter, 0xa1);
        emit_access(emitter, target, 0, true, true, true, prefix_cycles);
        translation->writes_memory = true;
        translation->reads_memory |= opcode == 0x93;
        translation->can_repeat = false;
        *cycles = opcode == 0x93 ? 6 : 5;
        return true;
    }
    if (las_load)
    {
        opcode = 0xb9;
        lax_load = true;
    }
    if ((opcode & 3) == 3 && (opcode & 0x1f) != 0x0b)
    {
        uint8_t family = opcode >> 5;
        lax_load = family == 5;
        sax_store = family == 4;
        if (!lax_load && !sax_store)
            combo = family + 1;
        opcode -= 2;
    }
    switch (opcode)
    {
    case 0x04: case 0x44: case 0x64: opcode = 0xa5; nop_read = true; break;
    case 0x14: case 0x34: case 0x54: case 0x74: case 0xd4: case 0xf4:
        opcode = 0xb5; nop_read = true; break;
    case 0x0c: opcode = 0xad; nop_read = true; break;
    case 0x1c: case 0x3c: case 0x5c: case 0x7c: case 0xdc: case 0xfc:
        opcode = 0xbd; nop_read = true; break;
    default: break;
    }
    if ((opcode & 3) == 1 && (opcode & 0x1c) != 0x08)
    {
        static const AddressMode modes[] = {IND_X,ZERO_PAGE,ZERO_PAGE,ABSOLUTE,IND_Y,ZERO_X,ABS_Y,ABS_X};
        mode = modes[(opcode >> 2) & 7];
        operation = opcode >> 5;
        kind = operation < 3 ? LOGIC : operation == 3 || operation == 7 ? ARITHMETIC :
               operation == 4 ? STORE : operation == 5 ? LOAD : COMPARE;
    }
    else
    {
        switch (opcode)
        {
        case 0xb6: case 0xbe: case 0xa6: case 0xae:
            kind = LOAD; field = STATE_X;
            if (opcode & 0x10) mode = size == 2 ? ZERO_Y : ABS_Y;
            break;
        case 0xb4: case 0xbc: case 0xa4: case 0xac:
            kind = LOAD; field = STATE_Y;
            if (opcode & 0x10) mode = size == 2 ? ZERO_X : ABS_X;
            break;
        case 0x96: case 0x86: case 0x8e:
            kind = STORE; field = STATE_X;
            if (opcode == 0x96) mode = ZERO_Y;
            break;
        case 0x94: case 0x84: case 0x8c:
            kind = STORE; field = STATE_Y;
            if (opcode == 0x94) mode = ZERO_X;
            break;
        case 0xe4: case 0xec: kind = COMPARE; field = STATE_X; break;
        case 0xc4: case 0xcc: kind = COMPARE; field = STATE_Y; break;
        case 0x24: case 0x2c: kind = BIT_TEST; break;
        case 0x06: case 0x0e: case 0x26: case 0x2e:
        case 0x46: case 0x4e: case 0x66: case 0x6e:
        case 0x16: case 0x1e: case 0x36: case 0x3e:
        case 0x56: case 0x5e: case 0x76: case 0x7e:
            kind = SHIFT; operation = opcode >> 5;
            if (opcode & 0x10) mode = size == 2 ? ZERO_X : ABS_X;
            break;
        case 0xf6: case 0xfe: case 0xe6: case 0xee:
            kind = INCREMENT;
            if (opcode & 0x10) mode = size == 2 ? ZERO_X : ABS_X;
            break;
        case 0xd6: case 0xde: case 0xc6: case 0xce:
            kind = DECREMENT;
            if (opcode & 0x10) mode = size == 2 ? ZERO_X : ABS_X;
            break;
        default: return false;
        }
    }
    if (combo)
    {
        operation = combo - 1;
        kind = combo <= 4 ? SHIFT : combo == 7 ? DECREMENT : INCREMENT;
    }
    if (original == 0x97 || original == 0xb7) mode = ZERO_Y;
    if (original == 0xbf) mode = ABS_Y;
    address = guest[1];
    if (size == 3)
        address = (uint16_t)(address | ((uint16_t)guest[2] << 8));
    writes = kind == STORE || kind == SHIFT || kind == INCREMENT || kind == DECREMENT;
    indirect = mode != ZERO_PAGE && mode != ABSOLUTE;
    known_ram = ((mode == ZERO_X || mode == ZERO_Y) && !writes) ||
                ((mode == ABS_X || mode == ABS_Y) && address >= 2 &&
                 (address <= 0x0f00 || (address >= 0x1000 && address <= 0x9f00) ||
                  (address >= 0xc000 && address <= 0xcf00)));
    page_cycles = !writes && (mode == ABS_X || mode == ABS_Y || mode == IND_Y);
    if ((mode == ABS_X || mode == ABS_Y) && !(address & 0xff)) page_cycles = false;
    indexed_ram = known_ram && !page_cycles && (mode == ABS_X || mode == ABS_Y);
    index_field = mode == ABS_X ? STATE_X : STATE_Y;
    mapped = !known_ram && target->read_byte && target->write_byte && (indirect ||
             (address >= 0xa000 && (address < 0xc000 || address >= 0xd000)) || (writes && address < 2));
    if (writes && (mapped || (indirect && (!known_ram || (uint16_t)(emitter->guest_pc - address) <= 255)) ||
                   (uint16_t)(address - emitter->guest_pc) < emitter->guest_size))
        translation->can_repeat = false;
    if (kind != STORE || mapped || (indirect && !known_ram))
        flush_nz(emitter);
    if (!mapped && !indirect && ((address >= 0xa000 && (address < 0xc000 || address >= 0xd000)) ||
                     (writes && address < 2)))
        return false;
    host = mapped ? address : (address < 0x1000 ? target->low_ram : target->high_ram) + address;
    if (target->low_ram > 0xfff000UL || target->high_ram > 0xff0000UL)
        return false;
    translation->reads_memory |= kind != STORE;
    translation->writes_memory |= writes;
    *cycles = size == 2 ? 3 : 4;
    if (mode == ZERO_X || mode == ZERO_Y) *cycles = 4;
    if (mode == IND_X) *cycles = 6;
    if (mode == IND_Y) *cycles = writes ? 6 : 5;
    if (writes && (mode == ABS_X || mode == ABS_Y)) *cycles = 5;
    if (kind == SHIFT || kind == INCREMENT || kind == DECREMENT)
        *cycles += 2;
    if (indirect)
    {
        translation->guarded |= !mapped && !known_ram;
        emit_indexed_address(emitter, mode, address, target, writes, pc, prefix_cycles,
                     page_cycles, mapped, known_ram, cached_index == index_field);
        if (page_cycles) ++*extra_cycles;
    }
    if (kind == STORE)
    {
        if (sax_store)
        {
            emit_load(emitter, STATE_X);
            emit_byte(emitter, 0x4f);
        }
        emit_load(emitter, field);
        if (sax_store) emit_byte(emitter, 0xa1);
        emit_access(emitter, target, host, indirect, mapped, true, prefix_cycles);
        if (indexed_ram)
            emitter->index_field = index_field;
        return true;
    }
    if (kind == SHIFT && (operation == 1 || operation == 3))
    {
        emit_load(emitter, STATE_C);
        emit_byte(emitter, 0x0f);
    }
    emit_access(emitter, target, host, indirect, mapped, false, prefix_cycles);
    if (kind == LOAD)
    {
        if (nop_read)
            return true;
        if (las_load)
        {
            emit_byte(emitter, 0x4f);
            emit_load(emitter, STATE_SP);
            emit_byte(emitter, 0xa1);
            emit_store(emitter, STATE_SP);
        }
        emit_store(emitter, field);
        if (lax_load) emit_store(emitter, STATE_X);
        emitter->defer_nz = field;
        emit_nz(emitter);
        if (indexed_ram && field != index_field && !(lax_load && index_field == STATE_X))
            emitter->index_field = index_field;
        return true;
    }
    if (kind == SHIFT || kind == INCREMENT || kind == DECREMENT)
    {
        emit_access(emitter, target, host, indirect, mapped, true, prefix_cycles);
        if (kind == SHIFT && operation == 2)
        {
            emit_byte(emitter, 0xcb);
            emit_byte(emitter, 0x3f);
        }
        else
            emit_byte(emitter, kind == INCREMENT ? 0x3c : kind == DECREMENT ? 0x3d :
                                  operation == 0 ? 0x87 : operation == 1 ? 0x17 : 0x1f);
        emit_access(emitter, target, host, indirect, mapped, true, prefix_cycles);
        if (kind == SHIFT)
            emit_carry(emitter, false);
        if (combo)
        {
            emit_byte(emitter, 0x4f);
            if (combo == 4 || combo == 8)
            {
                translation->binary_only |= !(target->decimal_adc && target->decimal_sbc);
                emit_arithmetic(emitter, target, combo == 4 ? 0x69 : 0xe9, 0, true);
                return true;
            }
            emit_load(emitter, STATE_A);
            emit_byte(emitter, combo == 1 ? 0xb1 : combo == 2 ? 0xa1 : combo == 3 ? 0xa9 : 0x91);
            if (combo == 7)
                emit_carry(emitter, true);
            else
                emit_store(emitter, STATE_A);
        }
        emit_nz(emitter);
        return true;
    }
    emit_byte(emitter, 0x4f);
    if (kind == BIT_TEST)
    {
        emit_byte(emitter, 0x07);
        emit_byte(emitter, 0xe6);
        emit_byte(emitter, 1);
        emit_store(emitter, STATE_N);
        emit_byte(emitter, 0x79);
        emit_byte(emitter, 0x07);
        emit_byte(emitter, 0x07);
        emit_byte(emitter, 0xe6);
        emit_byte(emitter, 1);
        emit_store(emitter, STATE_V);
        emit_load(emitter, STATE_A);
        emit_byte(emitter, 0xa1);
        emit_zero(emitter);
        return true;
    }
    if (kind == ARITHMETIC)
    {
        translation->binary_only |= !(target->decimal_adc && target->decimal_sbc);
        emit_arithmetic(emitter, target, operation == 3 ? 0x69 : 0xe9, 0, true);
        return true;
    }
    emit_load(emitter, field);
    if (kind == COMPARE)
    {
        emit_byte(emitter, 0x91);
        emit_carry(emitter, true);
    }
    else
    {
        emit_byte(emitter, operation == 0 ? 0xb1 : operation == 1 ? 0xa1 : 0xa9);
        emit_store(emitter, STATE_A);
        emitter->defer_nz = STATE_A;
    }
    emit_nz(emitter);
    return true;
}

static void emit_stack(Emitter *emitter, const JitTarget *target, bool push)
{
    if (push)
        emit_byte(emitter, 0x47);
    emit_load(emitter, STATE_SP);
    if (!push)
    {
        emit_byte(emitter, 0x3c);
        emit_store(emitter, STATE_SP);
    }
    emit_word(emitter, 0x21, target->low_ram + 0x100);
    emit_word(emitter, 0x11, 0);
    emit_byte(emitter, 0x5f);
    emit_byte(emitter, 0x19);
    if (push)
    {
        emit_byte(emitter, 0x78);
        emit_byte(emitter, 0x77);
        emit_load(emitter, STATE_SP);
        emit_byte(emitter, 0x3d);
        emit_store(emitter, STATE_SP);
    }
    else
        emit_byte(emitter, 0x7e);
}

static void emit_status(Emitter *emitter, bool pack)
{
    static const uint8_t fields[] = {STATE_C,STATE_Z,STATE_I,STATE_D,STATE_V,STATE_N};
    static const uint8_t shifts[] = {0,1,2,3,6,7};
    uint8_t index, shift;
    if (pack)
    {
        emit_byte(emitter, 0x0e);
        emit_byte(emitter, 0x30);
    }
    else
        emit_byte(emitter, 0x4f);
    for (index = 0; index < sizeof(fields); ++index)
    {
        if (pack)
            emit_load(emitter, fields[index]);
        else
            emit_byte(emitter, 0x79);
        for (shift = 0; shift < shifts[index]; ++shift)
            emit_byte(emitter, pack ? 0x07 : 0x0f);
        if (pack)
        {
            emit_byte(emitter, 0xb1);
            emit_byte(emitter, 0x4f);
        }
        else
        {
            emit_byte(emitter, 0xe6);
            emit_byte(emitter, 1);
            emit_store(emitter, fields[index]);
        }
    }
}

static bool emit_control(Emitter *emitter, const uint8_t *guest, const JitTarget *target,
                         JitTranslation *translation, uint8_t *cycles,
                         uint16_t pc, uint8_t prefix_cycles, bool *finished)
{
    uint8_t opcode = guest[0];
    if (target->low_ram > 0xfff000UL || target->high_ram > 0xff0000UL)
        return false;
    switch (opcode)
    {
    case 0x00:
        if (prefix_cycles || !target->brk)
            return false;
        emit_constant(emitter, STATE_PC, (uint8_t)(pc + 1));
        emit_constant(emitter, STATE_PC + 1, (uint8_t)((uint16_t)(pc + 1) >> 8));
        emit_byte(emitter, 0xfd);
        emit_byte(emitter, 0xe5);
        emit_word(emitter, 0xcd, target->brk);
        emit_byte(emitter, 0xfd);
        emit_byte(emitter, 0xe1);
        emit_immediate(emitter, 1);
        emit_byte(emitter, 0xc9);
        translation->reads_memory = translation->writes_memory = true;
        translation->can_repeat = false;
        *cycles = 7;
        *finished = true;
        return true;
    case 0x08: case 0x48:
        if (opcode == 0x08)
            emit_status(emitter, true);
        else
            emit_load(emitter, STATE_A);
        emit_stack(emitter, target, true);
        translation->writes_memory = true;
        translation->can_repeat = false;
        *cycles = 3;
        return true;
    case 0x28: case 0x68:
        emit_stack(emitter, target, false);
        if (opcode == 0x28)
            emit_status(emitter, false);
        else
        {
            emit_store(emitter, STATE_A);
            emit_nz(emitter);
        }
        translation->reads_memory = true;
        *cycles = 4;
        return true;
    case 0x20:
        emit_immediate(emitter, (uint8_t)((uint16_t)(pc + 2) >> 8));
        emit_stack(emitter, target, true);
        emit_immediate(emitter, (uint8_t)(pc + 2));
        emit_stack(emitter, target, true);
        translation->writes_memory = true;
        translation->can_repeat = false;
        translation->taken = (uint16_t)(guest[1] | ((uint16_t)guest[2] << 8));
        *cycles = 6;
        emit_finish(emitter, translation->taken, (uint8_t)(prefix_cycles + 6));
        *finished = true;
        return true;
    case 0x40: case 0x60:
        if (opcode == 0x40)
        {
            emit_stack(emitter, target, false);
            emit_status(emitter, false);
        }
        emit_stack(emitter, target, false);
        emit_byte(emitter, 0x4f);
        emit_stack(emitter, target, false);
        emit_byte(emitter, 0x47);
        emit_word(emitter, 0x21, 0);
        emit_byte(emitter, 0x69);
        emit_byte(emitter, 0x60);
        if (opcode == 0x60)
            emit_byte(emitter, 0x23);
        emit_byte(emitter, 0x7d);
        emit_store(emitter, STATE_PC);
        emit_byte(emitter, 0x7c);
        emit_store(emitter, STATE_PC + 1);
        translation->reads_memory = true;
        *cycles = 6;
        emit_finish_cycles(emitter, (uint8_t)(prefix_cycles + 6));
        *finished = true;
        return true;
    case 0x6c:
    {
        uint16_t pointer = (uint16_t)(guest[1] | ((uint16_t)guest[2] << 8));
        uint16_t next = (uint16_t)((pointer & 0xff00) | (uint8_t)(pointer + 1));
        bool mapped = pointer >= 0xa000 && (pointer < 0xc000 || pointer >= 0xd000);
        if (mapped && !target->read_byte)
            return false;
        emit_access(emitter, target, mapped ? pointer :
                    (pointer < 0x1000 ? target->low_ram : target->high_ram) + pointer,
                    false, mapped, false, prefix_cycles);
        emit_byte(emitter, 0x4f);
        emit_access(emitter, target, mapped ? next :
                    (next < 0x1000 ? target->low_ram : target->high_ram) + next,
                    false, mapped, false, prefix_cycles);
        emit_store(emitter, STATE_PC + 1);
        emit_byte(emitter, 0x79);
        emit_store(emitter, STATE_PC);
        translation->reads_memory = true;
        *cycles = 5;
        emit_finish_cycles(emitter, (uint8_t)(prefix_cycles + 5));
        *finished = true;
        return true;
    }
    default:
        return false;
    }
}

static uint8_t simple_nz_field(uint8_t opcode)
{
    switch (opcode)
    {
    case 0xa9: case 0xab: case 0x8a: case 0x98:
    case 0x09: case 0x29: case 0x49:
    case 0x0a: case 0x2a: case 0x4a: case 0x6a:
        return STATE_A;
    case 0xa2: case 0xaa: case 0xba: case 0xe8: case 0xca: case 0xcb:
        return STATE_X;
    case 0xa0: case 0xa8: case 0xc8: case 0x88:
        return STATE_Y;
    default:
        return UINT8_MAX;
    }
}

static bool stores_register(uint8_t opcode)
{
    switch (opcode)
    {
    case 0x81: case 0x85: case 0x8d: case 0x91: case 0x95: case 0x99: case 0x9d:
    case 0x84: case 0x8c: case 0x94: case 0x86: case 0x8e: case 0x96:
    case 0x83: case 0x87: case 0x8f: case 0x97:
        return true;
    default:
        return false;
    }
}

bool jit_compile(uint8_t *output, uint16_t capacity, const uint8_t *guest,
                 uint8_t available, uint16_t pc, const JitTarget *target,
                 JitTranslation *translation)
{
    Emitter emitter;
    uint8_t offset = 0;
    uint8_t cycles = 0;
    uint8_t extra_cycles = 0;
    uint8_t instructions = 0;
    bool finished = false;

    if (!output || !guest || !target || !translation || target->state > 0xffffefUL)
        return false;
    memset(translation, 0, sizeof(*translation));
    translation->can_repeat = true;
    emitter.output = output;
    emitter.capacity = capacity;
    emitter.position = 0;
    emitter.state = target->state;
    emitter.accumulator_field = UINT8_MAX;
    emitter.index_field = UINT8_MAX;
    emitter.pending_nz = UINT8_MAX;
    emitter.defer_nz = UINT8_MAX;
    emitter.overflow = false;
    if (available > JIT_MAX_GUEST_BYTES)
        available = JIT_MAX_GUEST_BYTES;
    emitter.guest_pc = pc;
    emitter.guest_size = available;

    while (offset < available && instructions < JIT_MAX_INSTRUCTIONS)
    {
        Emitter saved_emitter = emitter;
        JitTranslation saved_translation = *translation;
        uint8_t saved_offset = offset;
        uint8_t saved_cycles = cycles;
        uint8_t saved_extra = extra_cycles;
        uint8_t saved_instructions = instructions;
        bool stop = false;
        uint8_t opcode = guest[offset];
        uint8_t size = jit_instruction_size(opcode);
        uint8_t opcode_cycles = 2;
        uint8_t cached_index = emitter.index_field;
        uint16_t next = (uint16_t)(pc + offset + size);
        if (!size || offset + size > available)
            break;
        emitter.index_field = UINT8_MAX;
        emitter.defer_nz = simple_nz_field(opcode);
        if (emitter.defer_nz != UINT8_MAX)
            emitter.pending_nz = UINT8_MAX;
        else if (!stores_register(opcode))
            flush_nz(&emitter);
        if ((opcode & 0x1f) == 0x10)
        {
            static const uint8_t fields[] = {STATE_N, STATE_V, STATE_C, STATE_Z};
            uint16_t skip;
            uint16_t taken = (uint16_t)(next + (int8_t)guest[offset + 1]);
            cycles += 2;
            emit_load(&emitter, fields[opcode >> 6]);
            emit_byte(&emitter, 0xb7);
            skip = emit_jump(&emitter, opcode & 0x20 ? 0x20 : 0x28);
            emit_finish(&emitter, next, cycles);
            patch_jump(&emitter, skip);
            emit_finish(&emitter, taken, (uint8_t)(cycles + 1 + ((next >> 8) != (taken >> 8))));
            translation->conditional = true;
            translation->taken = taken;
            translation->min_cycles = cycles;
            translation->max_cycles = (uint8_t)(cycles + extra_cycles + 1 + ((next >> 8) != (taken >> 8)));
            offset += size;
            ++instructions;
            finished = true;
            goto instruction_done;
        }
        if (opcode == 0x4c)
        {
            uint16_t destination = (uint16_t)(guest[offset + 1] | ((uint16_t)guest[offset + 2] << 8));
            if (destination == (uint16_t)(pc + offset))
                break;
            cycles += 3;
            offset += size;
            ++instructions;
            translation->taken = destination;
            emit_finish(&emitter, destination, cycles);
            translation->min_cycles = cycles;
            translation->max_cycles = (uint8_t)(cycles + extra_cycles);
            finished = true;
            goto instruction_done;
        }
        if (!emit_control(&emitter, guest + offset, target, translation, &opcode_cycles,
                          (uint16_t)(pc + offset), cycles, &finished) &&
            !emit_simple(&emitter, target, opcode, size > 1 ? guest[offset + 1] : 0) &&
            !emit_memory(&emitter, guest + offset, target, translation, &opcode_cycles,
                         &extra_cycles, (uint16_t)(pc + offset), cycles, cached_index))
            break;
        if (opcode == 0x69 || opcode == 0xe9 || opcode == 0xeb)
            translation->binary_only |= !(target->decimal_adc && target->decimal_sbc);
        if (opcode == 0x6b)
            translation->binary_only = true;
        cycles += opcode_cycles;
        offset += size;
        ++instructions;
        if (finished)
        {
            translation->min_cycles = cycles;
            translation->max_cycles = (uint8_t)(cycles + extra_cycles);
        }
        if (opcode == 0xf8 || opcode == 0x28 || !translation->can_repeat)
            stop = true;
instruction_done:
        if (emitter.overflow || (!finished &&
            (capacity < (emitter.pending_nz == UINT8_MAX ? 28 : 54) ||
             emitter.position > capacity - (emitter.pending_nz == UINT8_MAX ? 28 : 54))))
        {
            emitter = saved_emitter;
            *translation = saved_translation;
            offset = saved_offset;
            cycles = saved_cycles;
            extra_cycles = saved_extra;
            instructions = saved_instructions;
            finished = false;
            break;
        }
        if (finished || stop)
            break;
    }
    if (!instructions)
        return false;
    if (!finished)
    {
        emit_finish(&emitter, (uint16_t)(pc + offset), cycles);
        translation->min_cycles = cycles;
        translation->max_cycles = (uint8_t)(cycles + extra_cycles);
    }
    if (translation->guarded)
        translation->min_cycles = 0;
    if (emitter.overflow)
        return false;
    translation->native_size = emitter.position;
    translation->guest_size = offset;
    translation->instructions = instructions;
    translation->fallthrough_pc = (uint16_t)(pc + offset);
    return true;
}