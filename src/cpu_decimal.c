#include "cpu.h"

#ifndef __TICE__

void cpu_decimal_adc(uint8_t operand)
{
    uint8_t accumulator = cpu.a;
    unsigned int binary = accumulator + operand + (cpu.cf ? 1u : 0u);
    unsigned int low = (accumulator & 15u) + (operand & 15u) + (cpu.cf ? 1u : 0u);
    unsigned int intermediate;
    if (low > 9)
        low = ((low + 6u) & 15u) + 16u;
    intermediate = (accumulator & 240u) + (operand & 240u) + low;
    cpu.zf = (uint8_t)binary == 0;
    cpu.nf = (intermediate & 128u) != 0;
    cpu.of = ((~(accumulator ^ operand) & (accumulator ^ intermediate)) & 128u) != 0;
    if (intermediate >= 160u)
        intermediate += 96u;
    cpu.cf = intermediate > 255u;
    cpu.a = (uint8_t)intermediate;
}

void cpu_decimal_sbc(uint8_t operand)
{
    uint8_t accumulator = cpu.a;
    int borrow = cpu.cf ? 0 : 1;
    int binary = accumulator - operand - borrow;
    int low = (accumulator & 15) - (operand & 15) - borrow;
    int adjusted;
    if (low < 0)
        low = ((low - 6) & 15) - 16;
    adjusted = (accumulator & 240) - (operand & 240) + low;
    if (adjusted < 0)
        adjusted -= 96;
    cpu.cf = binary >= 0;
    cpu.zf = (uint8_t)binary == 0;
    cpu.nf = (binary & 128) != 0;
    cpu.of = (((accumulator ^ operand) & (accumulator ^ binary)) & 128) != 0;
    cpu.a = (uint8_t)adjusted;
}

#endif