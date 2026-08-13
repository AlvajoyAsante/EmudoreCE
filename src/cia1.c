/*
 * emudore-CE: MOS 6526 CIA #1 (C port of cia1.cpp).
 *
 * The keyboard matrix is read from the CE keypad via io.c instead of SDL.
 */
#include "cia1.h"
#include "cpu.h"
#include "io.h"
#include "util.h"

enum { CIA_MODE_PROCESSOR, CIA_MODE_CNT, CIA_MODE_TIMER_A, CIA_MODE_TIMER_A_CNT };
enum { CIA_RUN_RESTART, CIA_RUN_ONE_TIME };

static int16_t timer_a_latch, timer_b_latch;
static int16_t timer_a_counter, timer_b_counter;
static bool timer_a_enabled, timer_b_enabled;
static bool timer_a_irq_enabled, timer_b_irq_enabled;
static bool timer_a_irq_triggered, timer_b_irq_triggered;
static uint8_t timer_a_run_mode, timer_b_run_mode;
static uint8_t timer_a_input_mode, timer_b_input_mode;
static unsigned int prev_cpu_cycles;
static uint8_t pra;

void cia1_reset(void)
{
    timer_a_latch = timer_b_latch = timer_a_counter = timer_b_counter = 0;
    timer_a_enabled = timer_b_enabled = false;
    timer_a_irq_enabled = timer_b_irq_enabled = false;
    timer_a_irq_triggered = timer_b_irq_triggered = false;
    timer_a_input_mode = timer_b_input_mode = CIA_MODE_PROCESSOR;
    timer_a_run_mode = timer_b_run_mode = CIA_RUN_RESTART;
    pra = 0xff;
    prev_cpu_cycles = 0;
}

static void reset_timer_a(void)
{
    if (timer_a_run_mode == CIA_RUN_RESTART)
        timer_a_counter = timer_a_latch;
    else
        timer_a_enabled = false;
}

static void reset_timer_b(void)
{
    if (timer_b_run_mode == CIA_RUN_RESTART)
        timer_b_counter = timer_b_latch;
    else
        timer_b_enabled = false;
}

void cia1_write_register(uint8_t r, uint8_t v)
{
    switch (r)
    {
    case 0x0: pra = v; break;                       /* PRA: keyboard cols, joy 2 */
    case 0x1: break;                                /* PRB */
    case 0x2: break;                                /* DDRA */
    case 0x3: break;                                /* DDRB */
    case 0x4: timer_a_latch = (timer_a_latch & 0xff00) | v; break;
    case 0x5: timer_a_latch = (timer_a_latch & 0x00ff) | (v << 8); break;
    case 0x6: timer_b_latch = (timer_b_latch & 0xff00) | v; break;
    case 0x7: timer_b_latch = (timer_b_latch & 0x00ff) | (v << 8); break;
    case 0xd:                                       /* interrupt control */
        if (ISSET_BIT(v, 0)) timer_a_irq_enabled = ISSET_BIT(v, 7);
        if (ISSET_BIT(v, 1)) timer_b_irq_enabled = ISSET_BIT(v, 7);
        break;
    case 0xe:                                       /* control timer A */
        timer_a_enabled = ((v & 1) != 0);
        timer_a_input_mode = (v & (1 << 5)) >> 5;
        if ((v & (1 << 4)) != 0)
            timer_a_counter = timer_a_latch;
        break;
    case 0xf:                                       /* control timer B */
        timer_b_enabled = ((v & 1) != 0);
        timer_b_input_mode = (v & (1 << 5)) | ((v & (1 << 6)) >> 5);
        if ((v & (1 << 4)) != 0)
            timer_b_counter = timer_b_latch;
        break;
    default: break;
    }
}

uint8_t cia1_read_register(uint8_t r)
{
    uint8_t retval = 0;

    switch (r)
    {
    case 0x0: break;                                /* PRA read */
    case 0x1:                                       /* PRB: keyboard rows */
        if (pra == 0xff) retval = 0xff;
        else if (pra)
        {
            int col = 0;
            uint8_t v = (uint8_t)~pra;
            while (v >>= 1)
                col++;
            retval = io_keyboard_matrix_row(col);
        }
        break;
    case 0x4: retval = (uint8_t)(timer_a_counter & 0x00ff); break;
    case 0x5: retval = (uint8_t)((timer_a_counter & 0xff00) >> 8); break;
    case 0x6: retval = (uint8_t)(timer_b_counter & 0x00ff); break;
    case 0x7: retval = (uint8_t)((timer_b_counter & 0xff00) >> 8); break;
    case 0xd:                                       /* interrupt status */
        if (timer_a_irq_triggered || timer_b_irq_triggered)
        {
            retval |= (1 << 7);
            if (timer_a_irq_triggered) retval |= 1 << 0;
            if (timer_b_irq_triggered) retval |= 1 << 1;
        }
        break;
    default: break;
    }
    return retval;
}

bool cia1_emulate(void)
{
    if (timer_a_enabled && timer_a_input_mode == CIA_MODE_PROCESSOR)
    {
        timer_a_counter -= cpu.cycles - prev_cpu_cycles;
        if (timer_a_counter <= 0)
        {
            if (timer_a_irq_enabled)
            {
                timer_a_irq_triggered = true;
                cpu_irq();
            }
            reset_timer_a();
        }
    }
    if (timer_b_enabled && timer_b_input_mode == CIA_MODE_PROCESSOR)
    {
        timer_b_counter -= cpu.cycles - prev_cpu_cycles;
        if (timer_b_counter <= 0)
        {
            if (timer_b_irq_enabled)
            {
                timer_b_irq_triggered = true;
                cpu_irq();
            }
            reset_timer_b();
        }
    }
    prev_cpu_cycles = cpu.cycles;
    return true;
}
