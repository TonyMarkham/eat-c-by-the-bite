/*
 * Bite 15 - Strings as char Arrays
 * =================================
 * Compile & run (from the repo root inside WSL):
 *
 *     gcc -Wall -Wextra -Werror -std=c17 -g bites/15-strings/strings.c -o /tmp/b15 && /tmp/b15
 *
 * The "Learn" list, translated into code below:
 *   - char s[6] = "hello"; stores SIX bytes: h e l l o \0. The '\0'
 *     terminator is DATA, not punctuation. Every string function's
 *     correctness hinges on that terminator being present and in bounds.
 *   - strlen is O(n): it WALKS to the '\0'. There is no hidden length
 *     field anywhere. sizeof is the box; strlen is the content up to
 *     the terminator.
 *
 * BREAK IT ON PURPOSE (two crimes):
 *   1. char s[5] = "hello"; -- six bytes needed, five given, terminator
 *      silently dropped. Newer compilers (GCC 15+) flag this with
 *      -Wunterminated-string-initialization; GCC 13 accepts it in
 *      silence, which is exactly the point: the language lets you:
 *
 *        gcc -DBREAK_IT=1 -Wall -Wextra -Werror -std=c17 -g \
 *            bites/15-strings/strings.c -o /tmp/b15break && /tmp/b15break
 *
 *   2. char s[3] = {'a','b','c'}; printf("%s", s); -- no '\0' at all, so
 *      printf keeps reading whatever bytes follow: trailing garbage, or
 *      an ASan stack-buffer-overflow report. UB doing its party trick.
 */

#include <stdio.h>
#include <string.h>   /* the REAL strlen/strcmp, for comparison */

/*
 * my_strlen: walk until the terminator. This is what strlen costs --
 * O(n) every single call. A 1 MB string takes a million comparisons to
 * measure, which is why APIs that call strlen in a loop are slow.
 */
static size_t my_strlen(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0')
        n++;
    return n;
}

/*
 * my_strcmp: lexicographic compare. Walk while the characters match and
 * are not the terminator; when they differ (or both end), the difference
 * of the two bytes IS the answer: negative = a first, 0 = equal,
 * positive = b first. The unsigned char cast is not decoration: plain
 * char is signed on x86, and comparing chars as signed gives wrong
 * order for bytes >= 128.
 */
static int my_strcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/*
 * my_strcpy WITH a destination size -- the parameter the real strcpy
 * criminally lacks. Returns dst on success, NULL if src (plus its
 * terminator) would not fit. Never overruns, even on failure.
 */
static char *my_strcpy(char *dst, size_t dstsz, const char *src)
{
    if (dstsz == 0)
        return NULL;

    size_t i = 0;
    while (i + 1 < dstsz && src[i] != '\0') {   /* reserve 1 slot for '\0' */
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';

    if (src[i] != '\0')     /* we stopped because the buffer filled up */
        return NULL;        /* (note: dst holds a truncated copy now)  */
    return dst;
}

#define SIGN(x) ((x) < 0 ? -1 : (x) > 0 ? 1 : 0)

int main(void)
{
    /* sizeof vs strlen for a PROPERLY terminated string: */
    char s[6] = "hello";
    printf("char s[6] = \"hello\":  sizeof s = %zu (the box), "
           "strlen(s) = %zu (content before '\\0')\n\n",
           sizeof s, my_strlen(s));

    /* ---- test harness: 5 cases each, checked against the real ones ---- */

    const char *const strlen_cases[] = { "", "a", "hello", "a b c", "0123456789" };
    printf("my_strlen vs strlen:\n");
    for (size_t i = 0; i < sizeof strlen_cases / sizeof strlen_cases[0]; i++) {
        size_t mine = my_strlen(strlen_cases[i]);
        size_t real = strlen(strlen_cases[i]);
        printf("  \"%-12s\" mine=%zu real=%zu  %s\n",
               strlen_cases[i], mine, real, mine == real ? "OK" : "FAIL");
    }

    const char *const cmp_pairs[][2] = {
        { "",      ""      },
        { "apple", "apple" },
        { "apple", "apples" },
        { "b",     "a"     },
        { "a",     "B"     },   /* 'a'(97) vs 'B'(66): byte order, not alphabet */
    };
    printf("my_strcmp vs strcmp (sign must match):\n");
    for (size_t i = 0; i < sizeof cmp_pairs / sizeof cmp_pairs[0]; i++) {
        int mine = SIGN(my_strcmp(cmp_pairs[i][0], cmp_pairs[i][1]));
        int real = SIGN(strcmp(cmp_pairs[i][0], cmp_pairs[i][1]));
        printf("  (\"%s\", \"%-7s\") mine=%+d real=%+d  %s\n",
               cmp_pairs[i][0], cmp_pairs[i][1], mine, real,
               mine == real ? "OK" : "FAIL");
    }

    /*
     * my_strcpy vs the safe standard idiom (real strcpy has no size
     * parameter, so the honest comparison is against snprintf):
     *   snprintf(dst, sizeof dst, "%s", src)
     */
    char buf[8];
    printf("my_strcpy vs snprintf idiom:\n");
    printf("  copy \"short\" into char[8]:    %s (content: \"%s\")\n",
           my_strcpy(buf, sizeof buf, "short") != NULL ? "OK" : "FAIL", buf);
    printf("  copy \"\" into char[8]:         %s (content: \"%s\")\n",
           my_strcpy(buf, sizeof buf, "") != NULL ? "OK" : "FAIL", buf);
    printf("  copy \"exactly\" into char[8]:  %s (content: \"%s\")\n",
           my_strcpy(buf, sizeof buf, "exactly") != NULL ? "OK" : "FAIL", buf);
    printf("  copy \"exactly7\" into char[8]: %s (8 chars + '\\0' = 9 > 8: rejected)\n",
           my_strcpy(buf, sizeof buf, "exactly7") == NULL ? "NULL (rejected)" : "OK");
    char before = buf[0];
    printf("  copy \"too long for eight\" into char[8]: %s (buf starts with '%c', truncated safely)\n",
           my_strcpy(buf, sizeof buf, "too long for eight") == NULL ? "NULL (rejected)" : "OK",
           before);

#ifndef BREAK_IT
#define BREAK_IT 0   /* set to 1 to commit the crimes */
#endif
#if BREAK_IT
    /* CRIME 1: six bytes needed, five provided. C allows this exact
     * initializer (the '\0' is silently dropped; GCC 15+ warns, GCC 13
     * does not). s5 is NOT a string -- it is 5 chars with no end. */
    char s5[5] = "hello";
    printf("\nsizeof s5 = %zu, and there is no '\\0' anywhere in it\n", sizeof s5);

    /* CRIME 2: no terminator at all. printf("%s") walks off the end and
     * prints whatever bytes live after s3 on the stack. Under ASan this
     * usually dies with a stack-buffer-overflow READ inside strlen. */
    char s3[3] = { 'a', 'b', 'c' };
    printf("printf(\"%%s\", s3) prints: [%s]\n", s3);
#endif

    return 0;
}
