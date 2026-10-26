/*
 * Bite 37 — Multi-file projects: contact module (INTERFACE).
 *
 * BUILD:  gcc -Wall -Wextra -Werror -std=c17 -g -o prog contact.c storage.c main.c
 * RUN:    ./prog
 * (A fuller breakdown of the build commands lives in main.c.)
 *
 * A module = foo.h (interface: types, prototypes, constants)
 *          + foo.c (implementation, private).
 * Consumers include the header; the .c file is invisible to them.
 *
 * Header rules obeyed here:
 *  - include guards ALWAYS. Without them, a header included twice in
 *    one translation unit produces redefinition errors (try it - see
 *    BREAK IT ON PURPOSE in main.c).
 *  - the header includes ONLY what the interface needs (stddef.h for
 *    size_t). Nothing else, no <stdio.h>, no hidden dependencies.
 *  - no global variables in headers: all state lives inside the
 *    contactbook_t struct that the caller owns and passes in.
 *  - internal helpers are NOT declared here. They are `static` in
 *    contact.c - module-private, invisible to other files and to the
 *    linker's global namespace.
 */

#ifndef CONTACT_H
#define CONTACT_H

#include <stddef.h>     /* size_t */

typedef struct {
    char name[32];
    char phone[16];
} contact_t;

/* The book owns a growable heap array (bite 25's doubling pattern).
 * Callers never touch items/cap directly - functions below are the API. */
typedef struct {
    contact_t *items;
    size_t count;
    size_t cap;
} contactbook_t;

void book_init(contactbook_t *b);                       /* start empty   */
void book_free(contactbook_t *b);                       /* release array */
int  book_add(contactbook_t *b, const char *name, const char *phone);
/*    ^ returns 0 ok, -1 out of memory or duplicate name             */
int  book_find(const contactbook_t *b, const char *name);
/*    ^ returns index (>= 0), or -1 if absent                        */
int  book_remove(contactbook_t *b, const char *name);
/*    ^ returns 0 removed, -1 name not found                         */
void book_print(const contactbook_t *b);

#endif /* CONTACT_H */
