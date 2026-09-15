#ifndef UI_KEYPADC_H
#define UI_KEYPADC_H

#include <stdint.h>

typedef uint16_t kb_lkey_t;
extern uint8_t kb_Data[8];

#define kb_KeyTrace ((kb_lkey_t)0x0102)
#define kb_KeyYequ ((kb_lkey_t)0x0110)
#define kb_KeyMode ((kb_lkey_t)0x0140)
#define kb_KeyAlpha ((kb_lkey_t)0x0280)
#define kb_KeyEnter ((kb_lkey_t)0x0601)
#define kb_KeyClear ((kb_lkey_t)0x0640)
#define kb_KeyDown ((kb_lkey_t)0x0701)
#define kb_KeyLeft ((kb_lkey_t)0x0702)
#define kb_KeyRight ((kb_lkey_t)0x0704)
#define kb_KeyUp ((kb_lkey_t)0x0708)
#define kb_IsDown(key) (kb_Data[(key) >> 8] & (key))

void kb_Scan(void);

#endif