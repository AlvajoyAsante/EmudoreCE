/* Test the production loop with deterministic chip stubs, not a copy of it. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "c64.h"
#include "cpu.h"
#include "memory.h"
#include "vic.h"
#include "cia1.h"
#include "cia2.h"
#include "io.h"
#include "drive.h"
#include "CEPackage.h"

CpuState cpu;
uint8_t cpu_jit_kernal;
unsigned int vic_raster_due;
static unsigned int line, stop_line, checks;
static char stop_at, expected;
static uint8_t rom[0x2000];

static void require(int ok, const char *message)
{
    ++checks;
    if (!ok) {
        fprintf(stderr, "FAIL scheduler at line %u (%c): %s\n", line, stop_at, message);
        exit(1);
    }
}

static void event(char kind)
{
    require(expected == kind, "device order/cadence");
}

bool cpu_run(unsigned int deadline)
{
    event('C');
    require(deadline == vic_raster_due, "CPU deadline");
    require(deadline == (unsigned int)(line * 63u + 0xfffff0u), "updated VIC deadline");
    if (stop_at == 'C' && line == stop_line) { expected = 'X'; return false; }
    expected = 'V';
    return true;
}

bool vic_emulate(void)
{
    event('V');
    if (stop_at == 'V' && line == stop_line) { expected = 'X'; return false; }
    ++line;
    vic_raster_due += 63u;
    expected = line % 8u == 0 ? 'I' : line % 4u == 0 ? '2' : 'C';
    return true;
}

bool io_emulate(void)
{
    event('I');
    if (stop_at == 'I' && line == stop_line) { expected = 'X'; return false; }
    expected = '2';
    return true;
}

bool cia2_emulate(void) { event('2'); expected = 'D'; return true; }
void drive_tick(void) { event('D'); expected = line % 16u == 0 ? '1' : 'C'; }
bool cia1_emulate(void) { event('1'); expected = 'C'; return true; }

/* Boot/shutdown dependencies are deliberately unused by this loop test. */
void io_init(void) {} void io_shutdown(void) {}
void mem_init(void) {} void mem_shutdown(void) {}
int mem_ready(void) { return 1; }
uint8_t *mem_basic_rom_ptr(void) { return rom; }
uint8_t *mem_kernal_rom_ptr(void) { return rom; }
uint8_t *mem_chars_rom_ptr(void) { return rom; }
int CEPackageRead(const char *id, uint8_t *out, uint32_t size)
{ (void)id; (void)out; (void)size; return 0; }
bool drive_init(const char *id) { (void)id; return false; }
void drive_patch_kernal(void) {}
void cia1_reset(void) {} void cia2_reset(void) {}
void vic_reset(void) {} void cpu_reset(void) {}

static void run_case(char stop, unsigned int limit)
{
    line = 0;
    stop_at = stop;
    stop_line = limit;
    expected = 'C';
    vic_raster_due = 0xfffff0u;
    c64_run();
    require(expected == 'X', "unexpected return");
    require(line == limit, "extra or missing VIC calls");
}

int main(void)
{
    unsigned int i;
    /* Includes 8/16-line scheduling edges, byte wrap, and 16-bit wrap.
       Each new run must reset the local scheduling phase. */
    for (i = 0; i < 520; ++i) {
        run_case('C', i);
        run_case('V', i);
        if (i && i % 8u == 0) run_case('I', i);
    }
    run_case('C', 65537);
    run_case('V', 65537);
    run_case('I', 65536);
    printf("PASS scheduler: 1108 runs, %u checks; deadlines, cadence, exits, wrap and re-entry\n", checks);
    return 0;
}
