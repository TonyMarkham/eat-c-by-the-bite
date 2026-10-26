/*
 * Bite 43 — CMake + tests: util module (interface): hex + endianness.
 * These are exactly the functions bite 43's test list names, plus the
 * put_/get_ pair bite 42 built. They follow you into Phase 7.
 * Build/run commands in CMakeLists.txt's header comment.
 */

#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <stdint.h>

/* '0'..'9','a'..'f','A'..'F' -> 0..15; garbage -> -1. */
int parse_hex_nibble(char c);

/* "1A" -> 0x1A (26). Returns 0..255, or -1 on garbage.
 * (int, not uint8_t: there is no in-range "invalid" value.) */
int parse_hex_byte(const char *s);

/* Big-endian put/get (network byte order). See bite 42 for the why. */
void put_u16be(uint8_t *p, uint16_t v);
uint16_t get_u16be(const uint8_t *p);
void put_u32be(uint8_t *p, uint32_t v);
uint32_t get_u32be(const uint8_t *p);

#endif /* UTIL_H */
