/*
 * Bite 44 — CHECKPOINT: Contact Book v3 — util implementation.
 * Interface docs in util.h; convention + build/run in contact.h and
 * README.md.
 */
#include "util.h"

#include <stdarg.h>
#include <stdio.h>

/* ---- logging -------------------------------------------------------- */

/* First variadic function of the project: va_start binds the va_list
 * to the last NAMED parameter (fmt), vfprintf consumes it, va_end
 * tidies up. One channel, one format, no per-module fprintf zoo. */
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

/* ---- hex ------------------------------------------------------------ */

int parse_hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int parse_hex_byte(const char *s)
{
    if (s == NULL)
        return -1;
    int hi = parse_hex_nibble(s[0]);
    if (hi < 0)
        return -1;
    int lo = parse_hex_nibble(s[1]);
    if (lo < 0)
        return -1;
    return (hi << 4) | lo;
}

/* ---- big-endian put/get --------------------------------------------- */

void put_u16be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)((v >> 8) & 0xFFu);
    p[1] = (uint8_t)(v & 0xFFu);
}

uint16_t get_u16be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

void put_u32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)((v >> 24) & 0xFFu);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

uint32_t get_u32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24)
         | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)
         |  (uint32_t)p[3];
}
