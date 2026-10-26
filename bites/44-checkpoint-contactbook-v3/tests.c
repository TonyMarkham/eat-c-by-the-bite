/*
 * Bite 44 — ✅ CHECKPOINT: Contact Book v3 — the test suite.
 *
 * BUILD & RUN (see README.md for the full story):
 *   cmake -S . -B build && cmake --build build && ctest --test-dir build
 * ASan variant (the checkpoint requires green under ASan):
 *   cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
 *   cmake --build build-debug && ctest --test-dir build-debug
 *
 * One test function per module, assert-based (bite 43's pattern).
 * The tests target FUNCTIONS, not the UI, and run against a temp file
 * under /tmp so they never touch the user's real contacts.txt.
 *
 * BREAK IT ON PURPOSE: flip an expected value anywhere below, rebuild,
 * ctest - the suite goes red naming the exact assert. Its value IS
 * the confidence of its red.
 */

/* getpid() is POSIX, not ISO C - say so explicitly so the header
 * declares it even under -std=c17 (strict ANSI mode). */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "contact.h"
#include "storage.h"
#include "strbuf.h"
#include "util.h"

static void test_contact(void)
{
    contactbook_t b;
    book_init(&b);

    /* adds + queries */
    assert(book_add(&b, "Alice", "555-0101") == CB_OK);
    assert(book_add(&b, "Bob", "555-0102") == CB_OK);
    assert(b.count == 2);
    assert(book_find(&b, "Alice") == 0);
    assert(book_find(&b, "Bob") == 1);
    assert(book_find(&b, "Nobody") == CB_ERR_NOTFOUND);

    /* failure paths */
    assert(book_add(&b, "Alice", "999") == CB_ERR_DUP);
    assert(b.count == 2);                       /* all-or-nothing */
    assert(book_add(&b, "", "555") == CB_ERR_ARG);
    assert(book_add(&b, NULL, "555") == CB_ERR_ARG);

    /* truncation instead of overflow: a 300-char name must not smash
     * the stack or the next struct field */
    char huge[300];
    memset(huge, 'x', sizeof huge - 1);
    huge[sizeof huge - 1] = '\0';
    assert(book_add(&b, huge, "555") == CB_OK);
    assert(strlen(b.items[b.count - 1].name) == 31);

    /* remove: head, middle, tail, missing (the three-cases habit) */
    assert(book_remove(&b, "Alice") == CB_OK);
    assert(book_remove(&b, "Alice") == CB_ERR_NOTFOUND);
    assert(b.count == 2);

    /* sort (bite 32's qsort comparator) */
    assert(book_add(&b, "Zack", "555-0105") == CB_OK);
    assert(book_add(&b, "Carol", "555-0103") == CB_OK);
    book_sort(&b);
    assert(strcmp(b.items[0].name, "Bob") == 0);
    /* the 31-'x' name sorts last: lowercase 'x' > 'Z' in ASCII, so
     * strcmp orders "Zack" before "xxx..." - uppercase first */
    assert(b.items[b.count - 1].name[0] == 'x');
    assert(strcmp(b.items[b.count - 2].name, "Zack") == 0);

    book_free(&b);
    printf("contact: OK\n");
}

static void test_storage(void)
{
    /* unique temp path so parallel test runs can't collide */
    char path[128];
    snprintf(path, sizeof path, "/tmp/cb44_test_%d.txt", (int)getpid());

    /* ---- round-trip ---- */
    contactbook_t src;
    book_init(&src);
    assert(book_add(&src, "Alice", "555-0101") == CB_OK);
    assert(book_add(&src, "Bob", "555-0102") == CB_OK);
    assert(book_add(&src, "Carol", "555-0103") == CB_OK);
    assert(storage_save(&src, path) == CB_OK);

    contactbook_t dst;
    book_init(&dst);
    assert(storage_load(&dst, path, NULL) == CB_OK);
    assert(dst.count == 3);
    assert(strcmp(dst.items[0].name, "Alice") == 0
           && strcmp(dst.items[0].phone, "555-0101") == 0);
    assert(strcmp(dst.items[2].name, "Carol") == 0);

    /* ---- dirty file: malformed lines skipped, good ones kept ---- */
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    fputs("Alice;555-0101\n"
          "no-semicolon-here\n"          /* malformed      */
          "\n"                            /* blank: harmless */
          ";555-0102\n"                   /* empty name     */
          "Bob;555-0102",                 /* truncated tail: still parsed */
          f);
    fclose(f);

    contactbook_t dirty;
    book_init(&dirty);
    size_t skipped = 99;
    assert(storage_load(&dirty, path, &skipped) == CB_OK);
    assert(dirty.count == 2);
    assert(skipped == 2);

    /* ---- empty file: OK, zero contacts ---- */
    f = fopen(path, "w");
    fclose(f);
    contactbook_t empty;
    book_init(&empty);
    assert(storage_load(&empty, path, NULL) == CB_OK);
    assert(empty.count == 0);

    /* ---- missing file: CB_ERR_IO, errno preserved (ENOENT) ---- */
    contactbook_t none;
    book_init(&none);
    assert(storage_load(&none, "/tmp/cb44_no_such_file_at_all.txt", NULL)
           == CB_ERR_IO);

    /* NULL arguments are rejected, not dereferenced */
    assert(storage_save(NULL, path) == CB_ERR_ARG);
    assert(storage_save(&src, NULL) == CB_ERR_ARG);

    remove(path);
    book_free(&src);
    book_free(&dst);
    book_free(&dirty);
    book_free(&empty);
    book_free(&none);
    printf("storage: OK\n");
}

static void test_strbuf(void)
{
    strbuf sb;
    sb_init(&sb);
    assert(sb_append(&sb, "hello") == 0);
    assert(sb_append(&sb, ", world") == 0);
    assert(sb.len == 12 && strcmp(sb.data, "hello, world") == 0);

    for (int i = 0; i < 50; i++) {
        char chunk[8];
        snprintf(chunk, sizeof chunk, ";%d", i);
        assert(sb_append(&sb, chunk) == 0);
    }
    /* after the loop: "hello, world;0;1;...;49" - start and end intact */
    assert(strncmp(sb.data, "hello, world", 12) == 0);
    assert(strcmp(sb.data + sb.len - 2, "49") == 0);

    sb_free(&sb);
    assert(sb.data == NULL);
    sb_free(&sb);                       /* double-free safe */
    printf("strbuf: OK\n");
}

static void test_util(void)
{
    /* logging channel: call both (output is a side effect; the real
     * assertion is that they exist, compile, and don't crash) */
    log_info("util test running");

    assert(parse_hex_byte("1A") == 26);
    assert(parse_hex_byte("G1") == -1);
    assert(parse_hex_byte("") == -1);
    assert(parse_hex_nibble('f') == 15);
    assert(parse_hex_nibble('z') == -1);

    for (uint32_t v = 0; v <= 0xFFFF; v++) {
        uint8_t b[2];
        put_u16be(b, (uint16_t)v);
        assert(get_u16be(b) == v);
    }
    uint8_t b[4];
    put_u32be(b, 0x12345678u);
    assert(b[0] == 0x12 && b[3] == 0x78);
    assert(get_u32be(b) == 0x12345678u);
    printf("util: OK\n");
}

int main(void)
{
    test_contact();
    test_storage();
    test_strbuf();
    test_util();
    printf("all tests passed\n");
    return 0;
}
