/*
 * GEOS XOR decryption test - to help identify what's missing from our bootstrap
 */
#define main target_unit_main
#include "target_test.c"
#undef main

// We'll create a simple check that validates the memory content at key addresses
int main(int argc, char **argv)
{
    // Standard setup for JIT tests 
    static const char *const rom_paths[] = {
        "../../assets/roms/basic.901226-01.bin", "../../assets/roms/kernal.bin",
        "../../assets/roms/characters.901225-01.bin"
    };
    
    emu_init();
    
    printf("GEOS XOR decryption test\n");
    
    // Load GEOS disk 
    const char *game_disk_path = "../GEOS64.d64";
    
    // Run a frame to let the system process
    emu_frame();
    
    // Check if we've called the virtual driver yet
    unsigned int geos_calls = mem_peek_long(symbol_address("_drive_geos_call_count"));
    printf("Virtual driver calls: %d\n", geos_calls);
    
    // Print some memory contents that might show us what's missing
    printf("Memory at $8610 (should be decrypted data): ");
    for(int i=0; i<32; ++i) {
        uint8_t val = mem_peek_byte(0x8610 + i);
        printf("%02x ", val);
    }
    printf("\n");
    
    // Print memory at key GEOS addresses to understand state
    printf("Memory at $D570 (resident code): ");
    for(int i=0; i<32; ++i) {
        uint8_t val = mem_peek_byte(0xd570 + i);
        printf("%02x ", val);
    }
    printf("\n");
    
    // Check if we have any of the expected memory patterns
    printf("Checking for known GEOS signatures:\n");
    uint32_t addr = 0x8610;
    bool found_pattern = false;
    
    // Look for potential XOR-decoded pattern (this is just a test to see what's there)
    for(int i=0; i<16 && !found_pattern; ++i) {
        uint8_t val = mem_peek_byte(addr + i);
        if(val != 0x00) { // Non-zero values might indicate decoded data
            printf("Non-zero byte at $%04X: %02X\n", addr+i, val);
        }
    }
    
    return 0;
}