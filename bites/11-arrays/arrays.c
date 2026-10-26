/*
 * Bite 11 - Arrays
 * =================
 * Compile & run (from the repo root inside WSL):
 *
 *     gcc -Wall -Wextra -Werror -std=c17 -g bites/11-arrays/arrays.c -o /tmp/b11 && /tmp/b11
 *
 * The "Learn" list, translated into code below:
 *   - int a[10] is ten ints side by side in memory, indexes 0..9.
 *     C performs NO bounds checking: a[10] is undefined behavior (UB),
 *     not an error. UB means "anything may happen" -- and on real
 *     machines it usually means "silently corrupts something, until
 *     the day it deletes your database".
 *   - THE BIG LIE: passing an array to a function actually passes a
 *     pointer to its first element. Inside the function, sizeof sees a
 *     pointer (8 bytes on x86-64), never the array's true size. That is
 *     why every array-taking function below also takes a length `n`.
 *   - Initializers: {1,2,3} fills the front, {0} zeroes EVERYTHING,
 *     int a[] = {...} lets the compiler count for you.
 *
 * BREAK IT ON PURPOSE:
 *   Write past the end: a[10] = 42 on a 10-element array. Run it -- it
 *   will probably "work", because UB often silently works. Then rebuild
 *   with AddressSanitizer and run again -- now it screams:
 *
 *     gcc -DBREAK_IT=1 -Wall -Wextra -Werror -std=c17 -g \
 *         bites/11-arrays/arrays.c -o /tmp/b11break && /tmp/b11break
 *
 *   Verified on GCC 13.3: the -DBREAK_IT=1 build compiles even under
 *   -Werror, and ASan reports "stack-buffer-overflow" on the run.
 *   ASan is your always-on safety net from now on.
 */

#include <stdio.h>
#include <stddef.h>   /* size_t */

/*
 * Note the parameter: `const int *a`, not `int a[10]`. To the compiler
 * those are THE SAME THING in a parameter list -- "array of int" silently
 * decays to "pointer to int". The `const` promises we only READ the array
 * (bite 21 makes this a contract).
 *
 * And that is exactly why `n` must ride along as a separate parameter:
 * inside this function there is no way to ask the array how long it is.
 * Nobody can. The size information dies at the function boundary.
 */
static void print_ints(const int *a, size_t n)
{
    for (size_t i = 0; i < n; i++)
        printf("  [%zu] = %d\n", i, a[i]);
}

static int sum(const int *a, size_t n)
{
    int total = 0;
    for (size_t i = 0; i < n; i++)
        total += a[i];
    return total;
}

/* Precondition (documented, not hidden): n >= 1. With n == 0 there IS no
 * maximum, so we return 0 rather than read a[0] out of bounds. */
static int max_in(const int *a, size_t n)
{
    int best = 0;
    for (size_t i = 0; i < n; i++)
        if (a[i] > best)
            best = a[i];
    return best;
}

/*
 * The lie, demonstrated live. `arr` arrived here as a pointer, so
 * sizeof(arr) is sizeof(int *) -- 8 on x86-64 -- no matter how big the
 * caller's array was. This single print is why C array APIs always look
 * like f(int *a, size_t n).
 */
static void show_sizeof_in_function(const int *arr)
{
    printf("inside function:  sizeof(arr) = %zu bytes  <- it's just a pointer!\n",
           sizeof arr);
}

int main(void)
{
    /* {0} zeroes every element -- the idiomatic "empty array". */
    int squares[10] = { 0 };

    /* Fill with squares: indexes 0..9, never 10. */
    for (size_t i = 0; i < 10; i++)
        squares[i] = (int)(i * i);

    printf("squares:\n");
    print_ints(squares, 10);
    printf("sum = %d, max = %d\n\n", sum(squares, 10), max_in(squares, 10));

    /* In main, `squares` is still a real array, so sizeof knows its size. */
    printf("in main:          sizeof(squares)                    = %zu bytes\n",
           sizeof squares);
    printf("in main:          sizeof(squares) / sizeof(squares[0]) = %zu elements\n",
           sizeof squares / sizeof squares[0]);
    show_sizeof_in_function(squares);   /* passing = decaying to &squares[0] */
    printf("\n");

    /* Initializer variants from the "Learn" list: */
    int counted[]  = { 1, 2, 3 };   /* compiler counts: exactly 3 elements */
    int zeroed[5]  = { 0 };         /* {0} zeroes all five                 */
    int partial[5] = { 1, 2 };      /* {1,2,0,0,0}: the rest become 0      */

    printf("int counted[] = {1,2,3}:  %zu elements\n",
           sizeof counted / sizeof counted[0]);
    printf("int zeroed[5] = {0}:     ");
    for (size_t i = 0; i < 5; i++)
        printf(" %d", zeroed[i]);
    printf("\nint partial[5] = {1,2}:  ");
    for (size_t i = 0; i < 5; i++)
        printf(" %d", partial[i]);
    printf("\n");

#ifndef BREAK_IT
#define BREAK_IT 0   /* set to 1 (or -DBREAK_IT=1) to commit the crime */
#endif
#if BREAK_IT
    /* THE CRIME: squares has indexes 0..9. Index 10 is past the end.
     * No compiler error, no runtime error -- just silent corruption of
     * whatever happens to live next to the array on the stack...
     * ...until AddressSanitizer catches the write red-handed. */
    printf("\n--- breaking: squares[10] = 42 (10-element array!) ---\n");
    squares[10] = 42;
    printf("wrote past the end and nothing crashed. That's UB for you.\n");
#endif

    return 0;
}
