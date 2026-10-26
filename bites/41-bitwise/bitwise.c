/*
 * Bite 41 — Bitwise ops, masks, and hex: becoming fluent in bits.
 *
 * BUILD:  gcc -Wall -Wextra -Werror -std=c17 -g -o prog bitwise.c
 * RUN:    ./prog
 *         (the program self-tests its own answers with assert and
 *          prints a tour of every concept)
 * TO SEE UB CAUGHT: rebuild with the deliberate bug enabled. Use -O1
 * (at -O0 gcc folds `x << 1` into `x + x` before the sanitizer sees
 * it - a known quirk; the optimized build keeps the real shift):
 *         gcc -Wall -Wextra -Werror -std=c17 -g -O1 -fsanitize=undefined \
 *             -DBREAK_SIGNED_SHIFT -o prog_ub bitwise.c && ./prog_ub
 *
 * Learn-bullets this file demonstrates:
 *  - & | ^ ~ << >> with unsigned types. Masks isolate fields:
 *    (x >> 4) & 0x0F. Shifts on SIGNED ints are a minefield (see the
 *    BREAK_SIGNED_SHIFT block) - shift unsigned types, always.
 *  - Hex is the native language of protocols: one hex digit = 4 bits,
 *    two hex digits = one byte. 0xDEADBEEF is 32 bits you can read.
 *  - The flag trio: set x |= bit, clear x &= ~bit, test if (x & bit).
 *
 * BREAK IT ON PURPOSE: signed shifting. Build with -DBREAK_SIGNED_SHIFT
 * and -fsanitize=undefined: the shift_signed() demo left-shifts a
 * NEGATIVE signed int, which is undefined behavior, and UBSan prints
 * "left shift of negative value". The right shift of a negative value
 * is "only" implementation-defined (gcc does arithmetic shift, giving
 * -4 for -8 >> 1) - still banned in this codebase from today. Redo it
 * with unsigned (shift_unsigned(), always compiled): clean, logical,
 * portable. That contrast IS the lesson.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* print_bits: the loop-and-mask classic                               */
/* ------------------------------------------------------------------ */

/* Fill out[32] with '0'/'1', MSB first, then NUL-terminate.
 * The mask work: (x >> i) & 1u walks bit i into position 0, where & 1u
 * keeps only it. i counts 31..0 so the string reads like a human wants. */
static void bits_to_str(char out[33], uint32_t x)
{
    for (int i = 31; i >= 0; i--)
        out[31 - i] = (char)('0' + ((x >> i) & 1u));
    out[32] = '\0';
}

/* The bite's required function. No libraries, just a loop and masks. */
void print_bits(uint32_t x)
{
    char s[33];
    bits_to_str(s, x);
    printf("0x%08X = %s\n", x, s);
}

/* ------------------------------------------------------------------ */
/* Hex parsing: one hex digit = 4 bits                                 */
/* ------------------------------------------------------------------ */

/* '0'..'9' -> 0..9, 'a'..'f'/'A'..'F' -> 10..15, garbage -> -1.
 * The character arithmetic: chars are small integers, so ranges like
 * c >= 'a' && c <= 'f' plus c - 'a' + 10 do the mapping. */
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

/* "1A" -> 0x1A (26). Two nibbles joined: (high << 4) | low.
 * Returns 0..255, or -1 if either digit is garbage.
 * NOTE: deviates from the lesson sketch's `uint8_t` return type on
 * purpose - no uint8_t value can mean "invalid", so failure needs a
 * wider type. Documented here so it's a decision, not an accident. */
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

/* "0x1A2B" (or "1A2B") -> bytes {0x1A, 0x2B}.
 * Accepts an optional 0x/0X prefix. An odd number of digits is an
 * error (each byte needs exactly two). Returns the number of bytes
 * written, or -1 on bad input / buffer too small.
 * This is the shape you will reuse when parsing protocol dumps in
 * Phases 5-7. */
long parse_hex_string(const char *s, uint8_t *out, size_t max)
{
    if (s == NULL || out == NULL)
        return -1;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;                          /* skip the prefix */

    size_t n = strlen(s);
    if (n == 0 || n % 2 != 0 || n / 2 > max)
        return -1;

    for (size_t i = 0; i < n; i += 2) {
        int hi = parse_hex_nibble(s[i]);
        int lo = parse_hex_nibble(s[i + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    return (long)(n / 2);
}

/* ------------------------------------------------------------------ */
/* Flag practice: the set/clear/test trio                              */
/* ------------------------------------------------------------------ */

/* Four named bits. 1u << n is the idiomatic way to write bit masks;
 * the u suffix matters (never shift a signed type). */
enum {
    FLAG_READ    = 1u << 0,     /* 0x1 */
    FLAG_WRITE   = 1u << 1,     /* 0x2 */
    FLAG_ADMIN   = 1u << 2,     /* 0x4 */
    FLAG_DELETED = 1u << 3      /* 0x8 */
};

static void flag_set(uint32_t *f, uint32_t bit)    { *f |= bit;  }  /* turn on   */
static void flag_clear(uint32_t *f, uint32_t bit)  { *f &= ~bit; }  /* turn off  */
static void flag_toggle(uint32_t *f, uint32_t bit) { *f ^= bit;  }  /* flip      */
static int  flag_test(uint32_t f, uint32_t bit)    { return (f & bit) != 0; }

static void flag_print(const char *what, uint32_t f)
{
    static const struct { const char *name; uint32_t bit; } table[] = {
        { "READ",    FLAG_READ    },
        { "WRITE",   FLAG_WRITE   },
        { "ADMIN",   FLAG_ADMIN   },
        { "DELETED", FLAG_DELETED },
    };
    printf("%-10s 0x%X =", what, f);
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        printf(" %s%s", flag_test(f, table[i].bit) ? "+" : "-",
               table[i].name);
    putchar('\n');
}

/* ------------------------------------------------------------------ */
/* BREAK IT ON PURPOSE (opt-in): the signed-shift minefield            */
/* ------------------------------------------------------------------ */

#ifdef BREAK_SIGNED_SHIFT
static void shift_signed(void)
{
    /* The shift must not be folded at compile time or UBSan never
     * sees it. A volatile variable forces a real load+shift at
     * runtime - the compiler cannot assume its value. */
    volatile int neg = -8;

    int r = neg >> 1;   /* implementation-defined: gcc -> -4 (arithmetic) */
    printf("(-8) >> 1 = %d   (implementation-defined!)\n", r);
    int bad = neg << 1; /* UNDEFINED: left shift of a negative value.
                         * UBSan prints: runtime error: left shift of
                         * negative value -8 by 1 places. */
    printf("(-8) << 1 = %d   (undefined behavior!)\n", bad);
}
#endif

/* The correct, portable version: unsigned right shift is a clean
 * LOGICAL shift - zeros come in from the left, no surprises. */
static void shift_unsigned(void)
{
    uint32_t u = 0x80000000u;       /* top bit set: the scary case */
    printf("0x80000000u >> 1 = 0x%08X  (logical: 0x40000000, clean)\n", u >> 1);
    printf("mask demo: (0xDEADBEEF >> 4) & 0x0F = 0x%X  (nibble extraction)\n",
           (0xDEADBEEFu >> 4) & 0x0Fu);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    /* ---- 1. print_bits, verified against known values -------------- */
    print_bits(0x00000000u);
    print_bits(0xFFFFFFFFu);
    print_bits(0xDEADBEEFu);

    char s[33];
    bits_to_str(s, 0xDEADBEEFu);
    assert(strcmp(s, "11011110101011011011111011101111") == 0);
    bits_to_str(s, 0x00000000u);
    assert(strcmp(s, "00000000000000000000000000000000") == 0);
    bits_to_str(s, 0xFFFFFFFFu);
    assert(strcmp(s, "11111111111111111111111111111111") == 0);
    printf("print_bits: verified against known bit patterns\n\n");

    /* ---- 2. hex parsing -------------------------------------------- */
    assert(parse_hex_byte("1A") == 26);      /* the "Done when" case */
    assert(parse_hex_byte("0a") == 10);
    assert(parse_hex_byte("FF") == 255);
    assert(parse_hex_byte("G1") == -1);      /* garbage rejected */
    assert(parse_hex_byte("1") == -1);       /* one nibble is not a byte */
    assert(parse_hex_byte("") == -1);
    printf("parse_hex_byte(\"1A\") = %d, rejects \"G1\" (returns -1)\n",
           parse_hex_byte("1A"));

    uint8_t bytes[8];
    long nb = parse_hex_string("0x1A2B", bytes, sizeof bytes);
    assert(nb == 2 && bytes[0] == 0x1A && bytes[1] == 0x2B);
    printf("parse_hex_string(\"0x1A2B\") -> %ld bytes: ", nb);
    for (long i = 0; i < nb; i++)
        printf("%02X ", bytes[i]);
    printf("\n");
    assert(parse_hex_string("1A2B3C4D", bytes, sizeof bytes) == 4);
    assert(parse_hex_string("0x1A2", bytes, sizeof bytes) == -1);  /* odd */
    assert(parse_hex_string("0xGG", bytes, sizeof bytes) == -1);   /* junk */
    printf("parse_hex_string: good/odd/garbage cases all behave\n\n");

    /* ---- 3. flag practice ------------------------------------------- */
    uint32_t perms = 0;
    printf("flag demo:\n");
    flag_print("start", perms);
    flag_set(&perms, FLAG_READ);
    flag_set(&perms, FLAG_WRITE);
    flag_print("+R+W", perms);
    flag_set(&perms, FLAG_ADMIN);
    flag_print("+ADMIN", perms);
    flag_clear(&perms, FLAG_WRITE);
    flag_print("-WRITE", perms);
    flag_toggle(&perms, FLAG_DELETED);
    flag_print("~DELETED", perms);
    assert(flag_test(perms, FLAG_READ) == 1);
    assert(flag_test(perms, FLAG_WRITE) == 0);
    printf("flag trio: |= set, &= ~ clear, & test - verified\n\n");

    /* ---- 4. shifts: unsigned clean, signed banned -------------------- */
    shift_unsigned();
#ifdef BREAK_SIGNED_SHIFT
    printf("\nsigned-shift minefield (compiled in on purpose):\n");
    shift_signed();
#else
    printf("\n(unsigned-only build; rebuild with -O1 -fsanitize=undefined "
           "-DBREAK_SIGNED_SHIFT to watch signed shifting get flagged)\n");
#endif

    printf("\nall bitwise self-tests passed\n");
    return 0;
}
