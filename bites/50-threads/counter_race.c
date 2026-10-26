/*
 * Bite 50 — Threads & Mutexes: the shared counter, broken then fixed
 *
 * Compile & run:
 *   gcc -Wall -Wextra -Werror -std=c17 -g -pthread bites/50-threads/counter_race.c -o /tmp/b50
 *   /tmp/b50 race       (4 threads x 100000 increments, NO mutex  -> wrong)
 *   /tmp/b50 mutex      (same work WITH a mutex                  -> right)
 *   /tmp/b50            (runs race first, then mutex, with timings)
 *
 * For extra credit, watch the race formally:
 *   valgrind --tool=helgrind /tmp/b50 race      (slow, but it screams)
 *
 * WHAT THIS TEACHES (the "Learn" bullets):
 *  - pthread_create/join: threads start in a function you choose and run
 *    CONCURRENTLY with main until join() collects them.
 *  - THE RACE: counter++ is not one operation. It is read-modify-write
 *    (load counter into a register; add 1; store it back). Two threads can
 *    both load 41, both store 42 — one increment vanishes. Do that 400000
 *    times and "lost updates" pile up into a visibly wrong total.
 *  - THE FIX: pthread_mutex_lock/unlock makes the whole read-modify-write
 *    one indivisible step. Rule: lock the DATA, not the code — decide per
 *    structure what its lock protects (here: exactly one counter).
 *  - Threads share memory: one bad write in one thread corrupts everyone.
 *    fork()ed processes (bite 49) get COPIES — different trade-offs.
 *
 * BREAK IT ON PURPOSE (built in — this program is the bug):
 *   `race` mode IS the deliberate bug: the total comes out different every
 *   run, almost never 400000. Then `mutex` mode fixes it: 400000, every
 *   run, at the cost of measurable time (lock/unlock is ~20ns each).
 *   Second exercise: break the mutex version by unlocking one path but not
 *   another (early return without unlock) — every later lock() hangs
 *   forever; diagnose with gdb or helgrind.
 *
 * Done when: you can reproduce a race on demand and fix it.
 */

/* POSIX APIs must be requested explicitly under -std=c17. */
#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define N_THREADS 4
#define INCREMENTS 100000

static long g_counter; /* the shared data. Note: NOT volatile — volatile has
                          nothing to do with atomicity; the mutex is what
                          makes access safe. */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- BROKEN: unsynchronized read-modify-write ---- */
static void *race_worker(void *arg)
{
    (void)arg; /* all four threads share the same global: no per-thread arg */
    for (int i = 0; i < INCREMENTS; i++)
        g_counter++; /* compiles to load/add/store — three separate steps */
    return NULL;
}

/* ---- FIXED: the same work with the mutex held around the whole RMW ---- */
static void *safe_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < INCREMENTS; i++) {
        pthread_mutex_lock(&g_lock);
        g_counter++;
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

static long run_threads(void *(*worker)(void *), double *ms_out)
{
    pthread_t ids[N_THREADS];
    struct timespec t0, t1;

    g_counter = 0;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < N_THREADS; i++) {
        int rc = pthread_create(&ids[i], NULL, worker, NULL);
        if (rc != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(rc));
            exit(1);
        }
    }
    /* join = wait for each thread to finish AND release its resources.
     * Un-joined threads are the thread-world's "zombies". */
    for (int i = 0; i < N_THREADS; i++)
        pthread_join(ids[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    *ms_out = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
              (double)(t1.tv_nsec - t0.tv_nsec) / 1000000.0;
    return g_counter;
}

int main(int argc, char **argv)
{
    long expected = (long)N_THREADS * INCREMENTS;
    double ms;
    long got;
    int do_race = 1, do_mutex = 1;

    if (argc > 1 && strcmp(argv[1], "race") == 0)
        do_mutex = 0;
    if (argc > 1 && strcmp(argv[1], "mutex") == 0)
        do_race = 0;

    if (do_race) {
        got = run_threads(race_worker, &ms);
        printf("race : %ld increments from %ld expected  (%6.1f ms)  %s\n",
               got, expected, ms,
               got == expected ? "" : "<-- LOST UPDATES (and it differs every run)");
    }
    if (do_mutex) {
        got = run_threads(safe_worker, &ms);
        printf("mutex: %ld increments from %ld expected  (%6.1f ms)  correct every run\n",
               got, expected, ms);
    }

    printf("\nwhy: counter++ = load -> add -> store. Two threads interleaving\n"
           "those steps lose increments. The mutex makes the 3 steps atomic.\n"
           "Note the timing: correctness costs real time — that's the deal\n"
           "you'll make in bite 56's threaded server.\n");
    return 0;
}
