/*
 * Bite 25 — Dynamic Arrays: intvec (push / pop / get / len / free)
 *
 * COMPILE & RUN (from the repo root, inside WSL):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/25-intvec/main.c -o /tmp/b25 && /tmp/b25
 *   valgrind --leak-check=full --show-leak-kinds=all /tmp/b25   → 0 errors, 0 leaks
 *
 * BREAK IT ON PURPOSE: the classic realloc bug. `p = realloc(p, new);` — if
 * realloc fails it returns NULL, you have just overwritten the ONLY pointer
 * to the old block (leak), and the next use of p is a NULL deref. The broken
 * variant forces a huge allocation to fail for real:
 *   gcc -Wall -Wextra -Werror -std=c17 -g -DBREAK_REALLOC bites/25-intvec/main.c -o /tmp/b25-broken
 *   valgrind --leak-check=full /tmp/b25-broken
 *   → "16 bytes in 1 blocks are definitely lost" (the cap-4 block holding our
 *     3 ints) and "Argument 'size' of function realloc has a fishy
 *     (possibly negative) value: -1". Then read the fixed iv_push below: the
 *     tmp-pointer idiom keeps the old block alive on failure.
 *
 * WHY (the "Learn" bullets):
 *   - realloc(p, newsize) may MOVE the block. The old pointer is dead either
 *     way. The only safe pattern:
 *         int *tmp = realloc(p, new);
 *         if (!tmp) { p is still valid here: handle the failure }
 *         p = tmp;
 *   - Growth strategy: capacity doubles (4 → 8 → 16 …). Pushes are then
 *     amortized O(1): N pushes do O(N) total copy work. Never grow by +1 —
 *     that makes every push a realloc (and reallocation is a copy).
 *   - Struct-of-state: { data, len, cap }. len = elements in use,
 *     cap = elements that fit. The gap between them is your headroom.
 */

#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int *data;
    size_t len;            /* elements in use          */
    size_t cap;            /* elements that fit        */
} intvec;

static void iv_init(intvec *v)
{
    v->data = NULL;        /* zero elements, zero capacity: malloc comes later */
    v->len = 0;
    v->cap = 0;
}

static void iv_free(intvec *v)
{
    free(v->data);
    v->data = NULL;        /* the free-then-NULL discipline from bite 24 */
    v->len = v->cap = 0;
}

/* Push one value. Returns 0 on success, -1 on allocation failure.
 * THE FIX FOR THE BREAK-IT BUG lives here: realloc into a tmp pointer,
 * so a failed realloc leaves v->data pointing at the still-valid block. */
static int iv_push(intvec *v, int x)
{
    if (v->len == v->cap) {                     /* full → grow (or first push) */
        size_t newcap = (v->cap == 0) ? 4 : v->cap * 2;   /* 4, 8, 16, 32… */
        int *tmp = realloc(v->data, newcap * sizeof *tmp);
        if (tmp == NULL)
            return -1;      /* v->data STILL VALID — nothing lost, nothing broken */
        v->data = tmp;      /* only now do we swap in the (possibly moved) block */
        v->cap = newcap;
    }
    v->data[v->len++] = x;
    return 0;
}

/* Pop the last value into *out. Returns 0 on success, -1 if empty.
 * We never shrink the allocation on pop — len/cap separation means popping
 * is free, and re-pushing reuses the headroom without a realloc. */
static int iv_pop(intvec *v, int *out)
{
    if (v->len == 0)
        return -1;
    *out = v->data[--v->len];
    return 0;
}

static size_t iv_len(const intvec *v)
{
    return v->len;
}

/* Precondition: i < iv_len(v). Documented, not defensive — in this course the
 * caller checks. (A production container might assert() here.) */
static int iv_get(const intvec *v, size_t i)
{
    return v->data[i];
}

#ifdef BREAK_REALLOC
/* THE BUG, STAGED FOR REAL. realloc with an impossibly large size reliably
 * fails and returns NULL — no need to actually exhaust memory. */
static void broken_grow(intvec *v)
{
    size_t impossible = (size_t)-1;   /* ~1.8 * 10^19 bytes: will never fit */
    /* BUG: assigning straight into v->data. On failure v->data is now NULL
     * and the old 12-byte block has no owner left. Two crimes for the price
     * of one line: a leak AND a future NULL deref. */
    v->data = realloc(v->data, impossible);
}
#endif

int main(void)
{
#ifdef BREAK_REALLOC
    /* ---- the broken build: watch valgrind name the leak ---------------- */
    (void)iv_pop;                          /* not exercised on this path */
    (void)iv_get;
    intvec v;
    iv_init(&v);
    for (int i = 0; i < 3; i++)
        if (iv_push(&v, i) != 0)
            return 1;                  /* block: 3 ints = 12 bytes, cap 4 */
    broken_grow(&v);                   /* realloc fails, v->data = NULL, block lost */
    printf("len is still %zu, but data is now %s — the 16-byte block is gone\n",
           iv_len(&v), v.data == NULL ? "NULL" : "valid");
    iv_free(&v);                       /* free(NULL) is fine — but frees nothing */
    return 0;
    /* Valgrind on this build says (actual):
     *   Argument 'size' of function realloc has a fishy (possibly negative)
     *   value: -1  ... by 0x...: broken_grow (main.c:104)
     *   16 bytes in 1 blocks are definitely lost in loss record 1 of 1
     *     at 0x...: realloc
     *     by 0x...: broken_grow (main.c:104)
     *   ERROR SUMMARY: 2 errors from 2 contexts
     * Note the 16 bytes, not 12: capacity had already doubled to 4 ints.
     * The leak summary is where this crime hides. */
#else
    /* ---- the real exercise: push 1000, spot-check, pop, free ----------- */
    intvec v;
    iv_init(&v);

    /* Capacity trace: 4 → 8 → 16 → 32 … Doubling means a value pushed N times
     * is copied at most log2(N) times total — that's amortized O(1). */
    size_t last_cap = 0;
    for (int i = 0; i < 20; i++) {
        if (iv_push(&v, i) != 0)
            return 1;
        if (v.cap != last_cap) {
            printf("push %2d: cap %2zu -> %2zu (realloc happened)\n",
                   i, last_cap, v.cap);
            last_cap = v.cap;
        }
    }

    /* Push 1000 total and verify get() against the formula. */
    for (int i = 20; i < 1000; i++)
        if (iv_push(&v, i) != 0)
            return 1;
    printf("len=%zu, v[0]=%d, v[500]=%d, v[999]=%d\n",
           iv_len(&v), iv_get(&v, 0), iv_get(&v, 500), iv_get(&v, 999));

    /* Pop: removes from the end, LIFO — a free stack in 5 lines. */
    int x;
    for (int i = 0; i < 3; i++)
        if (iv_pop(&v, &x) == 0)
            printf("pop -> %d (len now %zu)\n", x, iv_len(&v));
    printf("v[996]=%d still intact after 3 pops\n", iv_get(&v, 996));

    iv_free(&v);
    return 0;
    /* Expected: len=1000, v[0]=0, v[500]=500, v[999]=999;
     * pops 999, 998, 997; valgrind: "All heap blocks were freed". */
#endif
}
