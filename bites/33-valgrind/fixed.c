/*
 * Bite 33 — Valgrind Discipline: the FIXED build (same program, zero leaks)
 *
 * COMPILE (from the repo root, inside WSL):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/33-valgrind/fixed.c -o /tmp/b33-fixed
 * RUN (exact command):
 *   valgrind --leak-check=full --show-leak-kinds=all /tmp/b33-fixed
 *
 * Every bug from buggy.c is fixed here, and the fix comments name the class
 * it belonged to. The verification habit this bite installs:
 *
 *   1. Run under valgrind FIRST, before declaring a bite done:
 *        valgrind --leak-check=full --show-leak-kinds=all ./prog
 *   2. Also compile with sanitizers and rerun (fast, catches different things):
 *        gcc -Wall -Wextra -Werror -std=c17 -g -fsanitize=address,undefined fixed.c -o /tmp/b33-asan
 *        /tmp/b33-asan
 *      (observed: silent, exit 0 — ASan found nothing; two tools, one verdict)
 *   3. From bite 38 this pair becomes a `check:` target in your Makefile —
 *      one already exists in this folder; `make check` runs both valgrind
 *      runs in sequence.
 *
 * KEY VALGRIND FINDINGS (actual run of this file):
 *   ==NNNN== HEAP SUMMARY:
 *   ==NNNN==     in use at exit: 0 bytes in 0 blocks
 *   ==NNNN==   total heap usage: 5 allocs, 5 frees, 4,197 bytes allocated
 *   ==NNNN==   All heap blocks were freed -- no leaks are possible
 *   ==NNNN== ERROR SUMMARY: 0 errors from 0 contexts
 *
 *   (Same allocations as buggy.c — 4 program allocs plus stdio's buffer —
 *    but now every one is freed. The program's output is identical; only
 *    the hygiene changed.)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *g_log;    /* same global as buggy.c — but now it gets freed */

static char *my_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *copy = malloc(n);
    if (copy != NULL)
        memcpy(copy, s, n);
    return copy;
}

typedef struct {
    char *name;
    int age;
} person_t;

/* FIX for class 1: keep the pointer alive until it's freed. */
static void definitely_lost__fixed(void)
{
    char *buf = malloc(40);
    if (buf == NULL)
        return;
    snprintf(buf, 40, "contact:alice");
    puts(buf);

    free(buf);                     /* the owner frees, before the pointer dies */
    buf = NULL;                    /* dangling → NULL: later use crashes loudly */
}

/* FIX for class 2: free the child, then the owner — inside-out. */
static void indirectly_lost__fixed(void)
{
    person_t *p = malloc(sizeof *p);
    if (p == NULL)
        return;
    p->name = my_strdup("Grace Hopper");
    if (p->name == NULL) {
        free(p);                   /* rollback on partial construction */
        return;
    }
    p->age = 45;
    printf("%s is %d\n", p->name, p->age);

    free(p->name);                 /* child first (or after — order is style;
                                      completeness is the rule) */
    free(p);                       /* then the owner */
    p = NULL;
}

/* FIX for class 3: freeing happens even for "reachable" blocks — main frees
 * the global explicitly. `still reachable` is not a pass. */
static void still_reachable_log__fixed(void)
{
    g_log = malloc(32);
    if (g_log == NULL)
        return;
    snprintf(g_log, 32, "log line one");
    puts(g_log);
}

int main(void)
{
    definitely_lost__fixed();
    indirectly_lost__fixed();
    still_reachable_log__fixed();

    free(g_log);                   /* class 3's fix: the last owner frees */
    g_log = NULL;
    return 0;

    /* Reading the report, decoded once and for all:
     *   "Invalid read/write of size N"  → bounds bug or use-after-free
     *   "Invalid free() / delete"       → double free / freeing a non-heap ptr
     *   "definitely lost"               → pointer gone; must fix
     *   "indirectly lost"               → child of a definitely-lost owner
     *   "still reachable"               → pointer alive, free forgotten; fix it
     *   "suppressed"                    → libraries' noise, ignore
     * Goal state: "All heap blocks were freed -- no leaks are possible". */
}
