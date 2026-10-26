/*
 * Bite 39 — gdb basics: debug instead of guess.
 *
 * BUILD:  gcc -Wall -Wextra -Werror -std=c17 -g -o prog crashy.c
 *         (the -g flag embeds debug symbols; without it gdb shows
 *          only addresses, not your names and lines)
 * RUN:    ./prog               normal demo: adds 3 contacts, lists them
 *         ./prog fill          adds 100 contacts (for the conditional
 *                              breakpoint `break contact_add if count == 99`)
 *         ./prog doublefree    planted bug #1 -> abort() in free()
 *         ./prog nullderef     planted bug #2 -> SIGSEGV, NULL deref
 *
 * ============ GDB CHEATSHEET (the exact commands from the lesson) ====
 *
 *   gdb ./prog                       start gdb on the binary
 *   gdb --args ./prog doublefree     ...passing program arguments
 *   break contact_add                breakpoint at a function's entry
 *   break crashy.c:52                breakpoint at a line number
 *   break contact_add if book->count == 99   conditional breakpoint:
 *                                    fires only when the condition holds
 *   run                              (re)start the program under gdb
 *   next                    (n)      step OVER a call (stay in this frame)
 *   step                    (s)      step INTO a call
 *   print *c                (p)      print a struct - gdb knows its fields
 *   print book->items@5              print first 5 elements of an array
 *   print book->count                print an expression
 *   display i                        auto-print i after EVERY step
 *   undisplay i                      stop auto-printing
 *   bt                               backtrace: the stack of calls;
 *                                    the TOP frame is (usually) your bug
 *   frame 1                          move up the backtrace to inspect
 *   continue                (c)      resume until next breakpoint/crash
 *   quit                    (q)      leave gdb
 *
 * ============ GUIDED SESSIONS (try them in order) ====================
 *
 * Session A - watch a struct fill in (the bite's Do item 1):
 *   gdb ./prog
 *   (gdb) break contact_add
 *   (gdb) run
 *   (gdb) next          # step 1: memset zeroed the struct
 *   (gdb) print c       # name/phone all-zero right now
 *   (gdb) next          # step 2: name copied
 *   (gdb) print c       # name is filled, phone still zero
 *   (gdb) next          # step 3: phone copied
 *   (gdb) next          # step 4: struct stored, count bumped
 *   (gdb) print book->count
 *   (gdb) continue      # same breakpoint fires again for Bob...
 *
 * Session B - conditional breakpoint (Do item 3):
 *   gdb --args ./prog fill
 *   (gdb) break contact_add if book->count == 99
 *   (gdb) run           # runs through 99 adds silently, stops at #100
 *   (gdb) print book->count   # 99 - the "book full" case caught live
 *
 * Session C - diagnose the NULL deref (observed output, real lines):
 *   gdb --args ./prog nullderef
 *   (gdb) run
 *   Program received signal SIGSEGV, Segmentation fault.
 *   __strlen_avx2 () at ../sysdeps/.../strlen-avx2.S:76
 *   (gdb) bt
 *     #0  __strlen_avx2 () ...                      <- inside libc's printf
 *     ...
 *     #4  print_contact (c=0x0) at crashy.c:151
 *     #5  crash_null_deref () at crashy.c:157
 *     #6  main (argc=2, argv=...) at crashy.c:169
 *   (gdb) frame 4       # the crash is inside printf, but c=0x0 tells
 *                       # the story; the BUG is frame 5, which passed
 *                       # NULL without a check
 *   Fun fact you'll observe: the FIRST printf prints "name: (null)" -
 *   glibc quietly renders a NULL %s as "(null)" (an accident, not a
 *   guarantee). The segfault comes from the second field, whose
 *   address is 0x8: reading address 8 = NULL deref, just later.
 *
 * Session D - diagnose the double free:
 *   gdb --args ./prog doublefree
 *   (gdb) run
 *   free(): double free detected in tcache 2   <- glibc aborts loudly
 *   Program received signal SIGABRT, Aborted.
 *   (gdb) bt           # top frames are inside glibc; walk up to YOUR line:
 *     ...
 *     #9  crash_double_free () at crashy.c:140    <- the second free()
 *     #10 main (argc=2, argv=...) at crashy.c:165
 *
 * BREAK IT ON PURPOSE: the two planted bugs above ARE the exercise:
 *   1. the lesson's `free(c->name); free(c->name);` - crash_double_free()
 *   2. a NULL-pointer deref - crash_null_deref()
 * Run each under gdb, get the crash, `bt` to the exact line. Diagnosis
 * speed ("it crashes somewhere" -> "this line, this value" in under 2
 * minutes) is the skill this bite exists to build.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CONTACTS 100

typedef struct {
    char name[32];
    char phone[16];
} contact_t;

typedef struct {
    contact_t items[MAX_CONTACTS];
    size_t count;
} book_t;

/* Written in small single steps ON PURPOSE: under gdb you can `next`
 * line by line and watch the struct fill in (Session A above). */
static int contact_add(book_t *book, const char *name, const char *phone)
{
    if (book->count >= MAX_CONTACTS)
        return -1;          /* try: break contact_add if book->count == 99 */

    contact_t c;
    memset(&c, 0, sizeof c);                             /* step 1 */
    snprintf(c.name, sizeof c.name, "%s", name);         /* step 2 */
    snprintf(c.phone, sizeof c.phone, "%s", phone);      /* step 3 */
    book->items[book->count++] = c;                      /* step 4 */
    return 0;
}

static void list_contacts(const book_t *book)
{
    for (size_t i = 0; i < book->count; i++)
        printf("%zu. %s : %s\n", i + 1, book->items[i].name, book->items[i].phone);
    printf("count = %zu\n", book->count);
}

/* ---------------- planted bug #1: double free ----------------------
 * The lesson's exact pattern: the same pointer handed to free() twice.
 * glibc's allocator detects it and calls abort() -> SIGABRT. Under
 * gdb, `bt` walks through the allocator frames up to your line.
 *
 * `volatile` on the pointer is NOT part of the bug and not good
 * practice - it is how you keep deliberately-broken teaching code
 * compilable under -Wall -Werror: gcc 13's -Wuse-after-free can prove
 * `name` still points at the freed block, but a volatile variable
 * might have changed, so it must stay silent. At RUNTIME nothing
 * changed: the same pointer is freed twice, exactly as the lesson
 * plants it. */
static void crash_double_free(void)
{
    char *volatile name = malloc(32);
    if (name == NULL)
        return;
    snprintf(name, 32, "borrowed name");

    free(name);                 /* first free: fine */
    printf("freed once; now freeing the SAME pointer again...\n");
    free(name);                 /* boom: "free(): double free detected" */
    printf("this line is never reached\n");
}

/* ---------------- planted bug #2: NULL deref -----------------------
 * The crash (SIGSEGV reading address 0x0) happens in print_contact,
 * but the BUG is in crash_null_deref: passing NULL without a check.
 * gdb's backtrace shows you both - that's the lesson of `bt`. */
static void print_contact(const contact_t *c)
{
    printf("name: %s\n", c->name);      /* crash site: c == NULL */
    printf("phone: %s\n", c->phone);
}

static void crash_null_deref(void)
{
    printf("about to print a contact that was never created...\n");
    print_contact(NULL);                /* bug site: no NULL check */
}

int main(int argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "demo";

    if (strcmp(mode, "doublefree") == 0) {
        crash_double_free();
        return 0;
    }
    if (strcmp(mode, "nullderef") == 0) {
        crash_null_deref();
        return 0;
    }

    book_t book;
    memset(&book, 0, sizeof book);

    if (strcmp(mode, "fill") == 0) {
        char name[32];
        for (int i = 0; i < MAX_CONTACTS; i++) {
            snprintf(name, sizeof name, "person%03d", i);
            contact_add(&book, name, "555-0000");
        }
    } else {
        contact_add(&book, "Alice", "555-0101");
        contact_add(&book, "Bob",   "555-0102");
        contact_add(&book, "Carol", "555-0103");
    }

    list_contacts(&book);
    return 0;
}
