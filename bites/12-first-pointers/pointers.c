/*
 * Bite 12 - First Pointers
 * =========================
 * Compile & run (from the repo root inside WSL):
 *
 *     gcc -Wall -Wextra -Werror -std=c17 -g bites/12-first-pointers/pointers.c -o /tmp/b12 && /tmp/b12
 *
 * Draw on paper FIRST (two minutes of drawing beats an hour of staring at
 * asterisks). A pointer is just a variable whose VALUE is an address:
 *
 *       x                    p
 *   +---------+          +---------+
 *   |   42    | <------- |  &x     |     p "points at" x
 *   +---------+          +---------+
 *
 * The "Learn" list, translated into code below:
 *   - int *p = &x; read aloud: "p is a pointer to int; it holds the
 *     address of x." & is address-of. * is dereference (follow the arrow).
 *   - The * in `int *p` (declaration) and the * in `*p = 5` (expression)
 *     LOOK identical but mean different things. Everyone trips on this
 *     exactly once; get it over with.
 *   - NULL means "points to nothing". Dereferencing it is an instant,
 *     *honest* segfault.
 *
 * BREAK IT ON PURPOSE:
 *   Declare `int *p;` without initializing, then `*p = 5;` -- writing
 *   through a pointer that points at garbage. GCC 13 refuses this build
 *   under -Werror ("'wild' is used uninitialized") -- the compiler
 *   catching it before you even run it. Drop -Werror and run under
 *   ASan to see the crash itself:
 *
 *     gcc -DBREAK_IT=1 -Wall -Wextra -std=c17 -g -fsanitize=address \
 *         bites/12-first-pointers/pointers.c -o /tmp/b12break && /tmp/b12break
 *
 *   The uninitialized pointer is the most common segfault in human
 *   history; you have now met it under supervision.
 */

#include <stdio.h>

/*
 * bite 6's failure, kept for contrast. Every parameter here is a COPY of
 * the caller's value. Swapping the copies changes nothing the caller can
 * see. C passes arguments by value, always -- this function is a museum
 * exhibit explaining WHY pointers exist.
 */
static void attempt_swap(int a, int b)
{
    int tmp = a;
    a = b;
    b = tmp;    /* swapped... two copies that die when the function returns */
}

/*
 * The real thing. The parameters have type `int *` (pointer to int):
 * we don't receive the ints themselves, we receive their ADDRESSES, so
 * the writes below land in the CALLER's variables.
 */
static void swap(int *a, int *b)
{
    int tmp = *a;   /* follow the arrow in a, save the value         */
    *a = *b;        /* follow the arrow in b, write into a's target  */
    *b = tmp;       /* follow the arrow in b, store the saved value  */
}

int main(void)
{
    /* 1. The drawing, translated to code. */
    int x = 42;
    int *p = &x;    /* declaration *: "p is a pointer to int" */

    printf("four different things that all look like 'p/x':\n");
    printf("  &x  = %p   address-of x: WHERE x lives\n", (void *)&x);
    printf("  p   = %p   the pointer's VALUE (a copy of &x)\n", (void *)p);
    printf("  *p  = %d    dereference: follow the arrow, read x\n", *p);
    printf("  &p  = %p   the address OF the pointer variable itself\n", (void *)&p);

    /* Expression * means "follow the arrow". This writes 43 INTO x --
     * x changes even though its name never appears left of the '='. */
    *p = 43;
    printf("after *p = 43:  x = %d   (same memory, two names for it)\n\n", x);

    /* 2. swap, proved. */
    int u = 1, v = 2;
    attempt_swap(u, v);
    printf("attempt_swap(u, v) -> u=%d v=%d   (copies swapped, caller saw nothing)\n",
           u, v);
    swap(&u, &v);
    printf("swap(&u, &v)       -> u=%d v=%d   (addresses passed, caller changed!)\n\n",
           u, v);

    /* NULL: the "points to nothing" value. Printing it is fine;
     * dereferencing it is a segfault -- an honest one, at least. */
    int *nothing = NULL;
    printf("NULL prints as %p -- it points to nothing; never follow that arrow\n",
           (void *)nothing);

#ifndef BREAK_IT
#define BREAK_IT 0   /* set to 1 (or -DBREAK_IT=1) to commit the crime */
#endif
#if BREAK_IT
    /* THE CRIME: `wild` was never given an address. It holds leftover
     * stack garbage, and *wild = 5 writes 4 bytes at some arbitrary
     * address. Under ASan: "SEGV on unknown address". Without ASan it
     * may do nothing today and corrupt data next Tuesday. */
    int *wild;
    *wild = 5;
    printf("*wild = %d (you will never see this line under ASan)\n", *wild);
#endif

    return 0;
}
