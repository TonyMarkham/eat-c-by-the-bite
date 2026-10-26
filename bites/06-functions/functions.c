/*
 * Bite 6 — Functions
 *
 * Compile + run (from the repo root):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/06-functions/functions.c -o /tmp/b06 && /tmp/b06
 *
 * BREAK IT ON PURPOSE:
 *   Move max3's definition above main and DELETE its prototype (the
 *   `int max3(int a, int b, int c);` line near the top). With -Werror
 *   the build fails: calling a function the compiler hasn't seen is an
 *   "implicit declaration" error in C99+. That is precisely why headers
 *   exist — a header is a shared block of prototypes (bite 8 puts this
 *   to work across files).
 *
 * Learn tie-ins:
 *   - A *declaration* (prototype) tells the compiler a function exists
 *     and its types; the *definition* supplies the body. You need the
 *     declaration before the first call, not before the definition.
 *   - Arguments are passed BY VALUE: the function gets a copy. Changing
 *     a parameter never touches the caller's variable. (Fixing the swap
 *     needs pointers — the cliffhanger for bite 12.)
 *   - `void f(void)` — never leave the parameter list empty: legacy `()`
 *     means "unspecified arguments", so the compiler stops checking.
 */

#include <stdio.h>

/* --- Do 1: PROTOTYPE (declaration) before main, definition after.
 * Parameter names in a prototype are optional documentation; the
 * compiler only cares about the types: int max3(int, int, int). */
int max3(int a, int b, int c);

/* --- Do 2: attempt_swap takes copies. Note `void` return: it *does*
 * work on its parameters — on the copies. */
void attempt_swap(int a, int b);

/* --- Do 3: bite 5's multiplication table, refactored to take its
 * dimensions. Parameters make a function reusable. */
void print_table(int rows, int cols);

int main(void)
{
    printf("--- max3 ---\n");
    printf("max3(1, 2, 3)     = %d\n", max3(1, 2, 3));
    printf("max3(42, 7, 19)   = %d\n", max3(42, 7, 19));
    printf("max3(-1, -5, -3)  = %d\n", max3(-1, -5, -3));
    /* Ties are fine: any equal max value gives the same answer. */
    printf("max3(9, 9, 2)     = %d\n", max3(9, 9, 2));

    printf("--- attempt_swap: pass-by-value proof ---\n");
    int x = 1;
    int y = 2;
    printf("before: x = %d, y = %d\n", x, y);
    attempt_swap(x, y);
    printf("after:  x = %d, y = %d   <-- unchanged! the swap happened on COPIES\n",
           x, y);
    /* Why: attempt_swap's a and b are fresh variables initialized with
     * x's and y's values. Reassigning them rebinds the copies; nothing
     * travels back out. Phase 2's pointers will pass *addresses* so the
     * function can reach back into the caller's memory. */

    printf("--- print_table(4, 7): the bite-5 table, parameterized ---\n");
    print_table(4, 7);

    return 0;
}

/* Definition of max3. The if-chain is O(1) and obvious; the ternary
 * one-liner `a > b ? (a > c ? a : c) : (b > c ? b : c)` saves two lines
 * and costs readability — not worth it. */
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

/* Definition of attempt_swap. This REALLY swaps — a and b trade values
 * inside this frame. It just doesn't, and can't, affect the caller. */
void attempt_swap(int a, int b)
{
    printf("  inside attempt_swap (working on copies)\n");
    int tmp = a;
    a = b;
    b = tmp;
    printf("  inside: a = %d, b = %d\n", a, b);
}

/* Rows-outer, cols-inner, exactly like bite 5 — but now the caller
 * chooses the size. %4d still aligns anything up to 9999. */
void print_table(int rows, int cols)
{
    for (int row = 1; row <= rows; row++) {
        for (int col = 1; col <= cols; col++) {
            printf("%4d", row * col);
        }
        printf("\n");
    }
}
