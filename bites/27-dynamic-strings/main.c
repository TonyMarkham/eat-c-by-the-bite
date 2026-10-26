/*
 * Bite 27 — Dynamic Strings: my_strdup + a growable string builder
 *
 * COMPILE & RUN (from the repo root, inside WSL):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/27-dynamic-strings/main.c -o /tmp/b27 && /tmp/b27
 *   valgrind --leak-check=full --show-leak-kinds=all /tmp/b27   → 0 errors, 0 leaks
 *
 * BREAK IT ON PURPOSE: append WITHOUT growing (comment out the growth check)
 * — a heap buffer overrun. Build the broken variant and watch ASan catch the
 * exact write past the buffer:
 *   gcc -Wall -Wextra -Werror -std=c17 -g -DBROKEN_NO_GROWTH -fsanitize=address,undefined \
 *       bites/27-dynamic-strings/main.c -o /tmp/b27-broken && /tmp/b27-broken
 *   → "ERROR: AddressSanitizer: heap-buffer-overflow ... WRITE of size 7
 *      ... in sb_appendf (main.c:128)" (the second append renders past the
 *      exact-size buffer the first append allocated)
 * Reinstate the check (rebuild without the flag) and it's clean again. The
 * failure you caused is the exact reason the check exists.
 *
 * WHY (the "Learn" bullets):
 *   - The str-builder pattern: struct { char *buf; size_t len, cap; } with
 *     amortized doubling — the same skeleton as bite 25's intvec, holding
 *     chars instead of ints. len counts real bytes; buf[len] is ALWAYS the
 *     '\0', so the string is valid at every instant.
 *   - snprintf(NULL, 0, fmt, ...) returns the length the output WOULD need
 *     without writing anywhere — the standard trick to size an allocation
 *     exactly, for printf-style building.
 *   - my_strdup: malloc(strlen + 1), copy, return. The +1 for the terminator
 *     is the single most classic off-by-one in C.
 */

#define _POSIX_C_SOURCE 200809L   /* so the real strdup() is declared under -std=c17 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- my_strdup: clone a string into freshly owned heap memory ----------- */

char *my_strdup(const char *s)
{
    size_t n = strlen(s) + 1;              /* +1: the '\0' must fit too */
    char *copy = malloc(n);
    if (copy == NULL)
        return NULL;                       /* caller checks — we never abort */
    memcpy(copy, s, n);
    return copy;
}

/* ---- strbuf: the growable string ---------------------------------------- */

typedef struct {
    char *buf;             /* NUL-terminated at buf[len] at ALL times */
    size_t len;            /* strlen(buf) — tracked so we never re-scan */
    size_t cap;            /* bytes allocated for buf (including room for '\0') */
} strbuf;

/* Make sure `need` bytes (len + new bytes + 1 for '\0') fit. Returns 0 or -1.
 * This function contains the growth check that the broken build pretends
 * doesn't matter. It does. */
static int sb_reserve(strbuf *sb, size_t need)
{
#ifdef BROKEN_NO_GROWTH
    /* BUG: "the buffer will surely be big enough." On the FIRST append we
     * still must allocate (buf is NULL), sized to exactly what's asked —
     * which guarantees the SECOND append overruns it. */
    if (sb->buf == NULL) {
        sb->buf = malloc(need);
        if (sb->buf == NULL)
            return -1;
        sb->cap = need;
    }
    return 0;
#else
    if (need <= sb->cap)
        return 0;                          /* headroom already there: the common,
                                              O(1) case — this is why doubling pays */

    size_t newcap = sb->cap ? sb->cap : 16;      /* start at 16 bytes */
    while (newcap < need)
        newcap *= 2;                       /* double until it fits: amortized O(1) */

    char *tmp = realloc(sb->buf, newcap);  /* tmp-pointer idiom from bite 25 */
    if (tmp == NULL)
        return -1;                         /* old buf still valid, still NUL-terminated */
    sb->buf = tmp;
    sb->cap = newcap;
    return 0;
#endif
}

static void sb_init(strbuf *sb)
{
    sb->buf = NULL;
    sb->len = 0;
    sb->cap = 0;
}

/* Append a plain C string. */
static int sb_append(strbuf *sb, const char *s)
{
    size_t n = strlen(s);
    if (sb_reserve(sb, sb->len + n + 1) != 0)    /* +1: the '\0' after the copy */
        return -1;
    memcpy(sb->buf + sb->len, s, n);       /* no '\0' copied — we write it next */
    sb->len += n;
    sb->buf[sb->len] = '\0';               /* invariant restored, every time */
    return 0;
}

/* Append printf-style. Two passes:
 *   pass 1: vsnprintf(NULL, 0, ...) → how many bytes would it need?
 *   pass 2: reserve exactly that, then render straight into place. */
static int sb_appendf(strbuf *sb, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    int need = vsnprintf(NULL, 0, fmt, ap);      /* measures without writing */
    va_end(ap);
    if (need < 0)
        return -1;

    if (sb_reserve(sb, sb->len + (size_t)need + 1) != 0)
        return -1;

    va_start(ap, fmt);                     /* restart: pass 1 consumed the args */
    int written = vsnprintf(sb->buf + sb->len, (size_t)need + 1, fmt, ap);
    va_end(ap);
    if (written < 0)
        return -1;

    sb->len += (size_t)written;            /* vsnprintf always NUL-terminates */
    return 0;
}

static void sb_free(strbuf *sb)
{
    free(sb->buf);
    sb->buf = NULL;
    sb->len = sb->cap = 0;
}

int main(void)
{
    /* ---- 1) my_strdup vs the real strdup -------------------------------- */
    const char *orig = "heap-owned copy of a string literal";
    char *mine = my_strdup(orig);
    char *theirs = strdup(orig);
    if (mine == NULL || theirs == NULL) {
        fprintf(stderr, "allocation failed\n");
        free(mine);
        free(theirs);
        return 1;
    }
    printf("my_strdup: \"%s\" (strcmp == real: %s, pointers differ: %s)\n",
           mine,
           strcmp(mine, theirs) == 0 ? "yes" : "no",
           mine != theirs ? "yes" : "no");
    free(mine);
    free(theirs);

    /* ---- 2+3) build a 1000-word sentence by appending in a loop --------- */
    strbuf sb;
    sb_init(&sb);

    for (int i = 0; i < 1000; i++)
        if (sb_appendf(&sb, "word%d ", i) != 0) {
            fprintf(stderr, "append failed\n");
            sb_free(&sb);
            return 1;
        }
    /* Final touch with plain sb_append to show both entry points. */
    if (sb_append(&sb, "END.") != 0) {
        sb_free(&sb);
        return 1;
    }

    printf("built %zu bytes; capacity %zu (doubled from 16)\n", sb.len, sb.cap);
    printf("head: \"%.40s...\"\n", sb.buf);
    printf("tail: \"...%s\"\n", sb.buf + sb.len - 30);
    printf("NUL-terminated check: strlen(buf)=%zu == len=%zu -> %s\n",
           strlen(sb.buf), sb.len,
           strlen(sb.buf) == sb.len ? "OK" : "BROKEN");

    sb_free(&sb);

    /* Ownership recap: my_strdup hands you a block you now own — you free it.
     * strbuf owns its buffer internally and sb_free is the owner's cleanup.
     * Same rule as bite 26, one layer up. */
    return 0;
}
