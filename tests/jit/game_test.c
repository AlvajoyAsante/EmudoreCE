#define main target_unit_main
#include "target_test.c"
#undef main
#include "keypad.h"
#ifdef GAME_JIT_WATCH
#include "debug/debug.h"
#endif

#define GAME_DISK_FLASH 0x100000u

typedef struct {
    uint8_t name[16];
    uint8_t *bytes;
    size_t size;
} GameFile;

typedef struct {
    const char *name;
    uint32_t address;
    uint8_t opcode;
} GameHook;

static GameHook game_hooks[] = {
    {"_CEPackageRead", 0, 0},
    {"_CEPackageSize", 0, 0},
    {"_CEPackageGetDirectChunk", 0, 0},
    {"_CEPackageChunkPtr", 0, 0},
    {"_gfx_SetTextFGColor", 0, 0},
    {"_gfx_SetTextBGColor", 0, 0},
    {"_gfx_SetTextTransparentColor", 0, 0},
    {"_gfx_SetTextScale", 0, 0},
    {"_gfx_GetStringWidth", 0, 0},
    {"_gfx_PrintStringXY", 0, 0},
    {"_memcmp", 0, 0},
    {"_drive_trap_load", 0, 0}
};
static uint32_t game_load_return;
static uint8_t game_load_return_opcode, game_load_mode, game_load_count;
static uint8_t *game_roms[3];
static const size_t game_rom_sizes[] = {8192, 8192, 4096};
static uint32_t game_disk_size;
static const char *game_output = "build/parallax-on";
static bool game_jit_enabled = true;
static const char *game_input_script = "none";
static uint32_t game_input_mask;
static const char *game_resume_path = "none";
static const char *game_disk_path = "../Parallax.d64";
static unsigned int game_selected;
static bool game_resumed;

typedef struct {
    char magic[8];
    uint32_t elf_hash, disk_hash, rom_hash[3], input_hash;
    uint32_t jit, selected, frame, nonblank, changed;
} GameCheckpoint;

static GameCheckpoint game_checkpoint;

typedef struct {
    const char *name;
    uint8_t row, column;
} GameKey;

typedef struct {
    unsigned int frame, duration, key;
} GameInput;

static const GameKey game_keys[] = {
    {"fire", 1, 5}, {"space", 3, 7}, {"enter", 6, 0}, {"n", 2, 3},
    {"right", 7, 2}, {"left", 7, 1}, {"up", 7, 3}, {"down", 7, 0},
    {"f1", 1, 4}, {"f3", 1, 3}, {"f5", 1, 2}, {"f7", 1, 1},
    {"stop", 5, 6}, {"h", 6, 5}, {"r", 6, 3}, {"l", 5, 4}
};
static GameInput game_inputs[64];
static unsigned int game_input_count;

static void game_parse_input(void)
{
    const char *cursor = game_input_script;
    if (!strcmp(cursor, "none")) return;
    while (*cursor)
    {
        char key_name[16];
        GameInput input;
        int consumed = 0;
        if (game_input_count == 64 ||
            sscanf(cursor, "%u:%15[a-z0-9]:%u%n", &input.frame, key_name, &input.duration, &consumed) != 3 ||
            !consumed || !input.frame || input.frame > 100000 || !input.duration || input.duration > 100000)
            fail("input syntax is frame:key:duration, separated by commas");
        for (input.key = 0; input.key < sizeof(game_keys) / sizeof(game_keys[0]); ++input.key)
            if (!strcmp(key_name, game_keys[input.key].name)) break;
        if (input.key == sizeof(game_keys) / sizeof(game_keys[0])) fail("unknown input key");
        game_inputs[game_input_count++] = input;
        cursor += consumed;
        if (*cursor == ',') ++cursor;
        else if (*cursor) fail("invalid input separator");
    }
}

static uint32_t game_input_at(unsigned int frame)
{
    unsigned int index;
    uint32_t mask = 0;
    for (index = 0; index < game_input_count; ++index)
        if (frame >= game_inputs[index].frame && frame - game_inputs[index].frame < game_inputs[index].duration)
            mask |= 1u << game_inputs[index].key;
    return mask;
}

static void game_apply_input(unsigned int frame)
{
    unsigned int index;
    uint32_t mask = game_input_at(frame);
    for (index = 0; index < sizeof(game_keys) / sizeof(game_keys[0]); ++index)
        if ((mask ^ game_input_mask) & (1u << index))
            emu_keypad_event(game_keys[index].row, game_keys[index].column, (mask & (1u << index)) != 0);
    if (mask != game_input_mask)
        printf("INPUT frame=%u mask=%03lx\n", frame, (unsigned long)mask);
    game_input_mask = mask;
}

static uint32_t game_find_symbol(const char *name)
{
    uint16_t section_index;
    for (section_index = 0; section_index < header.section_count; ++section_index)
    {
        ElfSection section = section_at(section_index);
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
                    !strcmp(names + symbol.name, name)) return symbol.value;
            }
        }
    }
    return 0;
}

static uint32_t game_hash(const uint8_t *bytes, size_t size)
{
    uint32_t hash = 2166136261u;
    size_t index;
    for (index = 0; index < size; ++index)
        hash = (hash ^ bytes[index]) * 16777619u;
    return hash;
}

static GameCheckpoint game_identity(unsigned int frame)
{
    GameCheckpoint identity = {0};
    unsigned int index;
    memcpy(identity.magic, "EMUGAME2", 8);
    identity.elf_hash = game_hash(image, image_size);
    identity.disk_hash = game_hash(mem.flash.block + GAME_DISK_FLASH, game_disk_size);
    for (index = 0; index < 3; ++index)
        identity.rom_hash[index] = game_hash(game_roms[index], game_rom_sizes[index]);
    identity.input_hash = 2166136261u;
    for (index = 1; index <= frame; ++index)
    {
        uint32_t mask = game_input_at(index);
        identity.input_hash = (identity.input_hash ^ (uint8_t)mask) * 16777619u;
        identity.input_hash = (identity.input_hash ^ (uint8_t)(mask >> 8)) * 16777619u;
    }
    identity.jit = game_jit_enabled;
    identity.selected = game_selected;
    return identity;
}

static void game_restore_checkpoint(void)
{
    GameCheckpoint expected;
    FILE *file = fopen(game_resume_path, "rb");
    if (!file || fread(&game_checkpoint, 1, sizeof(game_checkpoint), file) != sizeof(game_checkpoint))
        fail("cannot read game checkpoint");
    if (memcmp(game_checkpoint.magic, "EMUGAME2", 8) || game_checkpoint.frame > 100000)
        fail("unsupported or corrupt game checkpoint");
    expected = game_identity(game_checkpoint.frame);
    expected.frame = game_checkpoint.frame;
    expected.nonblank = game_checkpoint.nonblank;
    expected.changed = game_checkpoint.changed;
    if (memcmp(&expected, &game_checkpoint, sizeof(expected)))
        fail("checkpoint ELF/assets/past input/JIT/file identity differs from this run");
    asic_free();
    asic_init();
    if (!asic_restore(file) || fclose(file)) fail("cannot restore CEmu game checkpoint");
    game_resumed = true;
    printf("RESUME %s frame=%lu ELF=%08lx\n", game_resume_path,
           (unsigned long)game_checkpoint.frame, (unsigned long)game_checkpoint.elf_hash);
}

static void game_save_checkpoint(unsigned int frame, unsigned int nonblank, unsigned int changed)
{
    char path[512];
    FILE *file;
    game_checkpoint = game_identity(frame);
    game_checkpoint.frame = frame;
    game_checkpoint.nonblank = nonblank;
    game_checkpoint.changed = changed;
    if (snprintf(path, sizeof(path), "%s.state", game_output) >= (int)sizeof(path))
        fail("checkpoint path too long");
    file = fopen(path, "wb");
    if (!file || fwrite(&game_checkpoint, 1, sizeof(game_checkpoint), file) != sizeof(game_checkpoint) ||
        !asic_save(file) || fclose(file)) fail("cannot save CEmu game checkpoint");
    printf("CHECKPOINT %s completed_frame=%u\n", path, frame);
}

static uint8_t *game_read(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    uint8_t *bytes;
    long length;
    if (!file || fseek(file, 0, SEEK_END) || (length = ftell(file)) <= 0)
    {
        fprintf(stderr, "Cannot read asset: %s\n", path);
        fail("game asset read failed");
    }
    rewind(file);
    *size = (size_t)length;
    bytes = malloc(*size);
    if (!bytes || fread(bytes, 1, *size, file) != *size)
        fail("game asset allocation/read failed");
    fclose(file);
    printf("ASSET %s bytes=%lu fnv1a=%08lx\n", path,
           (unsigned long)*size, (unsigned long)game_hash(bytes, *size));
    return bytes;
}

static unsigned int game_sectors(unsigned int track)
{
    if (!track || track > 40) fail("D64 track outside supported range");
    return track <= 17 ? 21 : track <= 24 ? 19 : track <= 30 ? 18 : 17;
}

static size_t game_sector(unsigned int track, unsigned int sector, size_t size)
{
    size_t offset = 0;
    unsigned int preceding;
    if (sector >= game_sectors(track)) fail("D64 sector outside track");
    for (preceding = 1; preceding < track; ++preceding)
        offset += game_sectors(preceding) * 256u;
    offset += sector * 256u;
    if (offset > size || size - offset < 256) fail("D64 truncated sector");
    return offset;
}

static void game_put24(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
    destination[2] = (uint8_t)(value >> 16);
}

static bool game_t64_signature_matches(const uint8_t *header, size_t header_size,
                                       const char *signature, size_t length)
{
    size_t offset;
    if (length > header_size)
        return false;
    for (offset = 0; offset <= header_size - length; ++offset)
    {
        size_t index;
        for (index = 0; index < length; ++index)
        {
            uint8_t value = header[offset + index];
            if (value >= 'A' && value <= 'Z')
                value = (uint8_t)(value - 'A' + 'a');
            if (value != (uint8_t)signature[index])
                break;
        }
        if (index == length)
            return true;
    }
    return false;
}

static void game_load_disk(const char *path)
{
    GameFile files[255] = {0};
    uint8_t seen_directory[800] = {0};
    uint8_t *disk, *payload;
    size_t size, offset, total, count = 0, file_index;
    unsigned int track = 18, sector = 1;
    disk = game_read(path, &size);
    while (track)
    {
        unsigned int slot;
        offset = game_sector(track, sector, size);
        if (offset / 256 >= sizeof(seen_directory) || seen_directory[offset / 256]++)
            fail("D64 directory loop");
        for (slot = 0; slot < 8; ++slot)
        {
            const uint8_t *entry = disk + offset + slot * 32;
            uint8_t seen_file[800] = {0};
            unsigned int data_track, data_sector;
            GameFile *file;
            if ((entry[2] & 0x87) != 0x82) continue;
            if (count == 255) fail("too many D64 PRGs");
            file = &files[count++];
            memcpy(file->name, entry + 5, 16);
            data_track = entry[3];
            data_sector = entry[4];
            while (data_track)
            {
                size_t position = game_sector(data_track, data_sector, size);
                const uint8_t *block = disk + position;
                size_t length = block[0] ? 254 : block[1] ? block[1] - 1u : 0;
                uint8_t *grown;
                if (position / 256 >= sizeof(seen_file) || seen_file[position / 256]++)
                    fail("D64 PRG chain loop");
                grown = realloc(file->bytes, file->size + length + 1);
                if (!grown) fail("D64 PRG allocation");
                file->bytes = grown;
                memcpy(file->bytes + file->size, block + 2, length);
                file->size += length;
                data_track = block[0];
                data_sector = block[1];
            }
            if (file->size < 2) fail("D64 empty PRG");
            printf("PRG index=%lu name=", (unsigned long)(count - 1));
            for (file_index = 0; file_index < 16 && file->name[file_index] != 0xa0; ++file_index)
                putchar(file->name[file_index] >= 32 && file->name[file_index] < 127 ?
                        file->name[file_index] : '?');
            printf(" bytes=%lu load=%04lx fnv1a=%08lx\n", (unsigned long)file->size,
                   (unsigned long)read_little(file->bytes, 2),
                   (unsigned long)game_hash(file->bytes, file->size));
        }
        track = disk[offset];
        sector = disk[offset + 1];
    }
    if (!count) fail("D64 has no closed PRGs");
    total = 1 + count * 22;
    for (file_index = 0; file_index < count; ++file_index) total += files[file_index].size;
    if (total > mem.flash.size - GAME_DISK_FLASH) fail("D64 package exceeds fixture flash");
    game_disk_size = (uint32_t)total;
    payload = mem.flash.block + GAME_DISK_FLASH;
    payload[0] = (uint8_t)count;
    offset = 1 + count * 22;
    for (file_index = 0; file_index < count; ++file_index)
    {
        uint8_t *entry = payload + 1 + file_index * 22;
        memcpy(entry, files[file_index].name, 16);
        game_put24(entry + 16, (uint32_t)offset);
        game_put24(entry + 19, (uint32_t)files[file_index].size);
        memcpy(payload + offset, files[file_index].bytes, files[file_index].size);
        offset += files[file_index].size;
        free(files[file_index].bytes);
    }
    free(disk);
    printf("PACKAGE DSK files=%lu bytes=%lu flash=%06x fnv1a=%08lx\n",
           (unsigned long)count, (unsigned long)total, GAME_DISK_FLASH,
           (unsigned long)game_hash(payload, total));
}

static void game_load_tape(const char *path)
{
    GameFile files[255] = {0};
    uint8_t *tape, *payload;
    size_t size, offset, total, count = 0, file_index;
    uint16_t entry_count;

    tape = game_read(path, &size);
    if (size < 0x40 || (!game_t64_signature_matches(tape, 32, "c64 tape image file", 19) &&
                         !game_t64_signature_matches(tape, 32, "c64 tape file", 13) &&
                         !game_t64_signature_matches(tape, 32, "c64s tape file", 14)))
        fail("invalid T64 image");
    entry_count = read_little(tape + 0x22, 2);
    if ((size_t)entry_count > (size - (size_t)0x40) / 32u)
        fail("T64 directory extends past image");
    for (file_index = 0; file_index < entry_count; ++file_index)
    {
        const uint8_t *entry = tape + 0x40 + file_index * 32;
        uint16_t start, end;
        uint32_t data_offset;
        GameFile *file;
        if (!entry[0])
            continue;
        if (entry[0] != 1 || (entry[1] & 7) != 2)
            continue;
        start = read_little(entry + 2, 2);
        end = read_little(entry + 4, 2);
        data_offset = read_little(entry + 8, 4);
        if (end < start || (size_t)data_offset < (size_t)0x40 + (size_t)entry_count * 32u ||
            (size_t)data_offset > size || (size_t)(end - start) > size - (size_t)data_offset)
            fail("invalid T64 PRG entry");
        if (count == 255)
            fail("too many T64 PRGs");
        file = &files[count++];
        memcpy(file->name, entry + 16, 16);
        file->bytes = malloc((size_t)(end - start) + 2);
        if (!file->bytes)
            fail("T64 PRG allocation");
        file->bytes[0] = (uint8_t)start;
        file->bytes[1] = (uint8_t)(start >> 8);
        memcpy(file->bytes + 2, tape + data_offset, end - start);
        file->size = (size_t)(end - start) + 2;
    }
    if (!count)
        fail("T64 has no normal PRGs");
    total = 1 + count * 22;
    for (file_index = 0; file_index < count; ++file_index)
        total += files[file_index].size;
    if (total > mem.flash.size - GAME_DISK_FLASH)
        fail("T64 package exceeds fixture flash");
    game_disk_size = (uint32_t)total;
    payload = mem.flash.block + GAME_DISK_FLASH;
    payload[0] = (uint8_t)count;
    offset = 1 + count * 22;
    for (file_index = 0; file_index < count; ++file_index)
    {
        uint8_t *entry = payload + 1 + file_index * 22;
        memcpy(entry, files[file_index].name, 16);
        game_put24(entry + 16, (uint32_t)offset);
        game_put24(entry + 19, (uint32_t)files[file_index].size);
        memcpy(payload + offset, files[file_index].bytes, files[file_index].size);
        offset += files[file_index].size;
        free(files[file_index].bytes);
    }
    free(tape);
    printf("PACKAGE T64 files=%lu bytes=%lu flash=%06x fnv1a=%08lx\n",
           (unsigned long)count, (unsigned long)total, GAME_DISK_FLASH,
           (unsigned long)game_hash(payload, total));
}

static void game_load_media(const char *path)
{
    size_t length = strlen(path);
    if (length >= 4 && path[length - 4] == '.' &&
        (path[length - 3] == 't' || path[length - 3] == 'T') &&
        path[length - 2] == '6' && path[length - 1] == '4')
        game_load_tape(path);
    else
        game_load_disk(path);
}

static uint32_t game_argument(unsigned int offset, unsigned int length)
{
    uint8_t bytes[4];
    unsigned int index;
    for (index = 0; index < length; ++index)
        bytes[index] = mem_peek_byte(cpu.registers.SPL + 3 + offset + index);
    return read_little(bytes, length);
}

static void game_package_hook(unsigned int hook)
{
    uint32_t name_address = game_argument(0, 3);
    char package[4];
    unsigned int index;
    for (index = 0; index < 4; ++index) package[index] = (char)mem_peek_byte(name_address + index);
    cpu.registers.HL = cpu.registers.DE = 0;
    if (hook == 0)
    {
        static const char *const ids[] = {"BAS", "KRN", "CHR"};
        uint32_t destination = game_argument(3, 3);
        uint32_t expected = game_argument(6, 4);
        for (index = 0; index < 3; ++index)
            if (!memcmp(package, ids[index], 4)) break;
        if (index == 3 || expected != game_rom_sizes[index]) fail("unexpected ROM package request");
        if (destination < RAM_BASE || destination + expected > RAM_BASE + SIZE_RAM)
            fail("ROM destination outside target RAM");
        memcpy(mem.ram.block + destination - RAM_BASE, game_roms[index], expected);
        printf("ROM %.3s destination=%06lx bytes=%lu tail=%02x%02x\n", package,
               (unsigned long)destination, (unsigned long)expected,
               mem_peek_byte(destination + expected - 3), mem_peek_byte(destination + expected - 4));
        cpu.registers.HL = 1;
    }
    else
    {
        uint32_t offset = hook == 2 ? game_argument(3, 1) * 0x8000u :
                          hook == 3 ? game_argument(3, 4) : 0;
        if (memcmp(package, "DSK", 4) || offset >= game_disk_size)
            fail("unexpected disk package request");
        cpu.registers.HL = hook == 1 ? game_disk_size : GAME_DISK_FLASH + offset;
    }
    startup_return();
}

static bool game_handle_hook(void)
{
    unsigned int hook;
    if (game_load_return && cpu.registers.PC == game_load_return + 1)
    {
        unsigned int count = mem_peek_byte(symbol_address("_drive_trace_count"));
        if (count > game_load_count && !game_load_mode && !mem_peek_byte(state_address + 6))
        {
            if (count > 16) fail("successful LOAD exceeded trace capacity");
            uint32_t trace = symbol_address("_drive_trace") + (count - 1) * 12;
            unsigned int entry = mem_peek_byte(trace + 6);
            uint16_t destination = (uint16_t)mem_peek_long(trace + 2);
            if (entry >= mem.flash.block[GAME_DISK_FLASH]) fail("successful LOAD has invalid directory entry");
            const uint8_t *record = mem.flash.block + GAME_DISK_FLASH + 1 + entry * 22;
            uint32_t offset = read_little(record + 16, 3);
            uint32_t length = read_little(record + 19, 3);
            if (length < 2 || offset > game_disk_size || length > game_disk_size - offset)
                fail("successful LOAD has invalid payload bounds");
            for (uint32_t index = 2; index < length; ++index)
            {
                uint16_t address = (uint16_t)(destination + index - 2);
                uint8_t actual = mem_peek_byte((address < 0x1000 ? low_address : high_address) + address);
                uint8_t expected = mem.flash.block[GAME_DISK_FLASH + offset + index];
                if (actual != expected)
                {
                    fprintf(stderr, "LOAD RAM[%04x] expected=%02x actual=%02x entry=%u\n",
                            address, expected, actual, entry);
                    fail("drive trap did not copy the selected PRG byte-for-byte");
                }
            }
            printf("PASS LOAD entry=%u destination=%04x bytes=%lu byte-for-byte\n",
                   entry, destination, (unsigned long)(length - 2));
        }
        mem_poke_byte(game_load_return, game_load_return_opcode);
        mem_poke_byte(game_hooks[11].address, 0x76);
        cpu.halted = false;
        cpu_flush(game_load_return, true);
        game_load_return = 0;
        return true;
    }
    for (hook = 0; hook < sizeof(game_hooks) / sizeof(game_hooks[0]); ++hook)
        if (game_hooks[hook].address && cpu.registers.PC == game_hooks[hook].address + 1) break;
    if (hook == sizeof(game_hooks) / sizeof(game_hooks[0])) return false;
    if (hook < 4)
        game_package_hook(hook);
    else if (hook == 11)
    {
        game_load_mode = mem_peek_byte(state_address + 3);
        game_load_count = mem_peek_byte(symbol_address("_drive_trace_count"));
        printf("TRAP LOAD pc=%04lx mode=%u sp=%02x\n",
               (unsigned long)(mem_peek_long(state_address) & 0xffff), game_load_mode,
               mem_peek_byte(state_address + 2));
        game_load_return = mem_peek_long(cpu.registers.SPL);
        game_load_return_opcode = mem_peek_byte(game_load_return);
        mem_poke_byte(game_load_return, 0x76);
        mem_poke_byte(game_hooks[hook].address, game_hooks[hook].opcode);
        cpu.halted = false;
        cpu_flush(game_hooks[hook].address, true);
    }
    else
    {
        if (hook == 10)
        {
            uint32_t left = game_argument(0, 3), right = game_argument(3, 3);
            uint32_t length = game_argument(6, 3), index;
            int result = 0;
            if (length > 65536) fail("unexpected libc memcmp length");
            for (index = 0; index < length && !result; ++index)
                result = (int)mem_peek_byte(left + index) - (int)mem_peek_byte(right + index);
            cpu.registers.HL = (uint32_t)result & 0xffffff;
        }
        if (hook == 8)
        {
            uint32_t address = game_argument(0, 3);
            unsigned int length = 0;
            while (length < 100 && mem_peek_byte(address + length)) ++length;
            cpu.registers.HL = length * 8u;
        }
        if (hook == 9)
        {
            uint32_t row = game_argument(6, 3);
            if (row >= 20 && row < 220) fail("TI text call attempted to replace the guest image");
        }
        startup_return();
    }
    return true;
}

static bool game_at_hook_entry(void)
{
    unsigned int hook;
    for (hook = 0; hook < sizeof(game_hooks) / sizeof(game_hooks[0]); ++hook)
        if (game_hooks[hook].address && cpu.registers.PC == game_hooks[hook].address &&
            mem_peek_byte(cpu.registers.PC) == 0x76)
            return true;
    return false;
}

static void game_begin(const char *name, uint32_t argument)
{
    mem_poke_byte(RETURN_ADDRESS, 0x76);
    mem_poke_long(STACK_ADDRESS, RETURN_ADDRESS);
    mem_poke_long(STACK_ADDRESS + 3, argument);
    cpu.registers.SPL = STACK_ADDRESS;
    cpu.registers.IX = 0x123456;
    cpu.registers.IY = 0xabcdef;
    cpu.halted = false;
    cpu.IEF1 = cpu.IEF2 = cpu.IEF_wait = cpu.NMI = false;
    cpu.ADL = true;
    cpu_flush(symbol_address(name), true);
    cpu_clear_signals();
}

static uint8_t game_call(const char *name, uint32_t argument)
{
    unsigned int step;
    game_begin(name, argument);
    for (step = 0; step < 2048; ++step)
    {
        cemu_throttle_run_until_halt(1000);
        if (!cpu.halted || cpu_check_signals() != CPU_SIGNAL_NONE)
        {
            fprintf(stderr, "%s stopped pc=%06x sp=%06x signals=%u\n", name,
                    cpu.registers.PC, cpu.registers.SPL, cpu_check_signals());
            fail("target initialization exceeded clock bound or faulted");
        }
        if (cpu.registers.PC == RETURN_ADDRESS + 1)
        {
            if (cpu.registers.SPL != STACK_ADDRESS + 3 || cpu.registers.IX != 0x123456)
                fail("game initialization C ABI mismatch");
            return cpu.registers.A;
        }
        if (!game_handle_hook()) fail("unexpected target halt");
    }
    fail("target package call limit exceeded");
    return 0;
}

static void game_put32(uint8_t *destination, uint32_t value)
{
    game_put24(destination, value);
    destination[3] = (uint8_t)(value >> 24);
}

static void game_bitmap(unsigned int frame)
{
    static const uint8_t palette[16][3] = {
        {0x00,0x00,0x00}, {0xff,0xff,0xff}, {0xab,0x31,0x26}, {0x66,0xda,0xff},
        {0xbb,0x3f,0xb8}, {0x55,0xce,0x58}, {0x1d,0x0e,0x97}, {0xea,0xf5,0x7c},
        {0xb9,0x74,0x18}, {0x78,0x53,0x00}, {0xdd,0x93,0x87}, {0x5b,0x5b,0x5b},
        {0x8b,0x8b,0x8b}, {0xb0,0xf4,0xac}, {0xaa,0x9d,0xef}, {0xb8,0xb8,0xb8}
    };
    char path[512];
    uint8_t bmp[54] = {'B', 'M'};
    uint8_t row[320 * 3];
    FILE *file;
    unsigned int row_index, column;
    if (snprintf(path, sizeof(path), "%s-f%06u.bmp", game_output, frame) >= (int)sizeof(path))
        fail("snapshot path too long");
    game_put32(bmp + 2, sizeof(bmp) + 320 * 200 * 3);
    game_put32(bmp + 10, sizeof(bmp));
    game_put32(bmp + 14, 40);
    game_put32(bmp + 18, 320);
    game_put32(bmp + 22, 200);
    bmp[26] = 1;
    bmp[28] = 24;
    file = fopen(path, "wb");
    if (!file || fwrite(bmp, 1, sizeof(bmp), file) != sizeof(bmp)) fail("cannot write BMP header");
    for (row_index = 0; row_index < 200; ++row_index)
    {
        const uint8_t *pixels = mem.ram.block + 0x40000 + (219 - row_index) * 320;
        for (column = 0; column < 320; ++column)
        {
            const uint8_t *color = palette[pixels[column] & 15];
            row[column * 3] = color[2];
            row[column * 3 + 1] = color[1];
            row[column * 3 + 2] = color[0];
        }
        if (fwrite(row, 1, sizeof(row), file) != sizeof(row)) fail("cannot write BMP pixels");
    }
    if (fclose(file)) fail("cannot finish BMP snapshot");
    printf("IMAGE %s 320x200 actual VIC framebuffer\n", path);
}

static void game_load_trace(void)
{
    uint32_t address = symbol_address("_drive_trace");
    unsigned int count = mem_peek_byte(symbol_address("_drive_trace_count"));
    unsigned int index;
    for (index = 0; index < count && index < 16; ++index)
    {
        const uint8_t *record = mem.ram.block + address - RAM_BASE + index * 12;
        printf("LOAD index=%u caller=%04lx address=%04lx end=%04lx entry=%u sa=%u name=%02x%02x%02x\n",
               index, (unsigned long)read_little(record, 2), (unsigned long)read_little(record + 2, 2),
               (unsigned long)read_little(record + 4, 2), record[6], record[7], record[9], record[10], record[11]);
    }
}

static void game_dump_jit_cache(void)
{
    enum { JIT_BLOCK_COUNT = 16, JIT_BLOCK_STRIDE = 586, JIT_NATIVE_SIZE = 8, JIT_READY = 5,
           JIT_GUEST = 24, JIT_NATIVE = 72 };
    uint32_t blocks_address = symbol_address("_jit_blocks");
    unsigned int slot;
    for (slot = 0; slot < JIT_BLOCK_COUNT; ++slot)
    {
        uint32_t block = blocks_address + slot * JIT_BLOCK_STRIDE;
        unsigned int index, native_size = mem_peek_byte(block + JIT_NATIVE_SIZE) |
                                         (mem_peek_byte(block + JIT_NATIVE_SIZE + 1) << 8);
        if (!mem_peek_byte(block + JIT_READY))
            continue;
        if (native_size > 512) fail("JIT cache metadata has an invalid native size");
        printf("JIT_BLOCK slot=%u pc=%04x guest=", slot,
               mem_peek_byte(block) | (mem_peek_byte(block + 1) << 8));
        for (index = 0; index < mem_peek_byte(block + 4); ++index)
            printf("%02x", mem_peek_byte(block + JIT_GUEST + index));
        printf(" native_size=%u native=", native_size);
        for (index = 0; index < native_size; ++index)
            printf("%02x", mem_peek_byte(block + JIT_NATIVE + 1 + index));
        putchar('\n');
    }
}

static void game_dump_jit_guards(unsigned int frame)
{
    enum { JIT_BLOCK_COUNT = 16, JIT_BLOCK_STRIDE = 586, JIT_CODE_SIZE = 4, JIT_READY = 5,
           JIT_INELIGIBLE = 6, JIT_BLACKLISTED = 7, JIT_NATIVE = 72, JIT_NATIVE_END = 585 };
    uint32_t blocks_address = symbol_address("_jit_blocks");
    unsigned int slot;
    for (slot = 0; slot < JIT_BLOCK_COUNT; ++slot)
    {
        uint32_t block = blocks_address + slot * JIT_BLOCK_STRIDE;
        uint8_t first = mem_peek_byte(block + JIT_NATIVE);
        uint8_t last = mem_peek_byte(block + JIT_NATIVE_END);
        if ((first == 0x5a && last == 0xa5) || (!first && !last))
            continue;
        printf("JIT_GUARD frame=%u slot=%u pc=%04x size=%u ready=%u ineligible=%u blacklisted=%u first=%02x last=%02x\n",
               frame, slot, mem_peek_byte(block) | (mem_peek_byte(block + 1) << 8),
               mem_peek_byte(block + JIT_CODE_SIZE), mem_peek_byte(block + JIT_READY),
               mem_peek_byte(block + JIT_INELIGIBLE), mem_peek_byte(block + JIT_BLACKLISTED),
               first, last);
    }
}

static void game_dump_page_table(const char *stage)
{
    uint32_t reads = symbol_address("_mem_rd_pg");
    uint32_t writes = symbol_address("_mem_wr_pg");
    fprintf(stderr, "PAGE_TABLE %s a0_rd=%06lx a0_wr=%06lx af_rd=%06lx af_wr=%06lx\n", stage,
            (unsigned long)mem_peek_long(reads + 0xa0 * 4),
            (unsigned long)mem_peek_long(writes + 0xa0 * 4),
            (unsigned long)mem_peek_long(reads + 0xaf * 4),
            (unsigned long)mem_peek_long(writes + 0xaf * 4));
}

static void game_verify_jit_cache(void)
{
    enum { JIT_BLOCK_COUNT = 16, JIT_BLOCK_STRIDE = 586, JIT_CODE_SIZE = 4, JIT_READY = 5,
           JIT_NATIVE_SIZE = 8, JIT_GUEST = 24, JIT_NATIVE = 72 };
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
    uint32_t blocks_address = symbol_address("_jit_blocks");
    unsigned int slot;

    for (slot = 0; slot < JIT_BLOCK_COUNT; ++slot)
    {
        uint32_t block = blocks_address + slot * JIT_BLOCK_STRIDE;
        uint8_t expected[512];
        JitTranslation translation;
        uint16_t actual_size, offset;
        if (!mem_peek_byte(block + JIT_READY))
            continue;
        actual_size = mem_peek_byte(block + JIT_NATIVE_SIZE) |
                      ((uint16_t)mem_peek_byte(block + JIT_NATIVE_SIZE + 1) << 8);
        if (!jit_compile(expected, sizeof(expected), mem.ram.block + block + JIT_GUEST - RAM_BASE,
                         mem_peek_byte(block + JIT_CODE_SIZE),
                         mem_peek_byte(block) | ((uint16_t)mem_peek_byte(block + 1) << 8),
                         &target, &translation))
        {
            fprintf(stderr, "JIT_NATIVE_REBUILD slot=%u failed\n", slot);
            continue;
        }
        if (actual_size != translation.native_size)
        {
            fprintf(stderr, "JIT_NATIVE_SIZE slot=%u actual=%u expected=%u\n",
                    slot, actual_size, translation.native_size);
            continue;
        }
        for (offset = 0; offset < actual_size; ++offset)
            if (mem_peek_byte(block + JIT_NATIVE + 1 + offset) != expected[offset])
            {
                fprintf(stderr, "JIT_NATIVE_DIFF slot=%u pc=%04x offset=%u actual=%02x expected=%02x\n",
                        slot, mem_peek_byte(block) | (mem_peek_byte(block + 1) << 8), offset,
                        mem_peek_byte(block + JIT_NATIVE + 1 + offset), expected[offset]);
                break;
            }
    }
}

#ifdef GAME_JIT_WATCH
static uint32_t game_jit_watch_address;
static unsigned int game_jit_watch_sequence;

static void game_watch_10k_cache(void)
{
    enum { JIT_BLOCK_STRIDE = 586, JIT_NATIVE = 72 };
    uint32_t blocks_address = symbol_address("_jit_blocks");

    if (!game_jit_watch_address)
    {
        game_jit_watch_address = blocks_address + JIT_NATIVE + 1 + 19;
        target_debug_page_entry = symbol_address("_mem_wr_pg") + 0xaf * 4;
        debug_watch(game_jit_watch_address, DBG_MASK_WRITE, true);
        printf("JIT_NATIVE_WATCH armed=%06lx slot=0 offset=19\n",
               (unsigned long)game_jit_watch_address);
    }
    if (target_debug_sequence != game_jit_watch_sequence)
    {
        if (target_debug_a == 0xc2)
            fprintf(stderr, "JIT_NATIVE_WATCH sequence=%u reason=%u address=%06lx target_pc=%06lx sp=%06lx ix=%06lx iy=%06lx hl=%06lx de=%06lx bc=%06lx af_wr=%06lx guest_pc=%04lx value=%02x\n",
                target_debug_sequence, target_debug_reason, (unsigned long)target_debug_data,
                (unsigned long)target_debug_pc, (unsigned long)target_debug_sp,
                (unsigned long)target_debug_ix, (unsigned long)target_debug_iy,
                (unsigned long)target_debug_hl, (unsigned long)target_debug_de,
                (unsigned long)target_debug_bc, (unsigned long)target_debug_page_value,
                (unsigned long)(mem_peek_long(state_address) & 0xffff),
                mem_peek_byte(game_jit_watch_address));
        game_jit_watch_sequence = target_debug_sequence;
    }
}
#endif

static uint32_t game_query(const char *name, uint32_t argument, bool byte_result)
{
    uint8_t saved_registers[sizeof(cpu.registers)];
    uint32_t result;
    memcpy(saved_registers, &cpu.registers, sizeof(saved_registers));
    if (cpu.registers.SPL < 0xd04000) fail("live game stack reached observation stack");
    mem_poke_long(0xd03000, RETURN_ADDRESS);
    mem_poke_long(0xd03003, argument);
    cpu.registers.SPL = 0xd03000;
    cpu.halted = false;
    cpu_flush(symbol_address(name), true);
    cemu_throttle_run(1);
    if (!cpu.halted || cpu.registers.PC != RETURN_ADDRESS + 1 || cpu.registers.SPL != 0xd03003)
        fail("compiled observation query failed");
    result = byte_result ? cpu.registers.A : cpu.registers.HL;
    memcpy(&cpu.registers, saved_registers, sizeof(saved_registers));
    return result;
}

static void game_escape_trace(void)
{
    uint32_t position, stack_end = cpu.registers.SPL + 48;
    fprintf(stderr, "ESCAPE target_pc=%06x sp=%06x ix=%06x guest_pc=%04lx\n",
            cpu.registers.PC, cpu.registers.SPL, cpu.registers.IX,
            (unsigned long)(mem_peek_long(state_address) & 0xffff));
    fputs("STACK_RAW", stderr);
    if (stack_end < cpu.registers.SPL || stack_end > STACK_ADDRESS + 3)
        stack_end = STACK_ADDRESS + 3;
    for (position = cpu.registers.SPL; position < stack_end; ++position)
        fprintf(stderr, " %02x", mem_peek_byte(position));
    fputc('\n', stderr);
    for (position = cpu.registers.SPL; position < STACK_ADDRESS + 3 && position < cpu.registers.SPL + 4096; ++position)
    {
        uint32_t address = mem_peek_long(position);
        if (address >= 0xd10004 && address < 0xd40000 && mem_peek_byte(address - 4) == 0xcd)
            fprintf(stderr, "STACK at=%06lx return=%06lx call=%06lx\n", (unsigned long)position,
                    (unsigned long)address, (unsigned long)mem_peek_long(address - 3));
    }
}

static void game_run(unsigned int limit)
{
    uint32_t boundary = game_find_symbol("_io_fps_overlay");
    uint32_t hits_address = symbol_address("_jit_native_hits");
    uint32_t observations_address = symbol_address("_jit_block_observations");
    uint32_t cached_address = symbol_address("_jit_cached_block_count");
    uint32_t ready_address = symbol_address("_jit_ready_block_count");
    uint32_t ineligible_address = symbol_address("_jit_ineligible_block_count");
    uint32_t mismatches_address = symbol_address("_jit_native_mismatches");
    uint32_t loads_address = symbol_address("_drive_trace_count");
    uint32_t return_probe = 0;
    uint8_t boundary_opcode, return_opcode = 0;
    unsigned int frame = game_checkpoint.frame, since_frame = 0;
    unsigned int changed_frames = game_checkpoint.changed, nonblank_frames = game_checkpoint.nonblank;
    unsigned int previous_mismatches = mem_peek_long(mismatches_address);
    unsigned int ram_code_frames = 0;
    uint8_t previous[320 * 200] = {0}, visible[320 * 200];
    char record_path[512];
    FILE *records;
    bool before_count = !boundary;
    if (snprintf(record_path, sizeof(record_path), "%s.csv", game_output) >= (int)sizeof(record_path))
        fail("record path too long");
    if (frame >= limit) fail("requested frame bound must exceed checkpoint frame");
    records = fopen(record_path, game_resumed ? "a" : "w");
    if (!records) fail("cannot open frame record");
    setvbuf(records, NULL, _IONBF, 0);
    if (fseek(records, 0, SEEK_END)) fail("cannot position frame record");
    if (!ftell(records))
    {
        fprintf(records, "# elf_fnv1a=%08lx jit=%s input=%s ti_os=none\n",
                (unsigned long)game_hash(image, image_size), game_jit_enabled ? "on" : "off", game_input_script);
        fputs("frame,pc,cycles,native,observations,cached,ready,ineligible,mismatches,loads,colors,framebuffer,ram,backend,input,joystick\n", records);
    }
    if (game_resumed)
    {
        size_t index;
        fprintf(records, "# resumed=%u input=%s\n", frame, game_input_script);
        for (index = 0; index < sizeof(previous); ++index)
            previous[index] = mem.ram.block[0x40000 + 20 * 320 + index] & 15;
    }
    if (!boundary) boundary = symbol_address("_io_screen_refresh");
    boundary_opcode = mem_peek_byte(boundary);
    mem_poke_byte(boundary, 0x76);
#ifdef GAME_JIT_WATCH
    game_watch_10k_cache();
#endif
    printf("RUN jit=%s frames=%u boundary=%06lx counted=%s TI-OS=none HUD-text=stubbed\n",
           game_jit_enabled ? "on" : "off", limit, (unsigned long)boundary,
           before_count ? "completed-render-before-counter" : "counter-incremented");
        game_apply_input(frame + 1);
        if (game_resumed)
        {
            return_probe = mem_peek_long(cpu.registers.SPL);
            return_opcode = mem_peek_byte(return_probe);
            mem_poke_byte(return_probe, 0x76);
            mem_poke_byte(boundary, boundary_opcode);
            cpu.halted = false;
            cpu_flush(boundary, true);
        }
        else
        {
            game_begin("_c64_run", 0);
        }
    while (frame < limit)
    {
        cemu_throttle_run(1);
#ifdef GAME_JIT_WATCH
        game_watch_10k_cache();
#endif
        if (cpu.halted && game_handle_hook()) continue;
        if ((cpu.registers.PC < RAM_BASE || cpu.registers.PC >= 0xd40000) && !game_at_hook_entry())
        {
            game_escape_trace();
            game_dump_page_table("escape");
            game_verify_jit_cache();
            game_dump_jit_cache();
            game_bitmap(frame);
            fail("execution left linked application/native RAM");
        }
        if (++since_frame > 30000)
        {
            static const struct {
                const char *name;
                uint8_t size;
            } scheduler_fields[] = {
                {"_vic_raster_due", 3}, {"_frame_count", 3},
                {"_raster_c", 1}, {"_cr1", 1}, {"_render_frame", 1},
                {"_render_suppressed", 1}, {"_boot_hidden", 1},
                {"_render_requested", 1}, {"_frame_skip_enabled", 1},
                {"until_cyc", 3}
            };
            unsigned int index;
            uint32_t pages = symbol_address("_mem_rd_pg");
            uint16_t guest_pc = (uint16_t)mem_peek_long(state_address);
            fprintf(stderr, "TIMEOUT frame=%u target_pc=%06x guest_pc=%04lx\n", frame,
                    cpu.registers.PC, (unsigned long)(mem_peek_long(state_address) & 0xffff));
            fprintf(stderr, "TARGET sp=%06x ix=%06x iy=%06x hl=%06x de=%06x bc=%06x halted=%u signals=%u\n",
                    cpu.registers.SPL, cpu.registers.IX, cpu.registers.IY, cpu.registers.HL,
                    cpu.registers.DE, cpu.registers.BC, cpu.halted, cpu_check_signals());
            fprintf(stderr, "STATE bank=%02x warp=%u bytes=", mem_peek_byte(low_address + 1),
                    mem_peek_byte(symbol_address("_cpu_warp")));
            for (index = 0; index < 16; ++index)
                fprintf(stderr, "%02x ", mem_peek_byte(state_address + index));
            fputc('\n', stderr);
            fputs("SCHED", stderr);
            for (index = 0; index < sizeof(scheduler_fields) / sizeof(scheduler_fields[0]); ++index)
            {
                uint32_t address = game_find_symbol(scheduler_fields[index].name);
                if (address)
                    fprintf(stderr, " %s=%06lx", scheduler_fields[index].name,
                            (unsigned long)(scheduler_fields[index].size == 1
                                ? mem_peek_byte(address) : mem_peek_long(address)));
            }
            fprintf(stderr, " boundary=%06lx opcode=%02x return_probe=%06lx\n",
                    (unsigned long)boundary, mem_peek_byte(boundary), (unsigned long)return_probe);
            for (index = 0; index < 2; ++index)
            {
                unsigned int offset;
                uint16_t start = index ? (uint16_t)cpu.registers.IY : guest_pc;
                fprintf(stderr, "VISIBLE guest=%04x bytes=", start);
                for (offset = 0; offset < 16; ++offset)
                {
                    uint16_t address = (uint16_t)(start + offset);
                    uint32_t page = mem_peek_long(pages + (address >> 8) * 4);
                    if (page) fprintf(stderr, "%02x ", mem_peek_byte((page + address) & 0xffffff));
                    else fputs("IO ", stderr);
                }
                fputc('\n', stderr);
            }
            game_escape_trace();
            game_bitmap(frame);
            fail("no completed guest frame within 30 target seconds");
        }
        if (!cpu.halted) continue;
        if (cpu.registers.PC == boundary + 1)
        {
            size_t index;
            uint16_t colors = 0;
            uint16_t guest_pc = (uint16_t)mem_peek_long(state_address);
            uint32_t code_page = mem_peek_long(symbol_address("_mem_rd_pg") + (guest_pc >> 8) * 4);
            uint32_t completed = game_query("_vic_frames", 0, false) + (before_count ? 1u : 0u);
            unsigned int mismatches = mem_peek_long(mismatches_address);
            uint8_t joystick = (uint8_t)game_query("_io_joystick2", 0, true);
            if (completed != frame + 1) fail("non-monotonic target VIC frame count");
            frame = completed;
            since_frame = 0;
            if (mem_peek_byte(loads_address) && (code_page == low_address || code_page == high_address))
                ++ram_code_frames;
            for (index = 0; index < sizeof(visible); ++index)
            {
                visible[index] = mem.ram.block[0x40000 + 20 * 320 + index] & 15;
                colors |= (uint16_t)(1u << visible[index]);
            }
            if (colors & (colors - 1)) ++nonblank_frames;
            if (memcmp(visible, previous, sizeof(visible))) ++changed_frames;
            memcpy(previous, visible, sizeof(previous));
                memcpy(guest_ram, mem.ram.block + low_address - RAM_BASE, 0x1000);
                memcpy(guest_ram + 0x1000, mem.ram.block + high_address - RAM_BASE + 0x1000, 0xf000);
                fprintf(records, "%u,%04lx,%lu,%lu,%lu,%u,%u,%u,%lu,%u,%04x,%08lx,%08lx,%u,%03lx,%02x\n", frame,
                    (unsigned long)(mem_peek_long(state_address) & 0xffff),
                    (unsigned long)mem_peek_long(state_address + 13),
                    (unsigned long)mem_peek_long(hits_address), (unsigned long)mem_peek_long(observations_address),
                    mem_peek_byte(cached_address), mem_peek_byte(ready_address), mem_peek_byte(ineligible_address),
                    (unsigned long)mismatches, mem_peek_byte(loads_address), colors,
                    (unsigned long)game_hash(visible, sizeof(visible)),
                    (unsigned long)game_hash(guest_ram, sizeof(guest_ram)),
                    mem_peek_byte(symbol_address("_jit_backend_ok")), (unsigned long)game_input_mask, joystick);
            if (frame == 1 || frame % 15 == 0 || frame == limit)
            {
                  printf("FRAME jit=%s frame=%u pc=%04lx cycles=%lu native=%lu observations=%lu cached=%u ready=%u ineligible=%u mismatches=%lu loads=%u colors=%04x fb=%08lx joy=%02x\n",
                       game_jit_enabled ? "on" : "off", frame,
                       (unsigned long)(mem_peek_long(state_address) & 0xffff),
                       (unsigned long)mem_peek_long(state_address + 13),
                      (unsigned long)mem_peek_long(hits_address), (unsigned long)mem_peek_long(observations_address),
                      mem_peek_byte(cached_address), mem_peek_byte(ready_address), mem_peek_byte(ineligible_address),
                      (unsigned long)mismatches, mem_peek_byte(loads_address),
                       colors, (unsigned long)game_hash(visible, sizeof(visible)), joystick);
            }
            if (mismatches != previous_mismatches)
            {
                game_dump_jit_guards(frame);
                game_verify_jit_cache();
                previous_mismatches = mismatches;
            }
            if (frame == 1 || frame % 60 == 0 || frame == limit) game_bitmap(frame);
            if (frame == limit) break;
            game_apply_input(frame + 1);
            return_probe = mem_peek_long(cpu.registers.SPL);
            return_opcode = mem_peek_byte(return_probe);
            mem_poke_byte(return_probe, 0x76);
            mem_poke_byte(boundary, boundary_opcode);
            cpu.halted = false;
            cpu_flush(boundary, true);
        }
        else if (return_probe && cpu.registers.PC == return_probe + 1)
        {
            mem_poke_byte(return_probe, return_opcode);
            mem_poke_byte(boundary, 0x76);
            cpu.halted = false;
            cpu_flush(return_probe, true);
            return_probe = 0;
        }
        else if (cpu.registers.PC == RETURN_ADDRESS + 1)
        {
            fprintf(stderr, "EARLY EXIT frame=%u pc=%04lx illegal=%02x illegal_pc=%04lx\n", frame,
                    (unsigned long)(mem_peek_long(state_address) & 0xffff),
                    mem_peek_byte(symbol_address("_cpu_illegal_op")),
                    (unsigned long)(mem_peek_long(symbol_address("_cpu_illegal_pc")) & 0xffff));
            game_bitmap(frame);
            game_load_trace();
            fail("compiled c64_run returned before requested frame bound");
        }
        else if (!game_handle_hook())
        {
            fprintf(stderr, "UNEXPECTED HALT target_pc=%06x guest_pc=%04lx\n", cpu.registers.PC,
                    (unsigned long)(mem_peek_long(state_address) & 0xffff));
            fail("unhandled target/library boundary");
        }
    }
    mem_poke_byte(boundary, boundary_opcode);
    game_load_trace();
    game_dump_jit_cache();
        printf("RESULT jit=%s frames=%u pc=%04lx native=%lu observations=%lu cached=%u ready=%u ineligible=%u mismatches=%lu loads=%u nonblank_frames=%u changed_frames=%u\n",
           game_jit_enabled ? "on" : "off", frame,
           (unsigned long)(mem_peek_long(state_address) & 0xffff),
            (unsigned long)mem_peek_long(hits_address), (unsigned long)mem_peek_long(observations_address),
            mem_peek_byte(cached_address), mem_peek_byte(ready_address), mem_peek_byte(ineligible_address),
            (unsigned long)mem_peek_long(mismatches_address), mem_peek_byte(loads_address), nonblank_frames, changed_frames);
    if (!game_jit_enabled && mem_peek_long(hits_address)) fail("disabled JIT executed native code");
    printf("STARTUP post_load_ram_code_frames=%u\n", ram_code_frames);
    if (limit >= 120 && !ram_code_frames)
        fail("bounded run never reached loaded RAM code; BASIC screen is not a game startup");
    if (limit >= 120 && (!mem_peek_byte(loads_address) || !nonblank_frames || !changed_frames))
    {
        fail("bounded run did not load a PRG and produce changing nonblank frames");
    }
    fprintf(records, "# completed=%u nonblank_frames=%u changed_frames=%u gameplay=unconfirmed\n",
            frame, nonblank_frames, changed_frames);
    if (fclose(records)) fail("cannot finish frame record");
    printf("RECORD %s completed=%u gameplay=unconfirmed\n", record_path, frame);
    game_save_checkpoint(frame, nonblank_frames, changed_frames);
}

int main(int argc, char **argv)
{
    static const char *const rom_paths[] = {
        "../../assets/roms/basic.901226-01.bin", "../../assets/roms/kernal.bin",
        "../../assets/roms/characters.901225-01.bin"
    };
    unsigned int index;
    unsigned int frames = argc > 2 ? (unsigned int)strtoul(argv[2], NULL, 10) : 120;
    unsigned int selected = argc > 4 ? (unsigned int)strtoul(argv[4], NULL, 10) : 0;
    if (argc > 3)
    {
        if (strcmp(argv[3], "on") && strcmp(argv[3], "off")) fail("JIT mode must be on or off");
        game_jit_enabled = !strcmp(argv[3], "on");
    }
    if (argc > 5) game_output = argv[5];
    if (argc > 6) game_input_script = argv[6];
    if (argc > 7) game_resume_path = argv[7];
    if (argc > 8) game_disk_path = argv[8];
    if (frames > 100000 || selected > 254) fail("game harness argument outside bounds");
    game_selected = selected;
    game_parse_input();
    setvbuf(stdout, NULL, _IONBF, 0);
    asic_init();
#ifdef GAME_JIT_WATCH
    debug_init();
#endif
    mem.flash.size = SIZE_FLASH_MIN;
    mem.flash.block = malloc(mem.flash.size);
    if (!mem.flash.block) fail("game flash fixture allocation");
    memset(mem.flash.block, 0xff, mem.flash.size);
    asic_reset();
    keypad.rows = keypad.cols = 8;
    keypad.rowWait = 64;
    keypad.scanWait = 1024;
    emu_set_keypad_ghosting(0);
    emu_set_run_rate(1000);
    load_image(argc > 1 ? argv[1] : "../../bin/EMUDORE.obj");
    printf("ELF bytes=%lu fnv1a=%08lx\n", (unsigned long)image_size,
           (unsigned long)game_hash(image, image_size));
    for (index = 0; index < 3; ++index)
    {
        size_t size;
        game_roms[index] = game_read(rom_paths[index], &size);
        if (size != game_rom_sizes[index]) fail("C64 ROM length mismatch");
    }
    game_load_media(game_disk_path);
    target_call("_mem_init", 0);
    if (!target_call("_jit_backend_selftest", 0)) fail("JIT startup self-test failed");
    if (!target_call("_cpu_asm_selftest", 0)) fail("CPU assembly self-test failed");
    for (index = 0; index < sizeof(game_hooks) / sizeof(game_hooks[0]); ++index)
    {
        game_hooks[index].address = index < 4 ? symbol_address(game_hooks[index].name) :
                                             game_find_symbol(game_hooks[index].name);
        if (!game_hooks[index].address) continue;
        game_hooks[index].opcode = mem_peek_byte(game_hooks[index].address);
        mem_poke_byte(game_hooks[index].address, 0x76);
    }
    if (strcmp(game_resume_path, "none"))
    {
        game_restore_checkpoint();
    }
    else
    {
        game_call("_io_init", 0);
        game_call("_mem_init", 0);
        if (!game_call("_c64_boot_roms", 0)) fail("compiled C64 ROM startup failed");
        memcpy(mem.flash.block + GAME_DISK_FLASH - 4, "DSK", 4);
        if (!game_call("_drive_init", GAME_DISK_FLASH - 4)) fail("compiled drive initialization failed");
        game_call("_drive_select_file", selected);
        game_call("_drive_patch_kernal", 0);
        game_call("_cia1_reset", 0);
        game_call("_cia2_reset", 0);
        game_call("_vic_reset", 0);
        game_call("_cpu_reset", 0);
        game_call("_c64_mark_ready", 0);
    }
    state_address = symbol_address("_cpu");
    low_address = mem_peek_long(symbol_address("_mem_low"));
    high_address = mem_peek_long(symbol_address("_mem_ram"));
    game_dump_page_table("startup");
        if (!game_resumed)
        {
            printf("STARTUP pc=%04lx drive_files=%u backend=%u low=%06lx high=%06lx port=%02x\n",
                   (unsigned long)(mem_peek_long(state_address) & 0xffff), game_call("_drive_file_count", 0),
                   mem_peek_byte(symbol_address("_jit_backend_ok")),
                   (unsigned long)low_address, (unsigned long)high_address, mem_peek_byte(low_address + 1));
            if ((mem_peek_long(state_address) & 0xffff) != read_little(game_roms[1] + 0x1ffc, 2))
                fail("compiled reset PC differs from local KERNAL reset vector");
            puts("PASS compiled C64 startup and local KERNAL reset vector");
            if (game_find_symbol("_vic_asm_status"))
            {
                printf("RENDERER native_rows_mask=%u\n", game_call("_vic_asm_status", 0));
            }
            else
            {
                puts("RENDERER target defaults retained; optional status accessor not linked");
            }
        }
        if (!game_jit_enabled) mem_poke_byte(symbol_address("_jit_backend_ok"), 0);
        if (frames) game_run(frames);
    for (index = 0; index < 3; ++index) free(game_roms[index]);
    free(image);
    asic_free();
    return 0;
}