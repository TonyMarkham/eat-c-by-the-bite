/*
 * Bite 44 — CHECKPOINT: Contact Book v3 — util module (interface).
 *
 * BUILD & RUN (see README.md in this folder for the full story):
 *   cmake -S . -B build && cmake --build build && ./build/contactbook
 * TESTS:
 *   cmake --build build && ctest --test-dir build
 *
 * util is the grab-bag module: the logging channel (bite 40's
 * convention), the hex parser (bite 41) and the big-endian put/get
 * pair (bite 42). Everything here is small, dependency-free, and
 * destined for reuse in Phase 5's networking code.
 *
 * ERROR-HANDLING CONVENTION (inherited from bite 40, obeyed project-wide):
 *   0 = success; < 0 = a named error code (see cb_result in contact.h).
 *   Syscall failures preserve errno; the CALLER renders it with
 *   strerror/perror. Only the top layer (main) prints user-facing
 *   errors, via log_error/log_info.
 */

#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <stdint.h>

/* ---- logging: the one error channel -------------------------------- */

/* "error: <msg>\n" on stderr. Variadic (stdarg.h / va_list). */
void log_error(const char *fmt, ...);
/* "info: <msg>\n" on stdout. */
void log_info(const char *fmt, ...);

/* ---- hex (bite 41) -------------------------------------------------- */

/* '0'..'9','a'..'f','A'..'F' -> 0..15; garbage -> -1. */
int parse_hex_nibble(char c);

/* "1A" -> 0x1A. Returns 0..255, or -1 on garbage. (int, not uint8_t:
 * no in-range value can mean "invalid".) */
int parse_hex_byte(const char *s);

/* ---- big-endian serialization (bite 42) ----------------------------- */

void put_u16be(uint8_t *p, uint16_t v);
uint16_t get_u16be(const uint8_t *p);
void put_u32be(uint8_t *p, uint32_t v);
uint32_t get_u32be(const uint8_t *p);

#endif /* UTIL_H */
