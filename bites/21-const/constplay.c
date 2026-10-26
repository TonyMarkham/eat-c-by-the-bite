/*
 * Bite 21 - const & Qualifiers
 * =============================
 * Compile & run (from the repo root inside WSL):
 *
 *     gcc -Wall -Wextra -Werror -std=c17 -g bites/21-const/constplay.c -o /tmp/b21 && /tmp/b21
 *
 * The "Learn" list, translated into code below:
 *   - Read declarations right-to-left:
 *       const char *p        "pointer to (const char)"  -> data locked,
 *                                                           pointer movable
 *       char *const p        "(const pointer) to char"  -> pointer pinned,
 *                                                           data writable
 *       const char *const p  both locked.
 *   - const in a function parameter is a CONTRACT: "I will not modify
 *     what you give me." It documents intent AND enables passing
 *     read-only data (string literals!) safely.
 *   - volatile and register exist; you will almost never need them.
 *     Know they exist, move on.
 *
 * BREAK IT ON PURPOSE:
 *   Cast away const and write through it: string literals live in
 *   READ-ONLY memory, so p[0] = 'X' crashes under ASan
 *   (global-buffer-overflow WRITE). A cast is you telling the compiler
 *   "shut up, I know better" -- have a good reason, or don't cast.
 *
 *     gcc -DBREAK_IT=1 -Wall -Wextra -Werror -std=c17 -g \
 *         bites/21-const/constplay.c -o /tmp/b21break && /tmp/b21break
 *
 * The #if 0 blocks below hold lines that are *supposed* to fail to
 * compile -- move them out one at a time to read each error.
 */

#include <stdio.h>
#include <string.h>

/* ---- bite 15-20 functions, retro-fitted with const ----
 * `const char *s`: the function promises to READ s, never write it.
 * That promise is what lets callers pass string literals and const
 * buffers. Once you have written a few of these, leaving const off a
 * read-only parameter feels like leaving a door open in winter. */
static size_t my_strlen(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0')
        n++;
    return n;
}

/* Read the signature like a sentence: returns char* (dst, to allow
 * chaining), takes a writable dst, a size, and a read-only src. */
static char *my_copy(char *dst, size_t dstsz, const char *src)
{
    if (dstsz == 0)
        return NULL;
    size_t i = 0;
    while (i + 1 < dstsz && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return src[i] == '\0' ? dst : NULL;
}

typedef struct {
    char name[32];
    int age;
} person_t;

/* const person_t *: "I read the struct, I change nothing." Every
 * read-only struct parameter in bites 19-20 should look like this. */
static void person_print(const person_t *p)
{
    printf("  %s, age %d\n", p->name, p->age);
}

/* The SAFE direction: char* -> const char* adds a promise, so it is
 * always allowed. Any strlen/strcmp/printf call does this constantly. */
static size_t len_of(const char *s)
{
    return my_strlen(s);
}

int main(void)
{
    /* 1. Three pointer kinds, one target variable. */
    int x = 42, y = 43;
    const int *pd = &x;         /* data const:     *pd is read-only   */
    int *const pp = &x;         /* pointer const:  pp cannot move     */
    const int *const pb = &x;   /* both const                         */

    printf("*pd = %d, *pp = %d, *pb = %d  (three views of x)\n", *pd, *pp, *pb);

    *pp = 44;                   /* ALLOWED: the data behind pp is writable */
    printf("*pp = 44      -> x = %d\n", x);
    pd = &y;                    /* ALLOWED: pd itself can move        */
    printf("pd = &y       -> *pd = %d\n", *pd);

    /* The four ILLEGAL moves live in the #if 0 block: */
#if 0
    /* Each line below fails to compile. Remove the #if 0 / #endif pair
     * (one line at a time!) and read the error messages:
     *   *pd = 7  -> "assignment of read-only location"
     *   pp = &y  -> "assignment of read-only variable 'pp'"
     *   pb = &y  -> "assignment of read-only variable 'pb'"
     *   *pb = 7  -> "assignment of read-only location"                */
    *pd = 7;
    pp = &y;
    pb = &y;
    *pb = 7;
#endif

    /* 2. The retro-fitted functions in action: */
    char buf[32] = "safe direction";
    printf("\nmy_strlen(buf) = %zu  (char[32] -> const char*: safe, adds a promise)\n",
           len_of(buf));

    person_t ada = { "Ada Lovelace", 36 };
    const person_t *reader = &ada;   /* a const reader is normal and good */
    person_print(reader);

    /* my_copy wants a writable dst and a read-only src: */
    char out[16];
    printf("my_copy: %s\n",
           my_copy(out, sizeof out, "hello") != NULL ? out : "(rejected)");

    /* 3. The REVERSE direction needs a cast -- and should make you
     * suspicious: */
#if 0
    /* Remove the #if 0 / #endif pair to see the error:
     *   "initialization of 'char *' from 'const char *' discards
     *    qualifiers" -- the compiler refusing to let you quietly
     *   drop a promise.                                                  */
    const char *ro = "read-only";
    char *rw = ro;

    /* The only way through is a cast -- which is exactly why a cast
     * should make you suspicious. Here it leads straight to the
     * BREAK_IT crash below. */
    char *evil = (char *)ro;
    (void)evil;
#endif

#ifndef BREAK_IT
#define BREAK_IT 0   /* set to 1 (or -DBREAK_IT=1) to commit the crime */
#endif
#if BREAK_IT
    /* THE CRIME: cast away const, then write into a STRING LITERAL.
     * Literals live in read-only memory (.rodata); the write faults.
     * Under ASan: "global-buffer-overflow WRITE". The cast silenced the
     * compiler; it did not change the physics. */
    char *p = (char *)"literal";
    printf("\n--- breaking: p[0] = 'X' on a string literal ---\n");
    p[0] = 'X';
    printf("wrote it (you will not see this under ASan)\n");
#endif

    /* volatile / register: know they exist, move on. volatile tells the
     * compiler "this variable can change behind your back" (hardware
     * registers, signal handlers); register is a 1970s optimization
     * hint the compilers ignore. Neither appears in healthy app code. */

    return 0;
}
