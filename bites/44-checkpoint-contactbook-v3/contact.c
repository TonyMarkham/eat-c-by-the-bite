/*
 * Bite 44 — ✅ CHECKPOINT: Contact Book v3 — contact implementation.
 * Convention in contact.h; build/run/abuse-script in README.md.
 */
#include "contact.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *cb_strerror(cb_result r)
{
    switch (r) {
    case CB_OK:           return "ok";
    case CB_ERR:          return "error";
    case CB_ERR_ARG:      return "bad argument";
    case CB_ERR_DUP:      return "duplicate name";
    case CB_ERR_NOTFOUND: return "not found";
    case CB_ERR_IO:       return "I/O error (see errno)";
    }
    return "unknown error";
}

/* Module-private truncating copy: static = not in the header, not in
 * the linker's global namespace. */
static void copy_field(char *dst, size_t dstsz, const char *src)
{
    size_t i = 0;
    if (dstsz == 0)
        return;
    while (i + 1 < dstsz && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';              /* truncate, never overflow */
}

void book_init(contactbook_t *b)
{
    b->items = NULL;
    b->count = 0;
    b->cap = 0;
}

void book_free(contactbook_t *b)
{
    if (b == NULL)
        return;
    free(b->items);
    b->items = NULL;
    b->count = 0;
    b->cap = 0;
}

static int book_grow(contactbook_t *b)
{
    size_t newcap = (b->cap == 0) ? 4 : b->cap * 2;
    contact_t *p = realloc(b->items, newcap * sizeof *p);
    if (p == NULL)
        return -1;              /* old block still valid: book unchanged */
    b->items = p;
    b->cap = newcap;
    return 0;
}

cb_result book_add(contactbook_t *b, const char *name, const char *phone)
{
    if (b == NULL || name == NULL || phone == NULL || name[0] == '\0')
        return CB_ERR_ARG;
    if (book_find(b, name) >= 0)
        return CB_ERR_DUP;
    if (b->count == b->cap && book_grow(b) != 0)
        return CB_ERR;

    contact_t *c = &b->items[b->count++];
    copy_field(c->name, sizeof c->name, name);
    copy_field(c->phone, sizeof c->phone, phone);
    return CB_OK;
}

int book_find(const contactbook_t *b, const char *name)
{
    if (b == NULL || name == NULL)
        return CB_ERR_NOTFOUND;
    for (size_t i = 0; i < b->count; i++)
        if (strcmp(b->items[i].name, name) == 0)
            return (int)i;
    return CB_ERR_NOTFOUND;
}

cb_result book_remove(contactbook_t *b, const char *name)
{
    if (b == NULL || name == NULL)
        return CB_ERR_ARG;

    int idx = book_find(b, name);
    if (idx < 0)
        return CB_ERR_NOTFOUND;

    memmove(&b->items[idx], &b->items[idx + 1],
            (b->count - (size_t)idx - 1) * sizeof b->items[0]);
    b->count--;
    return CB_OK;
}

/* qsort comparator (bite 32): compare by name, sign-only semantics. */
static int cmp_by_name(const void *pa, const void *pb)
{
    const contact_t *a = pa, *b = pb;
    int c = strcmp(a->name, b->name);
    return (c > 0) - (c < 0);
}

void book_sort(contactbook_t *b)
{
    if (b == NULL || b->count < 2)
        return;
    qsort(b->items, b->count, sizeof b->items[0], cmp_by_name);
}

void book_print(const contactbook_t *b)
{
    if (b->count == 0) {
        printf("(book is empty)\n");
        return;
    }
    for (size_t i = 0; i < b->count; i++)
        printf("%2zu. %-32s %s\n", i + 1, b->items[i].name, b->items[i].phone);
}
