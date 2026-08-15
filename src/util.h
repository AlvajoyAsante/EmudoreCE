/*
 * emudore-CE, Commodore 64 emulator for the TI-84 Plus CE
 * Small shared helpers (ported from util.h).
 */
#ifndef EMUDORE_CE_UTIL_H
#define EMUDORE_CE_UTIL_H

#include <stdint.h>
#include <stdbool.h>
#include <debug.h>

/* D() is the generic debug macro, mapping to CEdev's dbg_printf() (writes to
   the CEmu console in debug builds, compiles out in release). No custom
   debug writers -- we use only what CEdev provides. */
#ifndef NDEBUG
#define D(...) dbg_printf(__VA_ARGS__)
#else
#define D(...) do {} while (0)
#endif

/* Small readability helper used throughout the register-emulation code. */
#define ISSET_BIT(v, b) (((v) & (1 << (b))) != 0)

#endif /* EMUDORE_CE_UTIL_H */
