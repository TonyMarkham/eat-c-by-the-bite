/*
 * Bite 38 — Make: the same multi-file program, now built by a Makefile.
 *
 * BUILD:  make
 * RUN:    ./prog                 (menu; 'q' or Ctrl-D to quit)
 *         ./prog --selftest      (non-interactive round-trip check)
 * MORE:   make check             (ASan/UBSan build + valgrind)
 *         make clean             (remove prog and all .o files)
 *
 * The incremental-build experiments (from the bite's "Do" list):
 *   touch contact.c && make        -> only contact.o recompiles, then link
 *   touch contact.h  && make       -> BOTH contact.o AND main.o rebuild,
 *                                     because their rules list contact.h
 *                                     as a dependency. storage.o does not
 *                                     rebuild (it includes contact.h too...
 *                                     check its dep line in the Makefile).
 *   make clean && make             -> full rebuild from scratch
 *
 * BREAK IT ON PURPOSE: in the Makefile, delete `contact.h` from the
 * `main.o:` dependency line, then `touch contact.h && make`. Nothing
 * rebuilds - the STALE main.o is silently linked into the new prog.
 * This is the classic "I changed the .h but nothing rebuilt" trap, and
 * it produces bugs that look haunted. Re-add the dependency, touch the
 * header again, and watch make fix it. You will never ship a Makefile
 * without header dependencies again.
 */

#include <stdio.h>
#include <string.h>

#include "contact.h"
#include "storage.h"

#define DATAFILE "contacts.txt"

/* Ask the user for one line, truncated safely into dst. On EOF the
 * string comes back empty and callers must cope. */
static void ask(const char *prompt, char *dst, size_t dstsz)
{
    printf("%s", prompt);
    fflush(stdout);                     /* show the prompt before blocking */
    char tmp[256];
    if (fgets(tmp, sizeof tmp, stdin) == NULL) {
        dst[0] = '\0';
        return;
    }
    tmp[strcspn(tmp, "\n")] = '\0';
    snprintf(dst, dstsz, "%s", tmp);
}

/* Non-interactive round-trip, for `make check` (and for valgrind). */
static int selftest(void)
{
    contactbook_t book;
    book_init(&book);
    if (book_add(&book, "Alice", "555-0101") != 0
        || book_add(&book, "Bob", "555-0102") != 0
        || book_add(&book, "Carol", "555-0103") != 0)
        return 1;

    const char *tmpfile = "/tmp/cb38_selftest.txt";
    if (storage_save(&book, tmpfile) != 0)
        return 1;
    book_free(&book);

    contactbook_t loaded;
    book_init(&loaded);
    if (storage_load(&loaded, tmpfile) != 0 || loaded.count != 3)
        return 1;
    int ok = strcmp(loaded.items[0].name, "Alice") == 0
          && strcmp(loaded.items[2].name, "Carol") == 0;
    book_free(&loaded);
    printf("selftest: %s\n", ok ? "OK" : "MISMATCH");
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--selftest") == 0)
        return selftest();

    contactbook_t book;
    book_init(&book);

    char line[128];
    char name[32], phone[16];

    printf("contact book - commands: 1 add  2 list  3 find  4 remove  "
           "5 save  6 load  q quit\n");

    for (;;) {
        printf("> ");
        fflush(stdout);
        if (fgets(line, sizeof line, stdin) == NULL)
            break;                      /* EOF: quit cleanly */

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
            if (book_add(&book, name, phone) != 0)
                printf("could not add \"%s\" (duplicate or out of memory)\n", name);
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
            if (book_remove(&book, name) != 0)
                printf("not found\n");
            else
                printf("removed %s\n", name);
            break;

        case 5:
            if (storage_save(&book, DATAFILE) != 0)
                printf("save failed\n");
            else
                printf("saved %zu contacts to %s\n", book.count, DATAFILE);
            break;

        case 6:
            if (storage_load(&book, DATAFILE) != 0)
                printf("load failed\n");
            else
                printf("loaded %s (book now holds %zu contacts)\n",
                       DATAFILE, book.count);
            break;

        default:
            printf("unknown command\n");
        }
    }

    book_free(&book);
    return 0;
}
