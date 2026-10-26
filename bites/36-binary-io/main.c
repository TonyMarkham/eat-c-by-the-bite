/*
 * Bite 36 — Binary I/O: fwrite/fread a struct array, text vs binary.
 *
 * BUILD:  gcc -Wall -Wextra -Werror -std=c17 -g -o prog main.c
 * RUN:    ./prog
 *
 * Learn-bullets this file demonstrates:
 *  - fwrite(&c, sizeof c, 1, f) dumps RAW bytes: fast and exact on the
 *    machine that wrote it, but fragile across compilers/platforms
 *    (padding! endianness! - bite 42 is the cure).
 *  - Binary files are not human-readable or diffable. Look at the
 *    hexdump this program prints: your data is right there in the raw
 *    bytes, but nothing about the file is self-describing.
 *  - fread's return value = items ACTUALLY read. Less than requested
 *    means EOF or error. Check it. Check the count field too.
 *
 * What the program does:
 *   1. saves a contact book as binary (count first, then structs)
 *   2. loads it back -> round-trip test
 *   3. saves the same data as text and compares file sizes
 *   4. hexdumps the first bytes so you can see your names in the raw
 *   5. corrupts a COPY of the file (one byte!) and reloads it:
 *        - a flipped byte inside a name  -> SILENT garbage (scary!)
 *        - a flipped byte in the count   -> detected by sanity check
 *
 * BREAK IT ON PURPOSE: the lesson's version is manual:
 *     printf '\x58' | dd of=contacts.bin bs=1 seek=50 count=1 conv=notrunc
 * then reload - the 3rd contact's name is quietly garbled. This program
 * automates the same experiment in corrupt_demos() so you can see both
 * failure modes side by side: silent data corruption vs a detected
 * impossible count. Binary formats trust you completely; that is their
 * danger. A magic number / version header / checksum is how real
 * formats defend themselves - try adding one as an exercise.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAX_CONTACTS 100

/* 32 + 16 = 48 bytes, and because every member is a char array the
 * compiler inserts NO padding here. Add one `int age;` and sizeof
 * grows beyond 52 with unspecified padding bytes - which fwrite would
 * then write raw, making the file compiler-dependent. Bite 42 covers
 * this properly; for now, know that plain char arrays are the safe case. */
typedef struct {
    char name[32];
    char phone[16];
} contact;

/* ------------------------------------------------------------------ */
/* Binary save/load: count first, then structs                          */
/* ------------------------------------------------------------------ */

/* Format: [uint32 count][count x contact structs, raw].
 * Returns 0 on success, -1 on failure. */
static int save_binary(const char *path, const contact *items, size_t n)
{
    FILE *f = fopen(path, "wb");         /* the 'b' is ignored on Linux,
                                            required by the standard on
                                            other systems - always write it */
    if (f == NULL) {
        perror(path);
        return -1;
    }

    int failed = 0;
    uint32_t count = (uint32_t)n;
    /* fwrite returns the number of ITEMS written; anything less than
     * requested is an error. glibc marks fwrite warn_unused_result,
     * so ignoring it would not even compile under -Werror. */
    if (fwrite(&count, sizeof count, 1, f) != 1)
        failed = 1;
    for (size_t i = 0; i < n && !failed; i++)
        if (fwrite(&items[i], sizeof items[i], 1, f) != 1)
            failed = 1;                  /* (bulk form: fwrite(items, sizeof items[0], n, f)) */

    if (fclose(f) != 0)
        failed = 1;
    if (failed)
        fprintf(stderr, "%s: write failed\n", path);
    return failed ? -1 : 0;
}

/* Load a file written by save_binary.
 * Returns the number of contacts loaded, or -1 on any problem. */
static long load_binary(const char *path, contact *out, size_t max)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        perror(path);
        return -1;
    }

    uint32_t count = 0;
    if (fread(&count, sizeof count, 1, f) != 1) {
        fprintf(stderr, "%s: too short to even hold a count\n", path);
        fclose(f);
        return -1;
    }

    /* Sanity-check the count BEFORE trusting it with allocation or
     * reads. This is the one corruption the format can catch: a count
     * of 4 billion is not a file that "almost worked", it is garbage. */
    if (count > max) {
        fprintf(stderr, "%s: implausible count %u (corrupt file?)\n", path, count);
        fclose(f);
        return -1;
    }

    /* fread returns items actually read. Fewer than requested = EOF or
     * error: the count promised more structs than the file contains. */
    for (size_t i = 0; i < count; i++) {
        if (fread(&out[i], sizeof out[i], 1, f) != 1) {
            fprintf(stderr, "%s: truncated: count says %u, file holds %zu\n",
                    path, count, i);
            fclose(f);
            return -1;
        }
    }

    fclose(f);
    return (long)count;
}

/* ------------------------------------------------------------------ */
/* Text version (same data) for the size comparison                    */
/* ------------------------------------------------------------------ */

static long save_text(const char *path, const contact *items, size_t n)
{
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        perror(path);
        return -1;
    }
    for (size_t i = 0; i < n; i++)
        fprintf(f, "%s;%s\n", items[i].name, items[i].phone);
    fclose(f);
    return (long)n;
}

/* File size via fseek/ftell: seek to the end, ask where we are. */
static long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    return size;
}

/* ------------------------------------------------------------------ */
/* Hexdump: see through the machine                                    */
/* ------------------------------------------------------------------ */

/* offset | 16 hex bytes | ASCII column, '.' for unprintables.
 * Your contact names are plainly visible in the raw bytes. Binary
 * files only hide data from people who never look. */
static void hexdump(const void *data, size_t len)
{
    const unsigned char *p = data;
    for (size_t off = 0; off < len; off += 16) {
        printf("%04zx  ", off);
        for (size_t i = 0; i < 16; i++) {
            if (off + i < len)
                printf("%02x ", p[off + i]);
            else
                printf("   ");
        }
        printf(" ");
        for (size_t i = 0; i < 16 && off + i < len; i++) {
            unsigned char c = p[off + i];
            putchar((c >= 32 && c < 127) ? c : '.');
        }
        putchar('\n');
    }
}

/* Read a whole file into a malloc'd buffer (for the corruption demo). */
static unsigned char *slurp(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        perror(path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    unsigned char *buf = malloc((size_t)size ? (size_t)size : 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "%s: short read\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)size;
    return buf;
}

/* ------------------------------------------------------------------ */
/* BREAK IT ON PURPOSE, automated: two corruption modes                */
/* ------------------------------------------------------------------ */

static void corrupt_demos(const char *src)
{
    size_t len = 0;
    unsigned char *buf = slurp(src, &len);
    if (buf == NULL)
        return;

    /* Layout reminder: [4-byte count][contact 0][contact 1]... where
     * each contact is 48 bytes: name[32] then phone[16]. */

    /* Demo A: flip one byte INSIDE the 3rd contact's name -> silent
     * garbage. Nothing detects this: the format has no checksum, the
     * loader has no expectations about name contents. */
    size_t name_byte = 4 + 2 * sizeof(contact) + 2;   /* inside "Carol" -> "CaXol" */
    printf("\n-- corrupt demo A: flip byte %zu (inside a name) --\n", name_byte);
    buf[name_byte] = (unsigned char)'X';
    FILE *f = fopen("corrupt_name.bin", "wb");
    if (f) {
        fwrite(buf, 1, len, f);
        fclose(f);
    }
    contact reloaded[MAX_CONTACTS];
    long n = load_binary("corrupt_name.bin", reloaded, MAX_CONTACTS);
    printf("load returned %ld (SUCCESS - the corruption is silent)\n", n);
    if (n > 2)
        printf("contact 2 name is now: \"%s\"  <-- garbage nobody warned you about\n",
               reloaded[2].name);

    free(buf);

    /* Demo B: flip a HIGH bit in the count -> impossible count,
     * caught by the sanity check. This is what a magic number /
     * checksum would generalize to. */
    buf = slurp(src, &len);              /* re-read the PRISTINE file */
    if (buf == NULL)
        return;
    printf("\n-- corrupt demo B: flip a bit in the count field --\n");
    buf[3] |= 0x40;                      /* count 5 -> 0x40000005 */
    f = fopen("corrupt_count.bin", "wb");
    if (f) {
        fwrite(buf, 1, len, f);
        fclose(f);
    }
    load_binary("corrupt_count.bin", reloaded, MAX_CONTACTS);   /* detected! */
    free(buf);
}

int main(void)
{
    contact book[5] = {
        { "Alice", "555-0101" },
        { "Bob",   "555-0102" },
        { "Carol", "555-0103" },
        { "Dave",  "555-0104" },
        { "Eve",   "555-0105" },
    };

    /* 1 + 2. binary round-trip */
    if (save_binary("contacts.bin", book, 5) != 0)
        return 1;
    contact reloaded[MAX_CONTACTS];
    long n = load_binary("contacts.bin", reloaded, MAX_CONTACTS);
    printf("binary round-trip: loaded %ld contacts\n", n);
    int ok = (n == 5);
    for (long i = 0; i < n && ok; i++)
        if (strcmp(book[i].name, reloaded[i].name) != 0
            || strcmp(book[i].phone, reloaded[i].phone) != 0)
            ok = 0;
    printf("binary round-trip: %s\n", ok ? "OK" : "MISMATCH!");

    /* 3. text vs binary size for the SAME data */
    save_text("contacts.txt", book, 5);
    long bin = file_size("contacts.bin");
    long txt = file_size("contacts.txt");
    printf("\nsizes for 5 contacts:  binary = %ld bytes,  text = %ld bytes\n",
           bin, txt);
    printf("(binary wins on size here; text wins on diffability,\n");
    printf(" debuggability and surviving format drift - argue it per use case)\n");

    /* 4. see your names in the raw bytes */
    size_t len = 0;
    unsigned char *raw = slurp("contacts.bin", &len);
    if (raw) {
        printf("\nhexdump of contacts.bin (first 96 bytes):\n");
        hexdump(raw, len < 96 ? len : 96);
        printf("(find \"Alice\" at offset 4 - right after the 4-byte count)\n");
        free(raw);
    }

    /* 5. corruption demos */
    corrupt_demos("contacts.bin");

    printf("\nTry the manual version too:\n");
    printf("  xxd contacts.bin | head\n");
    printf("  printf '\\x58' | dd of=contacts.bin bs=1 seek=50 count=1 conv=notrunc\n");
    return ok ? 0 : 1;
}
