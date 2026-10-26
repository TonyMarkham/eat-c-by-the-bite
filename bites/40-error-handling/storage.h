/*
 * Bite 40 — Error handling conventions: storage module (INTERFACE).
 * The project-wide convention is documented in contact.h; the BREAK IT
 * ON PURPOSE experiment (a low-level function that prints its own
 * error) is demonstrated by the DEMO_BAD_ERROR code in storage.c.
 *
 * Failure behavior of these functions (documented = designed):
 *   storage_save  CB_OK, or CB_ERR_IO with errno set by the failing
 *                 fopen/fprintf/fclose (caller renders it).
 *   storage_load  CB_OK, or CB_ERR_IO (errno preserved). Malformed
 *                 lines are NOT errors: they are counted in *skipped
 *                 (if non-NULL) and skipped - a dirty file still loads
 *                 what it can. That is a design decision, written down.
 */

#ifndef STORAGE_H
#define STORAGE_H

#include "contact.h"

/* Format: one contact per line, "name;phone\n" (bites 35/37 format). */
cb_result storage_save(const contactbook_t *b, const char *path);

cb_result storage_load(contactbook_t *b, const char *path, size_t *skipped);

#endif /* STORAGE_H */
