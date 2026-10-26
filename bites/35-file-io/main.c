/*
 * Bite 35 — File I/O: text files, read and written defensively.
 *
 * BUILD:  gcc -Wall -Wextra -Werror -std=c17 -g -o prog main.c
 * RUN:    ./prog                (full demo: round-trip + 4 failure cases)
 *         ./prog crash-demo     (the deliberate "forgot fclose" experiment)
 *
 * Learn-bullets this file demonstrates:
 *  - fopen modes: "r" read (fails if the file is missing), "w" write
 *    (TRUNCATES an existing file to zero!), "a" append. fopen returns
 *    NULL on failure - always check it, and report *why* with perror
 *    (perror reads the errno that fopen set).
 *  - fgets on a file is the exact same discipline as fgets on stdin:
 *    fixed buffer, NULL means EOF/error, and beware lines longer than
 *    the buffer (no '\n' at the end means we only got a prefix).
 *  - Every fopen is paired with exactly one fclose on EVERY exit path,
 *    including error paths. That is why real C code grows "goto
 *    cleanup" patterns: many error exits, one place that releases
 *    resources. See save_book() for the seed of that pattern.
 *
 * File format: one contact per line,  name;phone\n
 * Parsed with sscanf(line, "%31[^;];%15s", name, phone):
 *   %31[^;]  scanset: read up to 31 chars that are NOT ';' (the name)
 *   ;        a literal semicolon separator
 *   %15s     up to 15 non-whitespace chars (the phone)
 * sscanf returns how many fields it filled: 2 = good line, else skip it.
 *
 * BREAK IT ON PURPOSE: run `./prog crash-demo`. It opens a file with
 * "w", fprintf()s one line, then "forgets" fclose and dies via _Exit()
 * - which is exactly what a crash or kill -9 does: no cleanup, no
 * flush. Now look at abandoned.txt: the file EXISTS but is EMPTY (or
 * missing). Why? stdio buffers writes in your process's memory; they
 * only reach the disk at fflush()/fclose(). If the process dies first,
 * the buffer dies with it. Fix: add fclose(f) before _Exit and run
 * again - the line appears. Buffered I/O is a promise, not a fact.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_BUF     256
#define MAX_CONTACTS 100

typedef struct {
    char name[32];
    char phone[16];
} contact;

/* ------------------------------------------------------------------ */
/* Writing                                                             */
/* ------------------------------------------------------------------ */

/* Save n contacts, one "name;phone\n" line each.
 * Returns 0 on success, -1 on failure. */
static int save_book(const char *path, const contact *items, size_t n)
{
    FILE *f = fopen(path, "w");          /* "w" truncates anything there! */
    if (f == NULL) {
        perror(path);                    /* e.g. "contacts.txt: Permission denied" */
        return -1;
    }

    int failed = 0;
    for (size_t i = 0; i < n && !failed; i++) {
        /* fprintf returns the char count, negative on error. Check it:
         * a full disk mid-loop is not hypothetical. */
        if (fprintf(f, "%s;%s\n", items[i].name, items[i].phone) < 0) {
            perror(path);
            failed = 1;
        }
    }

    /* ONE fclose, reached from both the success and the error path.
     * With more resources (2nd file, mallocs...) this grows into the
     * classic `goto cleanup` pattern: every error jumps to one label. */
    if (fclose(f) != 0) {                /* fclose flushes - it can fail too */
        perror(path);
        failed = 1;
    }
    return failed ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* Reading                                                             */
/* ------------------------------------------------------------------ */

/* Load contacts from path into out (capacity max).
 * The four failure cases this bite demands, all handled without crashing:
 *   missing file            -> perror + return -1
 *   empty file              -> 0 contacts loaded (not an error)
 *   malformed line (no ';') -> warn + skip, keep going
 *   truncated last line     -> fgets hands us the prefix; warn + skip
 * Returns the number of contacts loaded, or -1 if the file could not
 * be opened. */
static long load_book(const char *path, contact *out, size_t max)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        perror(path);    /* ENOENT vs EACCES vs EISDIR: perror tells you */
        return -1;
    }

    char line[LINE_BUF];
    long count = 0;

    while (fgets(line, sizeof line, f) != NULL) {
        size_t len = strlen(line);
        int at_eof = feof(f) != 0;

        /* A line longer than LINE_BUF arrives in pieces: fgets stopped
         * at the buffer size and there is no '\n' at the end yet. Drain
         * the rest so its tail is not misparsed as extra bogus records. */
        if (len > 0 && line[len - 1] != '\n' && !at_eof) {
            int ch;
            while ((ch = fgetc(f)) != EOF && ch != '\n')
                ;                        /* drain: discard the rest of the line */
            fprintf(stderr, "%s: warning: line too long, skipped\n", path);
            continue;
        }

        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';        /* strip the trailing newline */

        if (line[0] == '\0')             /* blank line: harmless, skip quietly */
            continue;

        contact c;
        /* The scanset %31[^;] accepts an EMPTY name before ';', so also
         * reject names that came out empty. (It also keeps leading
         * spaces - a stricter format would trim them; noted, not done.) */
        if (sscanf(line, "%31[^;];%15s", c.name, c.phone) != 2
            || c.name[0] == '\0') {
            fprintf(stderr,
                    "%s: warning: malformed line (want \"name;phone\"): \"%s\"\n",
                    path, line);
            continue;                    /* skip + warn, never crash */
        }

        if (count >= (long)max) {
            fprintf(stderr, "%s: warning: book full, ignoring the rest\n", path);
            break;
        }
        out[count++] = c;
    }

    fclose(f);
    return count;
}

/* ------------------------------------------------------------------ */
/* Demo scaffolding                                                    */
/* ------------------------------------------------------------------ */

/* Regenerate the failure-case inputs each run so the demo is
 * self-contained. The checked-in copies in this folder are identical. */
static void write_fixtures(void)
{
    static const char *malformed =
        "Alice;555-0101\n"     /* good line                    */
        "no-semicolon-here\n"  /* malformed: no ';' -> warn    */
        "Bob;555-0102\n";      /* good line                    */
    static const char *truncated =
        "Carol;555-0103\n"
        "Da";                  /* EOF mid-line: no ';' AND no '\n' */

    FILE *f = fopen("empty.txt", "w");           /* 0 bytes */
    if (f) fclose(f);
    f = fopen("malformed.txt", "w");
    if (f) { fputs(malformed, f); fclose(f); }
    f = fopen("truncated.txt", "w");
    if (f) { fputs(truncated, f); fclose(f); }
}

/* The "break it on purpose" experiment, automated. See file header. */
static void crash_demo(void)
{
    FILE *f = fopen("abandoned.txt", "w");
    if (f == NULL) {
        perror("abandoned.txt");
        return;
    }
    fprintf(f, "this line never reaches the disk\n");

    /* _Exit (ISO C, stdlib.h) ends the process IMMEDIATELY: no stdio
     * flush, no atexit handlers. Same observable effect as kill -9.
     * The buffer holding our line is simply lost with the process. */
    _Exit(0);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "crash-demo") == 0)
        crash_demo();                        /* never returns */

    write_fixtures();

    /* ---- the round-trip: save 5, load into a fresh array, compare --- */
    contact saved[5] = {
        { "Alice", "555-0101" },
        { "Bob",   "555-0102" },
        { "Carol", "555-0103" },
        { "Dave",  "555-0104" },
        { "Eve",   "555-0105" },
    };

    if (save_book("contacts_out.txt", saved, 5) != 0)
        return 1;

    contact loaded[MAX_CONTACTS];
    long n = load_book("contacts_out.txt", loaded, MAX_CONTACTS);
    if (n < 0)
        return 1;

    printf("round-trip: loaded %ld contacts\n", n);
    int identical = (n == 5);
    for (long i = 0; i < n && identical; i++)
        if (strcmp(saved[i].name, loaded[i].name) != 0
            || strcmp(saved[i].phone, loaded[i].phone) != 0)
            identical = 0;
    printf("round-trip: %s\n", identical ? "OK - identical output" : "MISMATCH!");

    /* ---- the four failure cases: warn, never crash ------------------ */
    printf("\n-- case 1: missing file --\n");
    load_book("missing.txt", loaded, MAX_CONTACTS);   /* perror explains */

    printf("\n-- case 2: empty file --\n");
    n = load_book("empty.txt", loaded, MAX_CONTACTS);
    printf("loaded %ld contacts from an empty file (fine, not an error)\n", n);

    printf("\n-- case 3: malformed lines --\n");
    n = load_book("malformed.txt", loaded, MAX_CONTACTS);
    printf("loaded %ld good contacts, skipped the bad one\n", n);

    printf("\n-- case 4: truncated last line --\n");
    n = load_book("truncated.txt", loaded, MAX_CONTACTS);
    printf("loaded %ld good contacts, skipped the cut-off one\n", n);

    printf("\nNow try: ./prog crash-demo   (then: ls -l abandoned.txt)\n");
    return identical ? 0 : 1;
}
