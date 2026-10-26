/*
 * Bite 31 — Function Pointers: behavior in variables + a dispatch table
 *
 * COMPILE & RUN (from the repo root, inside WSL):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/31-function-pointers/main.c -o /tmp/b31 && /tmp/b31
 *   valgrind --leak-check=full --show-leak-kinds=all /tmp/b31   → nothing to leak, 0 errors
 *
 * BREAK IT ON PURPOSE: call through a function pointer you never assigned —
 * a wild jump into garbage (here: NULL). Build the broken variant:
 *   gcc -Wall -Wextra -Werror -std=c17 -g -DBREAK_NULL_CALL bites/31-function-pointers/main.c -o /tmp/b31-bad
 *   /tmp/b31-bad            → SIGSEGV, exit 139 (observed). Under valgrind:
 *                             "Jump to the invalid address stated on the next line".
 * Lesson: an unassigned function pointer is exactly as dangerous as an
 * unassigned data pointer. NULL-check function pointers in dispatch tables
 * from now on — calc() below does.
 *
 * WHY (the "Learn" bullets):
 *   - int (*op)(int, int) — a pointer to a function taking two ints and
 *     returning int. Call it as op(3, 4) or (*op)(3, 4) — identical.
 *   - Functions decay to pointers like arrays do; that's why qsort can take
 *     a bare function name as its comparator argument.
 *   - Table-driven dispatch: an array of {name, fn} pairs. Add a new
 *     operation by adding a ROW, not a case. `mod` below required zero
 *     changes to the dispatch code. This is the architecture of real servers
 *     — and of the WebRTC backend's message dispatch in Phase 7.
 */

#include <stdio.h>
#include <string.h>

/* ---- the operations: plain functions with a common signature ------------ */

static int add(int a, int b) { return a + b; }
static int sub(int a, int b) { return a - b; }
static int mul(int a, int b) { return a * b; }
static int divide(int a, int b) { return b == 0 ? 0 : a / b; }   /* no crash in a demo */
static int modulo(int a, int b) { return b == 0 ? 0 : a % b; }

/* ---- the dispatch table: {name, fn} pairs -------------------------------
 * `mod` was added as exactly one row here — nothing else in the file changed.
 * In the checkpoint-1 calculator this same idea took a switch with a case
 * per operation; the switch grew a new case every time. Tables don't. */
typedef struct {
    const char *name;
    int (*fn)(int, int);
} op_t;

static const op_t OPS[] = {
    { "add", add },
    { "sub", sub },
    { "mul", mul },
    { "div", divide },
    { "mod", modulo },      /* <- added this bite; dispatch code untouched */
};

/* calc: find the row by name, call through the pointer.
 * Defensive NULL check on fn: a table row with a NULL fn (or a lookup miss)
 * must be an error message, not a wild jump — see BREAK_NULL_CALL. */
static int calc(int a, const char *opname, int b)
{
    for (size_t i = 0; i < sizeof OPS / sizeof OPS[0]; i++) {
        if (strcmp(OPS[i].name, opname) == 0) {
            if (OPS[i].fn == NULL) {           /* paranoia that pays rent */
                fprintf(stderr, "op '%s' has no function\n", opname);
                return 0;
            }
            return OPS[i].fn(a, b);            /* the indirect call */
        }
    }
    fprintf(stderr, "unknown op '%s'\n", opname);
    return 0;
}

/* ---- map a unary function over an array (map, the C way) ---------------- */

static void apply(int *a, size_t n, int (*fn)(int))
{
    for (size_t i = 0; i < n; i++)
        a[i] = fn(a[i]);
}

static int doubler(int x) { return x * 2; }
static int negator(int x) { return -x; }

int main(void)
{
    /* ---- 1) table-driven calculator ------------------------------------- */
    struct { int a, b; const char *op; } calls[] = {
        { 7,  2, "add" },
        { 7,  2, "sub" },
        { 7,  2, "mul" },
        { 7,  2, "div" },
        { 7,  2, "mod" },      /* worked on the first try — zero dispatch changes */
        { 1,  0, "div" },      /* divide-by-zero handled inside the fn */
        { 1,  0, "xor" },      /* unknown op: clean error, no crash */
    };
    for (size_t i = 0; i < sizeof calls / sizeof calls[0]; i++)
        printf("%d %s %d = %d\n",
               calls[i].a, calls[i].op, calls[i].b,
               calc(calls[i].a, calls[i].op, calls[i].b));

    /* ---- 2) apply(): the same idea with one int -------------------------- */
    int nums[] = { 1, 2, 3, 4 };
    size_t n = sizeof nums / sizeof nums[0];

    apply(nums, n, doubler);
    printf("after doubler:");
    for (size_t i = 0; i < n; i++)
        printf(" %d", nums[i]);
    printf("\n");

    apply(nums, n, negator);
    printf("after negator:");
    for (size_t i = 0; i < n; i++)
        printf(" %d", nums[i]);
    printf("\n");

    /* Function names decay to pointers: `doubler` above IS &doubler. Same
     * decay rule as arrays — and it's exactly how qsort takes comparators
     * in bite 32. */

#ifdef BREAK_NULL_CALL
    /* ---- BREAK IT: the wild call ---------------------------------------- */
    /* volatile denies the compiler proof that this is NULL (a provable NULL
     * call is UB it may warn about or fold); at runtime it still jumps to
     * address 0 and the process dies. valgrind would report
     * "Jump to the invalid address stated on the next line" / SIGSEGV. */
    int (*volatile wildcard)(int, int) = NULL;
    printf("about to call through a never-assigned function pointer...\n");
    fflush(stdout);                        /* make sure the message survives the crash */
    printf("got %d\n", wildcard(2, 3));    /* SIGSEGV — this line never prints */
#endif

    return 0;
}
