#include <string.h>
#include <stdbool.h>
#include <stddef.h>
#include <limits.h>
#include "jit.h"
#include "cpu.h"
#include "memory.h"

#define JIT_BLOCK_CAPACITY 16
#define JIT_SLOT_CAPACITY 512
#define JIT_HOT_THRESHOLD 8

extern bool jit_bytes_equal(const uint8_t *left, const uint8_t *right, uint8_t length);
extern uint8_t jit_run_fast(unsigned int until_cycles);
extern uint8_t jit_run_batch_fast(unsigned int until_cycles);
extern void jit_read_bridge(void);
extern void jit_write_bridge(void);

typedef struct
{
    uint16_t start;
    uint8_t mapping_generation;
    uint8_t observations;
    uint8_t code_size;
    bool ready;
    bool ineligible;
    bool blacklisted;
    JitTranslation translation;
    uint8_t code[JIT_MAX_GUEST_BYTES];
    uint8_t native[JIT_SLOT_CAPACITY + 2];
} JitBlock;

#if UINT_MAX == 0xffffffUL
_Static_assert(sizeof(JitBlock) == 586, "native dispatcher block stride");
_Static_assert(offsetof(JitBlock, start) == 0, "native dispatcher PC offset");
_Static_assert(offsetof(JitBlock, mapping_generation) == 2, "native dispatcher map offset");
_Static_assert(offsetof(JitBlock, code_size) == 4, "native dispatcher length offset");
_Static_assert(offsetof(JitBlock, ready) == 5, "native dispatcher ready offset");
_Static_assert(offsetof(JitBlock, translation) + offsetof(JitTranslation, max_cycles) == 17, "native dispatcher budget offset");
_Static_assert(offsetof(JitBlock, translation) + offsetof(JitTranslation, binary_only) == 19, "native dispatcher decimal offset");
_Static_assert(offsetof(JitBlock, translation) + offsetof(JitTranslation, can_repeat) == 23, "native dispatcher repeat offset");
_Static_assert(offsetof(JitBlock, code) == 24, "native dispatcher guest bytes offset");
_Static_assert(offsetof(JitBlock, native) == 72, "native dispatcher machine code offset");
_Static_assert(JIT_BLOCK_CAPACITY == 16 && JIT_SLOT_CAPACITY == 512, "native dispatcher cache dimensions");
#endif

uint8_t jit_page_generation[256];
uint8_t jit_page_generation_wrapped;
uint8_t jit_mapping_generation;
uint8_t jit_cached_block_count;
uint8_t jit_ready_block_count;
uint8_t jit_ineligible_block_count;
unsigned int jit_block_observations;
unsigned int jit_native_hits;
unsigned int jit_native_mismatches;
JitBlock jit_blocks[JIT_BLOCK_CAPACITY];
JitBlock *jit_entry_points[256];
static JitTarget jit_target;
static uint8_t jit_victim;
bool jit_backend_ok;

static bool block_is_current(JitBlock *block)
{
    uint8_t offset = 0;
    uint8_t first_page = (uint8_t)(block->start >> 8);
    uint8_t last_page = (uint8_t)((block->start + block->code_size - 1) >> 8);

    if (block->ready && !block->translation.writes_memory &&
        !jit_page_generation_wrapped &&
        block->blacklisted == jit_mapping_generation &&
        block->mapping_generation == jit_page_generation[first_page] &&
        block->observations == jit_page_generation[last_page])
        return true;

    while (offset < block->code_size)
    {
        uint16_t address = (uint16_t)(block->start + offset);
        const uint8_t *page = mem_rd_pg[address >> 8].ptr;
        unsigned int span = 0x100u - (address & 0xffu);
        if (span > (unsigned int)(block->code_size - offset))
            span = block->code_size - offset;
        if (!page || !jit_bytes_equal(block->code + offset, page + address, (uint8_t)span))
            return false;
        offset += (uint8_t)span;
    }
    if (block->ready && !block->translation.writes_memory)
    {
        block->mapping_generation = jit_page_generation[first_page];
        block->observations = jit_page_generation[last_page];
        block->blacklisted = jit_mapping_generation;
    }
    return true;
}

static void unindex_block(JitBlock *block)
{
    uint8_t key = (uint8_t)block->start;
    uint8_t index;
    if (jit_entry_points[key] != block)
        return;
    jit_entry_points[key] = NULL;
    for (index = 0; index < JIT_BLOCK_CAPACITY; ++index)
    {
        JitBlock *candidate = &jit_blocks[index];
        if (candidate != block && candidate->ready && (uint8_t)candidate->start == key)
        {
            jit_entry_points[key] = candidate;
            break;
        }
    }
}

static void discard_block(JitBlock *block)
{
    unindex_block(block);
    if (block->observations || block->ready)
        --jit_cached_block_count;
    if (block->ready)
        --jit_ready_block_count;
    if (block->ineligible)
        --jit_ineligible_block_count;
    memset(block, 0, sizeof(*block));
}

void jit_reset(void)
{
    memset(jit_page_generation, 0, sizeof(jit_page_generation));
    jit_page_generation_wrapped = 0;
    jit_mapping_generation = 0;
    memset(jit_blocks, 0, sizeof(jit_blocks));
    memset(jit_entry_points, 0, sizeof(jit_entry_points));
    jit_cached_block_count = 0;
    jit_ready_block_count = 0;
    jit_ineligible_block_count = 0;
    jit_block_observations = 0;
    jit_native_hits = 0;
    jit_native_mismatches = 0;
    jit_victim = 0;
    jit_target.state = (uint32_t)(uintptr_t)&cpu;
    jit_target.low_ram = (uint32_t)(uintptr_t)mem_low;
    jit_target.high_ram = (uint32_t)(uintptr_t)mem_ram;
    jit_target.read_byte = (uint32_t)(uintptr_t)&jit_read_bridge;
    jit_target.write_byte = (uint32_t)(uintptr_t)&jit_write_bridge;
    jit_target.decimal_adc = (uint32_t)(uintptr_t)&cpu_decimal_adc;
    jit_target.decimal_sbc = (uint32_t)(uintptr_t)&cpu_decimal_sbc;
    jit_target.brk = (uint32_t)(uintptr_t)&cpu_brk_native;
}

uint8_t jit_read_memory(uint16_t address, uint8_t bias)
{
    uint8_t value;
    cpu.cycles += bias;
    value = mem_read_byte(address);
    cpu_warp = 0;
    cpu.cycles -= bias;
    return value;
}

void jit_write_memory(uint16_t address, uint8_t value, uint8_t bias)
{
    cpu.cycles += bias;
    mem_write_byte(address, value);
    cpu.cycles -= bias;
}

bool jit_backend_selftest(void)
{
    static const uint8_t program[] = {0xa9,0x7f,0x18,0x69,0x01,0xaa,0xca,0xd0,0xfd};
    static const uint8_t expected[16] = {
        0x06,0x04,0xff,0x80,0x7f,0x00,0x00,0x00,
        0x00,0x00,0x01,0x01,0x00,0x05,0x00,0x00
    };
    uint8_t state[18] = {0};
    JitTarget target = {.state = (uint32_t)(uintptr_t)(state + 1)};
    JitTranslation translation;
    uint8_t index;

    jit_backend_ok = false;
    jit_reset();
    state[0] = 0x5a;
    state[17] = 0xa5;
    state[2] = 0x04;
    state[3] = 0xff;
    state[11] = 1;
    state[14] = 0xf8;
    state[15] = state[16] = 0xff;
    if (jit_compile(jit_blocks[0].native + 1, JIT_SLOT_CAPACITY, program,
                    sizeof(program), 0x400, &target, &translation))
    {
        jit_backend_ok = jit_execute(jit_blocks[0].native + 1) &&
            state[0] == 0x5a && state[17] == 0xa5;
        for (index = 0; index < sizeof(expected); ++index)
            if (state[index + 1] != expected[index])
                jit_backend_ok = false;
    }
    jit_reset();
    jit_native_mismatches = !jit_backend_ok;
    return jit_backend_ok;
}

bool jit_backend_ready(void)
{
    return jit_backend_ok;
}

void jit_invalidate_page(uint8_t page)
{
    if (!++jit_page_generation[page])
        jit_page_generation_wrapped = 1;
}

void jit_invalidate_range(uint16_t addr, unsigned int length)
{
    while (length)
    {
        unsigned int span = 0x100u - (addr & 0xffu);
        jit_invalidate_page((uint8_t)(addr >> 8));
        if (span >= length)
            return;
        addr = (uint16_t)(addr + span);
        length -= span;
    }
}

bool jit_run(unsigned int until_cycles)
{
    uint8_t index;
    uint8_t fast = jit_run_fast(until_cycles);
    unsigned int remaining = until_cycles - cpu.cycles;

    if (fast)
        return fast == 1;
    if (!jit_backend_ok || (int)remaining <= 0)
        return false;
    for (index = 0; index < JIT_BLOCK_CAPACITY; ++index)
    {
        JitBlock *block = &jit_blocks[index];
        if (!block->ready || block->start != cpu.pc)
            continue;
        if (remaining < block->translation.max_cycles ||
            (cpu.dmf && block->translation.binary_only))
            return false;
        if (!block_is_current(block))
        {
            discard_block(block);
            return false;
        }
        if (block->native[0] != 0x5a || block->native[JIT_SLOT_CAPACITY + 1] != 0xa5)
        {
            unindex_block(block);
            block->ready = false;
            block->blacklisted = true;
            --jit_ready_block_count;
            ++jit_native_mismatches;
            return false;
        }
        if (!jit_execute(block->native + 1))
            return false;
        ++jit_native_hits;
        return true;
    }
    return false;
}

bool jit_run_batch(unsigned int until_cycles)
{
    uint8_t fast = jit_run_batch_fast(until_cycles);
    return fast ? fast == 1 : jit_run(until_cycles);
}

void jit_observe_pc(uint16_t pc)
{
    JitBlock *slot = NULL;
    uint8_t index;

    if (!jit_backend_ok || pc < 2 || !mem_rd_pg[pc >> 8].ptr)
        return;
    ++jit_block_observations;
    for (index = 0; index < JIT_BLOCK_CAPACITY; ++index)
    {
        JitBlock *block = &jit_blocks[index];
        if ((block->observations || block->ready) && block->start == pc)
        {
            if (block->ready)
            {
                jit_entry_points[(uint8_t)pc] = block;
                return;
            }
            if (!block_is_current(block))
            {
                discard_block(block);
                slot = block;
                break;
            }
            if (block->observations != UINT8_MAX)
                ++block->observations;
            if (!block->ready && !block->ineligible && !block->blacklisted &&
                block->observations >= JIT_HOT_THRESHOLD)
            {
                block->native[0] = 0x5a;
                block->native[JIT_SLOT_CAPACITY + 1] = 0xa5;
                if (jit_compile(block->native + 1, JIT_SLOT_CAPACITY, block->code,
                                block->code_size, pc, &jit_target, &block->translation))
                {
                    block->code_size = block->translation.guest_size;
                    block->mapping_generation = jit_page_generation[pc >> 8];
                    block->observations = jit_page_generation[
                        (uint8_t)((pc + block->code_size - 1) >> 8)];
                    block->blacklisted = jit_mapping_generation;
                    block->ready = true;
                    jit_entry_points[(uint8_t)pc] = block;
                    ++jit_ready_block_count;
                }
                else
                {
                    block->ineligible = true;
                    ++jit_ineligible_block_count;
                }
            }
            return;
        }
        if (!block->observations && !block->ready && !slot)
            slot = block;
    }
    if (!slot)
    {
        slot = &jit_blocks[jit_victim];
        jit_victim = (uint8_t)((jit_victim + 1) % JIT_BLOCK_CAPACITY);
        discard_block(slot);
    }
    slot->start = pc;
    slot->mapping_generation = jit_mapping_generation;
    slot->observations = 1;
    for (index = 0; index < JIT_MAX_GUEST_BYTES; ++index)
    {
        uint16_t address = (uint16_t)(pc + index);
        const uint8_t *page = mem_rd_pg[address >> 8].ptr;
        if (address < 2 || !page)
            break;
        slot->code[index] = page[address];
    }
    slot->code_size = index;
    ++jit_cached_block_count;
}