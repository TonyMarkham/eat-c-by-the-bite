/*
 * Bite 43 — CMake + tests: the assert-based test suite.
 *
 * BUILD & RUN (out-of-source):
 *   cmake -S . -B build && cmake --build build && ctest --test-dir build
 * ASan variant:
 *   cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
 *   cmake --build build-debug && ctest --test-dir build-debug
 *
 * Tests target FUNCTIONS, not the UI: each module's behavior is
 * asserted directly. No framework - assert() aborts with a message
 * naming file, line, and expression, and a non-zero exit makes ctest
 * report the failure. A test that can fail loudly > a framework you
 * avoid using.
 *
 * BREAK IT ON PURPOSE: flip one expected value (e.g. `== 26` ->
 * `== 27` in test_util). Rebuild, run ctest, and watch the suite go
 * red with the failing function named. Then fix it and see green
 * again - that red/green cycle is the whole value of the suite.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mystring.h"
#include "util.h"
#include "strbuf.h"
#include "intvec.h"
#include "hashmap.h"

static void test_mystring(void)
{
    /* Equal strings -> 0. */
    assert(my_strcmp("hello", "hello") == 0);

    /* Orderings: only the SIGN is contractual. Compare against the
     * real strcmp's sign - that is the compatibility test. */
    assert((my_strcmp("apple", "banana") < 0) == (strcmp("apple", "banana") < 0));
    assert((my_strcmp("b", "a") > 0) == (strcmp("b", "a") > 0));

    /* Prefix vs longer string. */
    assert(my_strcmp("app", "apple") < 0);

    /* Empty strings. */
    assert(my_strcmp("", "") == 0);
    assert(my_strcmp("", "x") < 0);

    /* Bytes above 127 compare as unsigned (strcmp contract). */
    assert(my_strcmp("\x80", "\x01") > 0);

    assert(my_strlen("") == 0);
    assert(my_strlen("hello") == 5);
    printf("mystring: OK\n");
}

static void test_util(void)
{
    /* The "Done when" case from bite 41. */
    assert(parse_hex_byte("1A") == 26);
    assert(parse_hex_byte("ff") == 255);
    assert(parse_hex_byte("G1") == -1);         /* garbage rejected */
    assert(parse_hex_byte("") == -1);

    assert(parse_hex_nibble('0') == 0);
    assert(parse_hex_nibble('F') == 15);
    assert(parse_hex_nibble('g') == -1);

    /* Every u16 value must round-trip through big-endian bytes. */
    for (uint32_t v = 0; v <= 0xFFFF; v++) {
        uint8_t b[2];
        put_u16be(b, (uint16_t)v);
        assert(get_u16be(b) == v);
    }

    /* u32 edges + known values. */
    static const uint32_t cases[] = { 0x00000000u, 0x00000001u, 0x0000FFFFu,
                                      0xDEADBEEFu, 0xFFFFFFFFu };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint8_t b[4];
        put_u32be(b, cases[i]);
        assert(get_u32be(b) == cases[i]);
    }
    /* Explicit byte order check: 0x12345678 -> 12 34 56 78 (MSB first). */
    uint8_t b4[4];
    put_u32be(b4, 0x12345678u);
    assert(b4[0] == 0x12 && b4[1] == 0x34 && b4[2] == 0x56 && b4[3] == 0x78);
    printf("util: OK\n");
}

static void test_strbuf(void)
{
    strbuf sb;
    sb_init(&sb);

    assert(sb_append(&sb, "hello") == 0);
    assert(sb.len == 5 && strcmp(sb.data, "hello") == 0);
    assert(sb_append(&sb, ", ") == 0);
    assert(sb_append(&sb, "world") == 0);
    assert(sb.len == 12 && strcmp(sb.data, "hello, world") == 0);

    /* Growth must preserve the earlier content (forcing reallocs). */
    for (int i = 0; i < 100; i++) {
        char chunk[8];
        snprintf(chunk, sizeof chunk, "%d;", i);
        assert(sb_append(&sb, chunk) == 0);
    }
    /* after 100 appends: "hello, world0;1;2;...;98;99;" */
    assert(strncmp(sb.data, "hello, world", 12) == 0);
    assert(sb.len >= 5 && strcmp(sb.data + sb.len - 3, "99;") == 0);

    sb_free(&sb);
    assert(sb.data == NULL && sb.len == 0 && sb.cap == 0);
    sb_free(&sb);                       /* double free must be safe */
    printf("strbuf: OK\n");
}

static void test_intvec(void)
{
    intvec v;
    iv_init(&v);

    /* Push enough values to force multiple doublings (cap 4->8->16...). */
    for (int i = 0; i < 100; i++)
        assert(iv_push(&v, i * 3) == 0);
    assert(v.len == 100);
    assert(iv_get(&v, 0) == 0);
    assert(iv_get(&v, 99) == 297);
    assert(iv_get(&v, 100) == -1);      /* out of range */

    iv_free(&v);
    printf("intvec: OK\n");
}

static void test_hashmap(void)
{
    hashmap *h = hm_create();
    assert(h != NULL);

    assert(hm_count(h) == 0);
    assert(hm_get(h, "missing") == NULL);

    assert(hm_set(h, "alice", 1) == 0);
    assert(hm_set(h, "bob", 2) == 0);
    assert(hm_count(h) == 2);

    int *v = hm_get(h, "alice");
    assert(v != NULL && *v == 1);
    v = hm_get(h, "bob");
    assert(v != NULL && *v == 2);

    /* Overwrite keeps one entry. */
    assert(hm_set(h, "alice", 42) == 0);
    assert(hm_count(h) == 2);
    assert(*hm_get(h, "alice") == 42);

    /* Enough keys to exercise collisions + probing. */
    char key[16];
    for (int i = 0; i < 40; i++) {
        snprintf(key, sizeof key, "user%03d", i);
        assert(hm_set(h, key, i) == 0);
    }
    for (int i = 0; i < 40; i++) {
        snprintf(key, sizeof key, "user%03d", i);
        assert(hm_get(h, key) != NULL && *hm_get(h, key) == i);
    }
    /* Colliding-ish probe behavior: keys that hash near each other
     * must not overwrite each other (linear probing walks on). */
    assert(hm_get(h, "alice") != NULL && *hm_get(h, "alice") == 42);

    hm_destroy(h);
    printf("hashmap: OK\n");
}

int main(void)
{
    test_mystring();
    test_util();
    test_strbuf();
    test_intvec();
    test_hashmap();
    printf("all tests passed\n");
    return 0;
}
