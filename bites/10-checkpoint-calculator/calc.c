/*
 * Bite 10 — Checkpoint: Number Cruncher (math operations)
 *
 * Compiled together with main.c (see calc.h for the command). This file
 * knows nothing about menus or input — it only does math. That split is
 * the real lesson of the checkpoint: each file has ONE job.
 */

#include <stdio.h>
#include "calc.h"

long add(long a, long b)
{
    return a + b;
}

long sub(long a, long b)
{
    return a - b;
}

long mul(long a, long b)
{
    return a * b;
}

long idiv(long a, long b)
{
    if (b == 0) {
        /* Reject with a message, not a crash. A real hardware divide by
         * zero aborts the process — the whole point of this branch. */
        printf("error: division by zero is undefined — try again\n");
        return 0;
    }
    return a / b;   /* integer division: truncates, same as bite 3 */
}

long ipow(long base, long exp)
{
    if (exp < 0) {
        printf("error: negative exponents need fractions — try again\n");
        return 0;
    }

    /* Loop-based exponentiation: multiply into result exp times.
     * Anything^0 = 1, so starting at 1 with zero loop trips gives 1. */
    long result = 1;
    for (long i = 0; i < exp; i++) {
        result *= base;
        /* Overflow (e.g. 99^30) is undefined behavior for signed long.
         * We note it and let small inputs keep the demo honest. */
    }
    return result;
}
