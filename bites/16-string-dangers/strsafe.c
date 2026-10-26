/*
 * Bite 16 - string.h and the Dangerous Half
 * ==========================================
 * Compile & run (from the repo root inside WSL):
 *
 *     gcc -Wall -Wextra -Werror -std=c17 -g bites/16-string-dangers/strsafe.c -o /tmp/b16 && /tmp/b16
 *
 * The "Learn" list, translated into code below:
 *   - Dangerous set: strcpy, strcat, sprintf -- their safety depends on
 *     the CALLER'S HOPE that the destination is big enough. gets was so
 *     dangerous it was removed from the language entirely.
 *   - Safe set: snprintf (always terminates, returns the length it
 *     WANTED), strncpy (still traps: it does NOT terminate on
 *     truncation!), strncat, strtol for numbers.
 *   - Rule of thumb: any function whose safety depends on the caller's
 *     hope is the wrong function.
 *
 * BREAK IT ON PURPOSE:
 *   strcpy a 51-character string into a 16-byte buffer, under ASan.
 *   Read the stack-smash report -- this is the bug class behind half of
 *   all CVEs, seen here in captivity:
 *
 *     gcc -DBREAK_IT=1 -Wall -Wextra -std=c17 -g \
 *         bites/16-string-dangers/strsafe.c -o /tmp/b16break && /tmp/b16break
 *
 *   (GCC flags the overflow at compile time too -- "-Wstringop-overflow"
 *   -- which is why the break build drops -Werror: we want the runtime
 *   crash, not just the warning.)
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>   /* strtol */
#include <errno.h>
#include <stddef.h>   /* ptrdiff_t */

/*
 * Idiom #1, promoted to a function: copy with snprintf. This is your
 * default from today on. snprintf ALWAYS terminates the buffer (if
 * dstsz > 0) and returns the length it WANTED to write -- so you can
 * detect truncation instead of hoping there wasn't any.
 */
static void safe_copy(char *dst, size_t dstsz, const char *src)
{
    int needed = snprintf(dst, dstsz, "%s", src);
    if (needed < 0) {                    /* encoding error: rare, but handle */
        dst[0] = '\0';
        printf("  (snprintf failed -- rare encoding error)\n");
    } else if ((size_t)needed >= dstsz) {
        printf("  (truncated: wanted %d bytes incl. '\\0', buffer holds %zu)\n",
               needed, dstsz);
    }
    /* no else: on success the copy simply happened */
}

/* Build "Name: <name> (age N)" in one snprintf into a small buffer. */
static void build_bio(char *dst, size_t dstsz, const char *name, int age)
{
    int needed = snprintf(dst, dstsz, "Name: %s (age %d)", name, age);
    printf("  wanted %d bytes for the full text; buffer holds %zu -> \"%s\"\n",
           needed, dstsz, dst);
}

/*
 * strtol: parse a number AND see exactly where the digits ended via
 * endptr. atoi cannot do this; sscanf "%d" cannot tell you about
 * trailing junk cleanly. end == s means "no digits at all".
 */
static void parse_demo(const char *s)
{
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s) {
        printf("  \"%s\": no digits at all (endptr never moved)\n", s);
    } else if (errno == ERANGE) {
        printf("  \"%s\": value out of range for long\n", s);
    } else {
        printf("  \"%s\": value %ld, conversion stopped at offset %td, rest = \"%s\"%s\n",
               s, v, (ptrdiff_t)(end - s), end,
               *end == '\0' ? "  <- clean" : "  <- trailing junk");
    }
}

int main(void)
{
    /* 1. The default copy idiom, happy path and truncation path: */
    char small[16];
    printf("safe_copy(\"hello\", buf[16]):\n");
    safe_copy(small, sizeof small, "hello");
    printf("  buf = \"%s\"\n", small);

    printf("safe_copy(<100-char name>, buf[16]):\n");
    char huge[128];
    for (size_t i = 0; i < sizeof huge - 1; i++)
        huge[i] = 'x';
    huge[sizeof huge - 1] = '\0';
    safe_copy(small, sizeof small, huge);
    printf("  buf = \"%s\"  <- truncated, terminated, memory intact\n\n", small);

    /* 2. One snprintf builds the whole line; a 100-char name only
     * truncates (cleanly), it does not corrupt anything: */
    char bio[24];
    printf("build_bio with a normal name:\n");
    build_bio(bio, sizeof bio, "Grace", 36);
    printf("build_bio with the 100-char name:\n");
    build_bio(bio, sizeof bio, huge, 36);
    printf("\n");

    /* 3. strtol with endptr: */
    printf("strtol with endptr:\n");
    parse_demo("42");
    parse_demo("  42abc");   /* leading spaces are fine; junk after digits is visible */
    parse_demo("12.9");      /* stops at '.', the caller decides if that's OK */
    parse_demo("abc");

#ifndef BREAK_IT
#define BREAK_IT 0   /* set to 1 (or -DBREAK_IT=1) to commit the crime */
#endif
#if BREAK_IT
    /* THE CRIME -- the only strcpy in this codebase, imprisoned here:
     * 51 bytes into a 16-byte buffer. Under ASan: "stack-buffer-overflow
     * WRITE". Without ASan it smashes whatever comes next on the stack
     * and may appear to work -- the deadliest kind of bug. */
    char tiny[16];
    const char *big = "this string is definitely longer than sixteen bytes";
    printf("\n--- breaking: strcpy(tiny[16], <51 chars>) ---\n");
    strcpy(tiny, big);
    printf("copied \"%s\" -- maybe it worked. UB is a gambler.\n", tiny);
#endif

    return 0;
}
