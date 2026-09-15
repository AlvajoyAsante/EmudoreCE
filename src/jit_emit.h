#ifndef EMUDORE_CE_JIT_EMIT_H
#define EMUDORE_CE_JIT_EMIT_H

#include <stdbool.h>
#include <stdint.h>

#define JIT_MAX_GUEST_BYTES 48
#define JIT_MAX_INSTRUCTIONS 16
#define JIT_NATIVE_CAPACITY 1024

typedef struct
{
    uint32_t state;
    uint32_t low_ram;
    uint32_t high_ram;
    uint32_t read_byte;
    uint32_t write_byte;
    uint32_t decimal_adc;
    uint32_t decimal_sbc;
    uint32_t brk;
} JitTarget;

typedef struct
{
    uint16_t native_size;
    uint16_t fallthrough_pc;
    uint16_t taken;
    uint8_t guest_size;
    uint8_t instructions;
    uint8_t min_cycles;
    uint8_t max_cycles;
    bool conditional;
    bool binary_only;
    bool reads_memory;
    bool writes_memory;
    bool guarded;
    bool can_repeat;
} JitTranslation;

uint8_t jit_instruction_size(uint8_t opcode);
bool jit_compile(uint8_t *output, uint16_t capacity, const uint8_t *guest,
                 uint8_t available, uint16_t pc, const JitTarget *target,
                 JitTranslation *translation);

#endif