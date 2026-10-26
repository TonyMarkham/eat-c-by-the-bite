/*
 * Bite 24 — malloc/free, First Contact (the CLEAN program)
 *
 * COMPILE & RUN (from the repo root, inside WSL):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/24-malloc-free/main.c -o /tmp/b24 && /tmp/b24
 *   valgrind --leak-check=full --show-leak-kinds=all /tmp/b24     → 0 errors, 0 leaks
 *
 * BREAK IT ON PURPOSE: this bite's break-it IS the four crimes. One file per
 * crime in this folder, each runnable under valgrind:
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/24-malloc-free/crime1_leak.c        -o /tmp/b24-c1
 *   gcc -Wall -Wextra -Werror -std=c17 -g -Wno-use-after-free bites/24-malloc-free/crime2_use_after_free.c -o /tmp/b24-c2
 *   gcc -Wall -Wextra -Werror -std=c17 -g -Wno-use-after-free bites/24-malloc-free/crime3_double_free.c    -o /tmp/b24-c3
 *   gcc -Wall -Wextra -Werror -std=c17 -g -Wno-maybe-uninitialized bites/24-malloc-free/crime4_uninit_read.c -o /tmp/b24-c4
 *   valgrind --leak-check=full --show-leak-kinds=all /tmp/b24-cN    (N = 1..4)
 * Read every word of each report; the key lines are pasted into each file's
 * comments. These four reports are your catalog of future bugs.
 *
 * WHY (the "Learn" bullets):
 *   - void *malloc(size_t n) returns NULL on failure — ALWAYS check.
 *     void * converts implicitly to any object pointer, no cast needed.
 *   - free(p) hands the memory back; p becomes dangling. Setting p = NULL
 *     right after converts "possible silent corruption" into a loud,
 *     debuggable crash the next time you touch it.
 *   - The four crimes: leak, use-after-free, double-free, uninitialized read.
 *     Valgrind names each one explicitly.
 */

#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    /* ---- 1) The canonical single allocation ----------------------------
     * Note the idiom `sizeof *p`: if the type of p changes next month,
     * the malloc stays correct. Never write sizeof(int) here. */
    int *p = malloc(sizeof *p);
    if (p == NULL) {                       /* malloc CAN fail. Always check. */
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    *p = 42;                               /* the memory is ours now */
    printf("heap int  : *p = %d at %p\n", *p, (void *)p);

    free(p);                               /* promise kept */
    p = NULL;                              /* now any later *p crashes LOUDLY
                                              instead of corrupting silently */

    /* ---- 2) An array of 10 ints, initialized by loop -------------------
     * malloc does NOT zero memory (that's calloc). Never assume zeros:
     * freshly malloc'd bytes hold whatever the previous user left behind. */
    int *arr = malloc(10 * sizeof *arr);
    if (arr == NULL) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    for (int i = 0; i < 10; i++)
        arr[i] = i * i;                    /* initialize BEFORE reading */
    printf("heap array:");
    for (int i = 0; i < 10; i++)
        printf(" %d", arr[i]);
    printf("\n");

    free(arr);
    arr = NULL;

    /* Nothing else to do: exiting frees everything anyway, but we already
     * freed by hand. Under valgrind this program is spotless:
     *   "All heap blocks were freed -- no leaks are possible"
     *   "ERROR SUMMARY: 0 errors from 0 contexts" */
    return 0;
}
