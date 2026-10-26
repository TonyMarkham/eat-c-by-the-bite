/*
 * Bite 44 — CHECKPOINT: Contact Book v3 — strbuf module (interface).
 * Bite 27's growable string builder, reused (the checkpoint says so).
 * Convention + build/run in contact.h and README.md.
 */

#ifndef STRBUF_H
#define STRBUF_H

#include <stddef.h>

typedef struct {
    char *data;     /* NUL-terminated; NULL until first append */
    size_t len;     /* bytes used, NOT counting the NUL */
    size_t cap;     /* bytes allocated (or 0) */
} strbuf;

void sb_init(strbuf *sb);
/* Append s, growing as needed. 0 ok, -1 OOM (contents preserved). */
int sb_append(strbuf *sb, const char *s);
/* Release; safe to call twice. */
void sb_free(strbuf *sb);

#endif /* STRBUF_H */
