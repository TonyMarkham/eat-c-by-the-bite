/*
 * Bite 43 — CMake + tests: hashmap module (interface) - bite 30's
 * open-addressing string->int table, now a real module under test.
 * Build/run commands in CMakeLists.txt's header comment.
 */

#ifndef HASHMAP_H
#define HASHMAP_H

#include <stddef.h>

#define HM_BUCKETS 64

typedef struct hashmap hashmap;

hashmap *hm_create(void);
/* Insert or overwrite key->value. The key is COPIED (the table owns
 * its own copy; caller may free/alter its original afterwards).
 * Returns 0 ok, -1 OOM. */
int hm_set(hashmap *h, const char *key, int value);
/* Returns a pointer to the value, or NULL if absent. The pointer is
 * only valid until the next hm_set (rehashing/probing may move it). */
int *hm_get(hashmap *h, const char *key);
size_t hm_count(const hashmap *h);
void hm_destroy(hashmap *h);            /* frees keys + table */

#endif /* HASHMAP_H */
