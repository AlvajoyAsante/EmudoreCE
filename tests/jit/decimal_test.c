#include <stdio.h>
#include <string.h>
#include "../../src/cpu.h"

CpuState cpu;

int main(void)
{
    unsigned int operation, accumulator, operand, carry, cases = 0;
    for (operation = 0; operation < 2; ++operation)
        for (accumulator = 0; accumulator < 256; ++accumulator)
            for (operand = 0; operand < 256; ++operand)
                for (carry = 0; carry < 2; ++carry)
                {
                    int binary, units, tens;
                    uint8_t result, negative, overflow, carry_out;
                    memset(&cpu, 0, sizeof(cpu));
                    cpu.a = (uint8_t)accumulator;
                    cpu.cf = carry != 0;
                    if (!operation)
                    {
                        binary = accumulator + operand + carry;
                        units = (accumulator % 16) + (operand % 16) + carry;
                        tens = (accumulator / 16) + (operand / 16) + (units >= 10);
                        negative = (tens & 8) != 0;
                        overflow = ((accumulator ^ operand) & 128) == 0 &&
                                   ((accumulator >> 7) != negative);
                        carry_out = tens >= 10;
                        result = (uint8_t)((((tens + (carry_out ? 6 : 0)) % 16) << 4) |
                                          ((units + (units >= 10 ? 6 : 0)) % 16));
                        cpu_decimal_adc((uint8_t)operand);
                    }
                    else
                    {
                        binary = (int)accumulator - (int)operand - !carry;
                        units = (int)(accumulator % 16) - (int)(operand % 16) - !carry;
                        tens = (int)(accumulator / 16) - (int)(operand / 16) - (units < 0);
                        negative = (binary & 128) != 0;
                        overflow = ((accumulator ^ operand) & (accumulator ^ binary) & 128) != 0;
                        carry_out = binary >= 0;
                        result = (uint8_t)((((tens - (tens < 0 ? 6 : 0)) & 15) << 4) |
                                          ((units - (units < 0 ? 6 : 0)) & 15));
                        cpu_decimal_sbc((uint8_t)operand);
                    }
                    ++cases;
                    if (cpu.a != result || cpu.cf != carry_out || cpu.nf != negative ||
                        cpu.of != overflow || cpu.zf != ((uint8_t)binary == 0))
                    {
                        fprintf(stderr, "FAIL decimal %s A=%02x M=%02x C=%u\n",
                                operation ? "SBC" : "ADC", accumulator, operand, carry);
                        return 1;
                    }
                }
    printf("PASS %u NMOS decimal combinations, including invalid BCD digits\n", cases);
    return 0;
}