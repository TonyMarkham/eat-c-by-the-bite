/*
 * Bite 40 — Error handling conventions: storage implementation.
 *
 * Notice what this file does NOT do: it never prints a user-facing
 * error and never invents a message. It returns codes (and preserves
 * errno for I/O failures) so the top layer can decide how - and in
 * what language, log, or UI - the failure should appear. That is
 * rule 4/6 of the convention in contact.h.
 */
#include "storage.h"

#include <stdio.h>
#include <string.h>

cb_result storage_save(const contactbook_t *b, const char *path)
{
    if (b == NULL || path == NULL)
        return CB_ERR_ARG;

    FILE *f = fopen(path, "w");
    if (f == NULL)
        return CB_ERR_IO;               /* errno holds the reason (ENOENT...) */

    int failed = 0;
    for (size_t i = 0; i < b->count && !failed; i++)
        if (fprintf(f, "%s;%s\n", b->items[i].name, b->items[i].phone) < 0)
            failed = 1;                 /* errno again meaningful */

    if (fclose(f) != 0)
        failed = 1;
    return failed ? CB_ERR_IO : CB_OK;
}

/* static helper: parse one line. Returns 1 ok, 0 malformed. */
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
        return CB_ERR_IO;               /* missing file lands here; errno = ENOENT */

    char line[256];
    while (fgets(line, sizeof line, f) != NULL) {
        size_t len = strlen(line);

        /* Over-long line: drain the rest, count it as skipped. */
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
            continue;

        contact_t c;
        if (!parse_line(line, &c)) {
            if (skipped)
                (*skipped)++;           /* dirty data: counted, not fatal */
            continue;
        }
        if (book_add(b, c.name, c.phone) != CB_OK && skipped)
            (*skipped)++;               /* dup/full during load: also counted */
    }

    fclose(f);
    return CB_OK;
}

#ifdef DEMO_BAD_ERROR
/*
 * BREAK IT ON PURPOSE (the lesson's experiment, preserved for you):
 * this version PRINTS its error and returns void. Compile the whole
 * project with -DDEMO_BAD_ERROR and main() will call it. What you
 * observe:
 *   - the low-level layer and main BOTH report the same failure
 *     (double-reporting: confusing, and it grows worse with layers);
 *   - the caller cannot tell success from failure (void return), so
 *     it cannot recover, retry, or stay silent;
 *   - the message is hardcoded for a terminal user even if the caller
 *     is a test, a GUI, or a server that logs to a file.
 * Feel the reason conventions exist. Then delete this block.
 */
void storage_load_bad(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        perror(path);                   /* <-- the anti-pattern */
        return;
    }
    /* ... imagine the real loader here ... */
    fclose(f);
}
#endif /* DEMO_BAD_ERROR */
