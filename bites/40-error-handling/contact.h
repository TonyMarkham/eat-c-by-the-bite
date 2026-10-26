/*
 * Bite 40 — Error handling conventions: the contact module, documented.
 *
 * BUILD:  gcc -Wall -Wextra -Werror -std=c17 -g -o prog \
 *             log.c contact.c storage.c main.c
 * RUN:    ./prog
 * (Full story + BREAK IT ON PURPOSE at the top of this header.)
 *
 * =====================================================================
 * ERROR-HANDLING CONVENTION  -  the project's contract, written once,
 * obeyed by every function in every module. This comment block IS the
 * API documentation the bite asked for.
 * =====================================================================
 *
 * 1. RETURN CODES. Every function that can fail returns int:
 *       0        success
 *       < 0      failure (a cb_err code from cb_result below)
 *    Plain status failures use the enum; nothing returns magic
 *    numbers like -42 without a name and a doc line.
 *
 * 2. QUERIES are the one exception: a *lookup* cannot say "found" and
 *    "error" through the same channel, so book_find() returns an
 *    index >= 0 on success and CB_ERR_NOTFOUND (-1) otherwise. It is
 *    documented here so the exception is a decision, not an accident.
 *
 * 3. ERRNO. Functions that wrap system calls (fopen, unlink, ...)
 *    leave errno untouched from the failing call. The CALLER decides
 *    whether to render it with strerror(errno) / perror. errno is a
 *    thread-global detail row: read it before anything else clobbers
 *    it, and only when the return code says a syscall actually failed.
 *
 * 4. WHO PRINTS. Low-level layers NEVER print user-facing errors and
 *    NEVER print the same failure twice. They return codes. Only the
 *    top of the stack (main / UI) decides what the human sees, via
 *    log_error()/log_info() from log.h. A failure is reported exactly
 *    once, by whoever can actually act on it.
 *
 * 5. ALL-OR-NOTHING. A function either fails completely (returns < 0,
 *    observable state unchanged) or succeeds completely. No half-done
 *    states for callers to clean up.
 *
 * 6. PROPAGATION. Failures travel UP, codes travel up, strings stay
 *    local. storage_load() does not know if you're a CLI or a server;
 *    it returns CB_ERR_IO and that is all it says.
 */

#ifndef CONTACT_H
#define CONTACT_H

#include <stddef.h>

/* The error vocabulary. Small, named, documented. */
typedef enum {
    CB_OK           =  0,   /* success                                     */
    CB_ERR          = -1,   /* generic failure                             */
    CB_ERR_ARG      = -2,   /* bad argument (NULL, empty string...)        */
    CB_ERR_FULL     = -3,   /* no room left (see BOOK_CAP)                 */
    CB_ERR_DUP      = -4,   /* duplicate name                              */
    CB_ERR_NOTFOUND = -5,   /* lookup found nothing                        */
    CB_ERR_IO       = -6    /* system call failed; errno has the why       */
} cb_result;

/* Human-readable name for a cb_result - for the top level's reports. */
const char *cb_strerror(cb_result r);

#define BOOK_CAP 4      /* deliberately tiny so you can MEET the FULL
                           error live in main.c's demo */

typedef struct {
    char name[32];
    char phone[16];
} contact_t;

typedef struct {
    contact_t items[BOOK_CAP];
    size_t count;
} contactbook_t;

void book_init(contactbook_t *b);

/* Add a contact. Returns CB_OK, CB_ERR_ARG (null input), CB_ERR_FULL
 * (book is at BOOK_CAP) or CB_ERR_DUP (name already present). On any
 * failure the book is unchanged (rule 5). */
cb_result book_add(contactbook_t *b, const char *name, const char *phone);

/* Query, not operation: index >= 0, or CB_ERR_NOTFOUND. (Rule 2.) */
int book_find(const contactbook_t *b, const char *name);

/* Remove by name. Returns CB_OK or CB_ERR_NOTFOUND. */
cb_result book_remove(contactbook_t *b, const char *name);

void book_print(const contactbook_t *b);

#endif /* CONTACT_H */
