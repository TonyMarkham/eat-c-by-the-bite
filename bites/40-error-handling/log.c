/*
 * Bite 40 — Error handling conventions: log.c.
 *
 * Your first variadic function. printf-style functions take "..." -
 * extra arguments matched to the format string. Inside, va_start
 * binds a va_list to the LAST NAMED parameter (fmt), vfprintf consumes
 * the list, va_end tidies up. vfprintf (the v-prefixed family) is
 * exactly for this: same as fprintf, but takes a va_list.
 */
#include "log.h"

#include <stdarg.h>
#include <stdio.h>

void log_error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fputs("error: ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

void log_info(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fputs("info: ", stdout);
    vfprintf(stdout, fmt, args);
    fputc('\n', stdout);
    va_end(args);
}
