/*
 * Bite 19 - Structs
 * ==================
 * Compile & run (from the repo root inside WSL):
 *
 *     gcc -Wall -Wextra -Werror -std=c17 -g bites/19-structs/points.c -o /tmp/b19 -lm && /tmp/b19
 *
 * (Yes, that -lm is new: sqrt lives in the math library libm, which is
 * NOT linked by default. "undefined reference to `sqrt'" with a missing
 * -lm is one of the most-Googled C errors -- now you have met it.)
 *
 * The "Learn" list, translated into code below:
 *   - struct point { double x, y; } is a TEMPLATE; declaring a variable
 *     builds one. typedef makes the name short.
 *   - p.x for a struct, p->x for a pointer to struct -- and p->x is
 *     EXACTLY (*p).x. The arrow exists because (*p).x is annoying.
 *   - Structs are passed BY VALUE (copied!). Small structs by value are
 *     fine (16 bytes here); anything bigger or mutable goes by pointer.
 *   - PADDING: the compiler inserts alignment gaps, so sizeof(struct)
 *     can exceed the sum of its members. Reordering members changes it.
 *
 * BREAK IT ON PURPOSE:
 *   In move(), write p.x = ... instead of p->x = ... -- this one doesn't
 *   crash, it REFUSES TO COMPILE, and the error names exactly what is
 *   wrong. Read it:
 *
 *     gcc -DBREAK_IT=1 -Wall -Wextra -Werror -std=c17 -g \
 *         bites/19-structs/points.c -o /tmp/b19break -lm
 *
 *   Expected: "request for member 'x' in something not a structure or
 *   union" -- the compiler telling you p is a pointer, not a struct.
 */

#include <stdio.h>
#include <math.h>     /* sqrt */

/* typedef: "point_t" is now a first-class type name. The two names
 * (struct tag and typedef) doing one job is classic C ceremony. */
typedef struct {
    double x, y;
} point_t;

/*
 * Passed BY VALUE: a and b are copies. For two doubles that is exactly
 * right -- copying 16 bytes is cheaper than the indirection, and the
 * function can't accidentally modify the caller's points.
 */
static double dist(point_t a, point_t b)
{
    double dx = a.x - b.x;
    double dy = a.y - b.y;
    return sqrt(dx * dx + dy * dy);
}

static void point_print(point_t p)
{
    printf("(%.2f, %.2f)", p.x, p.y);
}

/*
 * Mutation through a pointer: we receive the ADDRESS of the caller's
 * struct, and p->x -- which is exactly (*p).x -- writes into it.
 * By-pointer is how a function CHANGES a struct (bite 12's swap lesson,
 * wearing a struct costume).
 */
static void move(point_t *p, double dx, double dy)
{
    p->x += dx;
    p->y += dy;
}

int main(void)
{
    point_t a = { 0.0, 0.0 };
    point_t b = { 3.0, 4.0 };

    printf("dist(");
    point_print(a);
    printf(", ");
    point_print(b);
    printf(") = %.2f   (the classic 3-4-5 triangle)\n", dist(a, b));

    move(&a, 1.5, -2.0);        /* pass the address: a mutates */
    printf("after move(&a, 1.5, -2.0): a = ");
    point_print(a);
    printf("\n\n");

    /* By value means "copy": mutating a copy leaves the original alone. */
    point_t copy = a;
    copy.x = 999.0;
    printf("copy.x = 999:  copy is now ");
    point_print(copy);
    printf(", but a is still ");
    point_print(a);
    printf("  (two separate objects)\n");

    /* ---- PADDING: alignment is real ---- */
    /* Doubles want 8-byte alignment, so the compiler inserts gaps:      */
    typedef struct { char tag; double x, y; } tag_first_t;
    /*  1 + [7 pad] + 8 + 8 = 24                                         */
    typedef struct { double x, y; char tag; } tag_last_t;
    /*  8 + 8 + 1 + [7 pad to next multiple of 8] = 24                   */
    typedef struct { char a, b; double x; } two_chars_t;
    /*  1 + 1 + [6 pad] + 8 = 16                                         */
    typedef struct { char c[9]; double d; } str_num_t;
    /*  9 + [7 pad] + 8 = 24                                             */

    printf("\nsizeof experiments -- same members, different order, different cost:\n");
    printf("  struct { double x, y; }        = %2zu  (no padding needed)\n",
           sizeof(point_t));
    printf("  struct { char tag; double x,y;} = %2zu  (1 + 7 pad + 16)\n",
           sizeof(tag_first_t));
    printf("  struct { double x,y; char tag;} = %2zu  (16 + 1 + 7 pad: tail padding)\n",
           sizeof(tag_last_t));
    printf("  struct { char a,b; double x; } = %2zu  (2 + 6 pad + 8)\n",
           sizeof(two_chars_t));
    printf("  struct { char c[9]; double d; } = %2zu  (9 + 7 pad + 8)\n",
           sizeof(str_num_t));
    printf("reordering members can shrink a struct -- and arrays of structs\n"
           "multiply the waste by every element.\n");

#ifndef BREAK_IT
#define BREAK_IT 0   /* set to 1 (or -DBREAK_IT=1) to break the compile */
#endif
#if BREAK_IT
    /* THE CRIME: p is a POINTER here; . is for structs, -> for pointers.
     * This does not crash -- it refuses to compile. Read the error. */
    point_t pt = { 1.0, 2.0 };
    point_t *p = &pt;
    p.x = 10.0;      /* ERROR: p is a pointer -- use p->x (or (*p).x) */
    printf("never reached: %f\n", p->x);
#endif

    return 0;
}
