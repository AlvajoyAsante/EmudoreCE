#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "asic.h"
#include "cpu.h"
#include "emu.h"
#include "mem.h"
#include "schedule.h"
#include "vrEmu6502.h"
#include "cemu_throttle.h"
#ifdef GAME_JIT_WATCH
#include "debug/debug.h"
#endif
#include "../../src/jit_emit.h"

#define RAM_BASE 0xd00000UL
#define RETURN_ADDRESS 0xd00100UL
#define STACK_ADDRESS 0xd05000UL

typedef struct {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version, entry, program_offset, section_offset, flags;
    uint16_t header_size, program_size, program_count, section_size, section_count, strings;
} ElfHeader;

typedef struct {
    uint32_t name, type, flags, address, offset, size, link, info, alignment, entry_size;
} ElfSection;

typedef struct {
    uint32_t name, value, size;
    uint8_t info, other;
    uint16_t section;
} ElfSymbol;

_Static_assert(sizeof(ElfHeader) == 52, "ELF32 header layout");
_Static_assert(sizeof(ElfSection) == 40, "ELF32 section layout");
_Static_assert(sizeof(ElfSymbol) == 16, "ELF32 symbol layout");

static uint8_t *image;
static size_t image_size;
static ElfHeader header;
static unsigned int target_calls;
static unsigned int integration_cases;
static VrEmu6502 *reference;
static uint8_t guest_ram[65536];
static uint32_t state_address, low_address, high_address;
static const uint8_t *expected_native;
static uint16_t expected_native_size;
static uint64_t last_call_cycles;
static uint32_t last_native_address;
static bool inject_native_fault;
unsigned int target_debug_reason, target_debug_sequence;
uint32_t target_debug_data, target_debug_pc, target_debug_sp, target_debug_ix;
uint32_t target_debug_hl, target_debug_de, target_debug_bc, target_debug_iy;
uint32_t target_debug_page_entry, target_debug_page_value;
uint8_t target_debug_a;

static uint8_t reference_read(uint16_t address, bool debug)
{
    (void)debug;
    return guest_ram[address];
}

static void reference_write(uint16_t address, uint8_t value)
{
    guest_ram[address] = value;
}

void gui_console_clear(void) {}
void gui_console_printf(const char *format, ...) { (void)format; }
void gui_console_err_printf(const char *format, ...) { (void)format; }
void gui_debug_close(void) {}
void gui_debug_open(int reason, uint32_t data)
{
    target_debug_reason = (unsigned int)reason;
    ++target_debug_sequence;
    target_debug_data = data;
    target_debug_pc = cpu.registers.PC;
    target_debug_sp = cpu.registers.SPL;
    target_debug_ix = cpu.registers.IX;
    target_debug_iy = cpu.registers.IY;
    target_debug_hl = cpu.registers.HL;
    target_debug_de = cpu.registers.DE;
    target_debug_bc = cpu.registers.BC;
#ifdef GAME_JIT_WATCH
    target_debug_a = debug_last_write_value;
#else
    target_debug_a = cpu.registers.A;
#endif
    target_debug_page_value = target_debug_page_entry ? mem_peek_long(target_debug_page_entry) : 0;
}
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

static void fail(const char *message)
{
    fprintf(stderr, "FAIL target image: %s\n", message);
    exit(1);
}

static const uint8_t *image_bytes(uint32_t offset, uint32_t size)
{
    if (offset > image_size || size > image_size - offset)
        fail("ELF range outside file");
    return image + offset;
}

static ElfSection section_at(uint16_t index)
{
    ElfSection section;
    if (index >= header.section_count)
        fail("invalid ELF section index");
    memcpy(&section, image_bytes(header.section_offset + (uint32_t)index * sizeof(section),
                                 sizeof(section)), sizeof(section));
    return section;
}

static uint32_t find_symbol_address(const char *name)
{
    uint16_t index;
    for (index = 0; index < header.section_count; ++index)
    {
        ElfSection section = section_at(index);
        if (section.type == 2)
        {
            ElfSection strings = section_at((uint16_t)section.link);
            const char *names = (const char *)image_bytes(strings.offset, strings.size);
            uint32_t offset;
            for (offset = 0; offset + sizeof(ElfSymbol) <= section.size; offset += sizeof(ElfSymbol))
            {
                ElfSymbol symbol;
                memcpy(&symbol, image_bytes(section.offset + offset, sizeof(symbol)), sizeof(symbol));
                if (symbol.name < strings.size &&
                    memchr(names + symbol.name, 0, strings.size - symbol.name) &&
                    !strcmp(names + symbol.name, name))
                    return symbol.value;
            }
        }
    }
    return 0;
}

static uint32_t symbol_address(const char *name)
{
    uint32_t address = find_symbol_address(name);
    if (!address)
    {
        fprintf(stderr, "missing symbol: %s\n", name);
        fail("required symbol not linked");
    }
    return address;
}

static void load_image(const char *path)
{
    FILE *file = fopen(path, "rb");
    long length;
    uint16_t index;
    if (!file || fseek(file, 0, SEEK_END) || (length = ftell(file)) < (long)sizeof(header))
        fail("cannot read linked ELF");
    rewind(file);
    image_size = (size_t)length;
    image = malloc(image_size);
    if (!image || fread(image, 1, image_size, file) != image_size)
        fail("cannot load linked ELF");
    fclose(file);
    memcpy(&header, image, sizeof(header));
    if (memcmp(header.ident, "\177ELF\1\1", 6) || header.type != 2 ||
        header.section_size != sizeof(ElfSection))
        fail("expected little-endian ELF32 executable");
    for (index = 0; index < header.section_count; ++index)
    {
        ElfSection section = section_at(index);
        if (!(section.flags & 2) || !section.size)
            continue;
        if (section.address < 0xd05000 || section.address > 0xd40000 ||
            section.size > 0xd40000 - section.address)
            fail("allocated ELF section outside application RAM");
        if (section.type == 8)
            memset(mem.ram.block + section.address - RAM_BASE, 0, section.size);
        else
            memcpy(mem.ram.block + section.address - RAM_BASE,
                   image_bytes(section.offset, section.size), section.size);
    }
}

static uint32_t read_little(const uint8_t *bytes, unsigned int count)
{
    uint32_t value = 0;
    unsigned int index;
    for (index = 0; index < count; ++index)
        value |= (uint32_t)bytes[index] << (index * 8);
    return value;
}

static uint8_t *read_ti_file(const char *path, size_t *length)
{
    FILE *file = fopen(path, "rb");
    long size;
    uint8_t *bytes;
    size_t index;
    uint16_t checksum = 0;
    if (!file || fseek(file, 0, SEEK_END) || (size = ftell(file)) < 57)
        fail("cannot read TI variable fixture");
    rewind(file);
    bytes = malloc((size_t)size);
    if (!bytes || fread(bytes, 1, (size_t)size, file) != (size_t)size)
        fail("cannot load TI variable fixture");
    fclose(file);
    *length = (size_t)size;
    if (memcmp(bytes, "**TI83F*", 8) || read_little(bytes + 53, 2) + 57 != *length)
        fail("invalid TI file header or data length");
    for (index = 55; index < *length - 2; ++index)
        checksum = (uint16_t)(checksum + bytes[index]);
    if (checksum != read_little(bytes + *length - 2, 2))
        fail("invalid TI file checksum");
    return bytes;
}

static uint32_t library_ram_bytes(const char *path)
{
    static const char required[][9] = {"FILEIOC", "GRAPHX", "KEYPADC"};
    uint8_t found = 0;
    size_t length, offset = 55;
    uint8_t *bytes = read_ti_file(path, &length);
    uint32_t total = 0;
    while (offset < length - 2)
    {
        uint32_t variable_size;
        unsigned int library;
        const uint8_t *entry = bytes + offset;
        if (length - 2 - offset < 19 || read_little(entry, 2) != 13)
            fail("invalid library variable header");
        variable_size = read_little(entry + 2, 2);
        if (variable_size < 2 || variable_size > length - 2 - offset - 17 ||
            read_little(entry + 15, 2) != variable_size ||
            read_little(entry + 17, 2) + 2 != variable_size)
            fail("invalid library variable length");
        for (library = 0; library < sizeof(required) / sizeof(required[0]); ++library)
            if (entry[4] == 0x15 && !memcmp(entry + 5, required[library], 8))
            {
                const uint8_t *payload = entry + 19;
                uint32_t payload_size = variable_size - 2;
                uint32_t code_offset, code_size, relocations;
                if (found & (1u << library) || payload_size < 9 || payload[0] != 0xc0 || payload[1] != 0xc1)
                    fail("invalid or duplicate required CE library");
                found |= (uint8_t)(1u << library);
                code_offset = read_little(payload + payload_size - 6, 3);
                code_size = read_little(payload + payload_size - 3, 3);
                if (code_offset > payload_size - 6 || code_size > payload_size - 6 - code_offset)
                    fail("invalid CE library relocation bounds");
                if (payload[code_offset] == 0xc0 || payload[code_offset] == 0xc1)
                    fail("CE library gained dependencies; extend startup budget test");
                relocations = payload_size - 6 - code_offset - code_size;
                if (relocations) total += code_size;
                printf("STARTUP %s RAM=%lu bytes%s\n", required[library],
                       (unsigned long)(relocations ? code_size : 0), relocations ? "" : " (runs from archive)");
            }
        offset += 17 + variable_size;
    }
    free(bytes);
    if (found != 7 || offset != length - 2)
        fail("standard CE library fixture is incomplete");
    return total;
}

static void startup_return(void)
{
    uint32_t address = mem_peek_long(cpu.registers.SPL);
    cpu.registers.SPL += 3;
    cpu.halted = false;
    cpu_flush(address, true);
}

static void test_compressed_startup(const char *program_path, const char *libraries_path)
{
    const uint32_t load = symbol_address("LOAD_ADDR");
    const uint32_t limit = 0xd40000;
    const uint32_t enough = 0x02072c, push_real = 0x020614, insert = 0x020514, pop_real = 0x0205dc;
    size_t length;
    uint8_t *bytes = read_ti_file(program_path, &length);
    uint32_t payload_size = read_little(bytes + 72, 2);
    uint32_t libraries = library_ram_bytes(libraries_path);
    uint32_t end, peak, request = 0, delete_call = 0;
    uint32_t entry_offset = header.entry - load;
    unsigned int steps, insertions = 0, pushes = 0;
    uint16_t section_index;
    bool completed = false;
    if (length < 76 || read_little(bytes + 55, 2) != 13 || bytes[59] != 6 || bytes[69] != 0x80 ||
        read_little(bytes + 57, 2) != payload_size + 2 || read_little(bytes + 70, 2) != payload_size + 2 ||
        payload_size + 76 != length || entry_offset > payload_size || payload_size - entry_offset < 80 ||
        load != 0xd1a87f || payload_size > limit - load)
        fail("expected archived single-variable compressed EMUDORE program");
    if (bytes[74 + entry_offset] != 0x21 || bytes[74 + entry_offset + 18] != 0x21 ||
        read_little(bytes + 74 + entry_offset + 24, 3) != enough ||
        read_little(bytes + 74 + entry_offset + 38, 3) != insert)
        fail("compressed loader layout changed; update startup test");
    if (bytes[74 + entry_offset + 57] == 0xcd)
        delete_call = read_little(bytes + 74 + entry_offset + 58, 3);
    memset(mem.ram.block, 0xa5, SIZE_RAM);
    memcpy(mem.ram.block + load - RAM_BASE, bytes + 74, payload_size);
    end = peak = load + payload_size;
    mem_poke_long(0xd0118c, payload_size - 2);
    mem.flash.block[enough] = mem.flash.block[push_real] = mem.flash.block[insert] = 0x76;
    mem.flash.block[pop_real] = 0xc9;
    if (delete_call) mem.flash.block[delete_call] = 0x76;
    mem_poke_long(STACK_ADDRESS, RETURN_ADDRESS);
    cpu.registers.SPL = STACK_ADDRESS;
    cpu.ADL = true;
    cpu.halted = false;
    cpu.IEF1 = cpu.IEF2 = cpu.IEF_wait = cpu.NMI = false;
    cpu_flush(header.entry, true);
    cpu_clear_signals();
    for (steps = 0; steps < 12; ++steps)
    {
        uint32_t stopped;
        cemu_throttle_run_until_halt(1000);
        if (!cpu.halted || cpu_check_signals() != CPU_SIGNAL_NONE)
            fail("compressed startup escaped its bounded OS-call fixture");
        stopped = cpu.registers.PC - 1;
        if (stopped == enough)
        {
            request = cpu.registers.HL;
            if (request > limit - end) fail("decompressor requested more RAM than an empty calculator can provide");
        }
        else if (stopped == insert)
        {
            uint32_t address = cpu.registers.DE;
            uint32_t count = cpu.registers.HL;
            if (++insertions != 1 || address != header.entry || count != request || count > limit - end)
                fail("unexpected compressed startup allocation");
            memmove(mem.ram.block + address + count - RAM_BASE,
                    mem.ram.block + address - RAM_BASE, end - address);
            end += count;
            peak = end;
        }
        else if (delete_call && stopped == delete_call)
        {
            uint32_t address = cpu.registers.HL, count = cpu.registers.DE;
            if (address < load || address > end || count > end - address)
                fail("invalid decompressor temporary-memory release");
            memmove(mem.ram.block + address - RAM_BASE, mem.ram.block + address + count - RAM_BASE,
                    end - address - count);
            end -= count;
        }
        else if (stopped == push_real)
        {
            if (++pushes == 2)
            {
                completed = true;
                break;
            }
        }
        else
            fail("unexpected OS call during compressed startup");
        startup_return();
    }
    if (!completed || insertions != 1 || mem_peek_long(0xd0118c) != end - load - 2)
        fail("compressed startup did not reach the C runtime");
    for (section_index = 0; section_index < header.section_count; ++section_index)
    {
        ElfSection section = section_at(section_index);
        if ((section.flags & 2) && section.type != 8 && section.size && section.address >= load &&
            memcmp(mem.ram.block + section.address - RAM_BASE, image_bytes(section.offset, section.size), section.size))
            fail("decompressed calculator program differs from linked ELF");
    }
    printf("STARTUP image=%lu loader_peak=%lu libraries=%lu total=%lu available=%lu spare=%ld bytes\n",
           (unsigned long)(symbol_address("___data_high") - load), (unsigned long)(peak - load),
           (unsigned long)libraries, (unsigned long)(end - load + libraries), (unsigned long)(limit - load),
           (long)(limit - end) - (long)libraries);
    if (libraries > limit - end || limit - end - libraries < 8192)
        fail("program plus CE libraries leaves less than 8 KiB startup headroom");
    puts("PASS shipped ZX7 decompressor, exact ELF byte parity and CE-library startup memory budget");
    mem.flash.block[enough] = mem.flash.block[push_real] = mem.flash.block[insert] = 0xff;
    mem.flash.block[pop_real] = 0xff;
    if (delete_call) mem.flash.block[delete_call] = 0xff;
    memset(mem.ram.block, 0, SIZE_RAM);
    for (section_index = 0; section_index < header.section_count; ++section_index)
    {
        ElfSection section = section_at(section_index);
        if ((section.flags & 2) && section.type != 8 && section.size)
            memcpy(mem.ram.block + section.address - RAM_BASE, image_bytes(section.offset, section.size), section.size);
    }
    free(bytes);
}

typedef struct {
    uint16_t address;
    uint8_t value;
    bool write;
    uint32_t cycles;
} DeviceAccess;

static bool trace_devices;
static DeviceAccess device_accesses[8];
static unsigned int device_access_count;

static void complete_device_calls(const uint32_t hooks[2], const uint8_t opcodes[2])
{
    while (cpu.halted && (cpu.registers.PC == hooks[0] + 1 || cpu.registers.PC == hooks[1] + 1))
    {
        unsigned int hook = cpu.registers.PC == hooks[1] + 1;
        uint32_t caller = mem_peek_long(cpu.registers.SPL);
        uint8_t saved = mem_peek_byte(caller);
        DeviceAccess *access;
        if (device_access_count == sizeof(device_accesses) / sizeof(device_accesses[0]))
            fail("unexpected extra device access");
        access = &device_accesses[device_access_count++];
        access->address = (uint16_t)mem_peek_long(cpu.registers.SPL + 3);
        access->value = mem_peek_byte(cpu.registers.SPL + 6);
        access->write = hook != 0;
        access->cycles = mem_peek_long(state_address + 13);
        mem_poke_byte(hooks[hook], opcodes[hook]);
        mem_poke_byte(caller, 0x76);
        cpu.halted = false;
        cpu_flush(hooks[hook], true);
        cemu_throttle_run_until_halt(1000);
        if (!cpu.halted || cpu.registers.PC != caller + 1)
            fail("real device callback failed to return");
        if (!hook) access->value = cpu.registers.A;
        mem_poke_byte(caller, saved);
        mem_poke_byte(hooks[hook], 0x76);
        cpu.halted = false;
        cpu_flush(caller, true);
        cemu_throttle_run_until_halt(1000);
    }
}

static uint8_t target_call(const char *name, uint32_t argument)
{
    static const char *cached_name;
    static uint32_t cached_address;
    uint32_t address;
    if (!cached_name || strcmp(cached_name, name))
    {
        cached_name = name;
        cached_address = symbol_address(name);
    }
    address = cached_address;
    bool startup = !strcmp(name, "_jit_backend_selftest");
    uint32_t probe = startup || expected_native ? symbol_address("_jit_execute") : 0;
    uint8_t saved_opcode = probe ? mem_peek_byte(probe) : 0;
    uint32_t device_hooks[2] = {0};
    uint8_t device_opcodes[2] = {0};
    uint64_t before;
    if (trace_devices)
    {
        unsigned int index;
        device_access_count = 0;
        device_hooks[0] = symbol_address("_mem_read_io");
        device_hooks[1] = symbol_address("_mem_write_io");
        for (index = 0; index < 2; ++index)
        {
            device_opcodes[index] = mem_peek_byte(device_hooks[index]);
            mem_poke_byte(device_hooks[index], 0x76);
        }
    }
    if (probe)
        mem_poke_byte(probe, 0x76);
    mem_poke_byte(RETURN_ADDRESS, 0x76);
    mem_poke_long(STACK_ADDRESS, RETURN_ADDRESS);
    mem_poke_long(STACK_ADDRESS + 3, argument);
    cpu.registers.SPL = STACK_ADDRESS;
    cpu.registers.IX = 0x123456;
    cpu.registers.IY = 0xabcdef;
    cpu.halted = false;
    cpu.IEF1 = cpu.IEF2 = cpu.IEF_wait = cpu.NMI = false;
    cpu.ADL = true;
    cpu_flush(address, true);
    cpu_clear_signals();
    before = sched_total_cycles() - cpu.haltCycles;
    cemu_throttle_run_until_halt(1000);
    if (probe && cpu.halted && cpu.registers.PC == probe + 1)
    {
        static const uint8_t program[] = {0xa9,0x7f,0x18,0x69,0x01,0xaa,0xca,0xd0,0xfd};
        uint32_t native = mem_peek_long(cpu.registers.SPL + 3);
        JitTarget target = {.state = mem_peek_long(native + 3) - 3};
        JitTranslation translation;
        uint8_t output[512];
        uint16_t index;
        const uint8_t *expected = expected_native;
        uint16_t size = expected_native_size;
        last_native_address = native;
        if (startup)
        {
            if (!jit_compile(output, sizeof(output), program, sizeof(program), 0x400, &target, &translation))
                fail("host rejected target self-test translation");
            expected = output;
            size = translation.native_size;
        }
        for (index = 0; index < size; ++index)
            if (expected[index] != mem_peek_byte(native + index))
            {
                fprintf(stderr, "emission byte %u at %06lx: target=%02x host=%02x state=%06lx\n",
                        index, (unsigned long)(native + index), mem_peek_byte(native + index),
                        expected[index], (unsigned long)target.state);
                fail("CEdev and host emitted different native bytes");
            }
        if (startup && !inject_native_fault)
            puts("PASS target/host machine-code byte parity before native entry");
        if (inject_native_fault)
            mem_poke_byte(native + 1, (uint8_t)(mem_peek_byte(native + 1) ^ 1));
        mem_poke_byte(probe, saved_opcode);
        cpu.halted = false;
        cpu_flush(probe, true);
        cemu_throttle_run_until_halt(1000);
    }
    else if (probe)
        fail("expected native entry was never reached");
    if (trace_devices)
    {
        unsigned int index;
        complete_device_calls(device_hooks, device_opcodes);
        for (index = 0; index < 2; ++index)
            mem_poke_byte(device_hooks[index], device_opcodes[index]);
    }
    last_call_cycles = sched_total_cycles() - cpu.haltCycles - before;
    ++target_calls;
    if (!cpu.halted || cpu.registers.PC != RETURN_ADDRESS + 1 ||
        cpu.registers.SPL != STACK_ADDRESS + 3 || cpu.registers.IX != 0x123456 ||
        cpu_check_signals() != CPU_SIGNAL_NONE)
    {
        fprintf(stderr, "%s: pc=%06x sp=%06x ix=%06x halted=%u signals=%u\n",
                name, cpu.registers.PC, cpu.registers.SPL, cpu.registers.IX,
                cpu.halted, cpu_check_signals());
        fail("target did not return with its C ABI intact within 48M clocks");
    }
    return cpu.registers.A;
}

static void test_missing_rom_startup(void)
{
    static const char *const names[] = {
        "_CEPackageFileExists", "_gfx_Begin", "_gfx_SetPalette", "_gfx_FillScreen",
        "_gfx_SetTextFGColor", "_gfx_GetStringWidth", "_gfx_PrintStringXY", "_kb_AnyKey", "_gfx_End"
    };
    uint32_t addresses[sizeof(names) / sizeof(names[0])];
    uint8_t saved[sizeof(names) / sizeof(names[0])];
    unsigned int index, step, lookups = 0, messages = 0;
    bool returned = false;
    for (index = 0; index < sizeof(names) / sizeof(names[0]); ++index)
    {
        addresses[index] = symbol_address(names[index]);
        saved[index] = mem_peek_byte(addresses[index]);
        mem_poke_byte(addresses[index], 0x76);
    }
    mem_poke_byte(RETURN_ADDRESS, 0x76);
    mem_poke_long(STACK_ADDRESS, RETURN_ADDRESS);
    cpu.registers.SPL = STACK_ADDRESS;
    cpu.registers.IX = 0x123456;
    cpu.registers.IY = 0xabcdef;
    cpu.halted = false;
    cpu.ADL = true;
    cpu.IEF1 = cpu.IEF2 = cpu.IEF_wait = cpu.NMI = false;
    cpu_flush(symbol_address("_main"), true);
    cpu_clear_signals();
    for (step = 0; step < 64; ++step)
    {
        cemu_throttle_run_until_halt(1000);
        if (!cpu.halted || cpu_check_signals() != CPU_SIGNAL_NONE)
            fail("no-ROM startup escaped its library fixture");
        if (cpu.registers.PC == RETURN_ADDRESS + 1)
        {
            returned = true;
            break;
        }
        for (index = 0; index < sizeof(names) / sizeof(names[0]); ++index)
            if (cpu.registers.PC == addresses[index] + 1)
                break;
        if (index == sizeof(names) / sizeof(names[0]))
            fail("unexpected call during no-ROM startup");
        if (index == 0)
        {
            ++lookups;
            cpu.registers.HL = 0;
        }
        else if (index == 5)
            cpu.registers.HL = 114;
        else if (index == 6)
        {
            static const char expected[] = "ROM AppVars missing!";
            uint32_t text = mem_peek_long(cpu.registers.SPL + 3);
            unsigned int character;
            for (character = 0; character < sizeof(expected); ++character)
                if (mem_peek_byte(text + character) != (uint8_t)expected[character])
                    fail("no-ROM startup displayed an unexpected message");
            ++messages;
        }
        else if (index == 7)
            cpu.registers.A = 1;
        startup_return();
    }
    if (!returned || lookups != 3 || messages != 1 || cpu.registers.HL != 1 ||
        cpu.registers.SPL != STACK_ADDRESS + 3 || cpu.registers.IX != 0x123456)
        fail("no-ROM startup did not return cleanly after its missing-ROM message");
    for (index = 0; index < sizeof(names) / sizeof(names[0]); ++index)
        mem_poke_byte(addresses[index], saved[index]);
    ++target_calls;
    puts("PASS compiled no-ROM startup displays ROM AppVars missing and returns (library calls stubbed)");
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

static void seed_guest_registers(const uint8_t *program, size_t length, uint16_t pc,
                                uint8_t flags, uint32_t cycles, uint8_t accumulator,
                                uint8_t index_x, uint8_t index_y, uint8_t stack)
{
    uint8_t setup[] = {0xa2,stack,0x9a,0xa9,flags,0x48,0xa9,accumulator,
                       0xa2,index_x,0xa0,index_y,0x28};
    uint8_t state[16];
    unsigned int instruction;
    memcpy(guest_ram, mem.ram.block + low_address - RAM_BASE, 0x1000);
    memcpy(guest_ram + 0x1000, mem.ram.block + high_address - RAM_BASE + 0x1000, 0xf000);
    for (size_t offset = 0; offset < length; ++offset)
        guest_ram[(uint16_t)(pc + offset)] = program[offset];
    memcpy(guest_ram + 0x200, setup, sizeof(setup));
    vrEmu6502SetPC(reference, 0x200);
    for (instruction = 0; instruction < 8; ++instruction)
        vrEmu6502InstCycle(reference);
    vrEmu6502SetPC(reference, pc);
    reference_state(state, cycles);
    memcpy(mem.ram.block + state_address - RAM_BASE, state, sizeof(state));
    memcpy(mem.ram.block + low_address - RAM_BASE, guest_ram, 0x1000);
    memcpy(mem.ram.block + high_address - RAM_BASE + 0x1000, guest_ram + 0x1000, 0xf000);
    target_call("_jit_invalidate_page", pc >> 8);
    if ((unsigned int)(pc & 0xff) + length > 0x100)
        target_call("_jit_invalidate_page", (pc >> 8) + 1);
}

static void seed_guest(const uint8_t *program, size_t length, uint16_t pc,
                       uint8_t flags, uint32_t cycles)
{
    seed_guest_registers(program, length, pc, flags, cycles, 0x81, 0x02, 0xff, 0xff);
}

static void compare_expected_guest(const uint8_t expected[16], const char *name, unsigned int budget)
{
    const uint8_t *actual = mem.ram.block + state_address - RAM_BASE;
    unsigned int index;
    ++integration_cases;
    if (memcmp(actual, expected, 16))
    {
        fprintf(stderr, "%s budget=%u expected:", name, budget);
        for (index = 0; index < 16; ++index) fprintf(stderr, " %02x", expected[index]);
        fprintf(stderr, "\nactual: ");
        for (index = 0; index < 16; ++index) fprintf(stderr, " %02x", actual[index]);
        fputc('\n', stderr);
        fail("production guest state diverged from independent 6502");
    }
    if (memcmp(mem.ram.block + low_address - RAM_BASE, guest_ram, 0x1000) ||
        memcmp(mem.ram.block + high_address - RAM_BASE + 0x1000, guest_ram + 0x1000, 0xf000))
    {
        for (index = 0; index < sizeof(guest_ram); ++index)
        {
            uint8_t value = mem_peek_byte((index < 0x1000 ? low_address : high_address) + index);
            if (value != guest_ram[index])
            {
                fprintf(stderr, "%s budget=%u RAM[%04x] expected=%02x actual=%02x\n",
                        name, budget, index, guest_ram[index], value);
                break;
            }
        }
        fail("production guest memory diverged from independent 6502");
    }
}

static void compare_guest(uint32_t cycles, const char *name, unsigned int budget)
{
    uint8_t expected[16];
    reference_state(expected, cycles);
    compare_expected_guest(expected, name, budget);
}

static void make_hot(uint16_t pc)
{
    unsigned int observation;
    for (observation = 0; observation < 8; ++observation)
        target_call("_jit_observe_pc", pc);
    if (!mem_peek_byte(symbol_address("_jit_ready_block_count")))
        fail("hot target block was not compiled");
}

static void test_batches(void)
{
    static const uint8_t programs[][24] = {
        {0xe8,0xca,0xd0,0xfd,0x4c,0x00,0x04},
        {0xa9,0x03,0xaa,0xca,0x48,0x68,0x85,0x40,0xad,0x40,0xc0,0xe8,0x4c,0x00,0x04},
        {0xa9,0x09,0x18,0x69,0x09,0x85,0x40,0xa5,0x40,0xe9,0x01,0x4c,0x00,0x04}
    };
    static const uint8_t lengths[] = {7,15,14};
    unsigned int program, budget, decimal;
    uint32_t hits = 0;
    for (program = 0; program < sizeof(lengths); ++program)
        for (decimal = 0; decimal < 2; ++decimal)
        {
            uint8_t flags = decimal ? FlagD : 0;
            target_call("_jit_reset", 0);
            seed_guest(programs[program], lengths[program], 0x400, flags, 0xfffff8);
            make_hot(0x400);
            for (budget = 0; budget <= 96; ++budget)
            {
                uint32_t reference_cycles = 0xfffff8;
                uint32_t until = (reference_cycles + budget) & 0xffffff;
                seed_guest(programs[program], lengths[program], 0x400, flags, reference_cycles);
                while (reference_cycles - 0xfffff8 < budget)
                    reference_cycles += vrEmu6502InstCycle(reference);
                if (!target_call("_cpu_run", until))
                    fail("production CPU batch rejected valid guest program");
                compare_guest(reference_cycles, "mixed batch", budget);
            }
            hits += mem_peek_long(symbol_address("_jit_native_hits"));
        }
    if (!hits)
        fail("mixed batch tests never executed native code");
    printf("PASS mixed target native/interpreter batches: %u cases, %lu native cache hits\n",
           integration_cases, (unsigned long)hits);
}

static void test_target_opcodes(bool assembly)
{
    unsigned int opcode, count = 0;
    vrEmu6502Destroy(reference);
    reference = vrEmu6502New(CPU_6502U, reference_read, reference_write);
    if (!reference) fail("undocumented opcode reference allocation");
    JitTarget target = {.state = state_address, .low_ram = low_address, .high_ram = high_address};
    target.read_byte = symbol_address("_jit_read_bridge");
    target.write_byte = symbol_address("_jit_write_bridge");
    target.decimal_adc = symbol_address("_cpu_decimal_adc");
    target.decimal_sbc = symbol_address("_cpu_decimal_sbc");
    target.brk = symbol_address("_cpu_brk_native");
    target_call("_mem_setup_banks", 0);
    for (opcode = 0; opcode < 256; ++opcode)
    {
        uint8_t program[48] = {0};
        uint8_t output[512];
        JitTranslation translation;
        unsigned int step;
        uint8_t initial_x;
        uint32_t reference_cycles = 0xfffffc;
        if (!jit_instruction_size((uint8_t)opcode))
            continue;
        program[0] = (uint8_t)opcode;
        program[1] = 0x40;
        program[2] = 0x04;
        if (jit_instruction_size((uint8_t)opcode) < 3) program[2] = 0;
        if (jit_instruction_size((uint8_t)opcode) < 2) program[1] = 0;
        target_call("_jit_reset", 0);
        mem_poke_byte(low_address + 0x40, 0xfd);
        mem_poke_byte(low_address + 0x41, 0x0f);
        mem_poke_byte(low_address + 0x42, 0xfd);
        mem_poke_byte(low_address + 0x43, 0x0f);
        seed_guest(program, sizeof(program), 0x400, (uint8_t)(opcode & ~FlagD), reference_cycles);
        initial_x = mem_peek_byte(state_address + 4);
        if (assembly)
        {
            reference_cycles += vrEmu6502InstCycle(reference);
            if (!target_call("_cpu_run_asm", 0xfffffd))
                fail("supported opcode failed assembly execution");
        }
        else
        {
            if (!jit_compile(output, sizeof(output), program, sizeof(program), 0x400, &target, &translation))
                fail("supported single opcode failed host compilation");
            make_hot(0x400);
            for (step = 0; step < translation.instructions; ++step)
                reference_cycles += vrEmu6502InstCycle(reference);
            expected_native = output;
            expected_native_size = translation.native_size;
            if (!target_call("_jit_run", 100))
                fail("supported single opcode failed native admission");
            expected_native = NULL;
        }
        if (opcode == 0x6b || opcode == 0xab || opcode == 0x8b)
        {
            uint8_t expected[16];
            reference_state(expected, reference_cycles);
            if (opcode == 0x6b)
            {
                expected[6] = (expected[3] & 0x40) != 0;
                expected[7] = expected[3] == 0;
                expected[11] = ((expected[3] >> 6) ^ (expected[3] >> 5)) & 1;
                expected[12] = (expected[3] & 0x80) != 0;
            }
            else if (opcode == 0x8b)
            {
                expected[3] = initial_x & program[1];
                expected[7] = expected[3] == 0;
                expected[12] = (expected[3] & 0x80) != 0;
            }
            else
                expected[4] = program[1];
            compare_expected_guest(expected, "target immediate undocumented opcode", opcode);
        }
        else
            compare_guest(reference_cycles, "target opcode", opcode);
        ++count;
    }
    if (count != 244)
        fail("native target coverage count changed");
    target_call("_mem_setup_banks", 7);
    vrEmu6502Destroy(reference);
    reference = vrEmu6502New(CPU_6502, reference_read, reference_write);
    if (!reference) fail("reference reset allocation");
        printf("PASS 151 official and 93 undocumented opcode encodings through %s and independent execution\n",
            assembly ? "assembly interpreter" : "CEdev emitter/cache");
}

static void test_memory_guards(void)
{
    static const uint8_t opcodes[] = {0xbd,0x9d,0x1e};
    static const uint16_t bases[] = {0x0000,0x0ffe,0x9ffe,0xaffe,0xbffe,0xcffe,0xfffe,0xffff};
    JitTarget target = {.state = state_address, .low_ram = low_address, .high_ram = high_address};
    unsigned int operation, base, prefix;
    target.read_byte = symbol_address("_jit_read_bridge");
    target.write_byte = symbol_address("_jit_write_bridge");
    for (operation = 0; operation < sizeof(opcodes); ++operation)
        for (base = 0; base < sizeof(bases) / sizeof(bases[0]); ++base)
            for (prefix = 0; prefix < 2; ++prefix)
            {
                uint8_t program[48] = {0};
                uint8_t output[512];
                JitTranslation translation;
                uint16_t effective = (uint16_t)(bases[base] + 2);
                uint8_t progress;
                unsigned int steps, step;
                uint32_t cycles = 0xfffffc;
                program[0] = 0xea;
                program[prefix] = opcodes[operation];
                program[prefix + 1] = (uint8_t)bases[base];
                program[prefix + 2] = (uint8_t)(bases[base] >> 8);
                target_call("_mem_setup_banks", 0);
                target_call("_jit_reset", 0);
                seed_guest(program, sizeof(program), 0x400, FlagC, cycles);
                if (!jit_compile(output, sizeof(output), program, sizeof(program), 0x400, &target, &translation))
                    fail("guarded opcode did not compile");
                make_hot(0x400);
                steps = translation.instructions;
                for (step = 0; step < steps; ++step)
                    cycles += vrEmu6502InstCycle(reference);
                expected_native = output;
                expected_native_size = translation.native_size;
                progress = target_call("_jit_run", 100) != 0;
                if (progress != (steps != 0))
                {
                    fprintf(stderr, "guard opcode=%02x effective=%04x prefix=%u progress=%u expected=%u\n",
                            opcodes[operation], effective, prefix, progress, steps != 0);
                    fail("guard exit returned incorrect progress");
                }
                expected_native = NULL;
                compare_guest(cycles, "memory guard", effective);
                if (mem_peek_long(symbol_address("_jit_native_hits")) != (steps != 0))
                    fail("guard exit recorded an incorrect native hit");
            }
    target_call("_mem_setup_banks", 7);
    puts("PASS target mapped memory helpers, CPU-port writes, address and cycle wrap");
}

static void test_mapped_devices(void)
{
    static const uint8_t reads[] = {0xea,0xad,0x1b,0xd4,0xaa,0xad,0x1b,0xd4,0x02};
    static const uint8_t modify[] = {0xea,0x0f,0x20,0xd0,0x02};
    uint8_t *state = mem.ram.block + state_address - RAM_BASE;
    uint8_t expected[16], first, second;
    target_call("_mem_setup_banks", 7);
    target_call("_jit_reset", 0);
    seed_guest(reads, sizeof(reads), 0x400, 0, 0xfffffc);
    memcpy(expected, state, sizeof(expected));
    trace_devices = true;
    make_hot(0x400);
    if (device_access_count) fail("code discovery speculatively read a device");
    mem_poke_byte(symbol_address("_cpu_warp"), 1);
    if (!target_call("_jit_run", 100)) fail("mapped device reads were not admitted");
    trace_devices = false;
    if (device_access_count != 2 || device_accesses[0].write || device_accesses[1].write ||
        device_accesses[0].address != 0xd41b || device_accesses[1].address != 0xd41b ||
        device_accesses[0].cycles != 0xfffffe || device_accesses[1].cycles != 4)
        fail("SID read count, address, or instruction-prefix timing differs");
    first = device_accesses[0].value;
    second = device_accesses[1].value;
    if (second != (uint8_t)(first * 13 + 7)) fail("SID RNG read side effect repeated or skipped");
    expected[0] = 8;
    expected[3] = second;
    expected[4] = first;
    expected[7] = second == 0;
    expected[12] = (second & 0x80) != 0;
    expected[13] = 8;
    expected[14] = expected[15] = 0;
    if (memcmp(state, expected, sizeof(expected)) || mem_peek_byte(symbol_address("_cpu_warp")))
        fail("mapped read changed flags, cycle accounting, or left an interpreter wait hint");
    ++integration_cases;

    mem_poke_long(STACK_ADDRESS + 6, 3);
    target_call("_mem_write_io", 0xd020);
    target_call("_jit_reset", 0);
    seed_guest(modify, sizeof(modify), 0x400, 0, 0xfffffc);
    make_hot(0x400);
    trace_devices = true;
    if (!target_call("_jit_run", 100)) fail("mapped RMW was not admitted");
    trace_devices = false;
    if (device_access_count != 3 || device_accesses[0].write ||
        !device_accesses[1].write || !device_accesses[2].write ||
        device_accesses[0].address != 0xd020 || device_accesses[1].address != 0xd020 ||
        device_accesses[2].address != 0xd020 ||
        device_accesses[1].value != device_accesses[0].value ||
        device_accesses[2].value != (uint8_t)(device_accesses[0].value << 1) ||
        device_accesses[0].cycles != 0xfffffe || device_accesses[1].cycles != 0xfffffe ||
        device_accesses[2].cycles != 0xfffffe || mem_peek_long(state_address + 13) != 4)
        fail("mapped RMW old/final writes, ordering, or prefix timing differs");
    first = device_accesses[2].value;
    if ((target_call("_mem_read_io", 0xd020) & 15) != (first & 15))
        fail("RMW did not change the real VIC border register");
    ++integration_cases;
    puts("PASS real SID/VIC device side effects, RMW double writes, prefix cycles, and wait-hint isolation");
}

static void test_mapped_rom(void)
{
    static const uint16_t starts[] = {0x0002,0x0100,0xa000,0xd000,0xe000,0xfffd};
    static const uint8_t program[] = {0xea,0xd0,0xfd};
    static const uint8_t data_access[] = {0xad,0x00,0xa1,0x8d,0x00,0xa1,0x02};
    uint32_t pages = symbol_address("_mem_rd_pg");
    uint8_t *state = mem.ram.block + state_address - RAM_BASE;
    unsigned int index;
    for (index = 0; index < sizeof(starts) / sizeof(starts[0]); ++index)
    {
        uint16_t pc = starts[index];
        uint32_t visible, backing;
        uint8_t initial[16], saved[sizeof(program)];
        target_call("_mem_setup_banks", 3);
        target_call("_jit_reset", 0);
        seed_guest(program, 0, pc, 0, 0);
        visible = (mem_peek_long(pages + (pc >> 8) * 4) + pc) & 0xffffff;
        backing = (pc < 0x1000 ? low_address : high_address) + pc;
        memcpy(saved, mem.ram.block + visible - RAM_BASE, sizeof(saved));
        memcpy(mem.ram.block + visible - RAM_BASE, program, sizeof(program));
        memcpy(initial, state, sizeof(initial));
        make_hot(pc);
        if (visible != backing) mem_poke_byte(backing + 1, 0);
        if (!target_call("_jit_run", 10) || (mem_peek_long(state_address) & 0xffff) != pc ||
            mem_peek_long(state_address + 13) != (pc == 0xfffd ? 6 : 5))
            fail("CPU-visible ROM/low/stack/wrapped code failed execution");
        memcpy(state, initial, sizeof(initial));
        if (visible != backing)
        {
            target_call("_mem_setup_banks", 0);
            if (target_call("_jit_run", 10) || memcmp(state, initial, sizeof(initial)))
                fail("bank switch to different instruction bytes admitted stale code");
            target_call("_mem_setup_banks", 3);
            make_hot(pc);
        }
        mem_poke_byte(visible + 1, 0xf0);
        if (target_call("_jit_run", 10) || memcmp(state, initial, sizeof(initial)))
            fail("visible banked code mutation was not rejected before execution");
        memcpy(mem.ram.block + visible - RAM_BASE, saved, sizeof(saved));
        ++integration_cases;
    }
    target_call("_mem_setup_banks", 7);
    target_call("_jit_reset", 0);
    seed_guest(data_access, sizeof(data_access), 0x400, 0, 0);
    {
        uint32_t visible = (mem_peek_long(pages + 0xa1 * 4) + 0xa100) & 0xffffff;
        uint8_t saved = mem_peek_byte(visible);
        mem_poke_byte(visible, 0x7d);
        mem_poke_byte(high_address + 0xa100, 0x31);
        make_hot(0x400);
        if (!target_call("_jit_run", 8) || state[3] != 0x7d ||
            mem_peek_byte(high_address + 0xa100) != 0x7d || mem_peek_byte(visible) != 0x7d ||
            mem_peek_long(state_address + 13) != 8)
            fail("ROM read or write to underlying RAM used the wrong mapping");
        mem_poke_byte(visible, saved);
        ++integration_cases;
    }
    puts("PASS target banked ROM code, stack code, FFFF wrap, visible-byte invalidation, and RAM beneath ROM");
}

static void test_code_windows(void)
{
    static const uint16_t starts[] = {0x300,0xffb,0xffd,0xfff,0x1000,0x9ffb,0x9ffd,0x9fff,0xc000,0xcffb,0xcffd,0xcfff};
    static const uint8_t program[48] = {0xe8,0xca,0x8a,0xaa,0xea};
    JitTarget target = {.state = state_address, .low_ram = low_address, .high_ram = high_address};
    unsigned int start;
    for (start = 0; start < sizeof(starts) / sizeof(starts[0]); ++start)
    {
        uint16_t pc = starts[start];
        uint16_t available = (uint16_t)((pc < 0xa000 ? 0xa000 : 0xd000) - pc);
        uint8_t output[512];
        JitTranslation translation;
        unsigned int step, offset;
        uint32_t cycles = 0;
        if (available > sizeof(program)) available = sizeof(program);
        target_call("_jit_reset", 0);
        seed_guest(program, sizeof(program), pc, 0, 0);
        if (!jit_compile(output, sizeof(output), program, (uint8_t)available, pc, &target, &translation))
            fail("ordinary RAM code window did not compile");
        make_hot(pc);
        for (step = 0; step < translation.instructions; ++step)
            cycles += vrEmu6502InstCycle(reference);
        expected_native = output;
        expected_native_size = translation.native_size;
        if (!target_call("_jit_run", 100)) fail("ordinary RAM code window rejected native execution");
        expected_native = NULL;
        compare_guest(cycles, "code window", pc);
        for (offset = 0; offset < translation.guest_size; ++offset)
        {
            uint16_t changed = (uint16_t)(pc + offset);
            uint32_t host = (changed < 0x1000 ? low_address : high_address) + changed;
            target_call("_jit_reset", 0);
            seed_guest(program, sizeof(program), pc, 0, 0);
            make_hot(pc);
            guest_ram[changed] ^= 0x80;
            mem_poke_byte(host, guest_ram[changed]);
            target_call("_jit_invalidate_page", changed >> 8);
            if (target_call("_jit_run", 100)) fail("stale code crossed a RAM backing boundary");
            compare_guest(0, "code window mutation", changed);
            if (mem_peek_byte(symbol_address("_jit_ready_block_count")))
                fail("stale code window remained ready");
        }
    }
    puts("PASS target low/split/upper/C000 code windows and every cached-byte mutation");
}

static void test_failure_fallback(void)
{
    static const uint8_t program[48] = {0xe8,0xca,0xd0,0xfc};
    uint8_t output[512];
    JitTarget target = {.state = state_address, .low_ram = low_address, .high_ram = high_address};
    JitTranslation translation;
    unsigned int step;
    uint32_t cycles = 0;
    target_call("_jit_reset", 0);
    seed_guest(program, sizeof(program), 0x400, 0, 0);
    make_hot(0x400);
    target_call("_mem_setup_banks", 7);
    if (!target_call("_jit_run", 100)) fail("unchanged visible code was discarded on a bank generation change");
    for (step = 0; step < 3; ++step) cycles += vrEmu6502InstCycle(reference);
    compare_guest(cycles, "unchanged mapping bytes", 100);
    cycles = 0;
    seed_guest(program, sizeof(program), 0x400, 0, 0);
    make_hot(0x400);
    if (!jit_compile(output, sizeof(output), program, sizeof(program), 0x400, &target, &translation))
        fail("guard test could not compile its fixture");
    expected_native = output;
    expected_native_size = translation.native_size;
    if (!target_call("_jit_run", 100)) fail("guard test never entered native code");
    expected_native = NULL;
    mem_poke_byte(last_native_address + sizeof(output), 0);
    seed_guest(program, sizeof(program), 0x400, 0, 0);
    if (target_call("_jit_run", 100) || mem_peek_byte(symbol_address("_jit_ready_block_count")) ||
        mem_peek_long(symbol_address("_jit_native_mismatches")) != 1)
        fail("damaged native guard was not blacklisted");
    for (step = 0; step < 8; ++step) target_call("_jit_observe_pc", 0x400);
    if (target_call("_jit_run", 100)) fail("blacklisted native block was promoted again");
    compare_guest(0, "native guard", 100);
    inject_native_fault = true;
    if (target_call("_jit_backend_selftest", 0)) fail("startup accepted incorrect emitted arithmetic");
    inject_native_fault = false;
    for (step = 0; step < 8; ++step) target_call("_jit_observe_pc", 0x400);
    if (target_call("_jit_run", 100) || mem_peek_byte(symbol_address("_jit_cached_block_count")) ||
        mem_peek_long(symbol_address("_jit_native_mismatches")) != 1)
        fail("failed startup did not disable the backend");
    seed_guest(program, sizeof(program), 0x400, 0, 0);
    while (cycles < 63) cycles += vrEmu6502InstCycle(reference);
    if (!target_call("_cpu_run", 63)) fail("disabled JIT prevented interpreter execution");
    compare_guest(cycles, "startup fallback", 63);
    if (mem_peek_long(symbol_address("_jit_native_hits"))) fail("disabled JIT still executed native code");
    if (!target_call("_jit_backend_selftest", 0)) fail("backend failed to recover after a clean self-test");
    puts("PASS target mapping invalidation, cache corruption blacklist, startup fault rejection and interpreter fallback");
}

static void test_native_self_modification(void)
{
    static const uint8_t programs[][8] = {
        {0xa9,0xea,0x8d,0x00,0x04,0xd0,0xf9,0x02},
        {0xa9,0xea,0x9d,0xfe,0x03,0xd0,0xf9,0x02}
    };
    unsigned int program, budget;
    JitTarget target = {.state = state_address, .low_ram = low_address, .high_ram = high_address};
    target.read_byte = symbol_address("_jit_read_bridge");
    target.write_byte = symbol_address("_jit_write_bridge");
    for (program = 0; program < sizeof(programs) / sizeof(programs[0]); ++program)
    {
        uint8_t output[512];
        JitTranslation translation;
        if (!jit_compile(output, sizeof(output), programs[program], sizeof(programs[program]),
                         0x400, &target, &translation) || translation.can_repeat ||
            translation.guest_size != 5)
            fail("self-modifying direct/indexed store was allowed to continue or repeat");
        for (budget = 1; budget <= 64; ++budget)
        {
            uint32_t cycles = 0;
            target_call("_jit_reset", 0);
            seed_guest(programs[program], sizeof(programs[program]), 0x400, 0, 0);
            make_hot(0x400);
            make_hot(0x405);
            while (cycles < budget) cycles += vrEmu6502InstCycle(reference);
            if (!target_call("_cpu_run", budget)) fail("self-modifying batch stopped unexpectedly");
            compare_guest(cycles, "self-modifying linked batch", budget);
        }
    }
    puts("PASS direct/indexed self-modification, linked branch re-entry, and every batch budget 1..64");
}

static void test_assembly_jit_reentry(void)
{
    static const uint8_t program[] = {
        0xa2,0xff,0x4c,0xa5,0x01,0xea,0xea,0xea,0xea,0xea,0xea,0xea,0xea,
        0x88,0xca,0xd0,0xef
    };
    uint32_t cycles = 0;
    unsigned int budget;
    for (budget = 1; budget <= 64; ++budget)
    {
        target_call("_jit_reset", 0);
        seed_guest_registers(program, sizeof(program), 0x0198, 0, 0, 0, 4, 4, 0xff);
        make_hot(0x01a5);
        mem_poke_byte(symbol_address("_cpu_jit_reentry"), 1);
        while (cycles < budget)
            cycles += vrEmu6502InstCycle(reference);
        if (!target_call("_cpu_run_asm", budget))
            fail("assembly JIT re-entry rejected a backward branch");
        mem_poke_byte(symbol_address("_cpu_jit_reentry"), 0);
        compare_guest(cycles, "assembly JIT re-entry", budget);
        cycles = 0;
    }
    puts("PASS assembly JIT re-entry across backward branch and every batch budget 1..64");
}

static void test_high_page_generation_index(void)
{
    static const uint8_t cached_program[] = {0x88,0xca,0xd0,0xfd};
    static const uint8_t write_program[] = {0xa9,0xc2,0x8d,0x36,0xaf,0x02};
    JitTarget target = {
        .state = state_address,
        .low_ram = low_address,
        .high_ram = high_address,
        .read_byte = symbol_address("_jit_read_bridge"),
        .write_byte = symbol_address("_jit_write_bridge"),
        .decimal_adc = symbol_address("_cpu_decimal_adc"),
        .decimal_sbc = symbol_address("_cpu_decimal_sbc"),
        .brk = symbol_address("_cpu_brk_native")
    };
    uint8_t expected[512];
    JitTranslation translation;
    uint32_t native = symbol_address("_jit_blocks") + 72 + 1;
    uint32_t generations = symbol_address("_jit_page_generation");
    uint16_t offset;

    target_call("_mem_setup_banks", 7);
    target_call("_jit_reset", 0);
    seed_guest(cached_program, sizeof(cached_program), 0x01a5, 0, 0);
    if (!jit_compile(expected, sizeof(expected), cached_program, sizeof(cached_program),
                     0x01a5, &target, &translation))
        fail("high-page generation fixture did not compile");
    make_hot(0x01a5);
    for (offset = 0; offset < translation.native_size; ++offset)
        if (mem_peek_byte(native + offset) != expected[offset])
            fail("target high-page generation fixture differs before its store");
    seed_guest(write_program, sizeof(write_program), 0x0400, 0, 0);
    mem_poke_byte(symbol_address("_cpu_jit_reentry"), 0);
    if (!target_call("_cpu_run_asm", 6))
        fail("assembly high-page store did not complete");
    if (mem_peek_byte(high_address + 0xaf36) != 0xc2 || mem_peek_byte(generations + 0xaf) != 1)
        fail("assembly high-page store did not update its RAM or page generation");
    for (offset = 0; offset < translation.native_size; ++offset)
        if (mem_peek_byte(native + offset) != expected[offset])
            fail("high-page generation update corrupted native JIT code");
    puts("PASS high-page assembly invalidation preserves JIT native cache");
}

static void test_indexed_copy_batches(void)
{
    static const uint16_t regions[][2] = {
        {0x2000, 0x3000}, {0x0f00, 0x2000}, {0x9f00, 0x0f00}, {0xcf00, 0xc000},
        {0x2000, 0x2001}, {0x2001, 0x2000}, {0x0f01, 0x2000}, {0x20ff, 0x20fd},
        {0x9f01, 0x2000}, {0xcf01, 0xc000}
    };
    static const uint8_t indices[] = {0, 1, 127, 255};
    unsigned int region, mode, initial, backend, budget, offset, count = 0;
    target_call("_mem_setup_banks", 0);
    for (region = 0; region < sizeof(regions) / sizeof(regions[0]); ++region)
        for (mode = 0; mode < 2; ++mode)
            for (initial = 0; initial < sizeof(indices); ++initial)
            {
                uint8_t program[] = {
                    mode ? 0xa0 : 0xa2, indices[initial],
                    mode ? 0xb9 : 0xbd, (uint8_t)regions[region][0], (uint8_t)(regions[region][0] >> 8),
                    mode ? 0x99 : 0x9d, (uint8_t)regions[region][1], (uint8_t)(regions[region][1] >> 8),
                    mode ? 0x88 : 0xca, 0xd0, 0xf7, 0x4c, 0x00, 0x04
                };
                target_call("_jit_reset", 0);
                seed_guest(program, sizeof(program), 0x400, 0, 0);
                for (offset = 0; offset < sizeof(program); offset += jit_instruction_size(program[offset]))
                    make_hot((uint16_t)(0x400 + offset));
                for (backend = 0; backend < 2; ++backend)
                    for (budget = 1; budget <= 64; ++budget)
                {
                    char scenario[96];
                    uint32_t start = budget & 1 ? 0xfffff0 : 0;
                    uint32_t elapsed = 0;
                    snprintf(scenario, sizeof(scenario), "indexed copy %04x->%04x %c=%u %s",
                             regions[region][0], regions[region][1], mode ? 'Y' : 'X',
                             indices[initial], backend ? "JIT" : "ASM");
                    for (offset = 0; offset < 256; ++offset)
                    {
                        uint16_t source = (uint16_t)(regions[region][0] + offset);
                        uint16_t destination = (uint16_t)(regions[region][1] + offset);
                        mem_poke_byte((source < 0x1000 ? low_address : high_address) + source,
                                      (uint8_t)(offset * 37 + initial));
                        mem_poke_byte((destination < 0x1000 ? low_address : high_address) + destination,
                                      (uint8_t)(offset * 19 - initial));
                    }
                    seed_guest(program, sizeof(program), 0x400, 0, start);
                    while (elapsed < budget)
                        elapsed += vrEmu6502InstCycle(reference);
                    if (!target_call(backend ? "_cpu_run" : "_cpu_run_asm", (start + budget) & 0xffffff))
                        fail("indexed copy batch stopped unexpectedly");
                    compare_guest((start + elapsed) & 0xffffff, scenario, budget);
                    ++count;
                }
            }
    target_call("_mem_setup_banks", 7);
    printf("PASS %u target indexed copy batches, ASM/JIT, budgets 1..64, X/Y, overlap, split RAM and cycle wrap\n", count);
}

static void benchmark_batches(void)
{
    static const struct {
        const char *name;
        uint8_t program[24];
        uint8_t length;
        uint8_t budget;
    } cases[] = {
        {"arithmetic", {0xa9,0x55,0x49,0xaa,0x29,0x7f,0x09,0x80,0xaa,0xca,0xe8,0x8a,0xc9,0xd5,0xd0,0xf0}, 16,63},
        {"direct RAM reads", {0xa5,0x40,0x45,0x41,0x25,0x42,0x05,0x43,0xaa,0xca,0xe8,0x8a,0xc9,0xd5,0x4c,0x00,0x04}, 17,75},
        {"indexed RAM copy", {0xa2,0x08,0xbd,0x00,0x20,0x9d,0x00,0x30,0xca,0xd0,0xf7,0x4c,0x00,0x04}, 14,63},
        {"RAM increment", {0xe6,0x40,0x4c,0x00,0x04}, 5,64}
    };
    unsigned int scenario;
    for (scenario = 0; scenario < sizeof(cases) / sizeof(cases[0]); ++scenario)
    {
        uint64_t native_cycles = 0, interpreter_cycles = 0;
        unsigned int iteration, offset;
        target_call("_jit_reset", 0);
        seed_guest(cases[scenario].program, sizeof(cases[scenario].program), 0x400, 0, 0);
        for (offset = 0; offset < cases[scenario].length;
             offset += jit_instruction_size(cases[scenario].program[offset]))
            make_hot((uint16_t)(0x400 + offset));
        for (iteration = 0; iteration < 32; ++iteration)
        {
            uint32_t cycles = 0;
            seed_guest(cases[scenario].program, sizeof(cases[scenario].program), 0x400, 0, 0);
            while (cycles < cases[scenario].budget) cycles += vrEmu6502InstCycle(reference);
            if (!target_call("_cpu_run", cases[scenario].budget)) fail("native benchmark batch failed");
            native_cycles += last_call_cycles;
            compare_guest(cycles, cases[scenario].name, cases[scenario].budget);
            cycles = 0;
            seed_guest(cases[scenario].program, sizeof(cases[scenario].program), 0x400, 0, 0);
            while (cycles < cases[scenario].budget) cycles += vrEmu6502InstCycle(reference);
            if (!target_call("_cpu_run_asm", cases[scenario].budget)) fail("interpreter benchmark batch failed");
            interpreter_cycles += last_call_cycles;
            compare_guest(cycles, cases[scenario].name, cases[scenario].budget);
        }
        printf("BENCH warm %s, 32 x %u guest cycles: JIT=%llu ASM=%llu eZ80 clocks; ASM/JIT=%.3f\n",
               cases[scenario].name, cases[scenario].budget,
               (unsigned long long)native_cycles, (unsigned long long)interpreter_cycles,
               (double)interpreter_cycles / (double)native_cycles);
    }
}

static void test_jit_reentry(void)
{
    static const uint8_t loop[] = {
        0xa9,0x55,0x49,0xaa,0x29,0x7f,0x09,0x80,
        0xaa,0xca,0xe8,0x8a,0xc9,0xd5,0xd0,0xf0
    };
    uint8_t program[48] = {0x4c,0x10,0x04};
    uint32_t cycles = 0, hits;
    uint64_t native_cycles, interpreter_cycles;
    unsigned int control, region, budget, cases = 0;

    memcpy(program + 0x10, loop, sizeof(loop));
    target_call("_jit_reset", 0);
    seed_guest(program, sizeof(program), 0x400, 0, 0);
    make_hot(0x410);
    while (cycles < 63)
        cycles += vrEmu6502InstCycle(reference);
    if (!target_call("_cpu_run", 63))
        fail("cold-entry JIT batch failed");
    native_cycles = last_call_cycles;
    hits = mem_peek_long(symbol_address("_jit_native_hits"));
    compare_guest(cycles, "cold-entry JIT", 63);

    cycles = 0;
    seed_guest(program, sizeof(program), 0x400, 0, 0);
    while (cycles < 63)
        cycles += vrEmu6502InstCycle(reference);
    if (!target_call("_cpu_run_asm", 63))
        fail("cold-entry interpreter batch failed");
    interpreter_cycles = last_call_cycles;
    compare_guest(cycles, "cold-entry interpreter", 63);
    printf("BENCH cold-entry arithmetic, 63 guest cycles: JIT=%llu ASM=%llu eZ80 clocks; native_hits=%u ASM/JIT=%.3f\n",
           (unsigned long long)native_cycles, (unsigned long long)interpreter_cycles,
           hits, (double)interpreter_cycles / (double)native_cycles);
    if (!hits)
        fail("cached branch target was not reached through the JIT after an entry miss");
    if (native_cycles >= interpreter_cycles)
        fail("cached branch re-entry did not improve execution cost");

    for (control = 0; control < 4; ++control)
        for (region = 0; region < 2; ++region)
            for (budget = 0; budget <= 64; ++budget)
            {
                static const char *const names[] = {"JMP", "BNE", "JSR", "RTS"};
                uint16_t pc = region ? 0x0ff0 : 0x400;
                uint8_t offset = control == 3 ? 3 : 16;
                uint16_t destination = (uint16_t)(pc + (control == 3 ? 40 : offset));
                uint32_t start = budget & 1 ? 0xfffff0 : 0;
                char name[64];

                memset(program, 0xea, sizeof(program));
                memcpy(program + offset, loop, sizeof(loop));
                program[0] = control == 0 ? 0x4c : control == 1 ? 0xd0 : 0x20;
                program[1] = control == 1 ? 14 : (uint8_t)destination;
                if (control != 1)
                    program[2] = (uint8_t)(destination >> 8);
                if (control == 3)
                    program[40] = 0x60;
                target_call("_jit_reset", 0);
                seed_guest(program, sizeof(program), pc, budget & 2 ? FlagD | FlagC : 0, start);
                make_hot((uint16_t)(pc + offset));
                cycles = 0;
                while (cycles < budget)
                    cycles += vrEmu6502InstCycle(reference);
                if (!target_call("_cpu_run", (start + budget) & 0xffffff))
                    fail("control-transfer re-entry batch failed");
                snprintf(name, sizeof(name), "%s re-entry at %04x", names[control], pc);
                compare_guest((start + cycles) & 0xffffff, name, budget);
                if (budget == 64 && !mem_peek_long(symbol_address("_jit_native_hits")))
                    fail("control-transfer re-entry never executed cached code");
                if (mem_peek_byte(symbol_address("_cpu_jit_reentry")))
                    fail("mixed CPU runner left re-entry enabled for direct interpreter calls");
                ++cases;
            }
    printf("PASS %u cached JMP/BNE/JSR/RTS re-entry batches, budgets 0..64, split RAM and cycle wrap\n", cases);
}

static void test_admission(void)
{
    static const uint8_t code[] = {0xe8,0xca,0xd0,0xfd};
    unsigned int budget;
    target_call("_jit_reset", 0);
    seed_guest(code, sizeof(code), 0x400, 0, 0xfffffc);
    make_hot(0x400);
    for (budget = 0; budget < 7; ++budget)
    {
        if (target_call("_jit_run", (0xfffffc + budget) & 0xffffff))
            fail("native block exceeded available budget");
        compare_guest(0xfffffc, "native miss", budget);
    }
    if (!target_call("_jit_run", 3))
        fail("exact-budget native block was not admitted");
    for (budget = 0; budget < 3; ++budget) vrEmu6502InstCycle(reference);
    compare_guest(3, "native exact budget", 7);
    seed_guest(code, sizeof(code), 0x400, 0, 0);
    mem_poke_byte(low_address + 0x400, 0x8d);
    guest_ram[0x400] = 0x8d;
    target_call("_jit_invalidate_page", 0x04);
    if (target_call("_jit_run", 20))
        fail("untracked self-modifying write executed stale code");
    compare_guest(0, "untracked write", 20);
    printf("PASS target budget admission, 24-bit wrap, and untracked code invalidation\n");
}

static void decimal_expected(uint8_t state[16], uint8_t operand, bool subtract)
{
    unsigned int accumulator = state[3], carry = state[6] != 0;
    int binary, units, tens;
    if (!subtract)
    {
        binary = accumulator + operand + carry;
        units = (accumulator % 16) + (operand % 16) + carry;
        tens = (accumulator / 16) + (operand / 16) + (units >= 10);
        state[12] = (tens & 8) != 0;
        state[11] = ((accumulator ^ operand) & 128) == 0 && ((accumulator >> 7) != state[12]);
        state[6] = tens >= 10;
        state[3] = (uint8_t)((((tens + (state[6] ? 6 : 0)) & 15) << 4) |
                            ((units + (units >= 10 ? 6 : 0)) & 15));
    }
    else
    {
        binary = (int)accumulator - operand - !carry;
        units = (int)(accumulator % 16) - (operand % 16) - !carry;
        tens = (int)(accumulator / 16) - (operand / 16) - (units < 0);
        state[12] = (binary & 128) != 0;
        state[11] = ((accumulator ^ operand) & (accumulator ^ binary) & 128) != 0;
        state[6] = binary >= 0;
        state[3] = (uint8_t)((((tens - (tens < 0 ? 6 : 0)) & 15) << 4) |
                            ((units - (units < 0 ? 6 : 0)) & 15));
    }
    state[7] = (uint8_t)binary == 0;
}

static void cpu_fixture_byte(uint16_t address, uint8_t value)
{
    mem_poke_byte((address < 0x1000 ? low_address : high_address) + address, value);
}

static uint16_t cpu_operand_address(uint8_t opcode, const uint8_t initial[16])
{
    uint16_t pc = (uint16_t)(initial[0] | initial[1] << 8);
    uint8_t operand = guest_ram[(uint16_t)(pc + 1)];
    uint16_t absolute = (uint16_t)(operand | guest_ram[(uint16_t)(pc + 2)] << 8);
    switch (opcode & 0x1f)
    {
    case 0x01: case 0x03:
        operand = (uint8_t)(operand + initial[4]);
        return (uint16_t)(guest_ram[operand] | guest_ram[(uint8_t)(operand + 1)] << 8);
    case 0x05: case 0x07: return operand;
    case 0x09: case 0x0b: return (uint16_t)(pc + 1);
    case 0x0d: case 0x0f: return absolute;
    case 0x11: case 0x13:
        return (uint16_t)((guest_ram[operand] | guest_ram[(uint8_t)(operand + 1)] << 8) + initial[5]);
    case 0x15: case 0x17: return (uint8_t)(operand + initial[4]);
    case 0x19: case 0x1b: return (uint16_t)(absolute + initial[5]);
    case 0x1d: case 0x1f: return (uint16_t)(absolute + initial[4]);
    default: return 0;
    }
}

static void test_cpu_opcodes(void)
{
    static const uint8_t flags[] = {0, FlagC, FlagZ, FlagI, FlagD, FlagV, FlagN, 0xff};
    static const struct {
        uint16_t pc, operand;
        uint8_t accumulator, index_x, index_y, stack;
        uint32_t cycles;
    } scenarios[] = {
        {0x0400,0x0040,0x81,0x02,0xff,0xff,0xfffffc},
        {0x04fe,0x00ff,0x7f,0x01,0x01,0x00,0xfffffe},
        {0x04ff,0x0fff,0xff,0xff,0xff,0x01,0xffffff},
        {0x0fff,0xcfff,0x00,0x01,0x01,0xfd,0},
        {0xfffd,0xfffe,0x99,0x02,0x02,0x80,0xfffff8},
        {0xfffe,0xffff,0x09,0x01,0x01,0xfe,0xfffffd},
        {0xffff,0x0080,0x80,0x80,0x80,0x7f,0xfffffb},
        {0x0400,0x007f,0x01,0x00,0x00,0x02,0x123456}
    };
    static const char *const forbidden[] = {
        "op_fallback", "_cpu_run_c", "jit_dispatch", "jit_try_swirler", "jit_try_ram_loop"
    };
    unsigned int encodings = 0, cases = 0, rejected = 0;
    uint8_t *state = mem.ram.block + state_address - RAM_BASE;
    vrEmu6502Destroy(reference);
    reference = vrEmu6502New(CPU_6502U, reference_read, reference_write);
    if (!reference) fail("native CPU oracle allocation");
    for (unsigned int opcode = 0; opcode < 256; ++opcode)
    {
        uint8_t length = jit_instruction_size((uint8_t)opcode);
        switch (opcode)
        {
        case 0x8b: case 0x93: length = 2; break;
        case 0x9b: case 0x9c: case 0x9e: case 0x9f: case 0xbb: length = 3; break;
        default: break;
        }
        if (!length)
        {
            uint8_t program = (uint8_t)opcode;
            uint8_t expected[16];
            uint32_t accepted;
            target_call("_mem_setup_banks", 0);
            seed_guest(&program, 1, 0xffff, 0xff, 0xffffff);
            memcpy(expected, state, sizeof(expected));
            expected[0] = expected[1] = 0;
            accepted = target_call("_cpu_run_asm", 0);
            if (accepted || memcmp(state, expected, sizeof(expected)) ||
                mem_peek_byte(symbol_address("_cpu_illegal_op")) != opcode ||
                (mem_peek_long(symbol_address("_cpu_illegal_pc")) & 0xffff) != 0)
            {
                fprintf(stderr, "native stop opcode=%02x accepted=%lu pc=%04lx cycles=%06lx illegal=%02x illegal_pc=%04lx tested=%u\n",
                        opcode, (unsigned long)accepted, (unsigned long)read_little(state, 2),
                        (unsigned long)read_little(state + 13, 3),
                        mem_peek_byte(symbol_address("_cpu_illegal_op")),
                        (unsigned long)(mem_peek_long(symbol_address("_cpu_illegal_pc")) & 0xffff), cases);
                fail("unsupported native opcode did not stop explicitly at the wrapped PC");
            }
            ++integration_cases;
            ++rejected;
            continue;
        }
        ++encodings;
        for (unsigned int scenario = 0; scenario < sizeof(scenarios) / sizeof(scenarios[0]); ++scenario)
            for (unsigned int flag = 0; flag < sizeof(flags); ++flag)
            {
                uint8_t program[] = {(uint8_t)opcode, (uint8_t)scenarios[scenario].operand,
                                     (uint8_t)(scenarios[scenario].operand >> 8)};
                uint8_t initial[16], expected[16];
                uint8_t pointer = program[1];
                uint16_t operand_address;
                uint8_t operand;
                uint32_t cycles;
                char name[96];
                target_call("_mem_setup_banks", 0);
                cpu_fixture_byte(scenarios[scenario].operand, scenarios[scenario].accumulator);
                cpu_fixture_byte((uint16_t)(scenarios[scenario].operand + scenarios[scenario].index_x), 0x81);
                cpu_fixture_byte((uint16_t)(scenarios[scenario].operand + scenarios[scenario].index_y), 0xff);
                if ((opcode & 0x1f) == 1 || (opcode & 0x1f) == 3)
                    pointer = (uint8_t)(pointer + scenarios[scenario].index_x);
                cpu_fixture_byte(pointer, program[1]);
                cpu_fixture_byte((uint8_t)(pointer + 1), program[2]);
                seed_guest_registers(program, length, scenarios[scenario].pc, flags[flag],
                                     scenarios[scenario].cycles, scenarios[scenario].accumulator,
                                     scenarios[scenario].index_x, scenarios[scenario].index_y,
                                     scenarios[scenario].stack);
                memcpy(initial, state, sizeof(initial));
                operand_address = cpu_operand_address((uint8_t)opcode, initial);
                operand = guest_ram[operand_address];
                cycles = (scenarios[scenario].cycles + vrEmu6502InstCycle(reference)) & 0xffffff;
                reference_state(expected, cycles);
                if (opcode == 0x6b)
                {
                    expected[3] = (uint8_t)(((initial[3] & operand) >> 1) | initial[6] << 7);
                    expected[6] = (expected[3] & 0x40) != 0;
                    expected[7] = expected[3] == 0;
                    expected[11] = ((expected[3] >> 6) ^ (expected[3] >> 5)) & 1;
                    expected[12] = (expected[3] & 0x80) != 0;
                }
                else if (opcode == 0xab)
                {
                    expected[3] = expected[4] = operand;
                    expected[7] = operand == 0;
                    expected[12] = (operand & 0x80) != 0;
                }
                else if (opcode == 0x8b)
                {
                    expected[3] = initial[4] & operand;
                    expected[7] = expected[3] == 0;
                    expected[12] = (expected[3] & 0x80) != 0;
                }
                else if (initial[9] && ((opcode & 0xe1) == 0x61 || (opcode & 0xe1) == 0xe1))
                {
                    uint8_t decimal[16];
                    bool combined = (opcode & 3) == 3 && opcode != 0xeb;
                    bool subtract = (opcode & 0x80) != 0;
                    memcpy(decimal, initial, sizeof(decimal));
                    if (combined)
                    {
                        if (subtract)
                            ++operand;
                        else
                        {
                            decimal[6] = operand & 1;
                            operand = (uint8_t)((operand >> 1) | initial[6] << 7);
                        }
                    }
                    decimal_expected(decimal, operand, subtract);
                    expected[3] = decimal[3];
                    expected[6] = decimal[6];
                    expected[7] = decimal[7];
                    expected[11] = decimal[11];
                    expected[12] = decimal[12];
                }
                snprintf(name, sizeof(name), "native opcode=%02x PC=%04x P=%02x scenario=%u",
                         opcode, scenarios[scenario].pc, flags[flag], scenario);
                if (!target_call("_cpu_run_asm", (scenarios[scenario].cycles + 1) & 0xffffff))
                {
                    fprintf(stderr, "%s\n", name);
                    fail("supported native opcode rejected");
                }
                compare_expected_guest(expected, name, 1);
                ++cases;
            }
    }
    if (encodings != 244 || rejected != 12 || cases != 15616)
        fail("native CPU opcode coverage count changed");
    printf("PASS native CPU instruction matrix: 151 official + 93 undocumented encodings, %u state/memory/flag/wrap cases, %u explicit stops; deterministic XAA/LAX convention\n",
           cases, rejected);
    for (unsigned int index = 0; index < sizeof(forbidden) / sizeof(forbidden[0]); ++index)
        if (find_symbol_address(forbidden[index]))
        {
            fprintf(stderr, "forbidden native CPU symbol: %s\n", forbidden[index]);
            fail("production image still links a C interpreter or hardcoded translator");
        }
    mem_poke_byte(symbol_address("_cpu_illegal_op"), 0);
    target_call("_mem_setup_banks", 7);
    vrEmu6502Destroy(reference);
    reference = vrEmu6502New(CPU_6502, reference_read, reference_write);
    if (!reference) fail("native CPU oracle reset");
    puts("PASS native CPU image has no C fallback or hardcoded translator");
}

static void test_jit_load_extensions(void)
{
    static const uint8_t opcodes[] = {0x8b,0xbb,0x93,0x9b,0x9c,0x9e,0x9f};
    static const uint16_t bases[] = {0x0080,0x0fff,0x9fff,0xafff,0xbfff,0xcfff,0xfffe,0xffff};
    JitTarget target = {.state = state_address, .low_ram = low_address, .high_ram = high_address};
    unsigned int count = 0;
    target.read_byte = symbol_address("_jit_read_bridge");
    target.write_byte = symbol_address("_jit_write_bridge");
    vrEmu6502Destroy(reference);
    reference = vrEmu6502New(CPU_6502U, reference_read, reference_write);
    if (!reference) fail("JIT load extension reference allocation");
    for (unsigned int operation = 0; operation < sizeof(opcodes); ++operation)
        for (unsigned int value = 0; value < 256; ++value)
            for (unsigned int prefix = 0; prefix < 2; ++prefix)
            {
                uint16_t base = bases[value % 8];
                uint16_t pc = prefix ? 0x04fe : 0x0400;
                uint8_t program[48] = {0xa9,0x80};
                uint8_t offset = prefix * 2;
                uint8_t length = offset + jit_instruction_size(opcodes[operation]);
                uint8_t output[512], expected[16];
                JitTranslation translation;
                uint32_t cycles = 0xfffffc;
                char name[96];
                program[offset] = opcodes[operation];
                program[offset + 1] = operation ? (uint8_t)base : (uint8_t)(value * 73 + 19);
                if (operation) program[offset + 2] = (uint8_t)(base >> 8);
                target_call("_mem_setup_banks", 0);
                target_call("_jit_reset", 0);
                cpu_fixture_byte((uint16_t)(base + value), (uint8_t)(value * 53 + 7));
                if (opcodes[operation] == 0x93)
                {
                    program[offset + 1] = 0xff;
                    program[offset + 2] = 0;
                    cpu_fixture_byte(0xff, (uint8_t)base);
                    cpu_fixture_byte(0, (uint8_t)(base >> 8));
                }
                seed_guest_registers(program, sizeof(program), pc, (uint8_t)value, cycles,
                                     (uint8_t)(value * 13 + 55), (uint8_t)value,
                                     operation < 2 ? (uint8_t)value : (uint8_t)(value ^ 0x55),
                                     (uint8_t)(value * 37 + 11));
                if (!jit_compile(output, sizeof(output), program, sizeof(program), pc, &target, &translation) ||
                    translation.instructions != prefix + 1 || translation.guest_size != length)
                    fail("JIT load extension was not fully compiled");
                if (operation >= 2 && (!translation.writes_memory || translation.can_repeat))
                    fail("JIT masked store did not terminate safely");
                for (unsigned int step = 0; step < translation.instructions; ++step)
                    cycles += vrEmu6502InstCycle(reference);
                reference_state(expected, cycles);
                if (!operation)
                {
                    expected[3] = (uint8_t)value & program[offset + 1];
                    expected[7] = expected[3] == 0;
                    expected[12] = (expected[3] & 0x80) != 0;
                }
                make_hot(pc);
                expected_native = output;
                expected_native_size = translation.native_size;
                if (!target_call("_jit_run", 100))
                    fail("JIT load extension was not admitted");
                expected_native = NULL;
                snprintf(name, sizeof(name), "JIT opcode=%02x value=%02x prefix=%u",
                         opcodes[operation], value, prefix);
                compare_expected_guest(expected, name, 1);
                ++count;
            }
    target_call("_mem_setup_banks", 7);
    vrEmu6502Destroy(reference);
    reference = vrEmu6502New(CPU_6502, reference_read, reference_write);
    if (!reference) fail("JIT load extension reference reset");
    printf("PASS %u JIT load/masked-store cases: byte values, flags, prefixes, pointer/page/cycle wrap and block exits\n", count);
}

static void test_native_port_rmw(void)
{
    static const uint8_t opcodes[] = {
        0x06,0x26,0x46,0x66,0xc6,0xe6,0x07,0x27,0x47,0x67,0xc7,0xe7,
        0x0e,0x2e,0x4e,0x6e,0xce,0xee,0x0f,0x2f,0x4f,0x6f,0xcf,0xef
    };
    uint8_t read_pages[256 * 4], write_pages[256 * 4];
    uint32_t read_address = symbol_address("_mem_rd_pg") - RAM_BASE;
    uint32_t write_address = symbol_address("_mem_wr_pg") - RAM_BASE;
    unsigned int count = 0;
    vrEmu6502Destroy(reference);
    reference = vrEmu6502New(CPU_6502U, reference_read, reference_write);
    if (!reference) fail("bank-port CPU oracle allocation");
    for (unsigned int index = 0; index < sizeof(opcodes); ++index)
        for (unsigned int bank = 0; bank < 8; ++bank)
            for (unsigned int carry = 0; carry < 2; ++carry)
            {
                uint8_t program[] = {opcodes[index], 0x01, 0x00};
                uint8_t expected[16];
                uint32_t cycles;
                char name[80];
                target_call("_mem_setup_banks", bank);
                seed_guest(program, jit_instruction_size(opcodes[index]), 0x400,
                           carry ? FlagC : 0, 0xfffffe);
                cycles = (0xfffffe + vrEmu6502InstCycle(reference)) & 0xffffff;
                reference_state(expected, cycles);
                snprintf(name, sizeof(name), "native RMW port opcode=%02x bank=%02x C=%u",
                         opcodes[index], bank, carry);
                if (!target_call("_cpu_run_asm", 0xffffff)) fail("bank-port RMW rejected");
                compare_expected_guest(expected, name, 1);
                memcpy(read_pages, mem.ram.block + read_address, sizeof(read_pages));
                memcpy(write_pages, mem.ram.block + write_address, sizeof(write_pages));
                target_call("_mem_setup_banks", guest_ram[1]);
                if (memcmp(read_pages, mem.ram.block + read_address, sizeof(read_pages)) ||
                    memcmp(write_pages, mem.ram.block + write_address, sizeof(write_pages)))
                {
                    fprintf(stderr, "%s result=%02x\n", name, guest_ram[1]);
                    fail("native RMW port left stale banking tables");
                }
                ++count;
            }
    target_call("_mem_setup_banks", 7);
    vrEmu6502Destroy(reference);
    reference = vrEmu6502New(CPU_6502, reference_read, reference_write);
    if (!reference) fail("bank-port CPU oracle reset");
    printf("PASS %u native RMW bank-port cases: register/flag/cycle state, RAM and page tables\n", count);
}

static void test_native_brk(void)
{
    static const uint16_t starts[] = {0x400, 0xffff};
    static const uint8_t program[] = {0};
    uint8_t *state = mem.ram.block + state_address - RAM_BASE;
    uint32_t pages = symbol_address("_mem_rd_pg");
    uint32_t count_address = symbol_address("_cpu_brk_count");
    uint32_t first_pc_address = symbol_address("_cpu_first_brk_pc");
    uint32_t first_sp_address = symbol_address("_cpu_first_brk_sp");
    uint32_t first_ret_address = symbol_address("_cpu_first_brk_ret");
    uint32_t last_pc_address = symbol_address("_cpu_last_brk_pc");
    unsigned int scenario, stack;
    for (scenario = 0; scenario < sizeof(starts) / sizeof(starts[0]); ++scenario)
    {
        uint16_t pc = starts[scenario], vector = scenario ? 0x56 : 0x4567;
        uint16_t return_pc = (uint16_t)(pc + 2);
        uint32_t visible;
        uint8_t saved_vector[2];
        target_call("_mem_setup_banks", scenario ? 0 : 7);
        visible = (mem_peek_long(pages + 255 * 4) + 0xfffe) & 0xffffff;
        memcpy(saved_vector, mem.ram.block + visible - RAM_BASE, sizeof(saved_vector));
        mem_poke_byte(visible, (uint8_t)vector);
        mem_poke_byte(visible + 1, (uint8_t)(vector >> 8));
        target_call("_jit_reset", 0);
        seed_guest(program, sizeof(program), pc, 0, 0xfffffe);
        make_hot(pc);
        for (stack = 0; stack < 256; ++stack)
        {
            uint8_t expected[16], expected_stack[256];
            seed_guest(program, sizeof(program), pc, (uint8_t)stack, 0xfffffe);
            state[2] = (uint8_t)stack;
            state[10] = 0;
            memset(expected_stack, 0xa5, sizeof(expected_stack));
            expected_stack[(uint8_t)(stack + 1)] = 0x34;
            expected_stack[(uint8_t)(stack + 2)] = 0x12;
            memcpy(mem.ram.block + low_address - RAM_BASE + 0x100, expected_stack, sizeof(expected_stack));
            memcpy(expected, state, sizeof(expected));
            expected[0] = (uint8_t)vector;
            expected[1] = (uint8_t)(vector >> 8);
            expected[2] = (uint8_t)(stack - 3);
            expected[8] = expected[10] = 1;
            expected[13] = 5;
            expected[14] = expected[15] = 0;
            expected_stack[stack] = (uint8_t)(return_pc >> 8);
            expected_stack[(uint8_t)(stack - 1)] = (uint8_t)return_pc;
            expected_stack[(uint8_t)(stack - 2)] = (uint8_t)(stack | 0x30);
            mem_poke_long(count_address, 0);
            if (!target_call("_jit_run", 5) || memcmp(state, expected, sizeof(expected)) ||
                memcmp(mem.ram.block + low_address - RAM_BASE + 0x100, expected_stack, sizeof(expected_stack)) ||
                mem_peek_long(count_address) != 1 ||
                (mem_peek_long(first_pc_address) & 0xffff) != pc ||
                mem_peek_byte(first_sp_address) != stack ||
                (mem_peek_long(first_ret_address) & 0xffff) != 0x1234 ||
                (mem_peek_long(last_pc_address) & 0xffff) != pc)
            {
                fprintf(stderr, "BRK target PC=%04x SP=%02x P=%02x\n", pc, stack, stack);
                fail("native BRK stack, status, vector, wrap or diagnostic mismatch");
            }
            ++integration_cases;
        }
        memcpy(mem.ram.block + visible - RAM_BASE, saved_vector, sizeof(saved_vector));
    }
    target_call("_mem_setup_banks", 7);
    puts("PASS 512 target BRK cases, every SP/status, banked vectors, return/cycle wrap and diagnostics");
}

static void test_native_decimal_rmw(void)
{
    unsigned int operation, operand, accumulator, carry, count = 0;
    uint8_t *state = mem.ram.block + state_address - RAM_BASE;
    uint8_t saved[3];
    memcpy(saved, mem.ram.block + low_address - RAM_BASE + 0x3f, sizeof(saved));
    for (operation = 0; operation < 2; ++operation)
    {
        uint8_t program[] = {operation ? 0xe7 : 0x67, 0x40, 0x02};
        uint8_t initial[16];
        target_call("_jit_reset", 0);
        seed_guest(program, sizeof(program), 0x400, FlagD | FlagV | FlagN | FlagZ, 0xfffffe);
        memcpy(initial, state, sizeof(initial));
        make_hot(0x400);
        for (operand = 0; operand < 256; ++operand)
            for (accumulator = 0; accumulator < 256; accumulator += 37)
                for (carry = 0; carry < 2; ++carry)
                {
                    uint8_t expected[16];
                    uint8_t result = operation ? (uint8_t)(operand + 1) :
                        (uint8_t)((operand >> 1) | (carry << 7));
                    initial[3] = (uint8_t)accumulator;
                    initial[6] = (uint8_t)carry;
                    memcpy(state, initial, sizeof(initial));
                    memcpy(expected, initial, sizeof(expected));
                    if (!operation) expected[6] = operand & 1;
                    decimal_expected(expected, result, operation != 0);
                    expected[0] = 2;
                    expected[13] = 3;
                    expected[14] = expected[15] = 0;
                    mem_poke_byte(low_address + 0x3f, 0xa5);
                    mem_poke_byte(low_address + 0x40, (uint8_t)operand);
                    mem_poke_byte(low_address + 0x41, 0x5a);
                    if (!target_call("_jit_run", 3) || memcmp(state, expected, sizeof(expected)) ||
                        mem_peek_byte(low_address + 0x40) != result ||
                        mem_peek_byte(low_address + 0x3f) != 0xa5 ||
                        mem_peek_byte(low_address + 0x41) != 0x5a)
                    {
                        fprintf(stderr, "decimal target %s A=%02x M=%02x C=%u\n",
                                operation ? "ISC" : "RRA", accumulator, operand, carry);
                        fail("generated decimal RMW arithmetic, flags, memory or cycles diverged");
                    }
                    ++integration_cases;
                    ++count;
                }
    }
    memcpy(mem.ram.block + low_address - RAM_BASE + 0x3f, saved, sizeof(saved));
    printf("PASS %u target decimal RRA/ISC executions, carry, flags, RAM and cycle wrap\n", count);
}

static void test_native_decimal(bool exhaustive)
{
    unsigned int operation, operand, accumulator, carry, count = 0;
    uint8_t *state = mem.ram.block + state_address - RAM_BASE;
    for (operation = 0; operation < 2; ++operation)
        for (operand = 0; operand < 256; ++operand)
        {
            uint8_t program[] = {operation ? 0xe9 : 0x69, (uint8_t)operand, 0x02};
            uint8_t initial[16];
            target_call("_jit_reset", 0);
            seed_guest(program, sizeof(program), 0x400, FlagD, 0xfffffe);
            memcpy(initial, state, sizeof(initial));
            make_hot(0x400);
            for (accumulator = 0; accumulator < 256; accumulator += exhaustive ? 1 : 37)
                for (carry = 0; carry < 2; ++carry)
                {
                    uint8_t expected[16];
                    initial[3] = (uint8_t)accumulator;
                    initial[6] = (uint8_t)carry;
                    memcpy(state, initial, sizeof(initial));
                    memcpy(expected, initial, sizeof(expected));
                    decimal_expected(expected, (uint8_t)operand, operation != 0);
                    expected[0] = 2;
                    expected[13] = expected[14] = expected[15] = 0;
                    if (!target_call("_jit_run", 0) || memcmp(state, expected, sizeof(expected)))
                    {
                        fprintf(stderr, "decimal target %s A=%02x M=%02x C=%u\n",
                                operation ? "SBC" : "ADC", accumulator, operand, carry);
                        fail("generated decimal arithmetic/flags/cycles diverged");
                    }
                    ++integration_cases;
                    ++count;
                }
        }
    printf("PASS %u target decimal ADC/SBC executions, NMOS flags and 24-bit cycle wrap\n", count);
}

int main(int argc, char **argv)
{
    bool cpu_only = argc > 2 && !strcmp(argv[2], "--cpu-only");
    bool edge_only = argc > 2 && !strcmp(argv[2], "--edge-only");
    bool benchmark_only = argc > 2 && !strcmp(argv[2], "--benchmark-only");
    bool copy_only = argc > 2 && !strcmp(argv[2], "--copy-only");
    bool reentry_only = argc > 2 && !strcmp(argv[2], "--reentry-only");
    bool extensions_only = argc > 2 && !strcmp(argv[2], "--extensions-only");
    setvbuf(stdout, NULL, _IONBF, 0);
    asic_init();
    mem.flash.size = SIZE_FLASH_MIN;
    mem.flash.block = malloc(mem.flash.size);
    if (!mem.flash.block)
        fail("flash fixture allocation");
    memset(mem.flash.block, 0xff, mem.flash.size);
    asic_reset();
    emu_set_run_rate(1000);
    load_image(argc > 1 ? argv[1] : "../../bin/EMUDORE.obj");
    if (edge_only)
         printf("JIT_ENTRY direct=%06lx block=%06lx run=%06lx fast=%06lx\n",
             (unsigned long)symbol_address("_jit_execute"),
             (unsigned long)symbol_address("_jit_execute_block"),
             (unsigned long)symbol_address("_jit_run"),
             (unsigned long)symbol_address("_jit_run_fast"));
    if (argc > 3)
        test_compressed_startup(argv[2], argv[3]);
    if (!extensions_only && !cpu_only && !edge_only && !benchmark_only && !copy_only && !reentry_only)
        test_missing_rom_startup();
    target_call("_mem_init", 0);
    if (!target_call("_jit_backend_selftest", 0))
        fail("production JIT backend self-test rejected its generated code");
    puts("PASS CEdev-compiled emitter, native execution bridge, JIT startup self-test");
    if (!target_call("_cpu_asm_selftest", 0))
        fail("production CPU assembly self-test");
    puts("PASS target CPU assembly self-test");
    target_call("_mem_init", 0);
    state_address = symbol_address("_cpu");
    low_address = mem_peek_long(symbol_address("_mem_low"));
    high_address = mem_peek_long(symbol_address("_mem_ram"));
    reference = vrEmu6502New(CPU_6502, reference_read, reference_write);
    if (!reference)
        fail("reference CPU allocation");
    if (extensions_only)
    {
        test_target_opcodes(false);
        test_jit_load_extensions();
    }
    if (cpu_only)
    {
        test_cpu_opcodes();
        test_native_port_rmw();
    }
    if (!extensions_only && !cpu_only && !edge_only && !benchmark_only && !copy_only && !reentry_only)
    {
        test_batches();
        test_admission();
        test_target_opcodes(false);
        test_jit_load_extensions();
        test_memory_guards();
        test_mapped_devices();
        test_mapped_rom();
        test_code_windows();
        test_failure_fallback();
        test_native_self_modification();
        test_assembly_jit_reentry();
        test_high_page_generation_index();
        test_native_decimal(argc > 2 && !strcmp(argv[2], "--decimal-exhaustive"));
    }
    if (edge_only)
        test_high_page_generation_index();
    if (edge_only || reentry_only)
        test_assembly_jit_reentry();
    if (!extensions_only && !cpu_only && !edge_only && !benchmark_only && !reentry_only)
        test_indexed_copy_batches();
    if (!extensions_only && !cpu_only && !edge_only && !copy_only && !reentry_only)
        benchmark_batches();
    if (!extensions_only && (reentry_only || (!cpu_only && !edge_only && !copy_only)))
        test_jit_reentry();
    if (!extensions_only && !cpu_only && !benchmark_only && !copy_only && !reentry_only)
    {
        test_native_brk();
        test_native_decimal_rmw();
    }
    printf("PASS target integration: %u cases, %u production C ABI calls in CEmu\n",
           integration_cases, target_calls);
    vrEmu6502Destroy(reference);
    free(image);
    asic_free();
    return 0;
}