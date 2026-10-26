/*
 * Bite 2 — Variables, Types, printf
 *
 * Compile + run (from the repo root):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/02-vars/vars.c -o /tmp/b02 && /tmp/b02
 *
 * BREAK IT ON PURPOSE:
 *   Print an int with %f and a double with %d. With -Wall gcc warns;
 *   with -Werror the build *fails*. Remove -Werror and it compiles —
 *   and prints garbage, because printf reads the bits with the wrong
 *   interpretation (int and double are passed differently through
 *   varargs). That garbage is undefined behavior, not an error.
 *   This is why warnings-as-errors is day-one policy. Try it:
 *
 *     int   i = 42;    printf("%f\n", i);   // wrong specifier
 *     double d = 3.14; printf("%d\n", d);   // wrong specifier
 *
 * Learn tie-ins:
 *   - sizeof is an *operator*, not a function. Only sizeof(char) == 1 is
 *     guaranteed by the standard; every other size is platform-dependent.
 *   - %zu is the correct specifier for sizeof results (type size_t).
 *   - Unsigned arithmetic is modulo 2^N — wraparound is *defined* in C,
 *     while signed overflow is undefined. "Not a bug" in C's eyes.
 */

#include <stdio.h>

int main(void)
{
    /* One variable per basic type, initialized right away. Uninitialized
     * locals hold indeterminate values — reading them is UB. */
    char        c = 'A';                 /* 1 byte by definition            */
    short       s = 30000;
    int         i = 2000000000;
    long        l = 9000000000L;         /* L suffix: at least 64-bit here  */
    long long   ll = 9000000000LL;
    float       f = 3.14159f;            /* f suffix = float, not double    */
    double      d = 3.14159265358979;
    unsigned int u = 4000000000u;        /* u suffix = unsigned             */

    /* --- Do 1: table of type -> sizeof -> sample value with right specifier */
    /* %-10s left-aligns the label in 10 columns; %zu prints a size_t. */
    printf("%-10s %zu byte(s)  sample: %d\n",  "char",      sizeof(char),      c);
    printf("%-10s %zu byte(s)  sample: %d\n",  "short",     sizeof(short),     s);
    printf("%-10s %zu byte(s)  sample: %d\n",  "int",       sizeof(int),       i);
    printf("%-10s %zu byte(s)  sample: %ld\n", "long",      sizeof(long),      l);
    printf("%-10s %zu byte(s)  sample: %lld\n","long long", sizeof(long long), ll);
    printf("%-10s %zu byte(s)  sample: %f\n",  "float",     sizeof(float),     f);
    printf("%-10s %zu byte(s)  sample: %f\n",  "double",    sizeof(double),    d);
    printf("%-10s %zu byte(s)  sample: %u\n",  "unsigned",  sizeof(unsigned int), u);
    /* %f defaults to 6 decimal places; %.2f is the everyday control. */
    printf("double with precision:  %.2f  and %.10f\n", d, d);
    /* %x prints an int as hexadecimal — you'll live in hex in Phase 4+. */
    printf("int as hex: %x\n", i);

    /* --- Do 2: print the address of an int.
     * &i takes the address; the cast to (void*) is required because %p
     * expects a void*. Addresses differ every run — that's ASLR. */
    printf("address of i: %p\n", (void *)&i);

    /* --- Do 3: unsigned wraparound. u is 0; subtracting 1 wraps to the
     * largest unsigned value (2^32 - 1 = 4294967295 on a 32-bit int
     * platform). This is *defined* behavior — modular arithmetic. */
    unsigned v = 0;
    v -= 1;
    printf("0u - 1 = %u  (unsigned wraparound, not a crash)\n", v);

    return 0;
}
