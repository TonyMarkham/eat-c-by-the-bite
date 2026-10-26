/*
 * Bite 37 — Multi-file projects: main.c = the UI, nothing else.
 *
 * BUILD (one shot - what you'd type before you know `make`):
 *   gcc -Wall -Wextra -Werror -std=c17 -g -o prog contact.c storage.c main.c
 *
 * BUILD (by hand, watching the pieces - exactly what bite 38's
 * Makefile will automate; run these from the bite folder):
 *   gcc -Wall -Wextra -Werror -std=c17 -g -c contact.c -o contact.o
 *   gcc -Wall -Wextra -Werror -std=c17 -g -c storage.c -o storage.o
 *   gcc -Wall -Wextra -Werror -std=c17 -g -c main.c    -o main.o
 *   gcc -Wall -Wextra -Werror -std=c17 -g -o prog contact.o storage.o main.o
 * Then: touch contact.c && recompile ONLY contact.o && relink. That
 * "only rebuild what changed" insight is what make computes for you.
 *
 * RUN:    ./prog        (menu; 'q' or Ctrl-D to quit)
 *
 * main.c knows the modules ONLY through contact.h and storage.h. It
 * cannot see book_grow, copy_field, or parse_line - they are static.
 * That is the whole point: interfaces in headers, machinery in .c.
 *
 * BREAK IT ON PURPOSE (two classics - both now familiar):
 *  1. Delete the `int book_add(...)` prototype from contact.h and
 *     rebuild. Every file still COMPILES, but the LINK fails:
 *       undefined reference to `book_add'
 *     Headers are the contract; the linker enforces it at link time.
 *  2. Remove the include guard from contact.h and #include it twice
 *     in main.c. Redefinition errors for contact_t. Same thing happens
 *     if you put a function DEFINITION in a header included by two .c
 *     files: "multiple definition" at link time. Definitions live in
 *     .c files; headers only declare.
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
    fflush(stdout);                     /* show the prompt before blocking on input */
    char tmp[256];
    if (fgets(tmp, sizeof tmp, stdin) == NULL) {
        dst[0] = '\0';                  /* EOF: signal "nothing" to the caller */
        return;
    }
    tmp[strcspn(tmp, "\n")] = '\0';     /* strip the trailing newline */
    snprintf(dst, dstsz, "%s", tmp);    /* truncates safely - never overflows */
}

int main(void)
{
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
            break;                      /* EOF: quit cleanly, no crash */

        int choice;
        if (sscanf(line, "%d", &choice) != 1) {
            if (line[0] == 'q')
                break;
            printf("unknown command\n");
            continue;
        }

        switch (choice) {
        case 1:                         /* add */
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

        case 2:                         /* list */
            book_print(&book);
            break;

        case 3:                         /* find */
            ask("name: ", name, sizeof name);
            int idx = book_find(&book, name);
            if (idx >= 0)
                printf("found: %s : %s\n",
                       book.items[idx].name, book.items[idx].phone);
            else
                printf("not found\n");
            break;

        case 4:                         /* remove */
            ask("name: ", name, sizeof name);
            if (book_remove(&book, name) != 0)
                printf("not found\n");
            else
                printf("removed %s\n", name);
            break;

        case 5:                         /* save */
            if (storage_save(&book, DATAFILE) != 0)
                printf("save failed\n");
            else
                printf("saved %zu contacts to %s\n", book.count, DATAFILE);
            break;

        case 6:                         /* load */
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
