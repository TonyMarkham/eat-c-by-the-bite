/*
 * Bite 59 — Time in C: monotonic clocks and fixed-rate pacing
 *
 * Real-time media runs on a clock. This program practices the two skills
 * the whole phase depends on:
 *
 *   1. MEASURING durations with CLOCK_MONOTONIC.
 *   2. PACING work at a fixed rate (20 ms ticks — one 48 kHz audio frame).
 *
 * Why CLOCK_MONOTONIC and never gettimeofday() for durations:
 *   gettimeofday/CLOCK_REALTIME is *wall* clock — NTP or the user can step
 *   it backwards/forwards at any moment, so "now - then" can be negative or
 *   3 hours long. CLOCK_MONOTONIC only ever moves forward (it counts some
 *   machine-internal reference since boot). It has no meaning as a calendar
 *   date, and that is exactly why it is right for measuring.
 *
 * Why sleep-until-deadline beats sleep-a-fixed-amount:
 *   - Wrong:  do_work(); sleep(20ms);          -> period = work + 20 ms,
 *     error accumulates every tick (drift). You fall behind forever.
 *   - Right:  next = now();                    -> each tick is an absolute
 *             do { do_work(); sleep_until(next); next += 20ms; } ...
 *     point on the timeline. If work runs long, you skip a tick (back-
 *     pressure) instead of drifting late forever. This is the RTP pacer.
 *
 * Compile + run (from the repo root, inside WSL):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/59-time/pacer.c -o /tmp/b59 && /tmp/b59
 *
 *   Or pick a mode explicitly:
 *   /tmp/b59 measure              # part 1: time a busy loop
 *   /tmp/b59 drift                # part 2a: fixed-sleep pacer (drifts)
 *   /tmp/b59 deadline             # part 2b: absolute-deadline pacer
 *   /tmp/b59 deadline 100 25      # ...with 25 ms of work per 20 ms tick
 *
 * BREAK IT ON PURPOSE (from phases/phase-6-rt-media.md):
 *   Make the tick body take 25 ms (> the 20 ms budget) and compare the two
 *   pacers:
 *     /tmp/b59 drift   100 25    -> intervals grow to ~45 ms and stay there:
 *                                   fixed-sleep *drifts*, never recovers.
 *     /tmp/b59 deadline 100 25   -> the deadline pacer cannot "catch up", so
 *                                   it SKIPS ticks: you see intervals of
 *                                   45/5/45/5... and "skipped" counters.
 *   Name the behavior in your notes: that is back-pressure — the producer
 *   refuses to queue an unbounded backlog and drops whole ticks instead.
 *   The same thing happens in a real media pipeline when encoding is slower
 *   than real time.
 *
 * Done when: the deadline pacer holds 20 ms +- small jitter over 100 ticks
 * (look at the min/avg/max summary) and you can explain monotonic vs wall
 * clock without looking.
 */

#define _POSIX_C_SOURCE 200809L /* clock_gettime + nanosleep declarations */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define TICK_MS 20 /* one 20 ms tick = 960 samples @ 48 kHz = 1920 bytes   */
#define NS_PER_MS 1000000L
#define NS_PER_SEC 1000000000L

/* --- clock helpers ------------------------------------------------------ */

/* Milliseconds since an arbitrary fixed point, as a double.
 * CLOCK_MONOTONIC never jumps -> differences are always honest durations. */
static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

/* Sleep until an absolute deadline (ms, same clock as now_ms()).
 * This is THE primitive for fixed-rate pacing.
 * nanosleep can return early (signals) and can be interrupted (EINTR), so we
 * loop until the deadline actually passes — never trust a single sleep. */
static void sleep_until_ms(double deadline_ms)
{
    for (;;) {
        double delta = deadline_ms - now_ms();
        if (delta <= 0.0)
            return; /* deadline reached (or already passed) */
        struct timespec req = {
            .tv_sec = (time_t)(delta / 1000.0),
            .tv_nsec = (long)((delta - (double)req.tv_sec * 1000.0) * NS_PER_MS),
        };
        if (nanosleep(&req, NULL) == 0)
            return;
        /* EINTR: interrupted by a signal -> compute the remaining time again */
        if (errno != EINTR)
            return;
    }
}

/* Burn ~ms milliseconds of CPU (simulates "the tick body did real work").
 * Uses the same monotonic clock — busy-wait, ugly, but honest for a demo. */
static void burn_ms(double ms)
{
    double end = now_ms() + ms;
    volatile double sink = 0.0; /* volatile: the optimizer must keep the loop */
    while (now_ms() < end)
        for (int i = 0; i < 10000; i++)
            sink += (double)i * 0.5;
    (void)sink;
}

/* --- part 1: measure a busy loop ---------------------------------------- */

static void part_measure(void)
{
    printf("== part 1: measure a busy loop with CLOCK_MONOTONIC ==\n");
    double t0 = now_ms();

    volatile unsigned long acc = 0; /* volatile so the loop is not optimized away */
    for (unsigned long i = 0; i < 50000000UL; i++)
        acc += i;

    double dt = now_ms() - t0;
    /* %.3f ms = microsecond precision on the printed value */
    printf("loop of 50,000,000 additions took %.3f ms (acc=%lu, kept volatile)\n",
           dt, acc);
    printf("(this is 'interval', not 'work' — the clock measures elapsed time,\n");
    printf(" not CPU time; see clock_gettime(CLOCK_PROCESS_CPUTIME_ID) for that)\n\n");
}

/* --- part 2: the two pacers --------------------------------------------- */

/* Pacer A — the classic mistake: sleep a FIXED amount after each tick.
 * Equivalent to usleep(20000) per iteration: usleep is obsolete POSIX-wise,
 * nanosleep(20 ms) has identical semantics. Period = work + sleep. Drifts. */
static void pacer_drift(int ticks, double work_ms)
{
    printf("== pacer A: fixed sleep (drifts) — %d ticks, %.1f ms work ==\n",
           ticks, work_ms);
    double sum = 0.0, mn = 1e9, mx = 0.0;
    int skipped = 0;

    for (int i = 0; i < ticks; i++) {
        double tick_start = now_ms();

        burn_ms(work_ms); /* the "work" of one tick */
        struct timespec nap = { .tv_sec = 0, .tv_nsec = TICK_MS * NS_PER_MS };
        nanosleep(&nap, NULL); /* fixed 20 ms sleep — the bug */

        double interval = now_ms() - tick_start;
        if (i > 0) {
            printf("tick %3d interval %7.3f ms\n", i, interval);
            sum += interval;
            if (interval < mn) mn = interval;
            if (interval > mx) mx = interval;
            if (interval > TICK_MS + 1.0) skipped++;
        }
    }
    printf("summary: avg %.3f ms  min %.3f  max %.3f  over-budget ticks %d\n",
           sum / (ticks - 1), mn, mx, skipped);
    printf("-> with any work at all the period is work+%d ms and it NEVER\n", TICK_MS);
    printf("   recovers: the pacer falls behind the timeline forever.\n\n");
}

/* Pacer B — the right way: absolute deadlines on the timeline.
 * The deadline advances by exactly 20 ms per tick no matter how long the
 * body takes. If the body overruns, the next deadline is already in the
 * past: we skip it (count it!) instead of drifting. That is back-pressure. */
static void pacer_deadline(int ticks, double work_ms)
{
    printf("== pacer B: sleep-until-deadline — %d ticks, %.1f ms work ==\n",
           ticks, work_ms);
    double t0 = now_ms();
    double next = now_ms() + TICK_MS; /* absolute deadline of tick 0 */
    double sum = 0.0, mn = 1e9, mx = 0.0;
    int skipped = 0, ran = 0;

    for (int i = 0; i < ticks; i++) {
        /* If the previous body overran, deadlines may already be stale:
         * fast-forward past them and count every skipped tick. */
        while (next < now_ms()) {
            skipped++;
            next += TICK_MS;
        }
        double tick_start = now_ms();

        burn_ms(work_ms);
        sleep_until_ms(next); /* sleep the REMAINDER, not a fixed amount */
        next += TICK_MS;

        double interval = now_ms() - tick_start;
        printf("tick %3d interval %7.3f ms%s\n", i, interval,
               interval > TICK_MS + 1.0 ? "  <- work overran, next tick skipped" : "");
        if (i > 0) {
            sum += interval;
            if (interval < mn) mn = interval;
            if (interval > mx) mx = interval;
        }
        ran++;
    }
    double elapsed = now_ms() - t0;
    printf("summary: %d ticks ran, %d skipped, avg %.3f ms  min %.3f  max %.3f\n",
           ran, skipped, sum / (ticks - 1 > 0 ? ticks - 1 : 1), mn, mx);
    printf("total %.3f ms for %d x %d ms of timeline = %.3f%% of real time\n",
           elapsed, ticks, TICK_MS, 100.0 * elapsed / ((double)ticks * TICK_MS));
    printf("-> the timeline stays honest: overruns are paid for by SKIPPING,\n");
    printf("   never by drifting late. That is exactly an RTP pacer's contract.\n\n");
}

/* --- part 3: media units — the 48 kHz timestamp -------------------------- */

static void part_media_timestamp(void)
{
    printf("== part 3: the same timeline in media units ==\n");
    /* RTP timestamps are NOT milliseconds. Each media type has a clock rate:
     *   video: 90 kHz (fixed by RFC 3550)   audio: the sample rate (48 kHz)
     * The timestamp of a frame = number of clock ticks since stream start.
     * For 20 ms of 48 kHz audio: 0.020 s * 48000 samples/s = 960 samples. */
    unsigned tick = 100;
    uint32_t rtp_ts = (uint32_t)((double)tick * (TICK_MS / 1000.0) * 48000.0);
    printf("tick %u (20 ms each) -> 48 kHz RTP timestamp %u (expected 96000)\n",
           tick, rtp_ts);
    printf("tick %u -> 90 kHz video RTP timestamp %u\n", tick, tick * 90000u);
    printf("Same wall-clock moment, two unit systems. Bites 61/68 carry the\n");
    printf("48 kHz flavor in their packet headers.\n");
}

int main(int argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "all";
    int ticks = (argc > 2) ? atoi(argv[2]) : 100;
    double work_ms = (argc > 3) ? atof(argv[3]) : 0.0;

    if (ticks < 2)
        ticks = 2;

    if (mode[0] == 'm') { /* "measure" only */
        part_measure();
        return 0;
    }
    if (mode[0] == 'd') { /* "deadline" only */
        pacer_deadline(ticks, work_ms);
        return 0;
    }
    if (mode[0] == 'f') { /* "fixed" (drifting) pacer only */
        pacer_drift(ticks, work_ms);
        return 0;
    }
    if (mode[0] == 'a') { /* "all": the full demo */
        part_measure();
        pacer_drift(ticks, work_ms);
        pacer_deadline(ticks, work_ms);
        part_media_timestamp();
        return 0;
    }

    fprintf(stderr, "usage: %s [all|measure|deadline|fixed] [ticks] [work_ms]\n",
            argv[0]);
    return 1;
}
