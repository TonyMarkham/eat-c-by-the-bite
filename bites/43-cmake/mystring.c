/*
 * Bite 43 — CMake + tests: mystring implementation.
 * Hand-rolled versions of bite 15's string functions, now in a module
 * with a header - and, for the first time, under a test suite.
 */
#include "mystring.h"

int my_strcmp(const char *a, const char *b)
{
    if (a == NULL || b == NULL)
        return (a == b) ? 0 : (a == NULL ? -1 : 1);

    const unsigned char *ua = (const unsigned char *)a;
    const unsigned char *ub = (const unsigned char *)b;
    while (*ua != '\0' && *ua == *ub) {
        ua++;
        ub++;
    }
    /* First difference (or both at NUL): the unsigned chars differ
     * exactly where the strings do. Return the signed gap - callers
     * should test its SIGN, not its exact value (strcmp only promises
     * <0 / 0 / >0). */
    return (int)*ua - (int)*ub;
}

size_t my_strlen(const char *s)
{
    if (s == NULL)
        return 0;
    size_t n = 0;
    while (s[n] != '\0')
        n++;
    return n;
}
