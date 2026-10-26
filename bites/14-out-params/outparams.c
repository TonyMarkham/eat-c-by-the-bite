/*
 * Bite 14 - Pointers as Out-Parameters
 * ====================================
 * Compile & run (from the repo root inside WSL):
 *
 *     gcc -Wall -Wextra -Werror -std=c17 -g bites/14-out-params/outparams.c -o /tmp/b14 && /tmp/b14
 *
 * Try it with piped input (so you don't have to type):
 *
 *     printf '3 4\ngarbage\n12 x\n-5 8\nq\n' | /tmp/b14
 *
 * The "Learn" list, translated into code below:
 *   - Convention: `const T *in` = "I only read this",
 *                 `T *out`      = "I write through this".
 *   - The CALLER owns the storage: it must pass a pointer to a real
 *     variable. The function must never write through a const pointer,
 *     and must leave out-params untouched when it fails.
 *   - Combined return+out pattern: return 0/-1 for status, deliver the
 *     data through out-params. You will use this shape for years.
 *
 * BREAK IT ON PURPOSE:
 *   Call minmax(vals, 5, NULL, &max); -- if the implementation
 *   dereferences `min` without checking, it crashes (segfault under
 *   ASan). Then decide your policy: runtime-check for NULL, or document
 *   "must not be NULL"? Both are valid; being CONSISTENT is the skill.
 *   This code documents the precondition and does not check -- the
 *   break mode proves what that choice costs if a caller misbehaves.
 *
 *     gcc -DBREAK_IT=1 -Wall -Wextra -Werror -std=c17 -g \
 *         bites/14-out-params/outparams.c -o /tmp/b14break && /tmp/b14break
 */

#include <stdio.h>

/*
 * Writes the smallest element through *min and the largest through *max.
 *
 * Contract (this comment IS the interface):
 *   - a points to n ints (or n == 0),
 *   - min and max must not be NULL   <-- documented precondition,
 *   - with n == 0 we write NOTHING: the caller's variables keep their
 *     old values. Leaving them untouched is kinder than writing garbage,
 *     and "untouched" is testable; "garbage" is not.
 */
static void minmax(const int *a, size_t n, int *min, int *max)
{
    if (n == 0)
        return;             /* documented: out-params left untouched */

    *min = a[0];
    *max = a[0];
    for (size_t i = 1; i < n; i++) {
        if (a[i] < *min)
            *min = a[i];
        if (a[i] > *max)
            *max = a[i];
    }
}

/*
 * Returns 0 on success (and fills *a and *b), -1 on garbage (and fills
 * NOTHING). sscanf returns how many items it converted -- anything less
 * than 2 means the line was not "two integers", so we reject the whole
 * line rather than use a half-converted result. This is the
 * return-status + out-param pattern in its smallest form.
 */
static int parse_two_ints(const char *line, int *a, int *b)
{
    return sscanf(line, "%d %d", a, b) == 2 ? 0 : -1;
}

/*
 * bite 10's calculator input handling, rebuilt with the pattern above:
 * read a line, hand it to a parser, act on the status code. The parser
 * never crashes, no matter what the user types.
 */
int main(void)
{
    /* minmax: two results from one function call. */
    int vals[5] = { 7, -3, 42, 0, 19 };
    int lo = 0, hi = 0;

    minmax(vals, 5, &lo, &hi);
    printf("minmax: min=%d max=%d\n", lo, hi);

    minmax(vals, 0, &lo, &hi);   /* n == 0: writes nothing */
    printf("after n==0 call: lo=%d hi=%d  (untouched, as documented)\n\n", lo, hi);

    /* The tiny read-eval loop. Note what does NOT happen: no scanf
     * leftover garbage, no crashes on "abc", just a status code. */
    char line[128];
    printf("type two integers per line ('q' quits):\n");
    for (;;) {
        printf("calc> ");
        fflush(stdout);         /* show the prompt before blocking on input */

        if (fgets(line, sizeof line, stdin) == NULL) {
            printf("\n(end of input)\n");   /* EOF: not an error, an exit */
            break;
        }
        if (line[0] == 'q' || line[0] == 'Q')
            break;

        int x, y;
        if (parse_two_ints(line, &x, &y) != 0) {
            printf("  garbage in, status -1 out: need exactly two integers\n");
            continue;
        }
        printf("  %d + %d = %d, %d * %d = %d\n", x, y, x + y, x, y, x * y);
    }

#ifndef BREAK_IT
#define BREAK_IT 0   /* set to 1 (or -DBREAK_IT=1) to commit the crime */
#endif
#if BREAK_IT
    /* THE CRIME: pass NULL where the contract demands a real address.
     * The implementation (honestly following its own docs) does
     * *min = a[0] -- and writes through NULL. Segfault, and under ASan
     * a clean report pointing at the exact line. If this feels
     * unfriendly, that is the lesson: pick a NULL policy and apply it
     * everywhere (check-and-return, or precondition-by-document). */
    int just_max;
    printf("\n--- breaking: minmax(vals, 5, NULL, &just_max) ---\n");
    minmax(vals, 5, NULL, &just_max);
    printf("survived (you will not see this under ASan): max=%d\n", just_max);
#endif

    return 0;
}
