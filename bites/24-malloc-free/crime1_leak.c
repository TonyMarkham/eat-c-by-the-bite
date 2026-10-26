/*
 * Bite 24 — CRIME 1 of 4: the LEAK
 *
 * COMPILE (from the repo root, inside WSL):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/24-malloc-free/crime1_leak.c -o /tmp/b24-c1
 * RUN UNDER VALGRIND:
 *   valgrind --leak-check=full --show-leak-kinds=all /tmp/b24-c1
 *
 * THE CRIME: memory is allocated, used, and never freed. The pointer dies at
 * end of scope, so the block is unreachable — but still allocated. Do this in
 * a loop and your program eats RAM until the OOM killer finds you.
 *
 * KEY VALGRIND FINDINGS (actual output of the run above):
 *   ==NNNN== HEAP SUMMARY:
 *   ==NNNN==     in use at exit: 4 bytes in 1 blocks
 *   ==NNNN==   total heap usage: 2 allocs, 1 frees, 4,100 bytes allocated
 *   ==NNNN==
 *   ==NNNN== 4 bytes in 1 blocks are definitely lost in loss record 1 of 1
 *   ==NNNN==    at 0x...: malloc
 *   ==NNNN==    by 0x...: main (crime1_leak.c:44)
 *   ==NNNN==
 *   ==NNNN== LEAK SUMMARY:
 *   ==NNNN==    definitely lost: 4 bytes in 1 blocks
 *   ==NNNN==    indirectly lost: 0 bytes in 0 blocks
 *   ==NNNN==    still reachable: 0 bytes in 0 blocks
 *   ==NNNN== ERROR SUMMARY: 1 errors from 1 contexts
 *
 * READ IT LIKE A PRO:
 *   - "1 frees" vs "2 allocs": the arithmetic of a leak. (The second alloc is
 *     stdio's output buffer, freed at exit — only our 4-byte block remains.)
 *   - "definitely lost" — no pointer to the block survives; it can never be
 *     freed now. This is the leak class you must drive to zero.
 *   - Note that --leak-check=full counts a definite leak AS an error, so the
 *     summary is not zero even though nothing was dereferenced illegally.
 *     A leak is not an invalid access — the program behaves "fine" while
 *     quietly bleeding. Only the leak summary catches it.
 */

#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    int *p = malloc(sizeof *p);
    if (p == NULL)
        return 1;

    *p = 1;
    printf("leaking one int: *p = %d\n", *p);

    /* BUG: no free(p) here. main returns, p vanishes from existence, and the
     * 4-byte block is orphaned forever. Valgrind will show it as
     * "definitely lost". */
    return 0;
}
