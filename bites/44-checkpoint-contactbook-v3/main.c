/*
 * Bite 44 — ✅ CHECKPOINT: Contact Book v3 — main.c (UI only).
 *
 * BUILD & RUN (full story in README.md):
 *   cmake -S . -B build && cmake --build build && ./build/contactbook [datafile]
 * RUN (plain gcc, no CMake):
 *   gcc -Wall -Wextra -Werror -std=c17 -g -o contactbook \
 *       contact.c storage.c strbuf.c util.c main.c
 *
 * main is the TOP layer of the convention: every failure surfaces
 * here, once, in human language. Modules below return codes and stay
 * silent. The menu is EOF-safe (Ctrl-D quits cleanly) and rejects
 * garbage input without crashing - feed it nonsense, it copes.
 *
 * BREAK IT ON PURPOSE: see README.md's "Abuse script" - garbage
 * files, huge inputs, EOF mid-menu, then valgrind across the whole
 * session. Whatever a fresh user gets wrong from the README alone is
 * a documentation bug - fix the README, not the user.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "contact.h"
#include "storage.h"
#include "util.h"

#define DEFAULT_DATAFILE "contacts.txt"

/* Ask for one line; empty result signals EOF (callers cope). */
static void ask(const char *prompt, char *dst, size_t dstsz)
{
    printf("%s", prompt);
    fflush(stdout);
    char tmp[256];
    if (fgets(tmp, sizeof tmp, stdin) == NULL) {
        dst[0] = '\0';
        return;
    }
    tmp[strcspn(tmp, "\n")] = '\0';
    snprintf(dst, dstsz, "%s", tmp);    /* truncate, never overflow */
}

/* Load at startup if the file exists - quiet, unless something's off.
 * This is main doing top-level decisions; storage stays silent. */
static void load_on_start(contactbook_t *book, const char *path)
{
    FILE *probe = fopen(path, "r");
    if (probe == NULL)
        return;                          /* no file yet: fresh start, no noise */
    fclose(probe);

    size_t skipped = 0;
    cb_result r = storage_load(book, path, &skipped);
    if (r == CB_OK) {
        if (skipped > 0)
            log_error("loaded %zu contacts from %s, skipped %zu bad line(s)",
                      book->count, path, skipped);
        else
            log_info("loaded %zu contacts from %s", book->count, path);
    } else {
        log_error("could not load %s: %s", path, cb_strerror(r));
    }
}

int main(int argc, char **argv)
{
    const char *datafile = (argc > 1) ? argv[1] : DEFAULT_DATAFILE;

    contactbook_t book;
    book_init(&book);
    load_on_start(&book, datafile);

    char line[128];
    char name[32], phone[16];

    printf("contact book v3 - datafile: %s\n", datafile);
    printf("commands: 1 add  2 list  3 find  4 remove  5 sort  "
           "6 save  7 load  q quit\n");

    for (;;) {
        printf("> ");
        fflush(stdout);
        if (fgets(line, sizeof line, stdin) == NULL) {
            putchar('\n');              /* EOF (Ctrl-D): quit cleanly */
            break;
        }

        int choice;
        if (sscanf(line, "%d", &choice) != 1) {
            if (line[0] == 'q')
                break;
            printf("unknown command\n");
            continue;
        }

        switch (choice) {
        case 1:
            ask("name: ", name, sizeof name);
            if (name[0] == '\0') {
                printf("cancelled (no name)\n");
                break;
            }
            ask("phone: ", phone, sizeof phone);
            cb_result r = book_add(&book, name, phone);
            if (r != CB_OK)
                log_error("could not add \"%s\": %s", name, cb_strerror(r));
            else
                printf("added %s\n", name);
            break;

        case 2:
            book_print(&book);
            break;

        case 3: {
            ask("name: ", name, sizeof name);
            int idx = book_find(&book, name);
            if (idx >= 0)
                printf("found: %s : %s\n",
                       book.items[idx].name, book.items[idx].phone);
            else
                printf("not found\n");
            break;
        }

        case 4:
            ask("name: ", name, sizeof name);
            if (book_remove(&book, name) != CB_OK)
                printf("not found\n");
            else
                printf("removed %s\n", name);
            break;

        case 5:
            book_sort(&book);
            printf("sorted by name\n");
            break;

        case 6: {
            cb_result r = storage_save(&book, datafile);
            if (r != CB_OK) {
                if (r == CB_ERR_IO)
                    log_error("could not save %s: %s", datafile, strerror(errno));
                else
                    log_error("could not save %s: %s", datafile, cb_strerror(r));
            } else {
                printf("saved %zu contacts to %s\n", book.count, datafile);
            }
            break;
        }

        case 7: {
            size_t skipped = 0;
            cb_result r = storage_load(&book, datafile, &skipped);
            if (r != CB_OK) {
                if (r == CB_ERR_IO)
                    log_error("could not load %s: %s", datafile, strerror(errno));
                else
                    log_error("could not load %s: %s", datafile, cb_strerror(r));
            } else {
                printf("loaded %s (book holds %zu contacts", datafile, book.count);
                if (skipped > 0)
                    printf(", %zu bad line(s) skipped", skipped);
                printf(")\n");
            }
            break;
        }

        default:
            printf("unknown command\n");
        }
    }

    book_free(&book);
    return 0;
}
