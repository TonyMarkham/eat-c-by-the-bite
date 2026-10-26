/*
 * Bite 44 — ✅ CHECKPOINT: Contact Book v3 — contact module (interface).
 *
 * BUILD & RUN (full story in README.md):
 *   cmake -S . -B build && cmake --build build && ./build/contactbook [datafile]
 * TESTS: cmake --build build && ctest --test-dir build
 * BREAK IT ON PURPOSE: see README.md ("Abuse script" section).
 *
 * =====================================================================
 * ERROR-HANDLING CONVENTION (designed in bite 40, obeyed everywhere):
 *   1. Functions that can fail return a cb_result: 0 = success, < 0 =
 *      a named error code (enum below).
 *   2. Queries (book_find) are the exception: index >= 0 or
 *      CB_ERR_NOTFOUND - a lookup can't share one channel for both.
 *   3. Syscall failures set CB_ERR_IO and preserve errno; the CALLER
 *      renders it with strerror(errno) / perror.
 *   4. Low-level modules NEVER print user-facing errors. Only main
 *      reports, via log_error/log_info (util.h). Failure is reported
 *      exactly once, by whoever can act on it.
 *   5. All-or-nothing: on failure, observable state is unchanged.
 * =====================================================================
 */

#ifndef CONTACT_H
#define CONTACT_H

#include <stddef.h>

typedef enum {
    CB_OK           =  0,
    CB_ERR          = -1,   /* generic failure (e.g. out of memory) */
    CB_ERR_ARG      = -2,   /* bad argument: NULL or empty          */
    CB_ERR_DUP      = -3,   /* duplicate name                       */
    CB_ERR_NOTFOUND = -4,   /* lookup found nothing                 */
    CB_ERR_IO       = -5    /* syscall failed; errno has the why    */
} cb_result;

const char *cb_strerror(cb_result r);

typedef struct {
    char name[32];
    char phone[16];
} contact_t;

/* Growable heap array (bite 25's doubling). Callers go through the
 * functions below; items/cap are exposed only so the struct can live
 * on the caller's stack. */
typedef struct {
    contact_t *items;
    size_t count;
    size_t cap;
} contactbook_t;

void book_init(contactbook_t *b);
void book_free(contactbook_t *b);

/* Add. Returns CB_OK, CB_ERR_ARG, CB_ERR_DUP or CB_ERR (OOM). */
cb_result book_add(contactbook_t *b, const char *name, const char *phone);

/* Query: index >= 0, or CB_ERR_NOTFOUND. */
int book_find(const contactbook_t *b, const char *name);

/* Remove by name. CB_OK or CB_ERR_NOTFOUND. */
cb_result book_remove(contactbook_t *b, const char *name);

/* Sort in place by name (qsort + comparator, bite 32). */
void book_sort(contactbook_t *b);

void book_print(const contactbook_t *b);

#endif /* CONTACT_H */
