/*
 * Bite 26 — malloc + Structs + OWNERSHIP
 *
 * COMPILE & RUN (from the repo root, inside WSL):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/26-malloc-structs/main.c -o /tmp/b26 && /tmp/b26
 *   valgrind --leak-check=full --show-leak-kinds=all /tmp/b26   → 0 errors, 0 leaks
 *
 * BREAK-IT VARIANTS (run each under valgrind and compare reports):
 *   gcc -Wall -Wextra -Werror -std=c17 -g -DBREAK_LEAK bites/26-malloc-structs/main.c -o /tmp/b26-leak
 *   valgrind --leak-check=full /tmp/b26-leak
 *     → the struct is freed but its strdup'd name never is:
 *       "4 bytes in 1 blocks are definitely lost ... by person_destroy_broken"
 *       (4 bytes = "Ada" plus the '\0'; Ada's struct itself IS freed — only
 *       the child allocation was forgotten)
 *   gcc -Wall -Wextra -Werror -std=c17 -g -DBREAK_DOUBLE bites/26-malloc-structs/main.c -o /tmp/b26-double
 *   valgrind --leak-check=full /tmp/b26-double
 *     → destroy called twice on the same person, 5 errors:
 *       "Invalid read of size 8"  (the second destroy reads the freed struct)
 *       "Invalid free() / delete" (the name string freed twice)
 *       "Invalid free() / delete" (the struct freed twice)
 *
 * WHY (the "Learn" bullets):
 *   - contact_t *c = malloc(sizeof *c); — the struct is yours; free it exactly once.
 *   - OWNERSHIP: if a struct contains char *name (a pointer), freeing the
 *     struct does NOT free what it points at. Freeing is not recursive.
 *     Every allocation has exactly one owner, and the owner frees.
 *   - calloc(n, size) zero-initializes; malloc does not. For structs with
 *     pointer members, calloc means every pointer starts NULL (free(NULL) is
 *     a safe no-op) instead of garbage (free(garbage) is a bomb).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- 1) heap-allocated struct without pointers: the trivial case ------- */

typedef struct {
    double x, y;
} point_t;

static void point_demo(void)
{
    /* Same sizeof *p idiom as bite 24 — survives type changes for free. */
    point_t *p = malloc(sizeof *p);
    if (p == NULL) {
        fprintf(stderr, "malloc failed\n");
        return;
    }
    p->x = 3.0;                       /* heap struct: access with -> just like a local */
    p->y = 4.0;
    printf("point_t on the heap: (%.1f, %.1f)\n", p->x, p->y);

    free(p);                          /* no pointer members → nothing else to own */
    p = NULL;
}

/* ---- 2) struct with a POINTER member: ownership gets real --------------- */

typedef struct {
    char *name;        /* OWNS a heap string — freeing the struct alone leaks it */
    int age;
} person_t;

/* Local strdup so this file needs no POSIX feature macros. (Bite 27 builds
 * the full version; here it's just a helper.) */
static char *my_strdup(const char *s)
{
    size_t n = strlen(s) + 1;              /* +1 for the '\0' — off-by-one central */
    char *copy = malloc(n);
    if (copy == NULL)
        return NULL;
    memcpy(copy, s, n);
    return copy;
}

static person_t *person_create(const char *name, int age)
{
    /* calloc = malloc + zero-fill. Every byte of the struct is defined,
     * which matters the day someone adds a pointer member and forgets to
     * set it: a NULL member is free-able, a garbage member is a crash. */
    person_t *p = calloc(1, sizeof *p);
    if (p == NULL)
        return NULL;

    p->name = my_strdup(name);             /* allocation #2, owned by p->name */
    if (p->name == NULL) {
        free(p);                           /* roll back allocation #1 — partial
                                              construction still means zero leaks */
        return NULL;
    }
    p->age = age;
    return p;
}

/* The OWNER frees: both allocations, in any consistent order, exactly once. */
static void person_destroy(person_t *p)
{
    if (p == NULL)
        return;                            /* destroy(NULL) is a safe no-op */
    free(p->name);                         /* free the thing it points at FIRST */
    free(p);                               /* then the struct itself */
    /* NOTE: we cannot set the caller's pointer to NULL from here — that
     * needs a person_t ** parameter. The caller does it after calling us. */
}

#ifdef BREAK_LEAK
/* A visibly separate broken destroy so the valgrind report names it. */
static void person_destroy_broken(person_t *p)
{
    if (p == NULL)
        return;
    /* BUG: "I freed the struct, that's enough" — it never is when the struct
     * owns pointers. The name string is now unreachable: valgrind calls this
     * "definitely lost". */
    free(p);
}
#endif

int main(void)
{
    point_demo();

    puts("");

    /* Three persons, each = TWO allocations (struct + name string).
     * Array of pointers: each element is an owner we can destroy one by one. */
    person_t *people[3] = { NULL, NULL, NULL };

    people[0] = person_create("Ada", 36);
    people[1] = person_create("Grace", 45);
    people[2] = person_create("Linus", 28);

    if (!people[0] || !people[1] || !people[2]) {
        fprintf(stderr, "allocation failed\n");
        for (int i = 0; i < 3; i++)
            person_destroy(people[i]);     /* partial cleanup still correct */
        return 1;
    }

    for (int i = 0; i < 3; i++)
        printf("%s is %d (name copied to the heap at %p)\n",
               people[i]->name, people[i]->age, (void *)people[i]->name);

#ifdef BREAK_LEAK
    /* BUG RUN: destroy Ada with the broken destroyer — struct freed, the
     * strdup'd name left behind. Everyone else is destroyed properly, so
     * the report shows exactly ONE definite leak: Ada's name. */
    person_destroy_broken(people[0]);
    people[0] = NULL;
    person_destroy(people[1]);
    people[1] = NULL;
    person_destroy(people[2]);
    people[2] = NULL;
    return 0;
#endif

#ifdef BREAK_DOUBLE
    /* BUG: destroy the same person twice. The first call frees name+struct;
     * the second call frees the (already freed) name → "Invalid free()". */
    person_destroy(people[0]);
    person_destroy(people[0]);
    people[0] = NULL;
    /* and the other two simply leak — one crime per run keeps reports readable */
    return 0;
#endif

    for (int i = 0; i < 3; i++) {
        person_destroy(people[i]);
        people[i] = NULL;                  /* dangling → NULL, the bite-24 habit */
    }

    /* The ownership rule, stated once more for the back of your hand:
     *   every pointer has exactly ONE owner, and the owner frees.
     * person_t owns p->name. The people[] array owns each person_t.
     * Destroy them inside-out and valgrind reports "All heap blocks freed". */
    return 0;
}
