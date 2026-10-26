/*
 * Bite 28 — Linked List I: push front, print, free the whole list
 *
 * COMPILE & RUN (from the repo root, inside WSL):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/28-linked-list-1/main.c -o /tmp/b28 && /tmp/b28
 *   valgrind --leak-check=full --show-leak-kinds=all /tmp/b28
 *   → "All heap blocks were freed -- no leaks are possible"
 *
 * BREAK IT ON PURPOSE: in list_free, free the node BEFORE saving next — you
 * then read head->next out of freed memory. Build the buggy variant and run
 * it under ASan (fastest way to see the exact byte):
 *   gcc -Wall -Wextra -Werror -std=c17 -g -DBUG_FREE_BEFORE_NEXT -fsanitize=address,undefined \
 *       -Wno-use-after-free bites/28-linked-list-1/main.c -o /tmp/b28-buggy && /tmp/b28-buggy
 *   → "ERROR: AddressSanitizer: heap-use-after-free ... READ of size 8"
 * (The extra -Wno-use-after-free is needed because GCC 13's -Wall already
 *  catches this pattern at compile time — leave it ON in your own code.)
 * Valgrind agrees: "Invalid read of size 8 ... block ... free'd". Then fix
 * (the default build): save next FIRST, free second. That ordering is the
 * entire lesson of this bite — the bug will try to visit you for your whole
 * career.
 *
 * WHY (the "Learn" bullets):
 *   - typedef struct node { int val; struct node *next; } node_t; — a struct
 *     containing a pointer to its own type. The pointer is fixed-size, so
 *     the self-reference compiles; a struct containing ITSELF would not.
 *   - Empty list = NULL head. Every operation must survive it.
 *   - push_front returns the NEW head because the head itself changes:
 *         head = push_front(head, 5);
 *     The caller reassigns — that's the contract.
 *   - Drawing first, always: linked lists are pointer manipulation; the code
 *     is just the drawing transcribed.
 */

#include <stdio.h>
#include <stdlib.h>

typedef struct node {
    int val;
    struct node *next;     /* pointer to own type: 8 bytes, always fits */
} node_t;

/* Allocate a node and make it the new first node.
 * Returns the new head; the caller MUST reassign: head = push_front(head, v);
 * Works on an empty list too: push_front(NULL, 5) → a one-node list. */
static node_t *push_front(node_t *head, int val)
{
    node_t *n = malloc(sizeof *n);
    if (n == NULL) {
        fprintf(stderr, "malloc failed\n");
        return head;       /* list unchanged — caller can compare pointers to detect */
    }
    n->val = val;
    n->next = head;        /* the old head (possibly NULL) becomes our tail */
    return n;              /* we are the new head */
}

/* Walk and print. const node_t * promises not to modify nodes.
 * The loop idiom to memorize: for (p = head; p != NULL; p = p->next). */
static void list_print(const node_t *head)
{
    for (const node_t *p = head; p != NULL; p = p->next)
        printf("%d -> ", p->val);
    printf("(null)\n");
}

/* THE free-everything loop — the entire point of this bite.
 * Correct order: SAVE next, THEN free current. Freeing first leaves `next`
 * readable only by luck, and reading head->next after free is use-after-free. */
static void list_free(node_t *head)
{
    while (head != NULL) {
#ifdef BUG_FREE_BEFORE_NEXT
        /* BUG: free now, read the dead node's next after. ASan/valgrind will
         * flag the very next line. */
        free(head);
        head = head->next;          /* use-after-free: head->next was just freed */
#else
        node_t *next = head->next;  /* 1. save the link BEFORE it dies */
        free(head);                 /* 2. return the node */
        head = next;                /* 3. step to the saved link */
#endif
    }
}

int main(void)
{
    node_t *head = NULL;                   /* empty list: the head IS the state */

    /* Push 10 values. push_front prepends, so the list ends up in REVERSE
     * insertion order — printing 10 first is the feature, not a bug. */
    for (int i = 1; i <= 10; i++)
        head = push_front(head, i);        /* reassign the head EVERY push */

    printf("list built by push_front (reverse order): ");
    list_print(head);

    /* Free every node. After this, head is... still the old (dangling) first
     * node's address, so we NULL it — the same discipline as bite 24. */
    list_free(head);
    head = NULL;

    /* Empty-list handling exercised for real: push onto NULL worked above
     * (first iteration), and freeing an empty list is a no-op loop. */
    node_t *empty = NULL;
    list_free(empty);                      /* must not crash */

    /* 6 allocations, 6 frees, 0 errors. Valgrind's exact verdict:
     *   "All heap blocks were freed -- no leaks are possible"
     *   "ERROR SUMMARY: 0 errors from 0 contexts" */
    return 0;
}
