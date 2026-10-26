/*
 * Bite 40 — Error handling conventions: main.c = the only layer that
 * talks to humans.
 *
 * BUILD:  gcc -Wall -Wextra -Werror -std=c17 -g -o prog \
 *             log.c contact.c storage.c main.c
 * RUN:    ./prog
 *         (a scripted demo - it walks every error path on purpose)
 *
 * What the demo shows, in order:
 *   1. the success path (CB_OK flows through silently),
 *   2. CB_ERR_DUP and CB_ERR_FULL propagating up from book_add,
 *      reported ONCE, here, by name (cb_strerror),
 *   3. CB_ERR_IO from a missing file, with errno preserved so main
 *      can print strerror(errno) - the real reason, from the OS,
 *   4. a malformed file: not an error at all, just skipped lines
 *      counted by storage_load and reported here,
 *   5. perror() - the one-liner that does "message: strerror(errno)"
 *      for quick top-level scripts.
 *
 * BREAK IT ON PURPOSE: compile with -DDEMO_BAD_ERROR (see storage.c).
 * The low-level loader then prints its own error and returns void:
 * main double-reports AND cannot tell failure from success. That
 * "cannot recover" feeling is why rule 4 exists.
 *
 *   gcc -Wall -Wextra -Werror -std=c17 -g -DDEMO_BAD_ERROR -o prog \
 *       log.c contact.c storage.c main.c && ./prog
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "contact.h"
#include "storage.h"

/* Top-level helper: translate a cb_result + errno into ONE user-facing
 * line. This is where reporting belongs - nowhere lower. */
static void report_load_failure(const char *path, cb_result r)
{
    if (r == CB_ERR_IO)
        log_error("cannot load %s: %s", path, strerror(errno));
    else
        log_error("cannot load %s: %s", path, cb_strerror(r));
}

/* Show that strerror(errno) reflects the most recent failing syscall.
 * Safe against short writes: snprintf truncates, never overflows. */
static void dump_errno_line(const char *what, const char *path)
{
    char msg[128];
    snprintf(msg, sizeof msg, "%s (after %s)", strerror(errno), what);
    log_info("%s: %s", path, msg);
}

int main(void)
{
    contactbook_t book;
    book_init(&book);

    /* ---- 1. success path: codes flow, nobody prints --------------- */
    cb_result r = book_add(&book, "Alice", "555-0101");
    log_info("add Alice: %s", cb_strerror(r));
    r = book_add(&book, "Bob", "555-0102");
    log_info("add Bob: %s", cb_strerror(r));

    /* ---- 2. duplicate + full: failures propagate, main reports ----- */
    r = book_add(&book, "Alice", "555-9999");
    if (r != CB_OK)
        log_error("add Alice again failed: %s", cb_strerror(r));

    /* BOOK_CAP is 4 - fill the rest, then meet CB_ERR_FULL. */
    r = book_add(&book, "Carol", "555-0103");
    r = book_add(&book, "Dave", "555-0104");
    r = book_add(&book, "Eve", "555-0105");
    if (r == CB_ERR_FULL)
        log_error("add Eve failed: %s (BOOK_CAP = %d)",
                  cb_strerror(r), BOOK_CAP);

    /* ---- 3. missing file: CB_ERR_IO + errno preserved -------------- */
    const char *missing = "/tmp/cb40_no_such_file.txt";
    size_t skipped = 0;
    r = storage_load(&book, missing, &skipped);
    if (r != CB_OK)
        report_load_failure(missing, r);
    else
        log_info("loaded %zu contacts from %s", book.count, missing);
    dump_errno_line("fopen of missing file", missing);

    /* ---- 4. malformed file: skipped lines, not an error ------------ */
    const char *dirty = "/tmp/cb40_malformed.txt";
    FILE *fx = fopen(dirty, "w");
    if (fx) {
        fputs("Alice;555-0101\nno-semicolon-here\n;555-0102\nBob;555-0102\n",
              fx);
        fclose(fx);
    }
    contactbook_t loaded;
    book_init(&loaded);
    r = storage_load(&loaded, dirty, &skipped);
    if (r != CB_OK) {
        report_load_failure(dirty, r);
    } else if (skipped > 0) {
        /* One report, from the top, with the actionable detail. */
        log_error("loaded %zu contacts from %s but skipped %zu bad line(s)",
                  loaded.count, dirty, skipped);
    } else {
        log_info("loaded %zu contacts from %s", loaded.count, dirty);
    }

    /* ---- 5. perror: strerror + message in one call ------------------ */
    FILE *f = fopen("/tmp/cb40_no_such_file.txt", "r");
    if (f == NULL)
        perror("/tmp/cb40_no_such_file.txt");   /* "path: No such file or directory" */
    else
        fclose(f);

    /* The book is a fixed-size array - nothing to free. Had it owned
     * heap memory, main would be responsible for releasing it before
     * return (and valgrind in bite 44 would verify that). */
    return 0;
}
