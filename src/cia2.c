/*
 * emudore-CE: MOS 6526 CIA #2 (C port of cia2.cpp).
 */
#include "cia2.h"
#include "cpu.h"
#include "vic.h"
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
static uint8_t pra, prb;

void cia2_reset(void)
{
    timer_a_latch = timer_b_latch = timer_a_counter = timer_b_counter = 0;
    timer_a_enabled = timer_b_enabled = false;
    timer_a_irq_enabled = timer_b_irq_enabled = false;
    timer_a_irq_triggered = timer_b_irq_triggered = false;
    timer_a_input_mode = timer_b_input_mode = CIA_MODE_PROCESSOR;
    timer_a_run_mode = timer_b_run_mode = CIA_RUN_RESTART;
    pra = prb = 0xff;
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

void cia2_write_register(uint8_t r, uint8_t v)
{
    switch (r)
    {
    case 0x0:
        if ((uint8_t)(pra ^ v) & 0x3)
            vic_render_catchup(); /* VIC bank switch mid-frame */
        pra = v;
        break;
    case 0x1: prb = v; break;
    case 0x4: timer_a_latch = (timer_a_latch & 0xff00) | v; break;
    case 0x5: timer_a_latch = (timer_a_latch & 0x00ff) | (v << 8); break;
    case 0x6: timer_b_latch = (timer_b_latch & 0xff00) | v; break;
    case 0x7: timer_b_latch = (timer_b_latch & 0x00ff) | (v << 8); break;
    case 0xd:
        if (ISSET_BIT(v, 0)) timer_a_irq_enabled = ISSET_BIT(v, 7);
        if (ISSET_BIT(v, 1)) timer_b_irq_enabled = ISSET_BIT(v, 7);
        break;
    case 0xe:
        timer_a_enabled = ((v & 1) != 0);
        timer_a_input_mode = (v & (1 << 5)) >> 5;
        if ((v & (1 << 4)) != 0)
            timer_a_counter = timer_a_latch;
        break;
    case 0xf:
        timer_b_enabled = ((v & 1) != 0);
        timer_b_input_mode = (v & (1 << 5)) | ((v & (1 << 6)) >> 5);
        if ((v & (1 << 4)) != 0)
            timer_b_counter = timer_b_latch;
        break;
    default: break;
    }
}

uint8_t cia2_read_register(uint8_t r)
{
    uint8_t retval = 0;

    switch (r)
    {
    case 0x0:
        retval = pra;
        vic_poll_check(3); /* IEC bit-bang waits: nothing to receive, warp */
        break;
    case 0x1: retval = prb; break;
    case 0x4: retval = (uint8_t)(timer_a_counter & 0x00ff); break;
    case 0x5: retval = (uint8_t)((timer_a_counter & 0xff00) >> 8); break;
    case 0x6: retval = (uint8_t)(timer_b_counter & 0x00ff); break;
    case 0x7: retval = (uint8_t)((timer_b_counter & 0xff00) >> 8); break;
    case 0xd:
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

/* PRA bits 0-1 select the 16 KiB VIC bank (inverted). */
uint16_t cia2_vic_base_address(void)
{
    return (uint16_t)((~pra & 0x3) << 14);
}

bool cia2_emulate(void)
{
    if (timer_a_enabled && timer_a_input_mode == CIA_MODE_PROCESSOR)
    {
        timer_a_counter -= cpu.cycles - prev_cpu_cycles;
        if (timer_a_counter <= 0)
        {
            if (timer_a_irq_enabled)
            {
                timer_a_irq_triggered = true;
                cpu_nmi();
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
                cpu_nmi();
            }
            reset_timer_b();
        }
    }
    prev_cpu_cycles = cpu.cycles;
    return true;
}
