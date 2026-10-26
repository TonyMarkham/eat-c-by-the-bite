/*
 * Bite 18 - Pointers to Pointers
 * ===============================
 * Compile & run (from the repo root inside WSL):
 *
 *     gcc -Wall -Wextra -Werror -std=c17 -g bites/18-ptr-to-ptr/argvtree.c -o /tmp/b18 && /tmp/b18
 *
 * Then BREAK IT ON PURPOSE by running it with different argument
 * counts -- predict argc BEFORE each run:
 *
 *     /tmp/b18
 *     /tmp/b18 one two "three four"
 *
 *   With zero arguments argc is 1 (argv[0] is the program name -- the
 *   standard guarantees at least that). The quoted run gives argc == 4:
 *   "three four" arrives as ONE argv element, because quoting is the
 *   shell's job, not C's.
 *
 * argv's structure, drawn (boxes and arrows, no magic left):
 *
 *     argv
 *      |
 *      v
 *   [ ptr0 ][ ptr1 ][ ptr2 ][ NULL ]     <- an array of char*
 *      |       |       |
 *      v       |       +-------------> "two\0"
 *   "./b18\0"  +---------------------> "one\0"
 *
 *   argv is char**: a pointer to an array of pointers, each pointing at
 *   a '\0'-terminated string. argv[argc] is guaranteed NULL by the
 *   standard -- that is how "for (char **p = argv; *p; p++)" loops know
 *   where to stop.
 */

#include <stdio.h>

/*
 * Why a pointer-to-pointer here? Because the callee must modify the
 * CALLER's pointer. A plain `const char *out` parameter would be a COPY
 * of the caller's pointer (bite 6's attempt_swap lesson again) -- writing
 * *out = "teal" through one more level of indirection is what makes the
 * change stick. This is exactly how APIs "return" strings via out-params.
 */
static void get_color(const char **out)
{
    *out = "teal";      /* follow the arrow to the caller's pointer, set it */
}

int main(int argc, char **argv)
{
    /* 1. Print argc and every argv[i] ... */
    printf("argc = %d\n", argc);
    for (int i = 0; i < argc; i++)
        printf("  argv[%d] = \"%s\" (at %p)\n", i, argv[i], (void *)argv[i]);

    /* ... and prove argv[argc] == NULL. This is the fencepost the
     * standard guarantees; code that walks argv without argc relies on
     * it. */
    printf("argv[%d] = %s  <- guaranteed NULL by the C standard\n", argc,
           argv[argc] == NULL ? "NULL" : "NOT NULL?!");

    /* 2. The out-param that needs one more level of indirection: */
    const char *color = NULL;
    get_color(&color);      /* pass the ADDRESS of my pointer */
    printf("\nget_color set my pointer to \"%s\"\n", color);

    /* 3. Three levels of indirection, three different numbers:
     *    &argv      -- where the argv VARIABLE itself lives
     *    argv[0]    -- the VALUE of the first char* (a string address)
     *    argv[0][0] -- the first CHARACTER byte of that string        */
    printf("\nthree levels of indirection:\n");
    printf("  &argv      = %p   (where the argv variable itself lives)\n",
           (void *)&argv);
    printf("  argv[0]    = %p   (address of the first string's first byte)\n",
           (void *)argv[0]);
    printf("  argv[0][0] = '%c'  (the actual byte stored there)\n", argv[0][0]);

    /* Bonus: the NULL-terminated walk that needs no argc at all. */
    printf("\nwalking argv without argc (stops at the NULL):\n");
    for (char **p = argv; *p != NULL; p++)
        printf("  \"%s\"\n", *p);

    return 0;
}
