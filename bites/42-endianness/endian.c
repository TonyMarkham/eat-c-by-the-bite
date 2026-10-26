/*
 * Bite 42 — Endianness & serialization: integers as portable bytes.
 *
 * BUILD:  gcc -Wall -Wextra -Werror -std=c17 -g -o prog endian.c
 * RUN:    ./prog
 *
 * Learn-bullets this file demonstrates:
 *  - Little-endian (x86): LEAST significant byte first in memory.
 *    Big-endian ("network byte order") is MSB first. htonl/htons/
 *    ntohl/ntohs convert - but the portable, dependency-free way is
 *    shift-and-mask BY HAND, which is what put_/get_ do here.
 *  - Portable serialization: (v >> 24) & 0xFF, one byte at a time.
 *    NEVER fwrite an int and call it a format (see BREAK IT ON PURPOSE).
 *
 * What the program does:
 *   1. detects this machine's endianness (the classic uint32_t x = 1
 *      first-byte trick)
 *   2. round-trips put_u16be/get_u16be for EVERY value 0..65535
 *   3. serializes a small record (u32 id, u16 len, payload) to a file
 *      in big-endian, byte by byte, and reads it back -> verify
 *   4. writes the SAME record raw with fwrite -> hexdump side by side
 *
 * BREAK IT ON PURPOSE: step 4 is the experiment. fwrite(&rec, sizeof
 * rec, 1, f) "works" on this machine - read it back here and you get
 * the same bytes. But reason through what breaks (comments at
 * native_demo()):
 *   - on a big-endian machine the file's integer bytes are reversed;
 *   - sizeof(rec) is 16 while its members sum to 14: the compiler
 *     inserted 2 PADDING bytes, which fwrite happily writes. Their
 *     contents are unspecified - a different compiler could make a
 *     different file for the same struct.
 * Byte-order + padding + no self-description = a format that silently
 * depends on the machine that wrote it. That reasoning IS the lesson.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 1. Detect endianness                                                */
/* ------------------------------------------------------------------ */

/* uint32_t x = 1 stores the value 1. In memory, little-endian puts
 * the 01 byte FIRST (lowest address), big-endian puts it LAST. Look
 * at the first byte and you know the machine's order. */
static const char *endianness(void)
{
    uint32_t x = 1;
    unsigned char first;
    memcpy(&first, &x, 1);      /* memcpy: type-safe byte peek */
    return (first == 1)
        ? "little-endian (x86/AMD64; most ARM in practice)"
        : "big-endian (network order; old SPARC/68k)";
}

/* ------------------------------------------------------------------ */
/* 2. put_/get_: the four functions that follow you into Phase 7       */
/* ------------------------------------------------------------------ */

/* Big-endian = MSB first. (v >> 8) & 0xFF isolates the high byte,
 * v & 0xFF the low byte. Writing byte-by-byte makes the on-disk
 * layout EXPLICIT - identical on every machine. */
void put_u16be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)((v >> 8) & 0xFFu);
    p[1] = (uint8_t)(v & 0xFFu);
}

uint16_t get_u16be(const uint8_t *p)
{
    /* p[0] is uint8_t, promoted to int - always non-negative, so the
     * shift is clean (bite 41's rule: no signed shifts). */
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
    /* The (uint32_t) casts matter: without them p[0] << 24 happens in
     * int, and 0xFF << 24 overflows a signed 32-bit int = undefined
     * behavior. Cast first, shift in unsigned land. */
    return ((uint32_t)p[0] << 24)
         | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)
         |  (uint32_t)p[3];
}

/* ------------------------------------------------------------------ */
/* 3. A small record, serialized explicitly                            */
/* ------------------------------------------------------------------ */

/* Layout we PUT ON DISK (big-endian, no padding, self-chosen):
 *   offset 0: u32 id
 *   offset 4: u16 len
 *   offset 6: len bytes of payload
 * This 6-byte header + payload is exactly the shape of the binary
 * protocol in bite 54 and of RTP's fixed header. */
#define PAYLOAD_LEN 8
#define REC_DISK_BYTES (4 + 2 + PAYLOAD_LEN)

typedef struct {
    uint32_t id;
    uint16_t len;
    char payload[PAYLOAD_LEN];
} record_t;

static int write_record_be(const char *path, const record_t *r)
{
    uint8_t buf[REC_DISK_BYTES];
    put_u32be(buf, r->id);
    put_u16be(buf + 4, r->len);
    memcpy(buf + 6, r->payload, PAYLOAD_LEN);

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        perror(path);
        return -1;
    }
    int ok = fwrite(buf, 1, sizeof buf, f) == sizeof buf;
    if (fclose(f) != 0)
        ok = 0;
    return ok ? 0 : -1;
}

static int read_record_be(const char *path, record_t *r)
{
    uint8_t buf[REC_DISK_BYTES];
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        perror(path);
        return -1;
    }
    int ok = fread(buf, 1, sizeof buf, f) == sizeof buf;
    fclose(f);
    if (!ok)
        return -1;

    r->id = get_u32be(buf);
    r->len = get_u16be(buf + 4);
    memcpy(r->payload, buf + 6, PAYLOAD_LEN);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Hexdump (same helper as bite 36 - it earns its keep)                */
/* ------------------------------------------------------------------ */

static void hexdump(const void *data, size_t len)
{
    const unsigned char *p = data;
    for (size_t off = 0; off < len; off += 16) {
        printf("%04zx  ", off);
        for (size_t i = 0; i < 16; i++)
            printf(off + i < len ? "%02x " : "   ", off + i < len ? p[off + i] : 0);
        printf(" ");
        for (size_t i = 0; i < 16 && off + i < len; i++) {
            unsigned char c = p[off + i];
            putchar((c >= 32 && c < 127) ? c : '.');
        }
        putchar('\n');
    }
}

/* Slurp a small file for dumping. */
static long slurp(const char *path, unsigned char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        perror(path);
        return -1;
    }
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return (long)n;
}

/* ------------------------------------------------------------------ */
/* 4. BREAK IT ON PURPOSE: the raw-fwrite contrast                     */
/* ------------------------------------------------------------------ */

static void native_demo(const record_t *r)
{
    printf("\n-- the raw fwrite experiment (what NOT to ship) --\n");
    printf("sizeof(record_t) = %zu, but its members sum to %zu:\n",
           sizeof(record_t), sizeof r->id + sizeof r->len + sizeof r->payload);
    printf("  -> the compiler inserted %zu PADDING byte(s); fwrite writes\n"
           "     them raw. Another compiler may pad differently: the file\n"
           "     becomes compiler-dependent. Combined with byte order\n"
           "     (reversed on big-endian machines), a raw struct dump is\n"
           "     NOT a portable format. Use put_/get_ instead.\n",
           sizeof(record_t) - (sizeof r->id + sizeof r->len + sizeof r->payload));

    FILE *f = fopen("record_native.bin", "wb");
    if (f == NULL) {
        perror("record_native.bin");
        return;
    }
    if (fwrite(r, sizeof *r, 1, f) != 1)   /* native order + padding */
        fprintf(stderr, "write failed\n");
    fclose(f);

    unsigned char buf[64];
    long n = slurp("record_native.bin", buf, sizeof buf);
    if (n < 0)
        return;
    printf("record_native.bin (%ld bytes) - note the byte ORDER of id\n", n);
    hexdump(buf, (size_t)n);
    /* On this little-endian x86 machine, id 0x0000002A lands on disk
     * as "2A 00 00 00" - reversed compared to the big-endian file. */
}

int main(void)
{
    /* ---- 1. verdict -------------------------------------------------- */
    printf("this machine is: %s\n\n", endianness());

    /* ---- 2. exhaustive u16 round-trip (the "Done when" test) --------- */
    for (uint32_t v = 0; v <= 0xFFFF; v++) {
        uint8_t b[2];
        put_u16be(b, (uint16_t)v);
        assert(get_u16be(b) == v);      /* fires on any mismatch, 0..65535 */
    }
    printf("put_u16be/get_u16be: all 65536 values round-trip\n");

    /* spot-check the u32 pair on edge values */
    {
        static const uint32_t cases[] = { 0x00000000u, 0x00000001u, 0x0000FFFFu,
                                          0xDEADBEEFu, 0xFFFFFFFFu };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            uint8_t b[4];
            put_u32be(b, cases[i]);
            assert(get_u32be(b) == cases[i]);
        }
    }
    printf("put_u32be/get_u32be: edge cases round-trip\n\n");

    /* ---- 3. the record, big-endian on disk ---------------------------- */
    record_t out = { .id = 0x0000002A, .len = PAYLOAD_LEN,
                     .payload = "IMDATA!" };   /* hmm: 7 chars + NUL = 8 */
    if (write_record_be("record_be.bin", &out) != 0)
        return 1;

    record_t in;
    if (read_record_be("record_be.bin", &in) != 0)
        return 1;
    printf("record round-trip: id=0x%08X len=%u payload=\"%.8s\" -> %s\n",
           in.id, in.len, in.payload,
           (in.id == out.id && in.len == out.len
            && memcmp(in.payload, out.payload, PAYLOAD_LEN) == 0) ? "OK" : "MISMATCH");

    unsigned char buf[64];
    long n = slurp("record_be.bin", buf, sizeof buf);
    if (n < 0)
        return 1;
    printf("record_be.bin (%ld bytes) - explicit layout, portable:\n", n);
    hexdump(buf, (size_t)n);

    /* ---- 4. the raw-fwrite contrast ----------------------------------- */
    native_demo(&out);

    printf("\nreal projects use htonl/ntohl for 16/32-bit ints; put_/get_\n");
    printf("are the same idea, hand-rolled, portable, and dependency-free.\n");
    return 0;
}
