/*
 * Bite 33 — Valgrind Discipline: the BUGGY build (three leak classes)
 *
 * COMPILE (from the repo root, inside WSL):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/33-valgrind/buggy.c -o /tmp/b33-buggy
 * RUN (exact command):
 *   valgrind --leak-check=full --show-leak-kinds=all /tmp/b33-buggy
 *
 * THE FIX lives in fixed.c in this folder; run it the same way:
 *   valgrind --leak-check=full --show-leak-kinds=all /tmp/b33-fixed
 *
 * This program deliberately commits THREE different leak classes so one run
 * teaches you to read all three report sections:
 *
 *   1. DEFINITELY LOST   — the last pointer to a block is gone (overwritten,
 *                          or the function returned). Nothing can ever free it.
 *   2. INDIRECTLY LOST   — a block whose only reference was held by ANOTHER
 *                          lost block. Owner and child vanish together; the
 *                          child shows up as "indirectly lost".
 *   3. STILL REACHABLE   — the pointer still exists at exit (here: a global)
 *                          but nobody freed it. Not a "loss" yet, still a bug
 *                          you fix — leak-check at exit is your safety net.
 *
 * Workflow being drilled (this is the deliverable of bite 33):
 *   run → read FROM THE FIRST ERROR/RECORD DOWN → fix → rerun.
 * Errors cascade; the first one is usually the real one.
 *
 * KEY VALGRIND FINDINGS (actual run; full summary pasted below):
 *   ==NNNN== HEAP SUMMARY:
 *   ==NNNN==     in use at exit: 101 bytes in 4 blocks
 *   ==NNNN==   total heap usage: 5 allocs, 1 frees, 4,197 bytes allocated
 *   ==NNNN==
 *   ==NNNN== 13 bytes in 1 blocks are indirectly lost in loss record 1 of 4
 *   ==NNNN==    at 0x...: malloc
 *   ==NNNN==    by 0x...: my_strdup (buggy.c:72)
 *   ==NNNN==    by 0x...: indirectly_lost (buggy.c:102)
 *   ==NNNN==
 *   ==NNNN== 29 (16 direct, 13 indirect) bytes in 1 blocks are definitely
 *   ==NNNN== lost in loss record 2 of 4
 *   ==NNNN==    at 0x...: malloc
 *   ==NNNN==    by 0x...: indirectly_lost (buggy.c:99)
 *   ==NNNN==
 *   ==NNNN== 32 bytes in 1 blocks are still reachable in loss record 3 of 4
 *   ==NNNN==    at 0x...: malloc
 *   ==NNNN==    by 0x...: still_reachable_log (buggy.c:119)
 *   ==NNNN==
 *   ==NNNN== 40 bytes in 1 blocks are definitely lost in loss record 4 of 4
 *   ==NNNN==    at 0x...: malloc
 *   ==NNNN==    by 0x...: definitely_lost (buggy.c:86)
 *   ==NNNN==
 *   ==NNNN== LEAK SUMMARY:
 *   ==NNNN==    definitely lost: 56 bytes in 2 blocks
 *   ==NNNN==    indirectly lost: 13 bytes in 1 blocks
 *   ==NNNN==    still reachable: 32 bytes in 1 blocks
 *   ==NNNN== ERROR SUMMARY: 2 errors from 2 contexts
 * (Byte counts: 40 = the overwritten contact buffer (class 1); 16 = the
 *  person struct, whose 13-byte strdup'd name shows up separately as
 *  "indirectly lost" (class 2); 32 = the never-freed global log (class 3).
 *  With --leak-check=full, definite leaks count as errors, hence 2.)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* LEAK CLASS 3 setup: a global pointer keeps the block "reachable" at exit. */
static char *g_log;

static char *my_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *copy = malloc(n);
    if (copy != NULL)
        memcpy(copy, s, n);
    return copy;
}

typedef struct {
    char *name;                    /* the child allocation for class 2 */
    int age;
} person_t;

/* ---- CLASS 1: definitely lost — overwrite the only pointer -------------- */
static void definitely_lost(void)
{
    char *buf = malloc(40);
    if (buf == NULL)
        return;
    snprintf(buf, 40, "contact:alice");
    puts(buf);

    buf = NULL;                    /* BUG: the last reference just vanished.
                                      The 40 bytes are now unreachable forever. */
}

/* ---- CLASS 2: indirectly lost — the owner dies, dragging the child ------ */
static void indirectly_lost(void)
{
    person_t *p = malloc(sizeof *p);
    if (p == NULL)
        return;
    p->name = my_strdup("Grace Hopper");   /* child block: 23 bytes */
    if (p->name == NULL) {
        free(p);
        return;
    }
    p->age = 45;
    printf("%s is %d\n", p->name, p->age);

    p = NULL;                      /* BUG: owner lost → struct "definitely lost"
                                      AND its name string "indirectly lost".
                                      A lone free(p->name) would NOT have saved
                                      the struct; the pair must be freed inside-out. */
}

/* ---- CLASS 3: still reachable — a live pointer, never freed -------------- */
static void still_reachable_log(void)
{
    g_log = malloc(32);            /* stored in a global: reachable at exit */
    if (g_log == NULL)
        return;
    snprintf(g_log, 32, "log line one");
    puts(g_log);
    /* "Fix" in fixed.c: free(g_log) in main before returning. Until then
     * valgrind files it under "still reachable", not "lost" — because with
     * the global still holding it, you COULD still free it. You just didn't. */
}

int main(void)
{
    definitely_lost();
    indirectly_lost();
    still_reachable_log();
    return 0;
    /* All three classes are counted in the LEAK SUMMARY at the bottom of the
     * report. Read it FIRST, then each loss record from the top down. */
}
