/*
 * Bite 44 — ✅ CHECKPOINT: Contact Book v3 — storage module (interface).
 *
 * PERSISTENCE FORMAT (the choice, documented as the checkpoint asks):
 *
 *   Text, one contact per line:   name;phone\n
 *
 *   Example:
 *     Alice;555-0101
 *     Bob;555-0102
 *
 *   Why text (the three-sentence argument): text is diffable,
 *   debuggable with cat/xxd/hexdump, and survives format drift - a
 *   human can patch a broken file by hand. Binary (bite 36) is faster
 *   and smaller but padding/endianness make it machine-dependent, and
 *   corruption is silent. For 100-contact books, debuggability wins.
 *
 * FAILURE CASES (all documented, none crash):
 *   - missing file        -> CB_ERR_IO, errno preserved (ENOENT)
 *   - empty file          -> CB_OK, 0 contacts (not an error)
 *   - malformed line      -> skipped, counted in *skipped (not fatal)
 *   - truncated last line -> fgets returns the prefix; same skip rule
 *   - line > 255 bytes    -> drained and skipped as one bad record
 *   - no trailing newline -> last line still parsed
 *
 * KNOWN LIMITATION (documented on purpose): a name containing ';' or
 * a newline cannot round-trip through this format. Fixing that needs
 * escaping - a deliberate exercise, not an oversight. Phone must be
 * one non-whitespace token (scanf %s rule).
 */

#ifndef STORAGE_H
#define STORAGE_H

#include "contact.h"

/* Save the whole book, one "name;phone\n" line per contact.
 * CB_OK, CB_ERR_ARG or CB_ERR_IO (errno preserved). */
cb_result storage_save(const contactbook_t *b, const char *path);

/* Load contacts from path, ADDING to the book.
 * Pass NULL for skipped if you don't care how many lines were bad.
 * CB_OK (with *skipped = count of skipped lines), CB_ERR_ARG or
 * CB_ERR_IO (errno preserved). */
cb_result storage_load(contactbook_t *b, const char *path, size_t *skipped);

#endif /* STORAGE_H */
