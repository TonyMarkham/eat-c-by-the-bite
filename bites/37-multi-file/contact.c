/*
 * Bite 37 — Multi-file projects: contact module (IMPLEMENTATION).
 * Build + run commands and the BREAK IT ON PURPOSE experiments are
 * documented in main.c; the header-rules rationale is in contact.h.
 *
 * Everything file-scope here is `static`: module-private helpers never
 * appear in the header, so main.c compiles knowing NOTHING about how
 * the book works internally. This is the boundary that keeps projects
 * from collapsing into spaghetti.
 */
#include "contact.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Truncating copy into a fixed-size field. Being defensive here is
 * what makes book_add unable to overflow no matter what input it gets
 * (the classic gets()/strcpy() lesson from bite 16, applied). */
static void copy_field(char *dst, size_t dstsz, const char *src)
{
    size_t i = 0;
    if (dstsz == 0)
        return;
    while (i + 1 < dstsz && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';              /* always NUL-terminate: truncate, don't overflow */
}

void book_init(contactbook_t *b)
{
    b->items = NULL;
    b->count = 0;
    b->cap = 0;
}

void book_free(contactbook_t *b)
{
    free(b->items);             /* free(NULL) is legal and does nothing */
    b->items = NULL;
    b->count = 0;
    b->cap = 0;
}

/* Grow the array by doubling (bite 25's pattern). Static: an internal
 * concern, not part of the interface. */
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

int book_add(contactbook_t *b, const char *name, const char *phone)
{
    if (name == NULL || phone == NULL)
        return -1;
    if (book_find(b, name) >= 0)
        return -1;              /* duplicate name: refuse rather than confuse */

    if (b->count == b->cap && book_grow(b) != 0)
        return -1;              /* out of memory */

    contact_t *c = &b->items[b->count++];
    copy_field(c->name, sizeof c->name, name);
    copy_field(c->phone, sizeof c->phone, phone);
    return 0;
}

int book_find(const contactbook_t *b, const char *name)
{
    if (name == NULL)
        return -1;
    for (size_t i = 0; i < b->count; i++)
        if (strcmp(b->items[i].name, name) == 0)
            return (int)i;
    return -1;
}

int book_remove(contactbook_t *b, const char *name)
{
    int idx = book_find(b, name);
    if (idx < 0)
        return -1;
    /* Close the gap by shifting the tail left one slot. The last
     * element is copied over itself when idx == count-1: harmless. */
    memmove(&b->items[idx], &b->items[idx + 1],
            (b->count - (size_t)idx - 1) * sizeof b->items[0]);
    b->count--;
    return 0;
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
