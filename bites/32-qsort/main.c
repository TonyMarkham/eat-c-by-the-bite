/*
 * Bite 32 — Callbacks with qsort: sort contacts by name (+ phone for ties)
 *
 * COMPILE & RUN (from the repo root, inside WSL):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/32-qsort/main.c -o /tmp/b32 && /tmp/b32
 *   valgrind --leak-check=full --show-leak-kinds=all /tmp/b32   → 0 errors, 0 leaks
 *
 * BREAK IT ON PURPOSE: the classic comparator bug — return `a - b` for ints.
 * It overflows when the values are far apart, and the "sorted" array comes
 * out wrong. Build the broken variant and see INT_MAX vs INT_MIN scramble:
 *   gcc -Wall -Wextra -Werror -std=c17 -g -DBROKEN_CMP bites/32-qsort/main.c -o /tmp/b32-bad && /tmp/b32-bad
 * The safe idiom `(a > b) - (a < b)` never overflows, because each comparison
 * is already 0 or 1. This bug ships in real products — you're now immune.
 *
 * WHY (the "Learn" bullets):
 *   - void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *,
 *     const void *)) — generic over ANY element type via void * + element
 *     size. It calls YOUR comparator back ("callback") to order two elements.
 *   - Comparator convention: negative → a before b, 0 → equal, positive →
 *     b before a.
 *   - Comparing strings inside a comparator: cast the void *s to your struct
 *     type, then strcmp the fields. Secondary keys only matter on ties.
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- 1) int comparator: the safe idiom ----------------------------------- */

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a;               /* void * → int * → value */
    int y = *(const int *)b;
    return (x > y) - (x < y);              /* each term is 0 or 1 → result is
                                              -1/0/+1, overflow-impossible */
}

#ifdef BROKEN_CMP
static int cmp_int_broken(const void *a, const void *b)
{
    /* BUG: overflow. INT_MAX - INT_MIN wraps to a NEGATIVE number, so the
     * comparator claims 2147483647 sorts BEFORE -2147483648. */
    return *(const int *)a - *(const int *)b;
}
#endif

/* ---- 2+3) contact comparator: name, then phone on ties ------------------- */

typedef struct {
    char name[32];
    char phone[16];
} contact_t;

static int cmp_contact(const void *a, const void *b)
{
    const contact_t *ca = a;               /* const void * → const contact_t *:
                                              the cast-back qsort forces on you */
    const contact_t *cb = b;

    int by_name = strcmp(ca->name, cb->name);
    if (by_name != 0)
        return by_name;                    /* names differ: that decides it */
    return strcmp(ca->phone, cb->phone);   /* secondary key, only on ties */
}

static void print_contacts(const char *tag, const contact_t *c, size_t n)
{
    printf("%s\n", tag);
    for (size_t i = 0; i < n; i++)
        printf("  %-12s %s\n", c[i].name, c[i].phone);
}

int main(void)
{
#ifdef BROKEN_CMP
    (void)cmp_int;                         /* not exercised on this path */
    (void)cmp_contact;
    (void)print_contacts;

    /* ---- the broken comparator, fed values designed to explode ---------- */
    int bad[] = { 5, INT_MAX, 3, INT_MIN, 4 };
    qsort(bad, sizeof bad / sizeof bad[0], sizeof bad[0], cmp_int_broken);
    printf("BROKEN a-b comparator on {5, INT_MAX, 3, INT_MIN, 4}:\n  ");
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        printf(" %d", bad[i]);
    printf("\nINT_MAX is NOT first — the comparator lied via overflow.\n");
    return 0;
    /* Actual output of this build:
     *   BROKEN a-b comparator on {5, INT_MAX, 3, INT_MIN, 4}:
     *     3 4 5 2147483647 -2147483648
     *   INT_MAX is NOT first — the comparator lied via overflow.
     * (qsort never even moved INT_MAX: every comparison that touched it
     *  returned a wrapped, meaningless sign.) */
#else
    /* ---- 1) sort ints ---------------------------------------------------- */
    int nums[] = { 42, -7, 19, 0, 42, -7, 300, 5 };
    size_t nn = sizeof nums / sizeof nums[0];
    qsort(nums, nn, sizeof nums[0], cmp_int);
    printf("sorted ints:");
    for (size_t i = 0; i < nn; i++)
        printf(" %d", nums[i]);
    printf("\n");

    /* ---- 2+3) sort contacts by name, phone as tiebreaker ------------------
     * Two "Ann Smith" entries with different phones prove the secondary key
     * fires; qsort is NOT stable, so without the phone tiebreak their order
     * would be arbitrary. */
    contact_t contacts[] = {
        { "Zoe",      "555-0199" },
        { "Ann Smith","555-0142" },
        { "Bob",      "555-0102" },
        { "Ann Smith","555-0107" },
        { "Carol",    "555-0103" },
        { "Bob",      "555-0101" },
    };
    size_t nc = sizeof contacts / sizeof contacts[0];

    print_contacts("before:", contacts, nc);
    qsort(contacts, nc, sizeof contacts[0], cmp_contact);
    print_contacts("after (name, then phone):", contacts, nc);

    /* Note what we did NOT write: a sorting algorithm. qsort is the library's;
     * our whole contribution is one comparator function — a callback. The
     * same {data, callback} shape returns in bite 31's dispatch table and in
     * every event loop of Phase 5+. */
    return 0;
#endif
}
