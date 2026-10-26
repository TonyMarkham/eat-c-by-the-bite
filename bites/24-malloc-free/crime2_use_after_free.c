/*
 * Bite 24 — CRIME 2 of 4: USE AFTER FREE
 *
 * COMPILE (from the repo root, inside WSL):
 *   gcc -Wall -Wextra -Werror -std=c17 -g -Wno-use-after-free bites/24-malloc-free/crime2_use_after_free.c -o /tmp/b24-c2
 * RUN UNDER VALGRIND:
 *   valgrind --leak-check=full --show-leak-kinds=all /tmp/b24-c2
 *
 * NOTE ON THE EXTRA FLAG: GCC 13's -Wall already detects this exact pattern
 * at compile time (-Wuse-after-free) and -Werror refuses to build it. The
 * flag is relaxed here ONLY so the crime can be committed and studied under
 * valgrind. In your own code: leave the warning on — your compiler is the
 * first line of defense; valgrind is for everything it can't see.
 *
 * THE CRIME: free() hands the block back to the allocator, but the pointer
 * still holds the old address. Reading/writing through it now touches memory
 * the allocator may have handed to someone else — silent corruption, or a
 * crash, depending on luck. This is why the idiom is `free(p); p = NULL;`.
 *
 * KEY VALGRIND FINDINGS (actual output of the run above):
 *   ==NNNN== Invalid read of size 4
 *   ==NNNN==    at 0x...: main (crime2_use_after_free.c:53)
 *   ==NNNN==  Address 0x... is 0 bytes inside a block of size 4 free'd
 *   ==NNNN==    at 0x...: free
 *   ==NNNN==    by 0x...: main (crime2_use_after_free.c:51)   <- the free
 *   ==NNNN==    by 0x...: main (crime2_use_after_free.c:44)   <- the malloc
 *   ==NNNN== Invalid write of size 4
 *   ==NNNN== Invalid read of size 4        (the printf AFTER the write)
 *   ==NNNN== ERROR SUMMARY: 3 errors from 3 contexts
 *
 * READ IT LIKE A PRO:
 *   - "Invalid read/write of size 4" — the access itself is illegal.
 *   - "block of size 4 free'd ... by main" — valgrind even points at the
 *     free() that created the dangling pointer, plus the malloc that made it.
 *   - The stack slot `p` still EXISTS (so no leak is reported), but what it
 *     points at is no longer ours. Dangling ≠ leaked; both are crimes.
 */

#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    int *p = malloc(sizeof *p);
    if (p == NULL)
        return 1;

    *p = 7;
    printf("before free: *p = %d\n", *p);

    free(p);                 /* the block now belongs to the allocator */

    /* BUG: two reads and a write through the dangling pointer. */
    printf("after free : *p = %d  <-- reading freed memory\n", *p);
    *p = 99;                 /* <-- writing freed memory: worse */
    printf("wrote %d through a dead pointer\n", *p);

    /* The fix would be: p = NULL; right after free(p) — then this code
     * crashes on a NULL deref immediately, in the debugger, instead of
     * corrupting an unrelated allocation three hours later. */
    return 0;
}
