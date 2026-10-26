/*
 * Bite 37 — Multi-file projects: storage module (IMPLEMENTATION).
 * Commands + break-it notes live in main.c and storage.h.
 */
#include "storage.h"

#include <stdio.h>
#include <string.h>

int storage_save(const contactbook_t *b, const char *path)
{
    FILE *f = fopen(path, "w");         /* "w": truncates - save = replace */
    if (f == NULL) {
        perror(path);
        return -1;
    }

    int failed = 0;
    for (size_t i = 0; i < b->count && !failed; i++)
        if (fprintf(f, "%s;%s\n", b->items[i].name, b->items[i].phone) < 0)
            failed = 1;

    if (fclose(f) != 0)                 /* fclose flushes - catch its errors too */
        failed = 1;
    return failed ? -1 : 0;
}

/* Parse one "name;phone" line into c. Returns 1 ok, 0 malformed.
 * static: this helper is an implementation detail, NOT in the header. */
static int parse_line(const char *line, contact_t *c)
{
    /* Same scanset discipline as bite 35:
     * %31[^;] = up to 31 non-';' chars, then a literal ';', then %15s. */
    return sscanf(line, "%31[^;];%15s", c->name, c->phone) == 2
           && c->name[0] != '\0';
}

int storage_load(contactbook_t *b, const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        perror(path);
        return -1;
    }

    char line[256];
    while (fgets(line, sizeof line, f) != NULL) {
        size_t len = strlen(line);

        /* Over-long lines: drain the remainder, warn, skip (bite 35). */
        if (len > 0 && line[len - 1] != '\n' && !feof(f)) {
            int ch;
            while ((ch = fgetc(f)) != EOF && ch != '\n')
                ;
            fprintf(stderr, "%s: warning: line too long, skipped\n", path);
            continue;
        }

        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';
        if (line[0] == '\0')
            continue;                   /* blank line: harmless */

        contact_t c;
        if (!parse_line(line, &c)) {
            fprintf(stderr, "%s: warning: malformed line skipped: \"%s\"\n",
                    path, line);
            continue;
        }
        if (book_add(b, c.name, c.phone) != 0)
            fprintf(stderr, "%s: warning: could not add \"%s\" "
                            "(out of memory or duplicate)\n", path, c.name);
    }

    fclose(f);
    return 0;
}
