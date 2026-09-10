#ifndef IO_TEST_SYS_TIMERS_H
#define IO_TEST_SYS_TIMERS_H

#include <stdint.h>

#define TIMER_32K 1
#define TIMER_CPU 0
#define TIMER_0INT 1
#define TIMER_NOINT 0
#define TIMER_UP 1
#define TIMER_DOWN 0
#define TIMER1_ENABLE (1u << 0)

extern volatile uint16_t timer_Control;

uint32_t timer_Get(unsigned int timer);
uint16_t timer_Disable(unsigned int timer);
uint16_t timer_Enable(unsigned int timer, uint8_t rate,
                      uint8_t interrupt, uint8_t direction);

#endif