#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "asic.h"
#include "cpu.h"
#include "emu.h"
#include "mem.h"
#include "vrEmu6502.h"
#include "cemu_throttle.h"
#include "../../src/jit_emit.h"

#define STATE_ADDRESS 0xd02000UL
#define CODE_ADDRESS 0xd08000UL
#define STOP_ADDRESS 0xd09000UL
#define STACK_ADDRESS 0xd10000UL
#define LOW_ADDRESS 0xd20000UL
#define HIGH_ADDRESS 0xd30000UL

static uint8_t guest_ram[65536];
static VrEmu6502 *reference;
static unsigned long cases;
static unsigned long assertions;
static const JitTarget target = {.state = STATE_ADDRESS, .low_ram = LOW_ADDRESS, .high_ram = HIGH_ADDRESS};

void gui_console_clear(void) {}
void gui_console_printf(const char *format, ...) { (void)format; }
void gui_console_err_printf(const char *format, ...) { (void)format; }
asic_rev_t gui_handle_reset(const boot_ver_t *version, asic_rev_t loaded,
                           asic_rev_t suggested, emu_device_t device, bool *python)
{
    (void)version;
    (void)loaded;
    (void)suggested;
    (void)device;
    (void)python;
    return ASIC_REV_PRE_A;
}

static uint8_t reference_read(uint16_t address, bool debug)
{
    (void)debug;
    return guest_ram[address];
}

static void reference_write(uint16_t address, uint8_t value)
{
    guest_ram[address] = value;
}

static void reference_state(uint8_t state[16], uint32_t cycles)
{
    uint16_t pc = vrEmu6502GetPC(reference);
    uint8_t flags = vrEmu6502GetStatus(reference);
    state[0] = (uint8_t)pc;
    state[1] = (uint8_t)(pc >> 8);
    state[2] = vrEmu6502GetStackPointer(reference);
    state[3] = vrEmu6502GetAcc(reference);
    state[4] = vrEmu6502GetX(reference);
    state[5] = vrEmu6502GetY(reference);
    state[6] = (flags & FlagC) != 0;
    state[7] = (flags & FlagZ) != 0;
    state[8] = (flags & FlagI) != 0;
    state[9] = (flags & FlagD) != 0;
    state[10] = 1;
    state[11] = (flags & FlagV) != 0;
    state[12] = (flags & FlagN) != 0;
    state[13] = (uint8_t)cycles;
    state[14] = (uint8_t)(cycles >> 8);
    state[15] = (uint8_t)(cycles >> 16);
}

static void seed_reference(uint16_t pc, uint8_t accumulator, uint8_t index_x,
                           uint8_t index_y, uint8_t flags)
{
    uint8_t setup[] = {0xa2, 0xff, 0x9a, 0xa9, flags, 0x48,
                       0xa9, accumulator, 0xa2, index_x, 0xa0, index_y, 0x28};
    unsigned int instruction;
    memcpy(guest_ram + 0x200, setup, sizeof(setup));
    vrEmu6502SetPC(reference, 0x200);
    for (instruction = 0; instruction < 8; ++instruction)
        vrEmu6502InstCycle(reference);
    vrEmu6502SetPC(reference, pc);
}

static bool run_limited_case(const uint8_t *program, uint8_t length, uint16_t pc,
                     uint8_t accumulator, uint8_t index_x, uint8_t index_y,
                     uint8_t flags, uint32_t cycles, uint8_t instruction_limit)
{
    uint8_t output[JIT_NATIVE_CAPACITY + 16];
    uint8_t initial[16], expected[16], actual[16];
    JitTranslation translation;
    unsigned int index;
    uint32_t reference_cycles = cycles;
    memset(output, 0xcc, sizeof(output));
    if (!jit_compile(output + 8, JIT_NATIVE_CAPACITY, program, length, pc, &target, &translation))
    {
        fprintf(stderr, "compile rejected opcode %02x pc=%04x\n", program[0], pc);
        return false;
    }
    if (translation.binary_only && (flags & FlagD))
        flags &= (uint8_t)~FlagD;
    for (index = 0; index < length; ++index)
        guest_ram[(uint16_t)(pc + index)] = program[index];
    seed_reference(pc, accumulator, index_x, index_y, flags);
    reference_state(initial, cycles);
    if (translation.reads_memory || translation.writes_memory)
    {
        memcpy(mem.ram.block + LOW_ADDRESS - 0xd00000, guest_ram, 0x1000);
        memcpy(mem.ram.block + HIGH_ADDRESS - 0xd00000 + 0x1000, guest_ram + 0x1000, 0xf000);
    }
    if (instruction_limit == UINT8_MAX)
        instruction_limit = translation.instructions;
    for (index = 0; index < instruction_limit; ++index)
        reference_cycles += vrEmu6502InstCycle(reference);
    reference_state(expected, reference_cycles);
    memcpy(mem.ram.block + STATE_ADDRESS - 0xd00000, initial, sizeof(initial));
    memcpy(mem.ram.block + CODE_ADDRESS - 0xd00000, output + 8, translation.native_size);
    mem_poke_byte(STATE_ADDRESS - 1, 0x5a);
    mem_poke_byte(STATE_ADDRESS + 16, 0xa5);
    mem_poke_byte(STOP_ADDRESS, 0x76);
    mem_poke_long(STACK_ADDRESS, STOP_ADDRESS);
    cpu.registers.SPL = STACK_ADDRESS;
    cpu.registers.IX = 0x123456;
    cpu.registers.IY = 0xabcdef;
    cpu.halted = false;
    cpu.IEF1 = cpu.IEF2 = cpu.IEF_wait = cpu.NMI = false;
    cpu.ADL = true;
    cpu_flush(CODE_ADDRESS, true);
    cpu_clear_signals();
    cemu_throttle_run(1);
    memcpy(actual, mem.ram.block + STATE_ADDRESS - 0xd00000, sizeof(actual));
    ++cases;
    assertions += 24;
    if (memcmp(actual, expected, sizeof(actual)) || !cpu.halted ||
        cpu.registers.SPL != STACK_ADDRESS + 3 || cpu.registers.IX != 0x123456 ||
        cpu.registers.IY != 0xabcdef || cpu_check_signals() != CPU_SIGNAL_NONE ||
        cpu.registers.A != (instruction_limit != 0) ||
        mem_peek_byte(STATE_ADDRESS - 1) != 0x5a || mem_peek_byte(STATE_ADDRESS + 16) != 0xa5)
    {
        fprintf(stderr, "native mismatch case=%lu opcode=%02x pc=%04x A=%02x X=%02x Y=%02x P=%02x cycles=%06x\n",
                cases, program[0], pc, accumulator, index_x, index_y, flags, cycles);
        fprintf(stderr, "expected:");
        for (index = 0; index < 16; ++index) fprintf(stderr, " %02x", expected[index]);
        fprintf(stderr, "\nactual:  ");
        for (index = 0; index < 16; ++index) fprintf(stderr, " %02x", actual[index]);
        fprintf(stderr, "\neZ80 pc=%06x sp=%06x halt=%u signals=%u\n",
                cpu.registers.PC, cpu.registers.SPL, cpu.halted, cpu_check_signals());
        return false;
    }
    for (index = 0; index < 8; ++index)
    {
        if (output[index] != 0xcc || output[JIT_NATIVE_CAPACITY + 8 + index] != 0xcc)
            return false;
    }
    if (translation.writes_memory)
    {
        assertions += 65536;
        if (memcmp(mem.ram.block + LOW_ADDRESS - 0xd00000, guest_ram, 0x1000) ||
            memcmp(mem.ram.block + HIGH_ADDRESS - 0xd00000 + 0x1000, guest_ram + 0x1000, 0xf000))
        {
            fprintf(stderr, "native memory mismatch opcode=%02x\n", program[0]);
            return false;
        }
    }
    return true;
}

static bool run_case(const uint8_t *program, uint8_t length, uint16_t pc,
                     uint8_t accumulator, uint8_t index_x, uint8_t index_y,
                     uint8_t flags, uint32_t cycles)
{
    return run_limited_case(program, length, pc, accumulator, index_x, index_y,
                            flags, cycles, UINT8_MAX);
}

static bool test_opcode_families(void)
{
    static const uint8_t opcodes[] = {
        0x09,0x18,0x29,0x38,0x49,0x58,0x69,0x78,0x88,0x8a,0x98,
        0xa0,0xa2,0xa8,0xa9,0xaa,0xb8,0xc0,0xc8,0xc9,0xca,0xd8,
        0xe0,0xe8,0xe9,0xea,0xf8,0x0a,0x2a,0x4a,0x6a,0xba,0x9a,
        0x10,0x30,0x50,0x70,0x90,0xb0,0xd0,0xf0,0x4c
    };
    size_t opcode_index;
    unsigned int value;
    for (opcode_index = 0; opcode_index < sizeof(opcodes); ++opcode_index)
        for (value = 0; value < 256; ++value)
        {
            uint8_t program[] = {opcodes[opcode_index], (uint8_t)(value * 73 + 19), 0x06};
            uint8_t length = jit_instruction_size(program[0]);
            if (!run_case(program, length, value & 1 ? 0x04fd : 0x0500,
                          (uint8_t)value, (uint8_t)(255 - value), (uint8_t)(value ^ 0x80),
                          (uint8_t)value, value & 2 ? 0xfffffeUL : 13))
                return false;
        }
    printf("PASS 42 native opcode encodings, all byte values and flag combinations\n");
    return true;
}

static bool test_traces(void)
{
    static const uint8_t programs[][16] = {
        {0xe8,0xca,0xd0,0xfd},
        {0xa2,0x00,0xe8,0xca,0xf0,0xfa},
        {0xa9,0xf3,0x29,0x0f,0xaa,0xca,0x8a,0x49,0x80,0xd0,0xf5},
        {0x38,0xa9,0x80,0x6a,0x2a,0xc9,0x80,0xf0,0x00},
        {0xa0,0xa2,0xc0,0xa9,0xe0,0xc0,0xa9,0x10,0xf0,0xf6},
        {0x18,0x69,0x7f,0xaa,0xe9,0x80,0x70,0xf8}
    };
    static const uint8_t lengths[] = {4,6,11,9,10,8};
    size_t trace;
    unsigned int value;
    for (trace = 0; trace < sizeof(lengths); ++trace)
        for (value = 0; value < 256; ++value)
            if (!run_case(programs[trace], lengths[trace], 0x04fa,
                          (uint8_t)value, (uint8_t)value, (uint8_t)value,
                          (uint8_t)(value & ~FlagD), 0xfffff0UL))
                return false;
    puts("PASS multi-instruction traces, internal branch targets, cycle wrap");
    return true;
}

static bool test_memory(void)
{
    static const uint8_t opcodes[] = {
        0x05,0x0d,0x25,0x2d,0x45,0x4d,0x65,0x6d,0x85,0x8d,
        0xa5,0xad,0xc5,0xcd,0xe5,0xed,0xa6,0xae,0xa4,0xac,
        0x86,0x8e,0x84,0x8c,0xe4,0xec,0xc4,0xcc,0x24,0x2c,
        0x06,0x0e,0x26,0x2e,0x46,0x4e,0x66,0x6e,0xe6,0xee,0xc6,0xce
    };
    size_t operation;
    unsigned int value, region;
    for (operation = 0; operation < sizeof(opcodes); ++operation)
        for (value = 0; value < 256; ++value)
            for (region = 0; region < 2; ++region)
            {
                uint8_t program[] = {opcodes[operation], 0x40, region ? 0x65 : 0x05};
                guest_ram[0x40] = guest_ram[0x540] = guest_ram[0x6540] = (uint8_t)value;
                if (!run_case(program, jit_instruction_size(program[0]), 0x400,
                              (uint8_t)(value * 29 + 37), (uint8_t)(value ^ 0xff),
                              (uint8_t)(value ^ 0x80), (uint8_t)(value & ~FlagD), 0xfffffcUL))
                    return false;
            }
    puts("PASS 42 direct-RAM encodings, split RAM, RMW, all input bytes");
    return true;
}

static bool test_indexed_memory(void)
{
    static const uint8_t opcodes[] = {
        0x01,0x11,0x15,0x19,0x1d,0x21,0x31,0x35,0x39,0x3d,
        0x41,0x51,0x55,0x59,0x5d,0x61,0x71,0x75,0x79,0x7d,
        0x81,0x91,0x95,0x99,0x9d,0xa1,0xb1,0xb5,0xb9,0xbd,
        0xc1,0xd1,0xd5,0xd9,0xdd,0xe1,0xf1,0xf5,0xf9,0xfd,
        0xb6,0xbe,0xb4,0xbc,0x96,0x94,
        0x16,0x1e,0x36,0x3e,0x56,0x5e,0x76,0x7e,0xf6,0xfe,0xd6,0xde
    };
    unsigned int operation, value, region, address;
    for (operation = 0; operation < sizeof(opcodes); ++operation)
        for (value = 0; value < 256; ++value)
            for (region = 0; region < 3; ++region)
            {
                uint8_t opcode = opcodes[operation];
                uint8_t program[] = {opcode, 0xfd, region == 0 ? 0x0e : region == 1 ? 0x0f : 0xc0};
                uint8_t index_x = (uint8_t)value;
                uint8_t index_y = (uint8_t)(255 - value);
                bool zero_page = (opcode & 0x1f) == 0x15 || opcode == 0xb6 ||
                    opcode == 0xb4 || opcode == 0x96 || opcode == 0x94 ||
                    (opcode & 0x1f) == 0x16;
                for (address = 2; address < 256; ++address)
                    guest_ram[address] = (uint8_t)(address * 73 + value);
                for (address = 0xe00; address < 0x1100; ++address)
                    guest_ram[address] = (uint8_t)(address * 17 + value);
                for (address = 0xc000; address < 0xc200; ++address)
                    guest_ram[address] = (uint8_t)(address * 31 + value);
                if ((opcode & 0x1f) == 0x01 || (opcode & 0x1f) == 0x11)
                {
                    uint8_t pointer = (uint8_t)(program[1] + ((opcode & 0x1f) == 1 ? index_x : 0));
                    guest_ram[pointer] = program[1];
                    guest_ram[(uint8_t)(pointer + 1)] = program[2];
                }
                if (zero_page)
                {
                    uint8_t offset = opcode == 0xb6 || opcode == 0x96 ? index_y : index_x;
                    if ((uint8_t)(program[1] + offset) < 2)
                        program[1] -= 2;
                }
                if (!run_case(program, jit_instruction_size(opcode), 0x400,
                              (uint8_t)(value * 29 + 37), index_x, index_y,
                              (uint8_t)(value & ~FlagD), 0xfffffcUL))
                    return false;
            }
    puts("PASS 58 indexed/indirect encodings, zero-page wrap, page penalties, split/upper RAM");
    return true;
}

static bool test_indexed_copy_traces(void)
{
    static const uint16_t regions[][2] = {
        {0x2000, 0x3000}, {0x0f00, 0x2000}, {0x9f00, 0x0f00}, {0xcf00, 0xc000},
        {0x2000, 0x2001}, {0x2001, 0x2000}, {0x0f01, 0x2000}, {0x20ff, 0x20fd}
    };
    unsigned int region, mode, value, offset;
    for (region = 0; region < sizeof(regions) / sizeof(regions[0]); ++region)
        for (mode = 0; mode < 4; ++mode)
            for (value = 0; value < 256; ++value)
            {
                uint16_t source = regions[region][0];
                uint16_t destination = regions[region][1];
                bool source_y = (mode & 1) != 0;
                bool destination_y = source_y != ((mode & 2) != 0);
                uint8_t load = source_y ? 0xb9 : 0xbd;
                uint8_t store = destination_y ? 0x99 : 0x9d;
                uint8_t program[] = {
                    load, (uint8_t)source, (uint8_t)(source >> 8),
                    store, (uint8_t)destination, (uint8_t)(destination >> 8),
                    load, (uint8_t)source, (uint8_t)(source >> 8),
                    destination_y ? 0x88 : 0xca,
                    store, (uint8_t)destination, (uint8_t)(destination >> 8)
                };
                for (offset = 0; offset < 256; ++offset)
                {
                    guest_ram[source + offset] = (uint8_t)(offset * 37 + value);
                    guest_ram[destination + offset] = (uint8_t)(offset * 19 - value);
                }
                if (!run_case(program, sizeof(program), 0x400, (uint8_t)(value ^ 0x55),
                              (uint8_t)value, (uint8_t)(255 - value),
                              (uint8_t)(value & ~FlagD), 0xfffffaUL))
                    return false;
            }
    puts("PASS indexed copy traces, same/different indices, overlap, changed indices and cycle wrap");
    return true;
}

static bool test_boundaries(void)
{
    static const uint16_t bases[] = {
        0x0000,0x0001,0x00ff,0x0f00,0x0f01,0x0fff,0x9f00,0x9f01,
        0x9fff,0xa000,0xbfff,0xcf00,0xcf01,0xcfff,0xd000,0xffff
    };
    static const uint8_t indexed[] = {0xbd,0x9d,0xfe,0xb9,0x99,0xb1,0x91};
    static const uint8_t branches[] = {0x10,0x30,0x50,0x70,0x90,0xb0,0xd0,0xf0};
    unsigned int base, operation, index, prefix, flags, offset;
    for (base = 0; base < sizeof(bases) / sizeof(bases[0]); ++base)
        for (operation = 0; operation < sizeof(indexed); ++operation)
            for (index = 0; index < 256; ++index)
                for (prefix = 0; prefix < 2; ++prefix)
                {
                    uint8_t opcode = indexed[operation];
                    uint8_t program[] = {0xea,opcode,(uint8_t)bases[base],(uint8_t)(bases[base] >> 8)};
                    uint16_t effective = (uint16_t)(bases[base] + index);
                    bool write = opcode == 0x9d || opcode == 0xfe || opcode == 0x99 || opcode == 0x91;
                    bool allowed = (effective < 0xa000 || (effective >= 0xc000 && effective < 0xd000)) &&
                        (!write || effective >= 2);
                    uint8_t size = jit_instruction_size(opcode);
                    if (size == 2)
                    {
                        program[2] = 0x80;
                        guest_ram[0x80] = (uint8_t)bases[base];
                        guest_ram[0x81] = (uint8_t)(bases[base] >> 8);
                    }
                    if (!run_limited_case(program + (1 - prefix), (uint8_t)(size + prefix), 0x600,
                                          0x81, (uint8_t)index, (uint8_t)index, 0, 0xfffffcUL,
                                          (uint8_t)(prefix + allowed)))
                        return false;
                }
    for (operation = 0; operation < sizeof(branches); ++operation)
        for (offset = 0; offset < 256; ++offset)
            for (flags = 0; flags < 2; ++flags)
                for (index = 0; index < 2; ++index)
                {
                    uint8_t program[] = {branches[operation], (uint8_t)offset};
                    if (!run_case(program, 2, index ? 0xffff : 0x04fe, 0,0,0,
                                  flags ? 0xc3 : 0, 0xfffffeUL))
                        return false;
                }
    {
        static const uint8_t self_modifying[] = {0x8d,0x03,0x04,0xe8,0xca,0xd0,0xfc};
        if (!run_limited_case(self_modifying, sizeof(self_modifying), 0x400, 0xea, 7, 0, 0, 0, 1))
            return false;
    }
    puts("PASS runtime side exits, RAM/I/O/port boundaries, all branch offsets, PC wrap, store-ending blocks");
    return true;
}

static bool test_output_limits(void)
{
    uint8_t program[48];
    uint8_t output[JIT_NATIVE_CAPACITY + 2];
    JitTranslation translation;
    unsigned int capacity, index;
    for (index = 0; index < sizeof(program); index += 2)
    {
        program[index] = 0x69;
        program[index + 1] = (uint8_t)index;
    }
    for (capacity = 0; capacity <= JIT_NATIVE_CAPACITY; ++capacity)
    {
        bool compiled;
        memset(output, 0xcc, sizeof(output));
        compiled = jit_compile(output + 1, (uint16_t)capacity, program, sizeof(program), 0x400,
                                &target, &translation);
        if (output[0] != 0xcc || output[capacity + 1] != 0xcc)
            return false;
        if (compiled && (translation.native_size > capacity || !translation.instructions ||
                         translation.instructions > JIT_MAX_INSTRUCTIONS ||
                         translation.guest_size != translation.instructions * 2))
            return false;
    }
    program[0] = 0x02;
    if (jit_compile(output, sizeof(output), program, sizeof(program), 0x400, &target, &translation))
        return false;
    program[0] = 0xad;
    if (jit_compile(output, sizeof(output), program, 2, 0x400, &target, &translation))
        return false;
    program[0] = 0xea;
    program[1] = 0x02;
    if (!jit_compile(output, sizeof(output), program, 2, 0x400, &target, &translation) ||
        translation.instructions != 1 || translation.guest_size != 1)
        return false;
    puts("PASS 1025 output capacities, complete-instruction prefixes, illegal/truncated opcode fallback");
    return true;
}

static bool test_stack_control(void)
{
    static const uint8_t opcodes[] = {0x08,0x20,0x28,0x40,0x48,0x60,0x68,0x6c};
    unsigned int operation, value, region, address;
    for (operation = 0; operation < sizeof(opcodes); ++operation)
        for (value = 0; value < 256; ++value)
            for (region = 0; region < 3; ++region)
            {
                uint8_t program[] = {0xa2,(uint8_t)value,0x9a,opcodes[operation],0xff,
                                     region == 0 ? 0x00 : region == 1 ? 0x0f : 0xcf};
                for (address = 0x100; address < 0x200; ++address)
                    guest_ram[address] = (uint8_t)(address * 73 + value);
                guest_ram[0xff] = guest_ram[0xfff] = guest_ram[0xcfff] = (uint8_t)value;
                guest_ram[0] = guest_ram[0xf00] = guest_ram[0xcf00] = (uint8_t)(value ^ 0x80);
                if (!run_case(program, (uint8_t)(3 + jit_instruction_size(opcodes[operation])), 0x400,
                              (uint8_t)(value * 29 + 37), 0, 0, (uint8_t)value, 0xfffffcUL))
                    return false;
            }
    {
        static const uint8_t rts[] = {0x60};
        static const uint8_t plp[] = {0x28,0x69,0x01};
        static const uint8_t indirect[] = {0x6c,0x00,0x45};
        guest_ram[0x100] = guest_ram[0x101] = 0xff;
        if (!run_case(rts, sizeof(rts), 0xffff, 0,0,0,0,0))
            return false;
        if (!run_limited_case(plp, sizeof(plp), 0x400, 0x09,0,0,0,0,1))
            return false;
        guest_ram[0x4500] = 0x12;
        guest_ram[0x4501] = 0x56;
        if (!run_case(indirect, sizeof(indirect), 0x04fe, 0,0,0,0,0))
            return false;
    }
    puts("PASS 8 stack/control encodings, all stack positions/status bytes, JSR/RTS/RTI, NMOS JMP-indirect wrap");
    return true;
}

static bool test_arithmetic_exhaustive(void)
{
    unsigned int accumulator, operand, carry, operation;
    for (operation = 0; operation < 2; ++operation)
        for (accumulator = 0; accumulator < 256; ++accumulator)
            for (operand = 0; operand < 256; ++operand)
                for (carry = 0; carry < 2; ++carry)
                {
                    uint8_t program[] = {operation ? 0xe9 : 0x69, (uint8_t)operand};
                    if (!run_case(program, 2, 0x400, (uint8_t)accumulator, 0, 0,
                                  (uint8_t)carry, 0))
                        return false;
                }
    puts("PASS exhaustive binary ADC/SBC: 262144 native cases");
    return true;
}

int main(int argc, char **argv)
{
    bool passed;
    asic_init();
    mem.flash.size = SIZE_FLASH_MIN;
    mem.flash.block = malloc(mem.flash.size);
    if (!mem.flash.block) return 1;
    memset(mem.flash.block, 0xff, mem.flash.size);
    asic_reset();
    emu_set_run_rate(1000);
    reference = vrEmu6502New(CPU_6502, reference_read, reference_write);
    if (!reference) return 1;
    passed = test_opcode_families() && test_traces() && test_memory() && test_indexed_memory() &&
        test_indexed_copy_traces() && test_boundaries() && test_output_limits() && test_stack_control();
    if (passed && argc > 1 && !strcmp(argv[1], "--exhaustive"))
        passed = test_arithmetic_exhaustive();
    printf("%s: %lu native executions, %lu assertions\n", passed ? "PASS" : "FAIL", cases, assertions);
    vrEmu6502Destroy(reference);
    asic_free();
    return passed ? 0 : 1;
}