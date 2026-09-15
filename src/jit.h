#ifndef EMUDORE_CE_JIT_H
#define EMUDORE_CE_JIT_H

#include <stdbool.h>
#include <stdint.h>
#include "jit_emit.h"

extern uint8_t jit_page_generation[256];
extern uint8_t jit_page_generation_wrapped;
extern uint8_t jit_mapping_generation;

void jit_reset(void);
void jit_invalidate_page(uint8_t page);
void jit_invalidate_range(uint16_t addr, unsigned int length);
void jit_observe_pc(uint16_t pc);
bool jit_run(unsigned int until_cycles);
bool jit_run_batch(unsigned int until_cycles);
bool jit_execute(const uint8_t *code);
bool jit_backend_selftest(void);
bool jit_backend_ready(void);

extern uint8_t jit_cached_block_count;
extern uint8_t jit_ready_block_count;
extern uint8_t jit_ineligible_block_count;
extern unsigned int jit_block_observations;
extern unsigned int jit_native_hits;
extern unsigned int jit_native_mismatches;

#endif /* EMUDORE_CE_JIT_H */