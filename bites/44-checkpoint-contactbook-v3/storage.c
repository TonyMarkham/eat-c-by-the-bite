/*
 * Bite 44 — ✅ CHECKPOINT: Contact Book v3 — storage implementation.
 * Format + failure cases documented in storage.h; convention in
 * contact.h. This file returns codes; it never prints user-facing
 * errors (rule 4 of the convention).
 */
#include "storage.h"

#include <stdio.h>
#include <string.h>

#include "strbuf.h"

cb_result storage_save(const contactbook_t *b, const char *path)
{
    if (b == NULL || path == NULL)
        return CB_ERR_ARG;

    FILE *f = fopen(path, "w");         /* save = replace */
    if (f == NULL)
        return CB_ERR_IO;               /* errno holds the reason */

    int failed = 0;
    for (size_t i = 0; i < b->count && !failed; i++) {
        /* strbuf earns its reuse here: build the line, then write it.
         * (One fprintf would also work; strbuf makes the line
         * inspectable and reusable - and it's the module we promised
         * to reuse in this checkpoint.) */
        strbuf line;
        sb_init(&line);
        if (sb_append(&line, b->items[i].name) == 0
            && sb_append(&line, ";") == 0
            && sb_append(&line, b->items[i].phone) == 0
            && sb_append(&line, "\n") == 0) {
            if (fputs(line.data, f) == EOF)
                failed = 1;
        } else {
            failed = 1;
        }
        sb_free(&line);
    }

    if (fclose(f) != 0)                 /* fclose flushes: it can fail */
        failed = 1;
    return failed ? CB_ERR_IO : CB_OK;
}

/* static helper: parse one "name;phone" line. 1 ok, 0 malformed. */
static int parse_line(const char *line, contact_t *c)
{
    return sscanf(line, "%31[^;];%15s", c->name, c->phone) == 2
           && c->name[0] != '\0';
}

cb_result storage_load(contactbook_t *b, const char *path, size_t *skipped)
{
    if (b == NULL || path == NULL)
        return CB_ERR_ARG;
    if (skipped)
        *skipped = 0;

    FILE *f = fopen(path, "r");
    if (f == NULL)
        return CB_ERR_IO;               /* missing file: errno = ENOENT */

    char line[256];
    while (fgets(line, sizeof line, f) != NULL) {
        size_t len = strlen(line);

        /* Over-long line: drain the remainder, count as one skip. */
        if (len > 0 && line[len - 1] != '\n' && !feof(f)) {
            int ch;
            while ((ch = fgetc(f)) != EOF && ch != '\n')
                ;
            if (skipped)
                (*skipped)++;
            continue;
        }

        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';
        if (line[0] == '\0')
            continue;                   /* blank line: harmless */

        contact_t c;
        if (!parse_line(line, &c)) {
            if (skipped)
                (*skipped)++;
            continue;                   /* dirty line: skip, not fatal */
        }
        if (book_add(b, c.name, c.phone) != CB_OK && skipped)
            (*skipped)++;               /* dup/full while loading: counted */
    }

    fclose(f);
    return CB_OK;
}
