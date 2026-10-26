/*
 * Bite 7 — Scope & Lifetime
 *
 * Compile + run (from the repo root):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/07-scope/scope.c -o /tmp/b07 && /tmp/b07
 *
 * BREAK IT ON PURPOSE:
 *   Return the address of a local variable:
 *
 *     int *f(void)
 *     {
 *         int x = 5;
 *         return &x;      // dangling pointer!
 *     }
 *
 *   GCC catches this at -Wall ("function returns address of local
 *   variable"), so -Werror stops the build. The bug it describes is
 *   real: `x` dies at the closing brace, and anyone using the returned
 *   pointer reads a corpse. It's kept as a comment here; you've now
 *   written a dangling-pointer bug ON PURPOSE so you never write one
 *   by accident. (Static and heap variables don't die this way — bite 23
 *   draws the full memory map.)
 *
 * Learn tie-ins:
 *   - automatic (local): lives on the stack, dies at `}`.
 *   - static local: ONE copy, persists across calls, but its *scope* is
 *     still local — other functions can't name it.
 *   - global (file scope): lives for the whole program, visible to all
 *     functions below its declaration. Use sparingly.
 *   - `static` on a global or function means "private to this file" —
 *     C's module privacy (fully exercised in bite 37).
 *   - Shadowing: an inner `int x` hides an outer one. Legal, dangerous.
 */

#include <stdio.h>

/* A file-scope (global) variable. Every function below can see it;
 * it exists before main starts and dies when the program does. */
static int global_var = 100;   /* static here = private to this file */

/* --- Do 1: a static local counter. `static int counter = 0` runs its
 * initializer ONCE (at program start, conceptually), not on every call.
 * The variable is invisible outside this function — scope is local,
 * lifetime is the whole program. That combination is the magic. */
static int next_id(void)
{
    static int counter = 0;
    return ++counter;
}

/* Do 2's control group: the SAME code without `static`. The initializer
 * re-runs every call, so every call returns 1. Same code, one keyword,
 * completely different behavior. */
static int next_id_no_static(void)
{
    int counter = 0;
    return ++counter;
}

int main(void)
{
    printf("--- next_id: static local (call it 5 times) ---\n");
    for (int i = 0; i < 5; i++) {
        printf("call %d -> id %d\n", i + 1, next_id());
    }

    printf("--- next_id_no_static: same code, no static ---\n");
    for (int i = 0; i < 5; i++) {
        printf("call %d -> id %d   <-- resets every time\n", i + 1,
               next_id_no_static());
    }

    /* --- Do 3: one variable of each kind, addresses printed with %p.
     * local_var sits on the stack (high address, near each other);
     * static_var and global_var live in the data segment (far away,
     * fixed for the whole run). The gap between those two regions is
     * the stack-vs-static divide — bite 23 draws the full map. */
    int local_var = 42;
    static int static_var = 7;   /* static LOCAL: one copy, still local scope */

    printf("--- three storage regions, by address ---\n");
    printf("local   &local_var  = %p\n", (void *)&local_var);
    printf("static  &static_var = %p\n", (void *)&static_var);
    printf("global  &global_var = %p\n", (void *)&global_var);

    /* --- Shadowing demo. The inner x hides the outer one; when the
     * inner block ends, the outer x is still 1. GCC's -Wshadow (not in
     * our flag set) would warn about this — a sign it's usually a bug. */
    int x = 1;
    {
        int x = 99;              /* shadows the outer x */
        printf("inner x = %d\n", x);
    }
    printf("outer x = %d (the inner one died with its block)\n", x);

    return 0;
}
