/*
 * Bite 43 — CMake + tests: strbuf implementation.
 * The doubling-growth pattern from bite 25, for strings. Note the
 * invariant kept at every step: data is always NUL-terminated, so any
 * caller may printf("%s") it between appends without a copy.
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

/* Grow to fit len + add + 1 (for the NUL). Returns 0 ok, -1 OOM. */
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
