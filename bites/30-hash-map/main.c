/*
 * Bite 30 — Hash Map: string → int, open addressing, FNV-1a, 64 buckets
 *
 * COMPILE & RUN (from the repo root, inside WSL):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/30-hash-map/main.c -o /tmp/b30 && /tmp/b30
 *   valgrind --leak-check=full --show-leak-kinds=all /tmp/b30   → 0 errors, 0 leaks
 *
 * BREAK IT ON PURPOSE: a hash function that returns 0 for EVERY key — all
 * keys collide into one probe chain. Build it:
 *   gcc -Wall -Wextra -Werror -std=c17 -g -DBAD_HASH bites/30-hash-map/main.c -o /tmp/b30-bad && /tmp/b30-bad
 * It still produces every correct answer — just ~10x more slot inspections
 * (actual runs: 2.55 probes/op with FNV-1a vs 26.01 with BAD_HASH, and it
 * gets worse linearly as the table fills).
 * That is the whole trade-off of hashing in one experiment: correctness comes
 * from the probing discipline; the hash only decides where you start probing.
 *
 * WHY (the "Learn" bullets):
 *   - A hash map = array of buckets + hash function + collision strategy.
 *     Open addressing: if the slot is taken, walk (linearly) to the next
 *     free slot. No chaining, no per-bucket lists — everything lives in one
 *     flat array.
 *   - FNV-1a is a tiny, decent string hash: hash = (hash ^ byte) * 16777619
 *     over the string's bytes. Good enough to spread keys over 64 buckets.
 *   - Load factor matters: at 64 buckets with open addressing, keep entries
 *     below ~50 or probe chains get long. Resizing comes later; fixed size
 *     is fine for this bite.
 *   - The map STRDUPs each key: it owns its copy, so callers may pass
 *     temporary buffers or free their strings right after put.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HB 64                       /* bucket count: fixed on purpose */

typedef struct {
    char *key;                      /* NULL when unused; map OWNS this string */
    int val;
    int used;
} bucket;

typedef struct {
    bucket b[HB];
    size_t count;                   /* how many slots are used */
} hm;

/* Probe statistics, so the BAD_HASH experiment can be *felt*, not just read. */
static long g_probes;      /* total slot inspections */
static long g_ops;         /* total put/get calls   */

/* FNV-1a 32-bit: xor each byte, multiply by a large prime. Two lines of
 * arithmetic that spread "key05" and "key06" into different neighborhoods. */
static unsigned fnv1a(const char *s)
{
#ifdef BAD_HASH
    (void)s;                        /* BUG ON PURPOSE: every key → bucket 0.
                                       One giant probe chain, correct but O(n). */
    return 0;
#else
    unsigned hash = 2166136261u;                    /* FNV offset basis */
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++)
        hash = (hash ^ *p) * 16777619u;             /* xor-then-multiply */
    return hash;
#endif
}

static char *my_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *copy = malloc(n);
    if (copy != NULL)
        memcpy(copy, s, n);
    return copy;
}

/* Insert or overwrite. Returns 0 = inserted, 1 = overwrote existing, -1 = full.
 * Linear probing: start at hash % HB, step +1 (wrapping), stop at the first
 * free slot — or at an equal key, which becomes an overwrite. */
static int hm_put(hm *m, const char *key, int val)
{
    size_t idx = fnv1a(key) % HB;
    g_ops++;

    for (size_t probe = 0; probe < HB; probe++) {
        bucket *slot = &m->b[(idx + probe) % HB];
        g_probes++;

        if (!slot->used) {                       /* empty slot: brand-new key */
            slot->key = my_strdup(key);          /* the map takes ownership */
            if (slot->key == NULL)
                return -1;
            slot->val = val;
            slot->used = 1;
            m->count++;
            return 0;
        }
        if (strcmp(slot->key, key) == 0) {       /* same key: overwrite value */
            slot->val = val;
            return 1;
        }
        /* else: occupied by a different key → collision, keep probing */
    }
    return -1;                                   /* table full, key not found */
}

/* Look up. Returns 0 and stores the value in *out on hit, -1 on miss. */
static int hm_get(const hm *m, const char *key, int *out)
{
    size_t idx = fnv1a(key) % HB;
    g_ops++;

    for (size_t probe = 0; probe < HB; probe++) {
        const bucket *slot = &m->b[(idx + probe) % HB];
        g_probes++;

        if (!slot->used)
            return -1;               /* hit an empty slot: the key can't be
                                        beyond it — that's WHY open addressing
                                        stops here (no delete op in this bite,
                                        so no tombstones to worry about yet) */
        if (strcmp(slot->key, key) == 0) {
            *out = slot->val;
            return 0;
        }
    }
    return -1;                       /* walked the whole table: full, and absent */
}

/* The map owns every key it strdup'd — freeing the table without freeing the
 * keys leaks one block per entry. Ownership rule from bite 26, again. */
static void hm_free(hm *m)
{
    for (int i = 0; i < HB; i++) {
        if (m->b[i].used) {
            free(m->b[i].key);
            m->b[i].key = NULL;
            m->b[i].used = 0;
        }
    }
    m->count = 0;
}

int main(void)
{
    hm map = {0};                    /* zeroed: every bucket starts unused */

    /* ---- 1) 30 keys in -------------------------------------------------- */
    char key[32];
    for (int i = 0; i < 30; i++) {
        snprintf(key, sizeof key, "key%02d", i);
        if (hm_put(&map, key, i * 10) != 0) {
            fprintf(stderr, "put failed\n");
            return 1;
        }
    }
    printf("inserted 30 keys, count=%zu\n", map.count);

    /* ---- 2) overwrite 5 of them, verify the overwrites took -------------- */
    int hits = 0;
    for (int i = 0; i < 30; i += 6) {            /* key00, key06, ... key24: 5 keys */
        snprintf(key, sizeof key, "key%02d", i);
        if (hm_put(&map, key, i * 100) != 1) {
            fprintf(stderr, "expected overwrite on %s\n", key);
            return 1;
        }
    }
    for (int i = 0; i < 30; i += 6) {
        int v;
        snprintf(key, sizeof key, "key%02d", i);
        if (hm_get(&map, key, &v) == 0 && v == i * 100)
            hits++;
    }
    printf("overwrites verified: %d/5 (count still %zu — overwrite, not insert)\n",
           hits, map.count);

    /* ---- 3) misses return -1 --------------------------------------------- */
    int v;
    printf("miss 'nope'      -> %d\n", hm_get(&map, "nope", &v));
    printf("miss 'key99'    -> %d\n", hm_get(&map, "key99", &v));
    printf("hit  'key07'    -> %d, val=%d\n", hm_get(&map, "key07", &v), v);

    /* ---- 4) a full table refuses the 65th key ---------------------------- */
    hm tiny = {0};
    int insert_failures = 0;
    for (int i = 0; i < HB + 1; i++) {           /* 65 puts into 64 buckets */
        snprintf(key, sizeof key, "fill%03d", i);
        if (hm_put(&tiny, key, i) != 0)
            insert_failures++;
    }
    printf("64 buckets took %d keys; %d put(s) correctly failed\n",
           HB, insert_failures);

    /* ---- 5) probe stats + free ------------------------------------------- */
    printf("total probes so far: %ld over %ld ops (%.2f slots inspected per op)\n",
           g_probes, g_ops, (double)g_probes / (double)g_ops);
#ifdef BAD_HASH
    puts("^^ BAD_HASH build: same answers, probes explode (one collision chain).");
#else
    puts("^^ FNV-1a build: probes stay close to 1 per operation.");
#endif

    hm_free(&map);
    hm_free(&tiny);

    /* Load-factor note: 30 entries in 64 buckets ≈ 47% load — inside the
     * "keep it under ~50" rule from the lesson. 64 entries = 100%: the table
     * above only accepted the 65th put as a failure. A real table would
     * resize (rehash into a bigger array) around 50–75%. */
    return 0;
}
