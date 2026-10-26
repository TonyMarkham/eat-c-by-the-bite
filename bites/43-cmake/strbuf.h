/*
 * Bite 43 — CMake + tests: strbuf module (interface) - bite 27's
 * growable string builder, now a real module under test.
 * Build/run commands in CMakeLists.txt's header comment.
 */

#ifndef STRBUF_H
#define STRBUF_H

#include <stddef.h>

typedef struct {
    char *data;     /* NUL-terminated; NULL until first append */
    size_t len;     /* bytes used, NOT counting the NUL */
    size_t cap;     /* bytes allocated for data (or 0) */
} strbuf;

void sb_init(strbuf *sb);
/* Append s, growing as needed. Returns 0 ok, -1 out of memory
 * (contents preserved on OOM - all-or-nothing). */
int sb_append(strbuf *sb, const char *s);
/* Release the buffer; safe to call twice (data set to NULL). */
void sb_free(strbuf *sb);

#endif /* STRBUF_H */
