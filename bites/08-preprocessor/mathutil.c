/*
 * Bite 8 — The Preprocessor & Multi-File Compile (IMPLEMENTATION)
 *
 * This file DEFINES the three functions that mathutil.h DESCRIBES.
 * Note it includes its own header — that's how the compiler checks
 * that the definitions actually match the promised prototypes (a
 * mismatch here is a compile error, which is exactly what you want).
 *
 * Compile this file to an object file (from bites/08-preprocessor/):
 *   gcc -Wall -Wextra -Werror -std=c17 -g -c mathutil.c
 */

#include "mathutil.h"

/* Quotes, not angle brackets: "..." searches the local directory first,
 * so the preprocessor finds OUR mathutil.h, not some system file. */

int max3(int a, int b, int c)
{
    int m = a;
    if (b > m) {
        m = b;
    }
    if (c > m) {
        m = c;
    }
    return m;
}

int clamp(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

/* Negatives use |n|, same documented decision as bite 5. */
int sum_digits(int n)
{
    if (n < 0) {
        n = -n;
    }
    int sum = 0;
    while (n > 0) {
        sum += n % 10;
        n /= 10;
    }
    return sum;
}
