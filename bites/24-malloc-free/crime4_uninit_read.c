/*
 * Bite 24 — CRIME 4 of 4: UNINITIALIZED READ
 *
 * COMPILE (from the repo root, inside WSL):
 *   gcc -Wall -Wextra -Werror -std=c17 -g -Wno-maybe-uninitialized bites/24-malloc-free/crime4_uninit_read.c -o /tmp/b24-c4
 * RUN UNDER VALGRIND:
 *   valgrind --leak-check=full --show-leak-kinds=all /tmp/b24-c4
 *
 * NOTE ON THE EXTRA FLAG: GCC's -Wmaybe-uninitialized (in -Wall) already
 * suspects this read-before-write and -Werror refuses to build it. Relaxed
 * here ONLY so the crime can be committed and studied under valgrind — the
 * compiler can only *suspect*; valgrind PROVES which byte was read
 * uninitialized, at runtime, through every copy.
 *
 * THE CRIME: malloc does NOT zero the memory it returns (that's calloc's job).
 * The bytes contain whatever the previous owner left there. Reading them and
 * acting on the values (printing, branching, arithmetic) is undefined
 * behavior — the program works on garbage.
 *
 * KEY VALGRIND FINDINGS (actual output of the run above):
 *   ==NNNN== Use of uninitialised value of size 8
 *   ==NNNN==    (inside printf, while formatting the garbage int)
 *   ==NNNN== Conditional jump or move depends on uninitialised value(s)
 *   ==NNNN==    (the if (p[2] > 1000) branch — and stdio internals too)
 *   ==NNNN== Use --track-origins=yes to see where uninitialised values come from
 *   ==NNNN== ERROR SUMMARY: 17 errors from 5 contexts
 *
 * READ IT LIKE A PRO:
 *   - "Use of uninitialised value" — different family than the other three
 *     crimes: the access is legal (you own the block), the DATA is not yours
 *     to trust yet. The printf happened to show "0 0 0 0" — fresh pages are
 *     often zeroed by luck — but valgrind STILL flagged every use: luck is
 *     not defined behavior.
 *   - Valgrind tracks uninitialized-ness per bit through copies; the report
 *     may fire far from the malloc, at the point of decision (the branch).
 *   - --track-origins=yes is the flag that connects the crime back to the
 *     malloc that left the bytes uninitialized.
 *   - Fix: initialize by loop (as main.c does), or use calloc(n, size) when
 *     you truly want zeros.
 */

#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    /* Four ints of fresh heap memory — contents are leftovers, NOT zeros. */
    int *p = malloc(4 * sizeof *p);
    if (p == NULL)
        return 1;

    /* BUG: reading p[0..3] before writing them. The printed values are
     * recycled garbage from previous allocations (often 0 by luck on a fresh
     * system — run it a few times / after other allocations to see real junk). */
    printf("uninitialized heap: %d %d %d %d\n", p[0], p[1], p[2], p[3]);

    /* Bonus crime: branching on garbage — valgrind reports this as a
     * separate "Conditional jump ... depends on uninitialised value(s)". */
    if (p[2] > 1000)
        printf("garbage steered this branch (p[2] = %d)\n", p[2]);

    /* The fix is exactly what main.c does: write before read.
     * for (int i = 0; i < 4; i++) p[i] = i;  */
    p[0] = 1; p[1] = 2; p[2] = 3; p[3] = 4;
    printf("after init       : %d %d %d %d\n", p[0], p[1], p[2], p[3]);

    free(p);
    return 0;
}
