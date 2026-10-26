/*
 * Bite 23 — The Memory Model: one map of a running program
 *
 * COMPILE & RUN (from the repo root, inside WSL):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/23-memory-model/main.c -o /tmp/b23 && /tmp/b23
 *
 * Optional demos:
 *   /tmp/b23 depth       watch stack frames march DOWNWARD in memory
 *   /tmp/b23 overflow    recurse until the stack overflows (crashes ON PURPOSE,
 *                        exit code 139 / SIGSEGV — that is the lesson; observed
 *                        run dies around depth 25000–30000, frames at
 *                        0x7ffd63... — deep inside the stack region)
 * Dangling-local break-it variant (still compiles warning-free):
 *   gcc -Wall -Wextra -Werror -std=c17 -g -DBREAK_IT bites/23-memory-model/main.c -o /tmp/b23-broken && /tmp/b23-broken
 *
 * BREAK IT ON PURPOSE: return a pointer to a local, then print through it
 * AFTER the function returned — twice, with a printf in between. The first
 * read usually still shows the old value (the corpse looks alive); the second
 * read shows recycled stack garbage. Stack memory is reused the instant a
 * function returns. The heap never gets that automatic recycle — which is the
 * whole reason free() exists.
 *
 * THE MAP — sort the addresses this program prints into these four regions:
 *
 *   high addresses
 *   ┌──────────────────────────────────────────────┐
 *   │ stack   locals, one frame per call           │ grows DOWN, ~8 MB, auto-recycled
 *   │   ↓                                          │
 *   │        (big unmapped gap between them)       │
 *   │   ↑                                          │
 *   │ heap    malloc/free blocks                   │ grows UP, *you* manage every byte
 *   ├──────────────────────────────────────────────┤
 *   │ bss     zero-initialized globals/statics     │ g_bss — lives forever
 *   │ data    initialized globals + static locals  │ g_init — lives forever
 *   │ text    the machine code + string literals   │ main itself lives here
 *   └──────────────────────────────────────────────┘
 *   low addresses
 *
 * WHY THIS MATTERS (the "Learn" bullets in one place):
 *   - Every function call pushes a stack frame; return pops it. That is why
 *     locals die and why returning their address is a hanging offense.
 *   - Globals/statics live for the whole program — no free() needed, but they
 *     exist even when "nobody is using them".
 *   - The heap has NO automatic cleanup, ever. Every malloc is a promise you
 *     keep with free.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- region representatives ------------------------------------------- */

int g_init = 7;            /* .data : initialized global — lives forever      */
int g_bss;                 /* .bss  : zero globals live here too              */
static int g_file_static;  /* .bss  : file-scope static, same treatment       */

/* A static LOCAL is not on the stack either: it lives in .data/.bss, which is
 * exactly why its value survives between calls. Returning its address is
 * perfectly legal (unlike returning a plain local's address). */
static int *static_local_addr(void)
{
    static int n;          /* born once, lives forever, NOT in any frame */
    n++;
    return &n;
}

/* Heap helper: the block lands way above the globals, no matter which
 * function called malloc. The heap belongs to the program, not the call. */
static void *take_from_heap(void)
{
    return malloc(24);
}

/* ---- stack demo -------------------------------------------------------- */

/* Each call pushes a frame. Deeper frames sit at LOWER addresses because the
 * stack grows down. volatile keeps the slot real (defeats optimization). */
static void frame(const char *tag, int depth)
{
    volatile int local = depth;
    printf("%-9s depth %d : local lives at %p\n", tag, depth, (void *)&local);
}

static void two_frames(void)
{
    frame("inner", 1);     /* one frame deeper than whoever called us */
}

/* Recurse and report frame addresses.
 *   limit >= 0 : stop there (the "depth" demo)
 *   limit < 0  : recurse forever (the "overflow" demo — crashes on purpose)
 * The `limit >= 0` check also stops gcc from proving the recursion infinite. */
static long descend(long depth, long limit)
{
    volatile char pad[256];               /* inflates each frame */
    pad[0] = (char)depth;
    if (depth <= 4 || depth % 5000 == 0) {
        printf("  depth %6ld : frame at %p\n", depth, (void *)pad);
        fflush(stdout);                   /* survive the on-purpose crash below */
    }
    if (limit >= 0 && depth >= limit)
        return depth;
    return descend(depth + 1, limit) + pad[0]; /* + pad[0] defeats tail-call opt */
}

/* ---- BREAK IT ON PURPOSE ---------------------------------------------- */
#ifdef BREAK_IT
/* The bug from bite 7, now with a memory model to explain it: `x` lives in
 * dangling()'s stack frame. When dangling() returns, the frame is popped and
 * the slot is free real estate for the next function call.
 *
 * GOTCHA DISCOVERED THE HONEST WAY: modern GCC (13 here) detects the direct
 * pattern `return &local;` and replaces the returned address with NULL at
 * ALL optimization levels — an anti-exploit mitigation. The dangling pointer
 * demo would segfault on a NULL read instead of showing recycled garbage.
 * To keep the lesson observable we launder the address through a volatile
 * global, which the compiler treats as opaque. -Wdangling-pointer is silenced
 * for exactly this function; it is OFF in all real code. */
#pragma GCC diagnostic ignored "-Wdangling-pointer"
static int *volatile g_leak;               /* volatile: opaque to the optimizer */

static void dangling(void)
{
    int x = 42;
    g_leak = &x;                           /* the address of a doomed local */
}

/* A helper whose frame covers the bytes dangling()'s frame used. It writes a
 * recognizable pattern (0x5A5A5A5A) over them. The array is sized so the
 * writes reach the same offsets that `x` occupied one call earlier. */
static void clobber(void)
{
    volatile int y[70];
    for (int i = 0; i < 70; i++)
        y[i] = 0x5A5A5A5A;
    (void)y;
}
#endif /* BREAK_IT */

int main(int argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "map";

    if (strcmp(mode, "depth") == 0) {
        puts("Frames of nested calls — watch the addresses FALL:");
        descend(0, 3);
        return 0;
    }
    if (strcmp(mode, "overflow") == 0) {
        puts("Recursing with no limit — expect SIGSEGV once the ~8 MB stack");
        puts("runs out. Note the crash address region: way up in the stack.");
        descend(0, -1);
        return 0;                              /* never reached */
    }

    puts("== THE MEMORY MAP: five addresses, four regions ==\n");

    /* text: the code itself, plus string literals (read-only, lives forever) */
    printf("text  : main              at %p  (the code is data too)\n",
           (void *)main);
    printf("text  : string literal    at %p  (literals sit with the code)\n",
           (void *)"literals live with the code");

    /* data/bss: globals and statics — born before main, die after main */
    printf("data  : g_init (global)   at %p\n", (void *)&g_init);
    printf("bss   : g_bss (global)    at %p\n", (void *)&g_bss);
    printf("bss   : file static       at %p\n", (void *)&g_file_static);

    int *sp = static_local_addr();
    printf("data  : static local      at %p  (count=%d, survived a call)\n",
           (void *)sp, *sp);
    sp = static_local_addr();
    printf("data  : same address      at %p  (count=%d — born once, lives on)\n",
           (void *)sp, *sp);

    /* stack: locals, one frame per call, deeper = lower address */
    frame("outer", 0);
    two_frames();
    int stack_local = 5;
    printf("stack  : main's local     at %p\n", (void *)&stack_local);

    /* heap: consecutive mallocs, far above the data segment, growing up */
    void *h1 = take_from_heap();
    void *h2 = malloc(24);
    if (!h1 || !h2) {                         /* always check — habit from bite 24 */
        fprintf(stderr, "malloc failed\n");
        free(h1);
        free(h2);
        return 1;
    }
    printf("heap   : malloc #1        at %p\n", h1);
    printf("heap   : malloc #2        at %p\n", h2);
    free(h1);
    free(h2);                                 /* heap cleanup is never automatic */

    puts("\nSort the addresses above into the map in the header comment:");
    puts("text < data/bss < heap ... and the stack sits at the top, growing down.");

#ifdef BREAK_IT
    puts("\n== BREAK IT: reading through a pointer to a dead local ==");
    dangling();
    int *d = g_leak;                          /* points into a popped frame */
    printf("first read : %d  (the corpse still looks alive — slot untouched yet)\n",
           *d);
    fflush(stdout);
    clobber();                                /* another frame reuses that memory */
    printf("second read: %d  (same address, stomped by recycled stack)\n", *d);
    printf("(0x5A5A5A5A = %d — clobber()'s pattern, not our 42)\n", 0x5A5A5A5A);
    puts("Stack memory is recycled instantly. This is why returning a local's");
    puts("address is a bug even when it *seems* to work: the first read lied to you.");
#endif

    return 0;
}
