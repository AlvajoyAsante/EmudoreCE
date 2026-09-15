#ifndef CEMU_THROTTLE_H
#define CEMU_THROTTLE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "emu.h"

typedef struct CemuThrottleSnapshot {
    uint64_t simulated_ticks;
    double wall_seconds;
} CemuThrottleSnapshot;

void cemu_throttle_init(void);
void cemu_throttle_shutdown(void);
void cemu_throttle_run(uint64_t ticks);
uint64_t cemu_throttle_run_until_halt(uint64_t max_ticks);
void cemu_throttle_snapshot(CemuThrottleSnapshot *out);

#ifndef CEMU_THROTTLE_IMPLEMENTATION
#define emu_run(...) _Static_assert(0, "Use the mandatory CEmu throttle wrapper")
#endif

#ifdef __cplusplus
}
#endif

#endif
