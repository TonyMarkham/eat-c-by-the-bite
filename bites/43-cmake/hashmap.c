/*
 * Bite 43 — CMake + tests: hashmap implementation.
 * Bite 30's design: HM_BUCKETS fixed buckets, FNV-1a hash, open
 * addressing with linear probing. No chaining (yet - that was the
 * bite's scope too).
 *
 * Tombstones: a removed slot can't simply be marked empty (it would
 * break probe chains), but this version has no delete - bite 30
 * didn't either, and the tests don't exercise it. Documented gap.
 */
#include "hashmap.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct hashmap {
    struct {
        char *key;
        int value;
        int used;
    } slot[HM_BUCKETS];
    size_t count;
};

/* FNV-1a: xor one byte, multiply by the prime. Tiny, fast, good
 * enough spread for a teaching table. */
static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s != '\0') {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

/* Private copy of the key: hand-rolled strdup (bite 27), so the
 * module stays ISO-C and owns its memory explicitly. */
static char *own_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

hashmap *hm_create(void)
{
    return calloc(1, sizeof(hashmap));   /* zeroed: all slots unused */
}

int hm_set(hashmap *h, const char *key, int value)
{
    if (h == NULL || key == NULL)
        return -1;

    /* Probe from hash(key) onward for the key (overwrite) or a free
     * slot (insert). The table never grows: fixed 64 buckets, and
     * hm_set fails if the caller fills them all. */
    size_t i = fnv1a(key) % HM_BUCKETS;
    for (size_t probe = 0; probe < HM_BUCKETS; probe++) {
        size_t idx = (i + probe) % HM_BUCKETS;
        if (!h->slot[idx].used) {
            char *copy = own_strdup(key);
            if (copy == NULL)
                return -1;
            h->slot[idx].key = copy;
            h->slot[idx].value = value;
            h->slot[idx].used = 1;
            h->count++;
            return 0;
        }
        if (strcmp(h->slot[idx].key, key) == 0) {
            h->slot[idx].value = value;      /* overwrite, key kept */
            return 0;
        }
    }
    return -1;                               /* table full */
}

int *hm_get(hashmap *h, const char *key)
{
    if (h == NULL || key == NULL)
        return NULL;
    size_t i = fnv1a(key) % HM_BUCKETS;
    for (size_t probe = 0; probe < HM_BUCKETS; probe++) {
        size_t idx = (i + probe) % HM_BUCKETS;
        if (!h->slot[idx].used)
            return NULL;                     /* hit a gap: key can't exist */
        if (strcmp(h->slot[idx].key, key) == 0)
            return &h->slot[idx].value;
    }
    return NULL;
}

size_t hm_count(const hashmap *h)
{
    return h ? h->count : 0;
}

void hm_destroy(hashmap *h)
{
    if (h == NULL)
        return;
    for (size_t i = 0; i < HM_BUCKETS; i++)
        free(h->slot[i].key);                /* free(NULL) is fine */
    free(h);
}
