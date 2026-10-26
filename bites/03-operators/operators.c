/*
 * Bite 3 — Operators & Precedence
 *
 * Compile + run (from the repo root):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/03-operators/operators.c -o /tmp/b03 && /tmp/b03
 *
 * BREAK IT ON PURPOSE:
 *   Write `if (x = 5)` instead of `if (x == 5)`. With -Wall gcc says
 *   "suggest parentheses around assignment used as truth value"; with
 *   -Werror the build fails. The danger: `=` *returns a value* (the
 *   assigned value), so `x = 5` is 5 = true, and it silently overwrites
 *   x. This bug ships in production C every year — you've now met it
 *   in a safe place. It's kept as a comment here because -Werror (our
 *   day-one policy) rightly refuses to compile it:
 *
 *     int x = 0;
 *     if (x = 5) printf("oops: x is %d\n", x);   // x is now 5, always taken
 *
 * Learn tie-ins:
 *   - Integer division truncates toward zero: 7/2 == 3, and 7%2 == 1.
 *   - If EITHER operand is floating point, the other promotes: 7/2.0 == 3.5.
 *   - Don't memorize precedence — use parentheses.
 *   - i++ returns the OLD value, ++i returns the new one. Mixing them with
 *     other uses of i in one expression (i = i++ + 1) is undefined behavior.
 */

#include <stdio.h>
#include <limits.h> /* INT_MAX */

/* Compound interest with a helper function instead of a loop (Do 1).
 * balance * 1.05 grows the balance by 5%; note both operands are
 * floating point, so there is no accidental integer division here. */
static double grow5pct(double balance)
{
    return balance * 1.05;
}

/* Do 2: integer average. Works for (1,2) -> 1... and overflows for
 * (INT_MAX, INT_MAX): a + b is 2 * INT_MAX, which wraps around (signed
 * overflow is undefined behavior!) before we divide by 2. The textbook
 * overflow-safe form is a + (b - a) / 2 — meet it again later. */
static int avg(int a, int b)
{
    return (a + b) / 2;
}

int main(void)
{
    /* --- Do 1: compound interest, principal 1000, 5% yearly, years 1-10.
     * No loops allowed — the ten calls below ARE the loop, unrolled.
     * %.2f because money wants exactly two decimals. */
    double balance = 1000.0;
    printf("compound interest, 1000 at 5%%:\n");
    balance = grow5pct(balance); printf("year  1: %8.2f\n", balance);
    balance = grow5pct(balance); printf("year  2: %8.2f\n", balance);
    balance = grow5pct(balance); printf("year  3: %8.2f\n", balance);
    balance = grow5pct(balance); printf("year  4: %8.2f\n", balance);
    balance = grow5pct(balance); printf("year  5: %8.2f\n", balance);
    balance = grow5pct(balance); printf("year  6: %8.2f\n", balance);
    balance = grow5pct(balance); printf("year  7: %8.2f\n", balance);
    balance = grow5pct(balance); printf("year  8: %8.2f\n", balance);
    balance = grow5pct(balance); printf("year  9: %8.2f\n", balance);
    balance = grow5pct(balance); printf("year 10: %8.2f\n", balance);

    /* --- Do 2: avg() on normal and extreme values.
     * (1,2) truncates to 1 — same 7/2 surprise. (INT_MAX, INT_MAX)
     * overflows: on this platform you'll likely see a huge negative
     * number. Predict before running! */
    printf("avg(1,2)             = %d\n", avg(1, 2));
    printf("avg(INT_MAX,INT_MAX) = %d  (integer overflow!)\n",
           avg(INT_MAX, INT_MAX));

    /* --- Do 3: four divisions, four lessons.
     *   7/2       -> both int,   truncate: 3
     *   7%2       -> remainder of integer division: 1
     *   7.0/2     -> double wins, promotion: 3.5
     *   (double)7/2 -> cast applies to 7 *before* the divide: 3.5
     * Caution: (double)(7/2) would be 3.0 — the cast comes after the
     * integer math already happened. Precedence in action. */
    printf("7/2 = %d, 7%%2 = %d, 7.0/2 = %f, (double)7/2 = %f\n",
           7 / 2, 7 % 2, 7.0 / 2, (double)7 / 2);

    /* Increments: i++ hands back the old value, ++i the new one.
     * Careful: never mix a ++ with another read of the same variable in
     * ONE expression (e.g. printf("...", i++, i)) — the order of argument
     * evaluation is unsequenced, so that's undefined behavior. Two
     * separate statements keep the sequence points explicit. */
    int i = 5, j = 5;
    int old_i = i++;
    int new_j = ++j;
    printf("i++ returned %d (i is now %d)\n", old_i, i);
    printf("++j returned %d (j is now %d)\n", new_j, j);

    return 0;
}
