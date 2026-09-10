#ifndef IO_TEST_KEYPADC_H
#define IO_TEST_KEYPADC_H

#include <stdint.h>

typedef uint8_t kb_key_t;
typedef uint16_t kb_lkey_t;

typedef enum {
    MODE_0_IDLE,
    MODE_1_INDISCRIMINATE,
    MODE_2_SINGLE,
    MODE_3_CONTINUOUS
} kb_scan_mode_t;

enum {
    kb_KeyGraph = 0x101, kb_KeyTrace = 0x102, kb_KeyZoom = 0x104,
    kb_KeyWindow = 0x108, kb_KeyYequ = 0x110, kb_Key2nd = 0x120,
    kb_KeyMode = 0x140, kb_KeyDel = 0x180,
    kb_KeySto = 0x202, kb_KeyLn = 0x204, kb_KeyLog = 0x208,
    kb_KeySquare = 0x210, kb_KeyRecip = 0x220, kb_KeyMath = 0x240,
    kb_KeyAlpha = 0x280,
    kb_Key0 = 0x301, kb_Key1 = 0x302, kb_Key4 = 0x304,
    kb_Key7 = 0x308, kb_KeyComma = 0x310, kb_KeySin = 0x320,
    kb_KeyApps = 0x340, kb_KeyGraphVar = 0x380,
    kb_KeyDecPnt = 0x401, kb_Key2 = 0x402, kb_Key5 = 0x404,
    kb_Key8 = 0x408, kb_KeyLParen = 0x410, kb_KeyCos = 0x420,
    kb_KeyPrgm = 0x440, kb_KeyStat = 0x480,
    kb_KeyChs = 0x501, kb_Key3 = 0x502, kb_Key6 = 0x504,
    kb_Key9 = 0x508, kb_KeyRParen = 0x510, kb_KeyTan = 0x520,
    kb_KeyVars = 0x540,
    kb_KeyEnter = 0x601, kb_KeyAdd = 0x602, kb_KeySub = 0x604,
    kb_KeyMul = 0x608, kb_KeyDiv = 0x610, kb_KeyPower = 0x620,
    kb_KeyClear = 0x640,
    kb_KeyDown = 0x701, kb_KeyLeft = 0x702, kb_KeyRight = 0x704,
    kb_KeyUp = 0x708
};

extern volatile uint8_t kb_Config;
extern uint8_t kb_Data[8];

uint8_t kb_SetMode(kb_scan_mode_t mode);
uint8_t kb_IsDown(kb_lkey_t key);
uint8_t kb_AnyKey(void);
void kb_Scan(void);

#endif