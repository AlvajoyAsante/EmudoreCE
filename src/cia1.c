/*
 * emudore-CE: MOS 6526 CIA #1 (C port of cia1.cpp).
 *
 * The keyboard matrix is read from the CE keypad via io.c instead of SDL.
 */
#include <sys/timers.h>
#include "cia1.h"
#include "cpu.h"
#include "io.h"
#include "util.h"
#include "vic.h"

enum { CIA_MODE_PROCESSOR, CIA_MODE_CNT, CIA_MODE_TIMER_A, CIA_MODE_TIMER_A_CNT };
enum { CIA_RUN_RESTART, CIA_RUN_ONE_TIME };

static int16_t timer_a_latch, timer_b_latch;
static int16_t timer_a_counter, timer_b_counter;
static bool timer_a_enabled, timer_b_enabled;
static bool timer_a_irq_enabled, timer_b_irq_enabled;
static bool timer_a_irq_triggered, timer_b_irq_triggered;
static uint8_t timer_a_run_mode, timer_b_run_mode;
static uint8_t timer_a_input_mode, timer_b_input_mode;
static unsigned int prev_rtc_ticks;
static unsigned int prev_cpu_cycles;
static unsigned int sampled_cpu_cycles;
static uint8_t rtc_sample_phase;
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
    prev_rtc_ticks = 0;
    prev_cpu_cycles = 0;
    sampled_cpu_cycles = 0;
    rtc_sample_phase = 0;
    /* CIA1 timers pace against REAL time (32 kHz crystal timer): the jiffy
       IRQ, cursor blink and key repeat then run at wall-clock speed no
       matter how fast the CPU core emulates. */
    timer_Disable(1);
    timer_Set(1, 0);
    timer_SetReload(1, 0);
    timer_Enable(1, TIMER_32K, TIMER_NOINT, TIMER_UP);
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
    case 0x0:                                       /* PRA: joy 2 (active low) */
        retval = (uint8_t)(pra & io_joystick2());
        vic_poll_check(1);
        break;
    case 0x1:                                       /* PRB: keyboard rows */
        vic_poll_check(2);
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

uint8_t cia1_peek_register(uint8_t r)
{
    uint8_t retval = 0;

    switch (r)
    {
    case 0x0: retval = (uint8_t)(pra & io_joystick2()); break;
    case 0x1:
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
    case 0xd:
        if (timer_a_irq_triggered || timer_b_irq_triggered)
        {
            retval |= 1 << 7;
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
    /* Elapsed REAL time -> C64 cycles (32768 Hz tick ~= 30 C64 cycles),
       CLAMPED to the cycles the CPU actually executed since the last call.
       Uncapped wall pacing livelocks slow scenes: at 8% speed a real-time
       60 Hz jiffy = 750 IRQs per emulated second, and the KERNAL handler
       alone outruns the 16.7 ms IRQ period, so the foreground never runs.
       Fast scenes (warp/idle) still pace at true 60 Hz. */
    unsigned int now;
    unsigned int elapsed;

    sampled_cpu_cycles += cpu.cycles - prev_cpu_cycles;
    prev_cpu_cycles = cpu.cycles;
    if (++rtc_sample_phase < 4)
        return true;
    rtc_sample_phase = 0;

    now = (unsigned int)timer_Get(1);
    elapsed = (now - prev_rtc_ticks) * 30;
    prev_rtc_ticks = now;
    if (elapsed > sampled_cpu_cycles)
        elapsed = sampled_cpu_cycles;
    sampled_cpu_cycles = 0;

    if (timer_a_enabled && timer_a_input_mode == CIA_MODE_PROCESSOR)
    {
        timer_a_counter -= elapsed;
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
        timer_b_counter -= elapsed;
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
    return true;
}
