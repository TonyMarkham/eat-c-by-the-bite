/*
 * Bite 1 — Hello World, Properly
 *
 * Compile + run (from the repo root):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/01-hello/hello.c -o /tmp/b01 && /tmp/b01
 *   echo $?        <- should print 0
 *
 * BREAK IT ON PURPOSE:
 *   1. Delete the `return 0;` line. It still compiles and runs clean — since
 *      C99, falling off the end of `main` is *defined* to return 0. Contrast:
 *      the same trick in any other function returning int is a bug.
 *   2. Change `return 0;` to `return 7;`, rebuild, run, then `echo $?`.
 *      You'll see 7. Exit codes are how a program talks to its *caller*
 *      (shells, scripts, other processes) — this matters in Phase 5.
 *
 * Anatomy, token by token:
 *   #include <stdio.h> — preprocessor directive: literally paste in the
 *     declarations of the standard I/O library (printf lives there). Angle
 *     brackets = "search the system include paths".
 *   int main(void) — the entry point the linker looks for. `int` = the exit
 *     code type, `void` = takes no parameters (never write empty `()`).
 *   printf("...") — a library call, declared by stdio.h.
 *   return 0 — hand 0 back to the OS. By convention 0 = success.
 */

#include <stdio.h>

int main(void)
{
    /* The classic. \n matters: stdout is often line-buffered on terminals,
     * so without it your text may sit in a buffer until the program exits. */
    printf("Hello, world!\n");

    /* Bite 1, Do-item 3: a second printf printing my name. One call per
     * line of output keeps things readable while you're learning. */
    printf("This is Tony, learning C one bite at a time.\n");

    /* Exit code 0 = "nothing went wrong". Check it with `echo $?`. */
    return 0;
}
