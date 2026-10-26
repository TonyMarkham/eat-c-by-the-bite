/*
 * Bite 43 — CMake + tests: mystring module (interface).
 * Build/run commands in CMakeLists.txt's header comment.
 * BREAK IT ON PURPOSE: flip an expected value in tests.c.
 */

#ifndef MYSTRING_H
#define MYSTRING_H

#include <stddef.h>     /* size_t */

/* Compare two NUL-terminated strings byte by byte (as unsigned char,
 * because plain char's signedness is platform-defined).
 * Returns < 0 if a < b, 0 if equal, > 0 if a > b - same contract as
 * strcmp(3), so tests can compare signs against the real one. */
int my_strcmp(const char *a, const char *b);

/* Length in bytes, NOT counting the NUL terminator. */
size_t my_strlen(const char *s);

#endif /* MYSTRING_H */
