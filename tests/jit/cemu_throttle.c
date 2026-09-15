#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#define CEMU_THROTTLE_IMPLEMENTATION
#include "cemu_throttle.h"
#include "emu.h"
#include "schedule.h"
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#ifdef _WIN32
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#else
#include <errno.h>
#include <time.h>
#endif

static uint64_t modeled_ticks;
static uint64_t epoch_ticks;
static double started_wall;
static double epoch_wall;
static unsigned int rebases;
static bool initialized;
#ifdef _WIN32
static double counter_frequency;
static HANDLE wait_timer;
#endif

static void throttle_fail(const char *message)
{
    fprintf(stderr, "FAIL mandatory CEmu throttle: %s\n", message);
    exit(1);
}

static double monotonic_seconds(void)
{
#ifdef _WIN32
    LARGE_INTEGER counter;
    if (!QueryPerformanceCounter(&counter))
        throttle_fail("QueryPerformanceCounter");
    return (double)counter.QuadPart / counter_frequency;
#else
    struct timespec counter;
    if (clock_gettime(CLOCK_MONOTONIC, &counter))
        throttle_fail("clock_gettime");
    return (double)counter.tv_sec + (double)counter.tv_nsec / 1000000000.0;
#endif
}

static void wait_seconds(double remaining)
{
#ifdef _WIN32
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)(remaining * 10000000.0 + 1.0);
    if (!SetWaitableTimer(wait_timer, &due, 0, NULL, NULL, FALSE) ||
        WaitForSingleObject(wait_timer, INFINITE) != WAIT_OBJECT_0)
        throttle_fail("waitable timer");
#else
    struct timespec delay;
    delay.tv_sec = (time_t)remaining;
    delay.tv_nsec = (long)((remaining - (double)delay.tv_sec) * 1000000000.0);
    while (nanosleep(&delay, &delay))
        if (errno != EINTR)
            throttle_fail("nanosleep");
#endif
}

void cemu_throttle_shutdown(void)
{
#ifdef _WIN32
    if (wait_timer)
        CloseHandle(wait_timer);
    wait_timer = NULL;
#endif
}

static void report_throttle(void)
{
    double wall = monotonic_seconds() - started_wall;
    double simulated = (double)modeled_ticks / 48000000.0;
    printf("THROTTLE requested_percent=135 throttle=on simulated_seconds=%.6f "
           "wall_seconds=%.6f achieved_percent=%.3f late_rebases=%u\n",
           simulated, wall, wall > 0 ? 100.0 * simulated / wall : 0, rebases);
    cemu_throttle_shutdown();
}

void cemu_throttle_init(void)
{
    if (initialized)
        return;
#ifdef _WIN32
    LARGE_INTEGER frequency;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
        throttle_fail("QueryPerformanceFrequency");
    counter_frequency = (double)frequency.QuadPart;
    wait_timer = CreateWaitableTimerExW(NULL, NULL,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!wait_timer)
        wait_timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    if (!wait_timer)
        throttle_fail("cannot create timer; refusing uncapped execution");
#endif
    if (emu_get_run_rate() != 1000u && !emu_set_run_rate(1000u))
        throttle_fail("cannot select 1000 Hz scheduler quanta");
    if (sched_get_clock_rate(CLOCK_48M) != 48000000u)
        throttle_fail("48 MHz reference clock changed");
    printf("THROTTLE requested_percent=135 throttle=on cpu_hz=%u quantum_hz=1000\n",
           sched_get_clock_rate(CLOCK_CPU));
    started_wall = epoch_wall = monotonic_seconds();
    initialized = true;
    if (atexit(report_throttle))
        throttle_fail("cannot register throttle report");
}

static void run_quantum(void)
{
    uint64_t before;
    uint64_t after;
    double deadline;
    double now;
    cemu_throttle_init();
    if (emu_get_run_rate() != 1000u || sched_get_clock_rate(CLOCK_48M) != 48000000u)
        throttle_fail("scheduler clock changed during test");
    before = sched_total_time(CLOCK_48M);
    emu_run(1);
    after = sched_total_time(CLOCK_48M);
    if (after < before)
        throttle_fail("calculator time reversed within a quantum");
    modeled_ticks += after - before;
    deadline = epoch_wall + (double)(modeled_ticks - epoch_ticks) / 64800000.0;
    now = monotonic_seconds();
    while (now < deadline)
    {
        wait_seconds(deadline - now);
        now = monotonic_seconds();
    }
    if (now - deadline > 0.002)
    {
        epoch_wall = now;
        epoch_ticks = modeled_ticks;
        ++rebases;
    }
}

void cemu_throttle_run(uint64_t ticks)
{
    for (uint64_t quantum = 0; quantum < ticks; ++quantum)
        run_quantum();
}

uint64_t cemu_throttle_run_until_halt(uint64_t max_ticks)
{
    for (uint64_t quantum = 0; quantum < max_ticks; ++quantum)
    {
        run_quantum();
        if (cpu.halted || cpu_check_signals() != CPU_SIGNAL_NONE)
            return quantum + 1;
    }
    return max_ticks;
}

void cemu_throttle_snapshot(CemuThrottleSnapshot *out)
{
    cemu_throttle_init();
    out->simulated_ticks = modeled_ticks;
    out->wall_seconds = monotonic_seconds() - started_wall;
}
