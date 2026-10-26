/*
 * Bite 40 — Error handling conventions: contact module implementation.
 * The convention block lives in contact.h; build/run + break-it in
 * main.c. Every function obeys it: return codes, no printing, no
 * half-done states.
 */
#include "contact.h"

#include <stdio.h>
#include <string.h>

const char *cb_strerror(cb_result r)
{
    switch (r) {
    case CB_OK:           return "ok";
    case CB_ERR:          return "error";
    case CB_ERR_ARG:      return "bad argument";
    case CB_ERR_FULL:     return "book full";
    case CB_ERR_DUP:      return "duplicate name";
    case CB_ERR_NOTFOUND: return "not found";
    case CB_ERR_IO:       return "I/O error (see errno)";
    }
    return "unknown error";
}

static void copy_field(char *dst, size_t dstsz, const char *src)
{
    size_t i = 0;
    if (dstsz == 0)
        return;
    while (i + 1 < dstsz && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

void book_init(contactbook_t *b)
{
    b->count = 0;
}

/* Every failure exits BEFORE any state change: all-or-nothing (rule 5). */
cb_result book_add(contactbook_t *b, const char *name, const char *phone)
{
    if (b == NULL || name == NULL || phone == NULL || name[0] == '\0')
        return CB_ERR_ARG;
    if (b->count >= BOOK_CAP)
        return CB_ERR_FULL;
    if (book_find(b, name) >= 0)
        return CB_ERR_DUP;

    contact_t *c = &b->items[b->count];
    copy_field(c->name, sizeof c->name, name);
    copy_field(c->phone, sizeof c->phone, phone);
    b->count++;
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

void book_print(const contactbook_t *b)
{
    if (b->count == 0) {
        printf("(book is empty)\n");
        return;
    }
    for (size_t i = 0; i < b->count; i++)
        printf("%2zu. %-32s %s\n", i + 1, b->items[i].name, b->items[i].phone);
}
