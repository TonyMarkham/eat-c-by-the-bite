/*
 * Bite 24 — CRIME 3 of 4: DOUBLE FREE
 *
 * COMPILE (from the repo root, inside WSL):
 *   gcc -Wall -Wextra -Werror -std=c17 -g -Wno-use-after-free bites/24-malloc-free/crime3_double_free.c -o /tmp/b24-c3
 * RUN UNDER VALGRIND:
 *   valgrind --leak-check=full --show-leak-kinds=all /tmp/b24-c3
 *
 * NOTE ON THE EXTRA FLAG: GCC 13's -Wall flags the second free(p) as
 * -Wuse-after-free (using p again after it was freed — which a double free
 * literally is) and -Werror refuses to build it. Relaxed here ONLY so the
 * crime can be committed and studied under valgrind. Leave it on in real code.
 *
 * THE CRIME: the same block is handed back to the allocator twice. The second
 * free() corrupts the allocator's own bookkeeping — the classic path to
 * exploitable heap bugs in real software. glibc usually aborts with
 * "free(): double free detected"; valgrind catches it more precisely first.
 *
 * KEY VALGRIND FINDINGS (actual output of the run above):
 *   ==NNNN== Invalid free() / delete / delete[]
 *   ==NNNN==    at 0x...: free
 *   ==NNNN==    by 0x...: main (crime3_double_free.c:NN)
 *   ==NNNN==  Address 0x... is 0 bytes inside a block of size 4 free'd
 *   ==NNNN==    at 0x...: free
 *   ==NNNN==    by 0x...: main (crime3_double_free.c:NN)   <- the FIRST free
 *   ==NNNN== ERROR SUMMARY: 1 errors from 1 contexts
 *
 * READ IT LIKE A PRO:
 *   - "Invalid free()" — the second free() is the crime scene.
 *   - Valgrind shows the block was already "free'd" and names the first
 *     free's line number. Two stack traces = the pair of culprits.
 *   - Classic real-world cause: two code paths both "clean up" the same
 *     resource, or a struct is freed twice via two aliases to it.
 *   - The p = NULL; after free idiom makes this a harmless no-op
 *     (free(NULL) is defined to do nothing) instead of a bomb.
 */

#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    int *p = malloc(sizeof *p);
    if (p == NULL)
        return 1;

    *p = 5;
    printf("value: %d\n", *p);

    free(p);      /* free #1 — correct */
    /* BUG: free #2. The block was already returned; the allocator's internal
     * structures now get corrupted. */
    free(p);

    /* Fix: after free #1, set p = NULL; then free(p) here would be legal
     * (freeing NULL is a no-op by definition). */
    return 0;
}
