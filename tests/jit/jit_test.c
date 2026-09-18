#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "../../src/jit.c"

CpuState cpu;
uint8_t cpu_warp;
static uint8_t ram[65536];
static uint8_t rom[65536];
uint8_t *mem_low = ram;
uint8_t *mem_ram = ram;
MemPage mem_rd_pg[256];
MemPage mem_wr_pg[256];
static unsigned int reference_steps;
static unsigned int native_calls;
static unsigned int checks;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

uint8_t mem_debug_peek_ram(uint16_t address) { return ram[address]; }
uint8_t mem_read_io(uint16_t address) { (void)address; return 0; }
void mem_write_io(uint16_t address, uint8_t value) { (void)address; (void)value; }
void mem_setup_banks(uint8_t value) { (void)value; ++jit_mapping_generation; }
void jit_read_bridge(void) {}
void jit_write_bridge(void) {}
void cpu_decimal_adc(uint8_t operand) { (void)operand; }
void cpu_decimal_sbc(uint8_t operand) { (void)operand; }
void cpu_brk_native(void) {}
uint8_t mem_debug_peek_cpu(uint16_t address)
{
    const uint8_t *page = mem_rd_pg[address >> 8].ptr;
    return page ? page[address] : 0;
}

bool jit_bytes_equal(const uint8_t *left, const uint8_t *right, uint8_t length)
{
    return memcmp(left, right, length) == 0;
}

uint8_t jit_run_fast(unsigned int until_cycles)
{
    (void)until_cycles;
    return 0;
}

uint8_t jit_run_batch_fast(unsigned int until_cycles)
{
    (void)until_cycles;
    return 0;
}

bool cpu_step_slow(void)
{
    uint8_t opcode = mem_debug_peek_cpu(cpu.pc++);
    uint8_t value;
    ++reference_steps;
    cpu.cycles += 2;
    switch (opcode)
    {
    case 0xa2:
        cpu.x = mem_debug_peek_cpu(cpu.pc++);
        SET_ZF(cpu.x);
        SET_NF(cpu.x);
        break;
    case 0xe8:
        ++cpu.x;
        SET_ZF(cpu.x);
        SET_NF(cpu.x);
        break;
    case 0xca:
        --cpu.x;
        SET_ZF(cpu.x);
        SET_NF(cpu.x);
        break;
    case 0xea:
        break;
    case 0xd0:
    case 0xf0:
        value = mem_debug_peek_cpu(cpu.pc++);
        if ((opcode == 0xf0) == cpu.zf)
        {
            uint16_t target = (uint16_t)(cpu.pc + (int8_t)value);
            cpu.cycles += 1 + ((target >> 8) != (cpu.pc >> 8));
            cpu.pc = target;
        }
        break;
    default:
        return false;
    }
    return true;
}

bool jit_execute(const uint8_t *code)
{
    uint8_t index;
    ++native_calls;
    for (index = 0; index < JIT_BLOCK_CAPACITY; ++index)
        if (code == jit_blocks[index].native + 1)
        {
            uint8_t step;
            for (step = 0; step < jit_blocks[index].translation.instructions; ++step)
                if (!cpu_step_slow())
                    return false;
            return true;
        }
    return false;
}

/* Host stand-in for the assembly block dispatcher: run the block's guest
   instructions through the reference stepper so CPU state, cycle budget
   and native_calls match the on-target dispatch path. */
bool jit_execute_block(const void *block)
{
    const JitBlock *entry = block;
    uint8_t step;
    ++native_calls;
    for (step = 0; step < entry->translation.instructions; ++step)
        if (!cpu_step_slow())
            return false;
    return true;
}

static void reset_fixture(void)
{
    unsigned int page;
    memset(ram, 0, sizeof(ram));
    memset(rom, 0, sizeof(rom));
    for (page = 0; page < 256; ++page)
        mem_rd_pg[page].ptr = ram;
    memset(&cpu, 0, sizeof(cpu));
    jit_reset();
    jit_backend_ok = true;
    jit_target = (JitTarget){.state = 0xd02000, .low_ram = 0xd20000, .high_ram = 0xd30000};
    reference_steps = 0;
    native_calls = 0;
}

static void make_hot(uint16_t address, const uint8_t *code, size_t length)
{
    unsigned int observation;
    memcpy(ram + address, code, length);
    for (observation = 0; observation < JIT_HOT_THRESHOLD; ++observation)
        jit_observe_pc(address);
    cpu.pc = address;
}

static bool test_decode(void)
{
    CHECK(jit_instruction_size(0xa2) == 2);
    CHECK(jit_instruction_size(0xa0) == 2);
    CHECK(jit_instruction_size(0xc0) == 2);
    CHECK(jit_instruction_size(0xe0) == 2);
    CHECK(jit_instruction_size(0x20) == 3);
    CHECK(jit_instruction_size(0xb9) == 3);
    CHECK(jit_instruction_size(0x02) == 0);
    return true;
}

static bool test_terminal_branch(void)
{
    static const uint8_t code[] = {0xe8, 0xca, 0xd0, 0xfd};
    reset_fixture();
    make_hot(0x400, code, sizeof(code));
    cpu.x = 2;
    CHECK(jit_ready_block_count == 1);
    CHECK(reference_steps == 0 && native_calls == 0);
    CHECK(cpu.pc == 0x400 && cpu.x == 2 && cpu.cycles == 0);
    CHECK(jit_run(7));
    CHECK(cpu.pc == 0x401 && cpu.x == 2 && cpu.cycles == 7);
    CHECK(reference_steps == 3 && native_calls == 1 && jit_native_hits == 1);
    return true;
}

static bool test_untracked_write(void)
{
    static const uint8_t code[] = {0xea, 0xd0, 0xfd};
    reset_fixture();
    make_hot(0x400, code, sizeof(code));
    ram[0x400] = 0x8d;
    jit_invalidate_page(0x04);
    CHECK(!jit_run(20));
    CHECK(native_calls == 0 && reference_steps == 0);
    CHECK(cpu.pc == 0x400 && cpu.cycles == 0);
    CHECK(jit_ready_block_count == 0 && jit_cached_block_count == 0);
    return true;
}

static bool test_blacklist(void)
{
    static const uint8_t code[] = {0xea, 0xd0, 0xfd};
    reset_fixture();
    make_hot(0x400, code, sizeof(code));
    jit_blocks[0].native[JIT_SLOT_CAPACITY + 1] = 0;
    CHECK(!jit_run(20));
    CHECK(jit_native_mismatches == 1);
    CHECK(jit_ready_block_count == 0);
    jit_observe_pc(0x400);
    CHECK(jit_ready_block_count == 0);
    CHECK(!jit_run(20));
    CHECK(native_calls == 0 && reference_steps == 0);
    return true;
}

static bool test_budget(void)
{
    static const uint8_t code[] = {0xe8,0xca,0xd0,0xfd};
    CpuState saved;
    unsigned int budget;
    reset_fixture();
    make_hot(0x400, code, sizeof(code));
    cpu.x = 2;
    saved = cpu;
    for (budget = 0; budget < 7; ++budget)
    {
        CHECK(!jit_run(budget));
        CHECK(memcmp(&saved, &cpu, sizeof(cpu)) == 0);
        CHECK(native_calls == 0 && reference_steps == 0);
    }
    cpu.cycles = UINT_MAX - 3;
    CHECK(!jit_run(2));
    CHECK(jit_run(3));
    CHECK(cpu.cycles == 3 && cpu.pc == 0x401);
    return true;
}

static bool test_gates(void)
{
    static const uint8_t arithmetic[] = {0x69,0x7f,0xd0,0xfc};
    static const uint8_t mapped[] = {0xea,0xd0,0xfd};
    CpuState saved;
    reset_fixture();
    make_hot(0x400, arithmetic, sizeof(arithmetic));
    CHECK(jit_ready_block_count == 1);
    cpu.dmf = true;
    saved = cpu;
    CHECK(!jit_run(20));
    CHECK(memcmp(&saved, &cpu, sizeof(cpu)) == 0);
    reset_fixture();
    make_hot(0x400, mapped, sizeof(mapped));
    ++jit_mapping_generation;
    CHECK(jit_run(20));
    ram[0x401] ^= 0x10;
    jit_invalidate_page(0x04);
    CHECK(!jit_run(20));
    CHECK(jit_ready_block_count == 0);
    make_hot(0x400, mapped, sizeof(mapped));
    jit_backend_ok = false;
    CHECK(!jit_run(20));
    CHECK(native_calls == 1 && reference_steps == 2);
    return true;
}

static bool test_cache_index(void)
{
    static const uint8_t code[] = {0xea,0xd0,0xfd};
    JitBlock *first, *second;
    reset_fixture();
    make_hot(0x400, code, sizeof(code));
    first = jit_entry_points[0];
    CHECK(first && first->ready && first->start == 0x400);
    make_hot(0x500, code, sizeof(code));
    second = jit_entry_points[0];
    CHECK(second && second != first && second->ready && second->start == 0x500);
    ram[0x500] = 0x02;
    jit_invalidate_page(0x05);
    CHECK(!jit_run(20));
    CHECK(jit_entry_points[0] == first);
    cpu.pc = 0x400;
    CHECK(jit_run(20));
    first->native[JIT_SLOT_CAPACITY + 1] = 0;
    CHECK(!jit_run(20));
    CHECK(jit_entry_points[0] == NULL);
    CHECK(jit_ready_block_count == 0);
    return true;
}

static bool test_cache_capacity(void)
{
    static const uint8_t supported[] = {0xea,0xd0,0xfd};
    static const uint8_t unsupported[] = {0x02};
    unsigned int entry, key;
    reset_fixture();
    for (entry = 0; entry < 96; ++entry)
    {
        make_hot((uint16_t)(0x400 + entry * 0x40), entry & 1 ? unsupported : supported,
                 entry & 1 ? sizeof(unsupported) : sizeof(supported));
        CHECK(jit_cached_block_count <= JIT_BLOCK_CAPACITY);
        CHECK(jit_ready_block_count + jit_ineligible_block_count == jit_cached_block_count);
        for (key = 0; key < 256; ++key)
        {
            JitBlock *indexed = jit_entry_points[key];
            CHECK(!indexed || (indexed >= jit_blocks && indexed < jit_blocks + JIT_BLOCK_CAPACITY &&
                              indexed->ready && (uint8_t)indexed->start == key));
        }
        for (key = 0; key < JIT_BLOCK_CAPACITY; ++key)
            CHECK(!jit_blocks[key].ready || jit_entry_points[(uint8_t)jit_blocks[key].start]);
    }
    CHECK(jit_cached_block_count == JIT_BLOCK_CAPACITY);
    CHECK(jit_ready_block_count == JIT_BLOCK_CAPACITY / 2);
    CHECK(jit_ineligible_block_count == JIT_BLOCK_CAPACITY / 2);
    jit_reset();
    CHECK(jit_cached_block_count == 0 && jit_ready_block_count == 0 && jit_ineligible_block_count == 0);
    for (key = 0; key < 256; ++key)
        CHECK(jit_entry_points[key] == NULL);
    return true;
}

static bool test_banked_code(void)
{
    static const uint16_t addresses[] = {0x0002,0x0100,0xa000,0xd000,0xe000,0xfffd};
    static const uint8_t code[] = {0xea,0xd0,0xfd};
    unsigned int entry, observation;
    for (entry = 0; entry < sizeof(addresses) / sizeof(addresses[0]); ++entry)
    {
        uint16_t address = addresses[entry];
        reset_fixture();
        memcpy(rom + address, code, sizeof(code));
        mem_rd_pg[address >> 8].ptr = rom;
        for (observation = 0; observation < JIT_HOT_THRESHOLD; ++observation)
            jit_observe_pc(address);
        CHECK(jit_ready_block_count == 1);
        cpu.pc = address;
        ram[address] = 0x02;
        CHECK(jit_run(8));
          CHECK(cpu.pc == address && cpu.cycles == 5u +
              (((uint16_t)(address + 3) >> 8) != (address >> 8)));
        rom[address + 1] = 0xf0;
          jit_invalidate_page((uint8_t)(address >> 8));
        CHECK(!jit_run(20));
        CHECK(jit_ready_block_count == 0);
    }
    reset_fixture();
    mem_rd_pg[0xd0].ptr = NULL;
    make_hot(0xd000, code, sizeof(code));
    CHECK(jit_cached_block_count == 0 && jit_ready_block_count == 0);
    return true;
}

int main(int argc, char **argv)
{
    static const struct {
        const char *name;
        bool (*run)(void);
    } tests[] = {
        {"decode", test_decode},
        {"terminal-branch", test_terminal_branch},
        {"untracked-write", test_untracked_write},
        {"blacklist", test_blacklist},
        {"budget", test_budget},
        {"gates", test_gates},
        {"banked-code", test_banked_code},
        {"cache-index", test_cache_index},
        {"cache-capacity", test_cache_capacity}
    };
    size_t index;
    unsigned int selected = 0;
    unsigned int failures = 0;
    for (index = 0; index < sizeof(tests) / sizeof(tests[0]); ++index)
    {
        bool passed;
        if (argc > 1 && strcmp(argv[1], tests[index].name))
            continue;
        ++selected;
        passed = tests[index].run();
        printf("%s %s\n", passed ? "PASS" : "FAIL", tests[index].name);
        failures += !passed;
    }
    printf("%u tests, %u checks, %u failures\n", selected, checks, failures);
    return !selected || failures ? 1 : 0;
}