/*
 * Bite 29 — Linked List II: append, insert sorted, delete by value
 *
 * COMPILE & RUN (from the repo root, inside WSL):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/29-linked-list-2/main.c -o /tmp/b29 && /tmp/b29
 *   valgrind --leak-check=full --show-leak-kinds=all /tmp/b29   → 0 errors, 0 leaks
 *
 * BREAK IT ON PURPOSE: delete the head node WITHOUT updating the caller's
 * head. The caller keeps pointing at freed memory and the next print walks
 * off a use-after-free cliff. Build the buggy variant:
 *   gcc -Wall -Wextra -Werror -std=c17 -g -DBREAK_DELETE_HEAD -fsanitize=address,undefined \
 *       bites/29-linked-list-2/main.c -o /tmp/b29-buggy && /tmp/b29-buggy
 *   → "ERROR: AddressSanitizer: heap-use-after-free ... READ of size 8" in list_print
 * The fix is structural, not a patch: operations that can change the head
 * RETURN the new head (head = list_delete(head, v)) or take node_t ** so they
 * can rewrite the caller's pointer. Both are used below.
 *
 * WHY (the "Learn" bullets):
 *   - Deletion/insertion at the head needs to change the head pointer itself
 *     — that's why many people pass node_t **head.
 *   - THE TRICK: node_t **pp = &head; ... pp = &(*pp)->next;  — pp points at
 *     "the pointer that points at the current node" (either the head variable
 *     or some node's next field). Once you stand THERE, insert and delete are
 *     uniform: no empty-list case, no head special case, no prev tracking.
 *     This trick appears everywhere in systems C.
 *   - Sorted insert = how you'll think about keeping ordered structures later.
 */

#include <stdio.h>
#include <stdlib.h>

typedef struct node {
    int val;
    struct node *next;
} node_t;

static node_t *node_new(int val)
{
    node_t *n = malloc(sizeof *n);
    if (n == NULL) {
        fprintf(stderr, "malloc failed\n");
        return NULL;
    }
    n->val = val;
    n->next = NULL;
    return n;
}

static void list_print(const char *tag, const node_t *head)
{
    printf("%-14s", tag);
    for (const node_t *p = head; p != NULL; p = p->next)
        printf(" %d ->", p->val);
    printf(" (null)\n");
}

static void list_free(node_t *head)
{
    while (head != NULL) {                 /* save-next-then-free, from bite 28 */
        node_t *next = head->next;
        free(head);
        head = next;
    }
}

/* ---- append: walk to the last node, link -------------------------------- */

static node_t *list_append(node_t *head, int val)
{
    node_t *n = node_new(val);
    if (n == NULL)
        return head;

    if (head == NULL)                      /* empty list: the new node IS the list */
        return n;

    node_t *p = head;
    while (p->next != NULL)                /* stop at the LAST node (not NULL!) */
        p = p->next;
    p->next = n;                           /* link it on */
    return head;                           /* head never changes in append */
}

/* ---- insert sorted, version 1: naive, tracking prev --------------------- */

static node_t *list_insert_sorted(node_t *head, int val)
{
    node_t *n = node_new(val);
    if (n == NULL)
        return head;

    node_t *prev = NULL;
    node_t *p = head;
    while (p != NULL && p->val < val) {    /* find first node >= val */
        prev = p;
        p = p->next;
    }
    n->next = p;                           /* splice in after prev, before p */
    if (prev != NULL)
        prev->next = n;
    else
        head = n;                          /* THE special case — forget this and
                                              you lose the whole list on a
                                              head-insert. Two code paths, two
                                              chances to be wrong. */
    return head;
}

/* ---- insert sorted, version 2: the node_t ** trick ---------------------- */

static node_t *list_insert_sorted_pp(node_t *head, int val)
{
    node_t *n = node_new(val);
    if (n == NULL)
        return head;

    /* pp walks the LINKS, not the nodes: it points either at `head` (the
     * caller's variable) or at some node's `next` field. The loop stops at
     * the first link whose target is >= val — exactly where n belongs. */
    node_t **pp = &head;
    while (*pp != NULL && (*pp)->val < val)
        pp = &(*pp)->next;

    n->next = *pp;                         /* steal the rest of the list */
    *pp = n;                               /* overwrite the link we stood on —
                                              works identically for the empty
                                              list, the head, and the middle. */
    return head;
}

/* ---- delete: remove the FIRST node holding val (documented choice) ------
 * Implemented with ** because deletion is where the trick shines. To delete
 * ALL matches, keep walking instead of returning — see the comment at the end. */

static node_t *list_delete(node_t *head, int val)
{
    node_t **pp = &head;
    while (*pp != NULL && (*pp)->val != val)
        pp = &(*pp)->next;

    if (*pp != NULL) {                     /* found it: *pp IS the link to us */
        node_t *dead = *pp;
        *pp = dead->next;                  /* unlink by overwriting that link */
        free(dead);                        /* exactly one free, exactly one node */
    }
    return head;                           /* head may be unchanged — or not */
}

#ifdef BREAK_DELETE_HEAD
/* THE BUG: free the head but never tell the caller. The caller's `head`
 * variable still holds the freed address → next list_print is use-after-free. */
static void list_delete_head_broken(node_t *head)
{
    if (head == NULL)
        return;
    node_t *second = head->next;
    free(head);
    (void)second;                          /* "forgot" to hand the new head back */
}
#endif

int main(void)
{
#ifdef BREAK_DELETE_HEAD
    /* not exercised on this path — present so the fixed build stays honest */
    (void)list_free;
    (void)list_insert_sorted;
    (void)list_insert_sorted_pp;
    (void)list_delete;

    node_t *head = NULL;
    head = list_append(head, 10);
    head = list_append(head, 20);
    head = list_append(head, 30);
    list_delete_head_broken(head);         /* frees 10, tells nobody */
    printf("about to walk a freed head...\n");
    list_print("", head);                  /* ASan: heap-use-after-free right here */
    return 0;
#else
    /* ---- lifecycle test ------------------------------------------------- */

    /* append on a growing list: 1 2 3 */
    node_t *a = NULL;
    a = list_append(a, 1);
    a = list_append(a, 2);
    a = list_append(a, 3);
    list_print("append 1,2,3:", a);

    /* insert sorted, 7 values INCLUDING duplicates, into an empty list */
    node_t *b = NULL;
    int vals[] = { 5, 3, 8, 3, 9, 1, 5 };
    for (size_t i = 0; i < sizeof vals / sizeof vals[0]; i++)
        b = list_insert_sorted_pp(b, vals[i]);
    list_print("sorted insert:", b);           /* 1 3 3 5 5 8 9 */

    /* delete the HEAD value */
    b = list_delete(b, 1);
    list_print("delete head 1:", b);           /* 3 3 5 5 8 9 */

    /* delete a MIDDLE value (first of the two 5s) */
    b = list_delete(b, 5);
    list_print("delete mid 5 :", b);           /* 3 3 5 8 9 */

    /* delete a value that ISN'T THERE — must be a clean no-op */
    b = list_delete(b, 42);
    list_print("delete 42     :", b);          /* unchanged */

    /* the naive version gets the same answers — keep both, compare in your head:
     * naive needed (prev tracking + a head special case); ** needed nothing. */
    node_t *c = NULL;
    c = list_insert_sorted(c, 7);
    c = list_insert_sorted(c, 2);
    list_print("naive insert :", c);

    /* sanity: both implementations agree on one input */
    node_t *d = NULL, *e = NULL;
    int vals2[] = { 9, 4, 6 };
    for (size_t i = 0; i < 3; i++) {
        d = list_insert_sorted(d, vals2[i]);
        e = list_insert_sorted_pp(e, vals2[i]);
    }
    printf("both versions agree: %s\n", "verify visually below");
    list_print("naive :", d);
    list_print("ptr-ptr:", e);

    list_free(a);
    list_free(b);
    list_free(c);
    list_free(d);
    list_free(e);

    /* To delete ALL occurrences instead of the first: don't return after the
     * free — keep advancing pp and repeat the found-branch. Same trick, loop it. */
    return 0;
#endif
}
