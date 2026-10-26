/*
 * Bite 4 — Control Flow: if/else, switch
 *
 * Compile + run (from the repo root):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/04-control-flow/fizzbuzz.c -o /tmp/b04 && /tmp/b04
 *
 * BREAK IT ON PURPOSE:
 *   Remove one `break` from the switch version of FizzBuzz (marked
 *   below as a comment). Then feed it the case that falls through and
 *   predict the output before running: C's switch falls through into
 *   the next case unless you break, so e.g. case 5 would print BOTH
 *   the "FizzBuzz" branch and the "Fizz" branch. Fall-through is the
 *   default; `break` is the fix.
 *
 * Learn tie-ins:
 *   - Truthiness: 0 is false, anything else is true. stdbool.h's `bool`
 *     is a convenience alias over that rule, not a new kind of type.
 *   - && and || short-circuit: the right side isn't evaluated if the
 *     left already decides the answer. That's why n % 3 == 0 &&
 *     print_word("Fizz") is a safe (if cheeky) pattern.
 *   - switch only accepts integer types with *constant* case labels.
 */

#include <stdio.h>
#include <stdbool.h>

/* --- Do 1: classic FizzBuzz for one number n, using if/else.
 * Order matters: the divisible-by-15 test MUST come first, otherwise
 * "15" matches plain %3 and prints "Fizz" instead of "FizzBuzz".
 * The single printf at the end is a small pattern worth stealing:
 * decide WHAT to print, then print it once. */
static void fizzbuzz_if(int n)
{
    if (n % 15 == 0) {
        printf("FizzBuzz");
    } else if (n % 3 == 0) {
        printf("Fizz");
    } else if (n % 5 == 0) {
        printf("Buzz");
    } else {
        printf("%d", n);
    }
    printf("\n");
}

/* --- Do 2: FizzBuzz via switch (n % 15). The 15 residues collapse
 * into 4 interesting groups: 0, multiples of 3, multiples of 5, rest.
 * Note case labels are compile-time constants, and that 3,6,9,12 all
 * share one branch by stacking labels. */
static void fizzbuzz_switch(int n)
{
    switch (n % 15) {
    case 0:                       /* divisible by both 3 and 5 */
        printf("FizzBuzz");
        break;
    case 3:
    case 6:
    case 9:
    case 12:                      /* divisible by 3 only */
        printf("Fizz");
        break;
    case 5:
    case 10:                      /* divisible by 5 only */
        printf("Buzz");
        break;
    default:                      /* everything else */
        printf("%d", n);
        break;
    }
    printf("\n");

    /* BREAK IT ON PURPOSE: delete the `break;` after case 0 above.
     * Then n = 15 falls through and prints "FizzBuzzFizz".
     * Predict it, run it, add the break back. */
}

/* --- Do 3: clamp v into [lo, hi]. Three boundaries to test:
 * below lo, inside the range, above hi. The second if can reuse v
 * because the first if already ran — plain sequential logic. */
static int clamp(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

int main(void)
{
    printf("--- FizzBuzz with if/else, 1..100 ---\n");
    for (int n = 1; n <= 100; n++) {
        fizzbuzz_if(n);
    }

    printf("--- FizzBuzz with switch (n %% 15), 1..15 ---\n");
    for (int n = 1; n <= 15; n++) {
        fizzbuzz_switch(n);
    }

    printf("--- clamp boundary tests ---\n");
    printf("clamp(-5,  0, 10) = %2d  (below lo -> clamped)\n", clamp(-5, 0, 10));
    printf("clamp( 5,  0, 10) = %2d  (inside     -> unchanged)\n", clamp(5, 0, 10));
    printf("clamp(50,  0, 10) = %2d  (above hi  -> clamped)\n", clamp(50, 0, 10));

    /* Truthiness demo: 0 is false, 1 and -7 are true. bool is a thin
     * convenience over "zero vs nonzero". */
    bool t = true;
    printf("bool as int: %d ; if (0) is false, if (-7) is true\n", t);

    return 0;
}
