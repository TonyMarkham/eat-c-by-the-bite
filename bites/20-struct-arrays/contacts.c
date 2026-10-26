/*
 * Bite 20 - Arrays of Structs
 * ============================
 * Compile & run (from the repo root inside WSL):
 *
 *     gcc -Wall -Wextra -Werror -std=c17 -g bites/20-struct-arrays/contacts.c -o /tmp/b20 && /tmp/b20
 *
 * Give it one contact on stdin (name, then phone):
 *
 *     printf 'Kevin\n555-4242\n' | /tmp/b20
 *
 * The "Learn" list, translated into code below:
 *   - Arrays of structs are THE workhorse data layout in C: one
 *     contiguous block, cache-friendly, no pointers, no malloc needed
 *     yet. The whole contact book is literally `contact_t book[CAP]`.
 *   - contacts[i].name  (array of structs -- the . goes to the field)
 *     vs contacts[i]->name (array of POINTERS to structs -- you'd need
 *     -> after dereferencing). Know which one you have.
 *   - Struct assignment copies EVERY field, arrays included:
 *     book[i] = book[j] needs no loops.
 *
 * BREAK IT ON PURPOSE:
 *   Add a too-long name with strcpy-thinking, then see the right way:
 *   the -DBREAK_IT=1 build strcpy's a 60-char name into the 32-char
 *   field. 40 chars would silently eat the phone field NEXT DOOR in the
 *   same struct (ASan cannot see inside one object!) -- 60 chars
 *   escapes the struct and ASan screams:
 *
 *     gcc -DBREAK_IT=1 -Wall -Wextra -std=c17 -g \
 *         bites/20-struct-arrays/contacts.c -o /tmp/b20break && /tmp/b20break
 *
 *   (GCC flags the overflow at compile time too -- that's why the break
 *   build drops -Werror: we want the runtime crash, not just the
 *   warning.)
 *
 *   The normal build does the same add with snprintf: graceful
 *   truncation instead of corruption.
 */

#include <stdio.h>
#include <string.h>

enum { CAP = 8, NAME_LEN = 32, PHONE_LEN = 16 };

typedef struct {
    char name[NAME_LEN];
    char phone[PHONE_LEN];
} contact_t;

/* hand-rolled strlen (bite 15) so the length checks below are honest */
static size_t my_strlen(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0')
        n++;
    return n;
}

/*
 * Returns 0 = added, -1 = book full. Note the size_t *count out-param:
 * the function must update the CALLER's count -- bite 14's out-param
 * pattern paying off. All writes go through snprintf (bite 16).
 */
static int contact_add(contact_t book[], size_t *count, size_t cap,
                       const char *name, const char *phone)
{
    if (*count == cap)
        return -1;

    /* Truncation, not corruption: the 32-byte field keeps what fits
     * (31 chars + '\0') and the memory of every other field stays
     * untouched. */
    snprintf(book[*count].name, sizeof book[*count].name, "%s", name);
    snprintf(book[*count].phone, sizeof book[*count].phone, "%s", phone);
    (*count)++;
    return 0;
}

static void contact_show(const contact_t *c)
{
    printf("  %-31s %-15s\n", c->name, c->phone);
}

static void contact_list(const contact_t book[], size_t count)
{
    if (count == 0) {
        printf("  (empty book)\n");
        return;
    }
    for (size_t i = 0; i < count; i++) {
        printf("  %zu.", i + 1);
        contact_show(&book[i]);
    }
}

/* Returns the index of the first exact match, or -1. */
static int contact_find(const contact_t book[], size_t count, const char *name)
{
    for (size_t i = 0; i < count; i++)
        if (strcmp(book[i].name, name) == 0)
            return (int)i;
    return -1;
}

/* strip trailing \n / \r from an fgets buffer (bite 9's helper) */
static void chomp(char *s)
{
    size_t n = my_strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = '\0';
}

int main(void)
{
    contact_t book[CAP];        /* one contiguous block of 8 * 48 bytes */
    size_t count = 0;

    /* Seed three contacts -- through the API, never by poking fields. */
    if (contact_add(book, &count, CAP, "Ada Lovelace", "555-0101") != 0 ||
        contact_add(book, &count, CAP, "Grace Hopper", "555-0102") != 0 ||
        contact_add(book, &count, CAP, "Linus Torvalds", "555-0103") != 0) {
        printf("seed failed?!\n");
        return 1;
    }

    /* Add ONE contact from stdin (bite 9's safe input pattern). */
    char nbuf[128], pbuf[128];
    printf("add a contact:\n");
    printf("  name> ");
    fflush(stdout);
    if (fgets(nbuf, sizeof nbuf, stdin) == NULL)
        nbuf[0] = '\0';
    chomp(nbuf);
    printf("  phone> ");
    fflush(stdout);
    if (fgets(pbuf, sizeof pbuf, stdin) == NULL)
        pbuf[0] = '\0';
    chomp(pbuf);

    if (nbuf[0] == '\0') {
        printf("  (empty name -- not added)\n");
    } else if (contact_add(book, &count, CAP, nbuf, pbuf) != 0) {
        printf("  book is full -- not added\n");
    } else {
        printf("  added.\n");
    }

    /* The 40+-char name, done RIGHT: snprintf truncates gracefully.
     * Compare with the break build, where strcpy wrecks the struct. */
    const char *long_name = "Anaksunamun The Great And Terrible Queen Of The Nile";
    printf("\nadding a %zu-char name into a %d-char field:\n",
           my_strlen(long_name), NAME_LEN);
    if (contact_add(book, &count, CAP, long_name, "555-4321") == 0)
        printf("  stored as: \"%s\" (truncated, everything else intact)\n",
               book[count - 1].name);

    /* List and find -- all through the interfaces. */
    printf("\nthe book (%zu contacts):\n", count);
    contact_list(book, count);

    int idx = contact_find(book, count, "Grace Hopper");
    if (idx >= 0)
        printf("\nfind \"Grace Hopper\": index %d\n", idx);
    idx = contact_find(book, count, "Nobody");
    if (idx < 0)
        printf("find \"Nobody\": -1 (not found is a VALUE, not a crash)\n");

    /* Struct assignment copies EVERY field, arrays included -- no loop. */
    contact_t clone = book[0];
    printf("\ncontact_t clone = book[0] copied: ");
    contact_show(&clone);
    printf("(change a field of clone and book[0] would not notice: the\n"
           "char arrays are copied byte-for-byte, not shared)\n");

#ifndef BREAK_IT
#define BREAK_IT 0   /* set to 1 (or -DBREAK_IT=1) to commit the crime */
#endif
#if BREAK_IT
    /* THE CRIME: strcpy-thinking -- "the destination is 32 bytes, the
     * string is 60, what could go wrong". The 60 bytes overwrite name
     * (32), phone (16), and 12 bytes BEYOND the struct. With only 40
     * chars the damage stays INSIDE the struct: the phone field of this
     * very contact gets eaten, silently, invisible even to ASan
     * (intra-object overflows are not instrumented). Escaping the whole
     * object is what ASan catches. */
    contact_t victim;
    printf("\n--- breaking: strcpy into contact_t.name (32 bytes) ---\n");
    strcpy(victim.name, "this sixty-character name is far, far too long for the "
                        "name field, honestly");
    printf("copied '%.60s' -- ASan should have screamed.\n", victim.name);
#endif

    return 0;
}
