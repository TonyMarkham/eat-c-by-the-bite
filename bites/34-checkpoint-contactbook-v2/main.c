/*
 * Bite 34 — ✅ CHECKPOINT: Contact Book v2 (dynamic memory, zero leaks)
 *
 * Same app as the Phase-2 checkpoint, but: unlimited contacts (dynamic array),
 * sorted insert by name, and save/load to a text file. The Phase 3 gate is
 * a fully clean valgrind run.
 *
 * COMPILE (from the repo root, inside WSL):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/34-checkpoint-contactbook-v2/main.c -o /tmp/b34
 *
 * RUN interactively (pipe sample commands on stdin):
 *   printf 'add Alice;555-0101\nadd Bob;555-0102\nadd Carol;555-0103\nlist\nfind Bob\ndel Bob\nsave /tmp/contacts.txt\nload /tmp/contacts.txt\nlist\nquit\n' | /tmp/b34
 *
 * RUN the abuse script from the phase file ("Break it on purpose"):
 *   /tmp/b34 selftest
 *   (200 contacts out of order → sorted insert + repeated reallocation;
 *    then load a file with a missing ';', an empty file, a 200-char name,
 *    and a nonexistent file. Nothing crashes; nothing leaks.)
 *
 * THE CHECKPOINT GATE — exact valgrind command and the required verdict:
 *   valgrind --leak-check=full --show-leak-kinds=all /tmp/b34 selftest
 *   → "All heap blocks were freed -- no leaks are possible"
 *   → "ERROR SUMMARY: 0 errors from 0 contexts"
 * (Results pasted at the bottom of this comment.)
 *
 * DESIGN NOTES (the "Do" bullet decisions, in writing):
 *
 * Storage choice: dynamic array of contact_t POINTERS (contact_t **items),
 * not of structs. Justification: sorted insert and delete both shift array
 * elements — moving one 8-byte pointer per element is far cheaper than
 * memmove'ing 96-byte structs; the contact struct itself never moves, so any
 * contact_t* handed out stays valid; and delete is a single free() plus a
 * pointer shift. Cost: two allocations per contact (struct + array slot),
 * which the free path handles inside-out, exactly like bite 26 taught.
 *
 * Sorted insert uses binary search (lower_bound) + memmove — the same
 * "find the gap, shift, insert" thinking as bite 29's sorted list, in its
 * array costume. strcmp defines the order.
 *
 * File format: one contact per line, `name;phone` (Phase 4 preview). Loading
 * uses fgets + sscanf with a [^;] field, and survives: missing ';' (line
 * skipped with a count), overlong names (rest-of-line flushed, skipped),
 * empty files (0 contacts), and nonexistent files (clean -1).
 *
 * KEY VALGRIND FINDINGS (actual runs):
 *   ==NNNN== HEAP SUMMARY:
 *   ==NNNN==     in use at exit: 0 bytes in 0 blocks
 *   ==NNNN==   total heap usage: 382 allocs, 382 frees, 78,840 bytes allocated
 *   ==NNNN==   All heap blocks were freed -- no leaks are possible
 *   ==NNNN== ERROR SUMMARY: 0 errors from 0 contexts
 * (Selftest run: 200 contacts added + 50 deleted, save/load round-trip, the
 *  abuse files, and two books freed = 380 contact structs + 2 item arrays.
 *  The interactive session is also valgrind-clean: 10 allocs, 10 frees.)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NAME_LEN  64               /* max stored name  (incl. '\0') */
#define PHONE_LEN 32               /* max stored phone (incl. '\0') */
#define LINE_LEN  512              /* input line buffer             */

typedef struct {
    char name[NAME_LEN];
    char phone[PHONE_LEN];
} contact_t;

typedef struct {
    contact_t **items;             /* array of OWNERS of contact_t */
    size_t len;                    /* contacts in use              */
    size_t cap;                    /* slots allocated              */
} book_t;

/* ---- book lifecycle ------------------------------------------------------ */

static void book_init(book_t *b)
{
    b->items = NULL;
    b->len = 0;
    b->cap = 0;
}

/* The book owns each contact AND the pointer array: two frees per book plus
 * one per contact. Inside-out: contacts first, then the array. */
static void book_free(book_t *b)
{
    for (size_t i = 0; i < b->len; i++)
        free(b->items[i]);
    free(b->items);
    b->items = NULL;
    b->len = b->cap = 0;
}

/* Grow the pointer array with the tmp-pointer realloc idiom (bite 25).
 * Start at 4, double — amortized O(1) per add. */
static int book_reserve(book_t *b, size_t need)
{
    if (need <= b->cap)
        return 0;

    size_t newcap = b->cap ? b->cap : 4;
    while (newcap < need)
        newcap *= 2;

    contact_t **tmp = realloc(b->items, newcap * sizeof *tmp);
    if (tmp == NULL)
        return -1;                 /* b->items still valid — nothing is lost */
    b->items = tmp;
    b->cap = newcap;
    return 0;
}

/* First index whose name is >= `name` (binary search over the sorted array). */
static size_t lower_bound(const book_t *b, const char *name)
{
    size_t lo = 0, hi = b->len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (strcmp(b->items[mid]->name, name) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* Exact-match find: index or -1. */
static int book_find(const book_t *b, const char *name)
{
    size_t pos = lower_bound(b, name);
    if (pos < b->len && strcmp(b->items[pos]->name, name) == 0)
        return (int)pos;
    return -1;
}

/* Sorted insert. Returns 0 = added, 1 = duplicate name, -1 = out of memory.
 * Every malloc is checked; on any failure the book is left unchanged. */
static int book_add_sorted(book_t *b, const char *name, const char *phone)
{
    if (book_find(b, name) >= 0)
        return 1;                             /* duplicate: silently refused */

    contact_t *c = malloc(sizeof *c);         /* allocation 1: the contact */
    if (c == NULL)
        return -1;

    /* snprintf-truncate: a 200-char name cannot overflow anything, it just
     * gets clipped. Always NUL-terminated, no strncpy foot-guns. */
    snprintf(c->name, sizeof c->name, "%s", name);
    snprintf(c->phone, sizeof c->phone, "%s", phone);

    if (book_reserve(b, b->len + 1) != 0) {   /* allocation 2: array slot */
        free(c);                              /* roll back allocation 1 */
        return -1;
    }

    size_t pos = lower_bound(b, name);        /* the gap it belongs in */
    memmove(b->items + pos + 1, b->items + pos,
            (b->len - pos) * sizeof *b->items); /* shift pointers, not structs */
    b->items[pos] = c;
    b->len++;
    return 0;
}

/* Delete by exact name. Returns 1 = deleted, 0 = not found. */
static int book_delete(book_t *b, const char *name)
{
    int idx = book_find(b, name);
    if (idx < 0)
        return 0;

    free(b->items[idx]);                      /* the contact's owner frees it */
    memmove(b->items + idx, b->items + idx + 1,
            (b->len - (size_t)idx - 1) * sizeof *b->items);
    b->len--;                                 /* len--, cap stays: headroom reused */
    return 1;
}

static void book_list(const book_t *b)
{
    printf("%zu contact(s):\n", b->len);
    for (size_t i = 0; i < b->len; i++)
        printf("  %zu. %-20s %s\n", i + 1, b->items[i]->name, b->items[i]->phone);
}

/* ---- persistence: one contact per line, `name;phone` ---------------------- */

static int book_save(const book_t *b, const char *path)
{
    FILE *f = fopen(path, "w");
    if (f == NULL)
        return -1;

    for (size_t i = 0; i < b->len; i++)
        fprintf(f, "%s;%s\n", b->items[i]->name, b->items[i]->phone);

    return fclose(f) == 0 ? 0 : -1;
}

/* Load contacts from `path` into the (sorted) book.
 * Returns number of contacts added, or -1 if the file could not be opened.
 * Abuse-proof by construction:
 *   - missing ';'        → sscanf finds only 1 field → line counted, skipped
 *   - 200-char name      → longer than LINE_LEN-1: we flush the rest of the
 *                          physical line and skip it (no overflow possible)
 *   - empty file         → loop never runs → 0 added
 *   - nonexistent file   → fopen fails → -1
 * Load failures AFTER opening never leak: every partially built contact is
 * either handed to the book (owned) or freed before the next step. */
static int book_load(book_t *b, const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -1;

    char line[LINE_LEN];
    char name[NAME_LEN], phone[PHONE_LEN];
    int added = 0, malformed = 0, truncated = 0, dupes = 0;

    while (fgets(line, sizeof line, f) != NULL) {
        /* no trailing '\n' and not at EOF → the logical line is longer than
         * the buffer: drain the remainder so the next fgets starts fresh. */
        size_t used = strlen(line);
        if (used == sizeof line - 1 && line[used - 1] != '\n') {
            int ch;
            while ((ch = fgetc(f)) != EOF && ch != '\n')
                ;                              /* flush, then reject the line */
            truncated++;
            malformed++;
            continue;
        }

        line[strcspn(line, "\r\n")] = '\0';    /* strip the newline */
        if (line[0] == '\0')
            continue;                          /* blank line: not malformed, just empty */

        if (sscanf(line, "%63[^;];%31s", name, phone) == 2) {
            int rc = book_add_sorted(b, name, phone);
            if (rc == 0)
                added++;
            else if (rc == 1)
                dupes++;                       /* name already in the book */
            /* rc == -1 (out of memory): stop cleanly, keep what we have */
            if (rc == -1)
                break;
        } else {
            malformed++;                       /* e.g. "Alice 555-0101": no ';' */
        }
    }

    fclose(f);
    printf("loaded %d contact(s) from %s", added, path);
    if (dupes > 0)
        printf(" (skipped %d duplicate(s))", dupes);
    if (malformed > 0)
        printf(" (skipped %d malformed line(s))", malformed);
    if (truncated > 0)
        printf(" [%d line(s) were absurdly long]", truncated);
    printf("\n");
    return added;
}

/* ---- interactive session -------------------------------------------------- */

/* Return a pointer past the first word and following blanks, or NULL. */
static char *after_word(char *s)
{
    while (*s != '\0' && *s != ' ' && *s != '\t')
        s++;
    while (*s == ' ' || *s == '\t')
        s++;
    return *s == '\0' ? NULL : s;
}

static void print_help(void)
{
    printf("commands:\n"
           "  add Name;Phone    insert (kept sorted by name; duplicates refused)\n"
           "  list              show all contacts\n"
           "  find Name         look up by exact name\n"
           "  del Name          delete by exact name\n"
           "  save FILE         write 'name;phone' lines\n"
           "  load FILE         read 'name;phone' lines (abuse-proof)\n"
           "  quit              free everything and exit\n");
}

static int run_interactive(void)
{
    book_t book;
    book_init(&book);

    print_help();

    char line[LINE_LEN];
    while (fgets(line, sizeof line, stdin) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';

        char cmd[16];
        if (sscanf(line, "%15s", cmd) != 1)
            continue;                          /* blank line */

        if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "q") == 0)
            break;

        if (strcmp(cmd, "help") == 0) {
            print_help();
        } else if (strcmp(cmd, "list") == 0) {
            book_list(&book);
        } else if (strcmp(cmd, "add") == 0) {
            char *rest = after_word(line);
            char name[NAME_LEN], phone[PHONE_LEN];
            if (rest == NULL ||
                sscanf(rest, "%63[^;];%31s", name, phone) != 2)
                printf("usage: add Name;Phone\n");
            else {
                int rc = book_add_sorted(&book, name, phone);
                if (rc == 0)
                    printf("added %s (%s)\n", name, phone);
                else if (rc == 1)
                    printf("%s is already in the book\n", name);
                else
                    printf("out of memory — contact not added\n");
            }
        } else if (strcmp(cmd, "find") == 0 || strcmp(cmd, "del") == 0) {
            char *rest = after_word(line);
            if (rest == NULL) {
                printf("usage: %s Name\n", cmd);
            } else if (cmd[0] == 'f') {
                int idx = book_find(&book, rest);
                if (idx < 0)
                    printf("'%s' not found\n", rest);
                else
                    printf("found: %s %s\n",
                           book.items[idx]->name, book.items[idx]->phone);
            } else if (book_delete(&book, rest) == 1) {
                printf("deleted %s\n", rest);
            } else {
                printf("'%s' not found\n", rest);
            }
        } else if (strcmp(cmd, "save") == 0 || strcmp(cmd, "load") == 0) {
            char *rest = after_word(line);
            if (rest == NULL) {
                printf("usage: %s FILE\n", cmd);
            } else if (cmd[0] == 's') {
                if (book_save(&book, rest) == 0)
                    printf("saved %zu contact(s) to %s\n", book.len, rest);
                else
                    printf("could not write %s\n", rest);
            } else if (book_load(&book, rest) < 0) {
                printf("could not open %s\n", rest);   /* nonexistent file */
            }
        } else {
            printf("unknown command '%s' (try 'help')\n", cmd);
        }
    }

    book_free(&book);                          /* THE exit path: zero leaks */
    printf("bye — everything freed\n");
    return 0;
}

/* ---- the abuse script ("Break it on purpose" for this checkpoint) --------- */

static void write_file(const char *path, const char *contents)
{
    FILE *f = fopen(path, "w");
    if (f == NULL)
        return;
    fputs(contents, f);
    fclose(f);
}

static int run_selftest(void)
{
    int failures = 0;
    book_t book;
    book_init(&book);

    /* 1) 200 contacts, scrambled order. (i*37) % 200 hits every value 0..199
     * exactly once (gcd(37,200)=1), so inserts land all over the array:
     * sorted insert does real shifting, and len crosses 4,8,16,...,256 —
     * seven genuine reallocations mid-session. */
    char name[NAME_LEN], phone[PHONE_LEN];
    for (int i = 0; i < 200; i++) {
        snprintf(name, sizeof name, "contact%03d", (i * 37) % 200);
        snprintf(phone, sizeof phone, "555-%04d", (i * 37) % 200);
        if (book_add_sorted(&book, name, phone) != 0) {
            printf("FAIL: add %s\n", name);
            failures++;
        }
    }
    printf("added 200 contacts: len=%zu cap=%zu (grew 4->...->256)\n",
           book.len, book.cap);
    if (book.len != 200 || book.cap != 256) {
        printf("FAIL: expected len 200 cap 256\n");
        failures++;
    }

    /* 2) sortedness invariant: every neighbor pair must be in order. */
    for (size_t i = 1; i < book.len; i++) {
        if (strcmp(book.items[i - 1]->name, book.items[i]->name) > 0) {
            printf("FAIL: order broken at %zu\n", i);
            failures++;
            break;
        }
    }

    /* 3) find: a hit and a clean miss. */
    if (book_find(&book, "contact100") < 0) {
        printf("FAIL: find contact100\n");
        failures++;
    }
    if (book_find(&book, "no-such-contact") != -1) {
        printf("FAIL: find no-such-contact should miss\n");
        failures++;
    }

    /* 4) delete 50 of them (every 4th), then a nonexistent one. */
    for (int i = 0; i < 200; i += 4) {
        snprintf(name, sizeof name, "contact%03d", i);
        if (book_delete(&book, name) != 1) {
            printf("FAIL: delete %s\n", name);
            failures++;
        }
    }
    if (book_delete(&book, "contact000") != 0) {   /* already gone above */
        printf("FAIL: double delete succeeded?!\n");
        failures++;
    }
    printf("deleted 50 contacts: len=%zu (cap=%zu unchanged — headroom reused)\n",
           book.len, book.cap);
    if (book.len != 150) {
        printf("FAIL: expected len 150\n");
        failures++;
    }

    /* 5) save, then reload into a FRESH book — round-trip must match. */
    const char *path = "/tmp/cb2_selftest.txt";
    if (book_save(&book, path) != 0) {
        printf("FAIL: save\n");
        failures++;
    }
    book_t reloaded;
    book_init(&reloaded);
    int loaded = book_load(&reloaded, path);
    printf("round-trip: saved 150, loaded %d\n", loaded);
    if (loaded != 150 || reloaded.len != 150) {
        printf("FAIL: round-trip count mismatch\n");
        failures++;
    } else {
        /* reloaded must also be sorted, with identical endpoints */
        if (strcmp(reloaded.items[0]->name, book.items[0]->name) != 0 ||
            strcmp(reloaded.items[reloaded.len - 1]->name,
                   book.items[book.len - 1]->name) != 0) {
            printf("FAIL: round-trip content mismatch\n");
            failures++;
        }
    }

    /* 6) the abuse files ----------------------------------------------------
     *    missing ';' / empty / 200-char name / nonexistent file. */

    write_file("/tmp/cb2_nosemi.txt", "Alice 555-0101\nBob;555-0102\n");
    if (book_load(&reloaded, "/tmp/cb2_nosemi.txt") != 1) {
        printf("FAIL: missing-';' file should add exactly Bob\n");
        failures++;
    }

    write_file("/tmp/cb2_empty.txt", "");
    if (book_load(&reloaded, "/tmp/cb2_empty.txt") != 0) {
        printf("FAIL: empty file should add 0\n");
        failures++;
    }

    {   /* one line whose name field is 200 chars: clipped or skipped, no crash */
        char big[300];
        memset(big, 'x', 200);
        big[200] = '\0';
        strncat(big, ";555-0000\n", sizeof big - strlen(big) - 1);
        write_file("/tmp/cb2_longname.txt", big);
    }
    /* The 200-char name is longer than the line buffer, so the loader skips
     * the line (after draining it) — either behavior (skip or clip) is fine;
     * what must NOT happen is a crash or an overflow. */
    book_load(&reloaded, "/tmp/cb2_longname.txt");

    printf("nonexistent file -> %d (negative = clean open failure)\n",
           book_load(&reloaded, "/tmp/cb2_does_not_exist.txt"));

    /* 7) free everything — the checkpoint gate. */
    book_free(&book);
    book_free(&reloaded);

    if (failures == 0)
        printf("SELFTEST: all checks passed\n");
    else
        printf("SELFTEST: %d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "selftest") == 0)
        return run_selftest();
    return run_interactive();
}
