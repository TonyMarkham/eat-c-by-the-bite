/*
 * Bite 5 — Loops
 *
 * Compile + run (from the repo root):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/05-loops/loops.c -o /tmp/b05 && /tmp/b05
 *   printf '10 20 30 -1 5 6 -1\n' | /tmp/b05
 *   (two stream segments: the while version sums the first (-> 60, stops
 *   at -1), the for version sums the second (-> 11, stops at the next -1))
 *   printf '' | /tmp/b05                      (EOF with no input: both sums 0)
 *
 * BREAK IT ON PURPOSE:
 *   In print_table, change the inner loop condition from `col <= cols`
 *   to something off by one (e.g. make the OUTER loop `row <= rows`
 *   with rows = 10): you get an 11-row table whose last row is all
 *   zeros. Predict the corruption, then verify. Also write a loop
 *   that never runs — `for (int i = 10; i < 5; i++)` — and confirm
 *   the body is simply skipped. No error, no warning: the condition
 *   is checked first and false on the very first test.
 *
 * Learn tie-ins:
 *   - for = init; condition; step — all three optional (for(;;) forever).
 *   - while checks first; do/while runs at least once (right for menus,
 *     which is exactly what the checkpoint in bite 10 will use).
 *   - break exits the loop; continue skips to the next iteration.
 *   - Off-by-one is the #1 loop bug: `<` vs `<=` matters.
 */

#include <stdio.h>

/* --- Do 1: 10x10 multiplication table with aligned columns.
 * %4d reserves 4 columns per number: products go up to 100 (3 digits)
 * plus a separating space. Change 4 to 3 and watch the columns smear. */
static void print_table(void)
{
    for (int row = 1; row <= 10; row++) {
        for (int col = 1; col <= 10; col++) {
            printf("%4d", row * col);
        }
        printf("\n"); /* one newline per finished row */
    }
}

/* --- Do 2a: sum integers from stdin until EOF or a negative number,
 * `while` flavor. scanf returns the number of items converted; when
 * it's not 1 (EOF or non-numeric garbage) we stop. Note: raw scanf is
 * replaced by the fgets+sscanf pattern in bite 9 — fine for plain
 * number streams like this one. */
static int sum_while(void)
{
    int sum = 0;
    int v;
    while (scanf("%d", &v) == 1) {
        if (v < 0) {
            break;               /* negative number ends the input, by design */
        }
        sum += v;
    }
    return sum;
}

/* --- Do 2b: the same job, `for(;;)` + break flavor — applied to the
 * NEXT segment of the stream (the while version already consumed the
 * first). Some people find "loop forever, exit in the middle" clearer
 * when the exit condition is complicated. Both shapes agree. */
static int sum_for(void)
{
    int sum = 0;
    for (;;) {
        int v;
        if (scanf("%d", &v) != 1 || v < 0) {
            break;               /* EOF or negative: same exit as sum_while */
        }
        sum += v;
    }
    return sum;
}

/* --- Do 3: sum of decimal digits. n = 12345 -> 1+2+3+4+5 = 15.
 * Documented behavior for negatives: we sum the digits of the absolute
 * value, so -123 -> 6. n % 10 peels off the last digit; n /= 10
 * (integer division!) drops it. Loop ends when nothing is left. */
static int sum_digits(int n)
{
    if (n < 0) {
        n = -n;                  /* documented: negatives use |n| */
    }
    int sum = 0;
    while (n > 0) {
        sum += n % 10;
        n /= 10;
    }
    return sum;
}

int main(void)
{
    printf("--- 10x10 multiplication table ---\n");
    print_table();

    /* --- BREAK IT (second half): a loop that never executes is NOT an
     * error. The condition fails on the first check and the body is
     * skipped entirely. */
    for (int i = 10; i < 5; i++) {
        printf("this line can never print (i=%d)\n", i);
    }
    printf("zero-trip loop: skipped cleanly, no error\n");

    printf("--- sum_digits tests ---\n");
    printf("sum_digits(0)     = %d\n", sum_digits(0));
    printf("sum_digits(12345) = %d\n", sum_digits(12345));
    printf("sum_digits(-123)  = %d  (negatives use |n|)\n", sum_digits(-123));

    /* Do 2 on real stdin: each version consumes numbers until it hits a
     * negative number or EOF. They share ONE stdin stream, so the test
     * input carries two segments: the while version eats the first
     * segment (stops at its -1), the for version eats the second.
     * With no input at all, both give 0. Both shapes must agree. */
    printf("--- sum until EOF or negative (while version) ---\n");
    printf("sum = %d\n", sum_while());

    printf("--- sum until EOF or negative (for version) ---\n");
    printf("sum = %d\n", sum_for());

    /* continue demo: skip multiples of 3 while counting to 10.
     * In a for loop, `continue` still runs the step expression. */
    printf("--- continue demo: 1..10 skipping multiples of 3 ---\n");
    for (int i = 1; i <= 10; i++) {
        if (i % 3 == 0) {
            continue;
        }
        printf("%d ", i);
    }
    printf("\n");

    return 0;
}
