/*
 * Bite 13 - Pointer Arithmetic
 * =============================
 * Compile & run (from the repo root inside WSL):
 *
 *     gcc -Wall -Wextra -Werror -std=c17 -g bites/13-pointer-arithmetic/ptrmath.c -o /tmp/b13 && /tmp/b13
 *
 * The "Learn" list, translated into code below:
 *   - p + 1 advances by sizeof(*p) BYTES, not 1 byte. Pointer arithmetic
 *     is typed: int* steps in 4-byte strides, char* in 1-byte strides.
 *   - arr[i] is *defined* by the language as *(arr + i). The subscript
 *     operator is pointer arithmetic wearing a costume.
 *   - p - q (two pointers into the SAME array) gives the element count
 *     between them, as ptrdiff_t -- print it with %td.
 *
 * BREAK IT ON PURPOSE:
 *   Take a pointer one past the end, add 5 more, and PRINT it -- forming
 *   and printing an out-of-bounds address is (practically) harmless and
 *   ASan stays silent. Then DEREFERENCE it. ASan kills the process:
 *   stack-buffer-overflow READ. The crime was never the arithmetic;
 *   it is following the arrow.
 *
 *     gcc -DBREAK_IT=1 -Wall -Wextra -Werror -std=c17 -g \
 *         bites/13-pointer-arithmetic/ptrmath.c -o /tmp/b13break && /tmp/b13break
 */

#include <stdio.h>
#include <stddef.h>   /* size_t, ptrdiff_t */

/*
 * find returns a POINTER, not an index -- and NULL on miss. Callers turn
 * the pointer back into an index by subtracting the array's base:
 * `hit - a` is an element count (ptrdiff_t), not a byte count.
 */
static int *find(int *a, size_t n, int val)
{
    for (size_t i = 0; i < n; i++)
        if (a[i] == val)
            return &a[i];
    return NULL;
}

int main(void)
{
    int a[6] = { 10, 20, 30, 40, 50, 60 };
    size_t n = sizeof a / sizeof a[0];

    /* 1. Walk the array with ONLY a pointer. p starts at &a[0]; p++ moves
     * it forward one ELEMENT (sizeof(int) bytes here); the loop stops at
     * a + n -- the one-past-the-end address, which is legal to compute
     * and compare, never legal to dereference. */
    printf("walking with a pointer only:\n  ");
    for (int *p = a; p < a + n; p++)
        printf("%d ", *p);
    printf("\n\n");

    /* 2. The stride, made visible. Same "+1", different distance: */
    char c[5] = "abcd";

    printf("int array a (stride = sizeof(int) = %zu bytes):\n", sizeof(int));
    for (size_t i = 0; i < 5; i++)
        printf("  a[%zu] at %p  (+%td bytes from a)\n",
               i, (void *)(a + i), (ptrdiff_t)((char *)(a + i) - (char *)a));

    printf("char array c (stride = sizeof(char) = %zu byte):\n", sizeof(char));
    for (size_t i = 0; i < 4; i++)
        printf("  c[%zu] at %p  (+%td bytes from c)\n",
               i, (void *)(c + i), (ptrdiff_t)((char *)(c + i) - (char *)c));
    printf("pointer arithmetic is TYPED -- the type decides the stride.\n\n");

    /* 3. a[i] and *(a + i) are the same expression. Compile them and the
     * machine code is identical (assembly-brained: a+3 is base + 3*4). */
    printf("a[3] = %d and *(a + 3) = %d -- identical by definition\n\n",
           a[3], *(a + 3));

    /* find: pointer in, pointer out, NULL on miss. */
    int *hit = find(a, n, 30);
    if (hit != NULL)
        printf("find(a, %zu, 30): found at index %td  (hit - a)\n", n, hit - a);
    hit = find(a, n, 99);
    if (hit == NULL)
        printf("find(a, %zu, 99): NULL -- the miss is a value, not a crash\n", n);

#ifndef BREAK_IT
#define BREAK_IT 0   /* set to 1 (or -DBREAK_IT=1) to commit the crime */
#endif
#if BREAK_IT
    /* THE CRIME, step 1: form a pointer 5 elements PAST the end and print
     * it. Computing and printing an out-of-bounds address is technically
     * UB already, but on real machines it is harmless -- ASan says
     * nothing. (Only pointers one-past-the-end are fully legal; keep
     * your mental model at "one past the end is the fence".) */
    int *far_out = a + n + 5;
    printf("\nfar_out = %p  -- printed fine. ASan: silence.\n", (void *)far_out);

    /* step 2: dereference it. THIS is what ASan was waiting for:
     * "stack-buffer-overflow ... READ". Drawing the arrow on paper is
     * fine; following it off the cliff is not. */
    printf("*far_out = %d\n", *far_out);
#endif

    return 0;
}
