/*
 * Bite 44 — CHECKPOINT: Contact Book v3 — strbuf implementation.
 * Doubling growth; data is always NUL-terminated between appends.
 */
#include "strbuf.h"

#include <stdlib.h>
#include <string.h>

void sb_init(strbuf *sb)
{
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

static int sb_reserve(strbuf *sb, size_t need)
{
    if (need <= sb->cap)
        return 0;
    size_t newcap = (sb->cap == 0) ? 16 : sb->cap;
    while (newcap < need)
        newcap *= 2;
    char *p = realloc(sb->data, newcap);
    if (p == NULL)
        return -1;              /* old block still valid: sb unchanged */
    sb->data = p;
    sb->cap = newcap;
    return 0;
}

int sb_append(strbuf *sb, const char *s)
{
    if (sb == NULL || s == NULL)
        return -1;
    size_t add = strlen(s);
    if (sb_reserve(sb, sb->len + add + 1) != 0)
        return -1;
    memcpy(sb->data + sb->len, s, add + 1);     /* +1 copies the NUL */
    sb->len += add;
    return 0;
}

void sb_free(strbuf *sb)
{
    if (sb == NULL)
        return;
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}
