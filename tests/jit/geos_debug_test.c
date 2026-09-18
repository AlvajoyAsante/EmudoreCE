/*
 * Simple GEOS debug test to understand what's happening with the bootstrap.
 */
#define main target_unit_main
#include "target_test.c"
#undef main

// This is a minimal version of the game test that focuses on GEOS loading and debugging
int main(int argc, char **argv)
{
    // Standard setup for JIT tests
    static const char *const rom_paths[] = {
        "../../assets/roms/basic.901226-01.bin", "../../assets/roms/kernal.bin",
        "../../assets/roms/characters.901225-01.bin"
    };
    
    // Load the GEOS disk
    const char *game_disk_path = "../GEOS64.d64";
    uint8_t *disk, *payload;
    size_t size, offset, total;
    unsigned int track = 17, sector = 0; /* GEOS launcher */
    bool geos_found = false;

    // Initialize the system
    emu_init();
    
    printf("Starting GEOS debug test\n");
    
    // Load and parse D64
    disk = game_read(game_disk_path, &size);
    if (!disk) {
        fprintf(stderr, "Failed to read GEOS disk\n");
        return 1;
    }
    
    // Look for GEOS boot entry first - this is what we expect to find
    while (track)
    {
        unsigned int slot;
        offset = game_sector(track, sector, size);
        if (offset / 256 >= sizeof(seen_directory) || seen_directory[offset / 256]++)
            fail("D64 directory loop");
        
        for (slot = 0; slot < 8; ++slot)
        {
            const uint8_t *entry = disk + offset + slot * 32;
            if ((entry[2] & 0x87) != 0x82) continue;
            
            // Check if this is a GEOS entry
            if (entry[5] == 'G' && entry[6] == 'E' && entry[7] == 'O' && entry[8] == 'S')
            {
                geos_found = true; 
                printf("Found GEOS entry at track %d, sector %d\n", track, sector);
                
                // Now let's load the actual file to see what we get
                unsigned int data_track = entry[3];
                unsigned int data_sector = entry[4];
                
                if (data_track && data_sector)
                {
                    printf("GEOS data at track=%d sector=%d\n", data_track, data_sector);
                    
                    // This is where the actual loading happens in drive.c
                    size_t position = game_sector(data_track, data_sector, size);
                    const uint8_t *block = disk + position;
                    printf("Block at %lx: first 16 bytes:", (unsigned long)position);
                    for(int i=0; i<16 && i < block[0]; ++i)
                        printf("%02x ", block[i+2]);
                    printf("\n");
                }
            }
        }
        
        track = disk[offset];
        sector = disk[offset + 1];
    }
    
    if (!geos_found) {
        fprintf(stderr, "GEOS entry not found in D64\n");
        return 1;
    }
    
    // Run a short frame to see what happens
    emu_frame();
    
    // Check if our bootstrap worked by checking the call count
    unsigned int geos_calls = mem_peek_long(symbol_address("_drive_geos_call_count"));
    printf("GEOS calls after frame: %d\n", geos_calls);
    
    if (geos_calls == 0) {
        printf("No virtual driver calls - bootstrap not working properly\n");
    } else {
        printf("Virtual driver called successfully - bootstrap working\n");
    }
    
    free(disk);
    return 0;
}