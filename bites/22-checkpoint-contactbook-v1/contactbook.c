/*
 * Bite 22 (CHECKPOINT) - Contact Book v1
 * =======================================
 * Compile & run (from the repo root inside WSL):
 *
 *     gcc -Wall -Wextra -Werror -std=c17 -g bites/22-checkpoint-contactbook-v1/contactbook.c -o /tmp/b22 && /tmp/b22
 *
 * Everything through bite 21 in one program: fixed array of 100
 * contacts, menu loop, add / list / find / delete, and input handling
 * that survives any abuse you can type at it.
 *
 * Smoke test (pipe a session):
 *     printf '2\n4\n1\ny\n2\n1\nZoe\n555-9999\n3\nAda\n5\n' | /tmp/b22
 *
 * BREAK IT ON PURPOSE -- the abuse list. Every item must produce a
 * graceful message, never a crash. Run the pre-written abuse session:
 *
 *     printf 'abc\n1\n\n555\n1\n0123456789012345678901234567890123456789\n555\n1\nBob\nphone-with-letters\n3\n\n3\nzzz\n4\n99\n4\nabc\n2\n5\n' | /tmp/b22
 *     printf '4\n3\n\n5\n' | /tmp/b22 empty        # empty-book deletion + empty search
 *
 *   Abuse covered: garbage menu choice, empty name, 40-char name
 *   (truncated with a note), phone with letters (rejected), empty
 *   search, search miss, delete with an out-of-range number, delete
 *   with non-numeric input, delete from an empty book, and EOF at a
 *   prompt (just pipe without the trailing '5' and see it exit cleanly).
 *   Extra credit: paste a 300-character line at any prompt -- the
 *   leftover input is drained, the line rejected, the next prompt
 *   still works.
 *
 * Design rules honored (the checkpoint's real point):
 *   - main never touches .name/.phone: ALL mutation goes through the
 *     book_* API functions.
 *   - every read-only parameter is const (bite 21).
 *   - every string write goes through snprintf (bite 16).
 *   - string helpers are hand-rolled (bite 15); there is no
 *     <string.h> include in this file on purpose.
 *   - input is fgets + hand-rolled trim + strtol (bites 9 and 16),
 *     with stdin drained when a line overflows the buffer.
 */

#include <stdio.h>
#include <stdlib.h>   /* strtol */
#include <errno.h>
#include <limits.h>   /* INT_MAX, INT_MIN */

#define CAP       100
#define NAME_LEN  32
#define PHONE_LEN 16

typedef struct {
    char name[NAME_LEN];
    char phone[PHONE_LEN];
} contact_t;

/* ---------------- hand-rolled string layer (bite 15) ---------------- */

static size_t my_strlen(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0')
        n++;
    return n;
}

static int my_streq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;    /* both reached '\0' together -> equal */
}

/* does hay contain needle anywhere? (empty needle matches everything,
 * but callers reject empty queries before asking) */
static int contains(const char *hay, const char *needle)
{
    if (needle[0] == '\0')
        return 1;
    for (size_t i = 0; hay[i] != '\0'; i++) {
        size_t j = 0;
        while (needle[j] != '\0' && hay[i + j] == needle[j])
            j++;
        if (needle[j] == '\0')
            return 1;
    }
    return 0;
}

/* strip trailing \n / \r from a fgets buffer */
static void chomp(char *s)
{
    size_t n = my_strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = '\0';
}

/* ---------------- input layer: the bite 9 safe pattern -------------- */

/*
 * Reads one line into buf. Returns 1 on success, 0 on EOF/error.
 * The caller decides what EOF means (here: quit gracefully).
 */
static int read_line(const char *prompt, char *buf, size_t sz)
{
    printf("%s", prompt);
    fflush(stdout);     /* show the prompt before blocking on input */

    if (fgets(buf, sz, stdin) == NULL)
        return 0;

    /* If the line filled the buffer without a newline, the rest of the
     * line is still queued in stdin and would poison the NEXT prompt.
     * Detect that, then drain. This is what "crash-proof" actually
     * looks like: boring defensive code. */
    size_t len = my_strlen(buf);
    int line_overflowed = (len == sz - 1 && buf[len - 1] != '\n');

    chomp(buf);

    if (line_overflowed) {
        int c;
        while ((c = getchar()) != '\n' && c != EOF)
            ;           /* discard the rest of the overlong line */
    }
    return 1;
}

/* Strict integer parse: strtol (bite 16) + the WHOLE line must be a
 * number + range check. Returns 0 on success, -1 on anything else. */
static int parse_int(const char *s, int *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno == ERANGE)
        return -1;
    if (v < INT_MIN || v > INT_MAX)
        return -1;
    *out = (int)v;
    return 0;
}

/* ---------------- the book API: all mutation lives here -------------- */

/* phone: non-empty, digits/spaces/+/-/( ) only. Letters = rejected. */
static int valid_phone(const char *s)
{
    if (s[0] == '\0')
        return 0;
    for (size_t i = 0; s[i] != '\0'; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || c == ' ' ||
              c == '+' || c == '-' || c == '(' || c == ')'))
            return 0;
    }
    return 1;
}

/*
 * Returns 0 = added, -1 = book full, -2 = empty name, -3 = bad phone.
 * The caller maps codes to messages; the API maps them to nothing.
 * (Yes, this function prints the confirmation -- a real library would
 * return the stored contact instead. Kept here so main never needs to
 * read .name directly, which is this checkpoint's rule.)
 */
static int book_add(contact_t book[], size_t *count, size_t cap,
                    const char *name, const char *phone)
{
    if (*count == cap)
        return -1;
    if (name[0] == '\0')
        return -2;
    if (!valid_phone(phone))
        return -3;

    /* Warn about truncation up front (bite 16: snprintf reports what it
     * wanted -- but checking the source length is clearer here). */
    if (my_strlen(name) >= NAME_LEN)
        printf("  note: name truncated to %d characters\n", NAME_LEN - 1);
    if (my_strlen(phone) >= PHONE_LEN)
        printf("  note: phone truncated to %d characters\n", PHONE_LEN - 1);

    /* snprintf truncates gracefully instead of corrupting memory. */
    snprintf(book[*count].name, sizeof book[*count].name, "%s", name);
    snprintf(book[*count].phone, sizeof book[*count].phone, "%s", phone);

    printf("  added: \"%s\" <%s>\n", book[*count].name, book[*count].phone);
    (*count)++;         /* update the CALLER's count -- bite 14's out-param */
    return 0;
}

static void book_list(const contact_t book[], size_t count)
{
    if (count == 0) {
        printf("  (the book is empty)\n");
        return;
    }
    for (size_t i = 0; i < count; i++)
        printf("  %2zu. %-31s %-15s\n", i + 1, book[i].name, book[i].phone);
}

/* index of the first exact match, or -1 */
static int book_find_exact(const contact_t book[], size_t count, const char *name)
{
    for (size_t i = 0; i < count; i++)
        if (my_streq(book[i].name, name))
            return (int)i;
    return -1;
}

/* print every contact whose name CONTAINS needle; return how many */
static size_t book_search_sub(const contact_t book[], size_t count,
                              const char *needle)
{
    size_t hits = 0;
    for (size_t i = 0; i < count; i++) {
        if (contains(book[i].name, needle)) {
            printf("  %2zu. %-31s %-15s\n", i + 1, book[i].name, book[i].phone);
            hits++;
        }
    }
    return hits;
}

/*
 * Delete by index using the swap-with-last trick: move the LAST contact
 * into the hole, shrink the count. O(1), no shifting -- the price is
 * that order is not preserved. Struct assignment copies every field,
 * arrays included (bite 20).
 */
static void book_remove_at(contact_t book[], size_t *count, size_t index)
{
    book[index] = book[*count - 1];
    (*count)--;
}

/* ---------------- menu actions (thin shells around the API) ---------- */

static void do_add(contact_t book[], size_t *count)
{
    /* Roomy input buffers: overlong input must be HANDLED, not overflowed. */
    char name[256], phone[256];

    if (!read_line("  name> ", name, sizeof name) ||
        !read_line("  phone> ", phone, sizeof phone)) {
        printf("  (end of input) -- add cancelled\n");
        return;
    }

    switch (book_add(book, count, CAP, name, phone)) {
    case 0:  break;
    case -1: printf("  book is full (%d contacts) -- not added\n", CAP); break;
    case -2: printf("  name cannot be empty -- not added\n"); break;
    case -3: printf("  phone may contain only digits, spaces and + - ( ) -- not added\n"); break;
    default: printf("  unknown error -- not added\n"); break;
    }
}

static void do_find(const contact_t book[], size_t count)
{
    char query[256];

    if (!read_line("  find> ", query, sizeof query)) {
        printf("  (end of input) -- find cancelled\n");
        return;
    }
    if (query[0] == '\0') {
        printf("  empty search string -- nothing to look for\n");
        return;
    }

    int exact = book_find_exact(book, count, query);
    if (exact >= 0)
        printf("  exact match: #%d\n", exact + 1);

    size_t hits = book_search_sub(book, count, query);
    if (hits == 0 && exact < 0)
        printf("  no contact matches \"%s\"\n", query);
    else if (hits > 0)
        printf("  %zu substring match(es) for \"%s\"\n", hits, query);
}

static void do_delete(contact_t book[], size_t *count)
{
    if (*count == 0) {
        printf("  the book is empty -- nothing to delete\n");
        return;
    }

    book_list(book, *count);

    char line[256];
    if (!read_line("  delete which number? (0 cancels) ", line, sizeof line)) {
        printf("  (end of input) -- delete cancelled\n");
        return;
    }
    int n;
    if (parse_int(line, &n) != 0) {
        printf("  \"%s\" is not a number -- cancelled\n", line);
        return;
    }
    if (n == 0) {
        printf("  cancelled\n");
        return;
    }
    if (n < 1 || (size_t)n > *count) {
        printf("  no contact #%d (valid: 1-%zu) -- cancelled\n", n, *count);
        return;
    }

    if (!read_line("  really delete? (y/n) ", line, sizeof line)) {
        printf("  (end of input) -- delete cancelled\n");
        return;
    }
    if (line[0] == 'y' || line[0] == 'Y') {
        book_remove_at(book, count, (size_t)(n - 1));
        printf("  deleted. %zu contact(s) left.\n", *count);
    } else {
        printf("  cancelled\n");
    }
}

int main(int argc, char **argv)
{
    contact_t book[CAP];    /* fixed capacity: no malloc in this phase */
    size_t count = 0;
    char line[256];
    int running = 1;

    /* bite 18 paying off: run as `b22 empty` to start with no seeds, so
     * "delete from empty book" can be exercised. */
    int seed = !(argc > 1 && my_streq(argv[1], "empty"));

    if (seed) {
        if (book_add(book, &count, CAP, "Ada Lovelace", "555-0101") != 0 ||
            book_add(book, &count, CAP, "Grace Hopper", "555-0102") != 0 ||
            book_add(book, &count, CAP, "Linus Torvalds", "555-0103") != 0) {
            printf("seeding failed?! aborting\n");
            return 1;
        }
    }

    printf("\nContact Book v1 -- %zu/%zu contacts.\n"
           "Type whatever you like at every prompt; the program must survive it.\n",
           count, (size_t)CAP);

    do {
        printf("\n=== menu ===  1) add  2) list  3) find  4) delete  5) quit\n");
        if (!read_line("choice> ", line, sizeof line)) {
            printf("\n(end of input) -- goodbye\n");
            break;
        }

        int choice;
        if (parse_int(line, &choice) != 0) {
            printf("  \"%s\" is not a number -- pick 1 to 5\n", line);
            continue;
        }

        switch (choice) {
        case 1: do_add(book, &count);    break;
        case 2: book_list(book, count);  break;
        case 3: do_find(book, count);    break;
        case 4: do_delete(book, &count); break;
        case 5: running = 0;             break;
        default:
            printf("  no menu item %d -- pick 1 to 5\n", choice);
            break;
        }
    } while (running);

    printf("bye. %zu contact(s) were in the book.\n", count);
    return 0;
}
