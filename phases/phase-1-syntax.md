# Phase 1 — The Beak Full of Sand: Syntax (Bites 1–10)

*Goal: read simple C without fear. Zero pointers yet.*

Lesson files for every bite. Each bite = one sitting (30–90 min).

---

## Bite 1 — Hello World, Properly

> **Goal:** Compile and run a program by hand, and understand every character of it.
>
> **Learn:**
> - The anatomy: `#include <stdio.h>` (preprocessor pulls in declarations), `int main(void)` (entry point), `printf` (library call), `return 0` (process exit code — check with `echo $?`).
> - Compilation is two steps: compile to object code, then link. `gcc -c hello.c` produces `hello.o`; look at it with `ls`.
>
> **Do:**
> 1. Type (don't paste) hello world, compile with `gcc -Wall -Wextra -Werror -std=c17 -g hello.c -o hello`, run `./hello`.
> 2. Run `echo $?` after it — confirm it prints `0`.
> 3. Add a second `printf` printing your name, recompile, rerun.
> 4. Write in your notes: what does each token on line 1 and line 2 do?
>
> **Break it on purpose:** Delete `return 0;`. Does it still compile and run? (Yes — C99+ implies it for `main`.) Now change `main` to return `7` and check `echo $?`. Exit codes are how programs talk to other programs — remember this for Phase 5.
>
> **Done when:** You can compile from a cold terminal without looking anything up, and you can explain `#include`, `main`, and exit codes in your own words.

## Bite 2 — Variables, Types, printf

> **Goal:** Know the basic types cold and control output formatting.
>
> **Learn:**
> - Types: `char` (1 byte), `short`, `int`, `long`, `long long`, `float`, `double`. Signed vs unsigned.
> - `sizeof` is an operator, not a function. Sizes are platform-dependent; only `sizeof(char) == 1` is guaranteed.
> - Format specifiers: `%d %u %ld %lld %zu %f %c %s %p %x`. Mismatched specifier = undefined behavior, not an error.
>
> **Do:**
> 1. Print a table: one row per type → `sizeof` → a sample value with the right specifier.
> 2. Declare an `int` and print its address with `printf("%p\n", (void*)&i);`.
> 3. Demonstrate unsigned wraparound: `unsigned u = 0; u -= 1; printf("%u\n", u);` — predict before running.
>
> **Break it on purpose:** Print an `int` with `%f` and a `double` with `%d`. With `-Wall` you get warnings; with `-Werror` the build fails. Then delete `-Werror`, watch the garbage output. This is why warnings-as-errors is day-one policy.
>
> **Done when:** Your type table prints without warnings, and you can explain why `unsigned` wraparound isn't a "bug" in C's eyes.

## Bite 3 — Operators & Precedence

> **Goal:** Never be surprised by integer math or `=` vs `==` again.
>
> **Learn:**
> - Integer division truncates: `7/2 == 3`. `%` is remainder. Mixed types promote: `7/2.0 == 3.5`.
> - Precedence table exists; you don't memorize it, you use parentheses.
> - `=` (assignment) returns a value — the root of the classic `if (x = 5)` bug.
> - `i++` vs `++i` (post returns old value), and why "sequence points" make `i = i++ + 1` undefined.
>
> **Do:**
> 1. Compute compound interest: principal 1000, 5% yearly, print balance for years 1–10 (no loops — write the expression 10 times, or with a helper function).
> 2. Write `int avg(int a, int b)` that returns the average. Test with `(1,2)` and `(INT_MAX, INT_MAX)`. The second result is your first taste of integer overflow.
> 3. Print `7/2`, `7%2`, `7.0/2`, `(double)7/2` — four different answers' worth of education.
>
> **Break it on purpose:** Write `if (x = 5)` with `-Wall`. Read the warning (`suggest parentheses around assignment...`). This bug ships in production C every year; you've now seen it in a safe place.
>
> **Done when:** You can predict the output of `printf("%d %d\n", 7/2, 7%2);` and `(double)7/2` before running.

## Bite 4 — Control Flow: if/else, switch

> **Goal:** Branch correctly and know when `switch` beats `if`.
>
> **Learn:**
> - Truthiness: 0 is false, everything else is true. There is no boolean type in classic C — `stdbool.h` gives you `bool` as a convenience.
> - Comparison chains and short-circuit `&&` / `||`.
> - `switch` only works on integer types; `case` values must be constants; fall-through is the default (and `break` is the fix).
>
> **Do:**
> 1. FizzBuzz 1–100. Do it now, for real, even if you've done it before — in C, with a function `void fizzbuzz(int n)`.
> 2. Rewrite FizzBuzz with `switch (n % 15)` and constant cases. Feel which version is clearer.
> 3. Write `int clamp(int v, int lo, int hi)` and test the three boundaries.
>
> **Break it on purpose:** Remove one `break` from your switch. Feed it the case that falls through. Predict the output before running.
>
> **Done when:** FizzBuzz compiles clean under `-Werror` and you can state one situation where `switch` is clearly better than `if/else`.

## Bite 5 — Loops

> **Goal:** Control repetition fluently, including the early-exit patterns.
>
> **Learn:**
> - `for` = init; condition; step. All three are optional (`for(;;)` is an infinite loop).
> - `while` (check first) vs `do/while` (run at least once — rare but right for menus).
> - `break` exits the loop, `continue` skips to the next iteration.
> - Off-by-one errors: the #1 loop bug. `<` vs `<=` matters.
>
> **Do:**
> 1. Print a 10×10 multiplication table with aligned columns (`%4d`).
> 2. Sum integers from stdin until EOF or a negative number — one version with `while`, one with `for(;;) { ... break; }`.
> 3. Write `int sum_digits(int n)` with a loop. Test with `0`, `12345`, `-123` (decide what negative input should do and document it).
>
> **Break it on purpose:** Change a loop condition from `<` to `<=` in the table printer. Predict the corruption, then verify. Now write a loop that never executes (`for (i = 10; i < 5; i++)`) and confirm the body is skipped, not an error.
>
> **Done when:** You can write all three loop forms from memory and have been bitten by an off-by-one at least once today.

## Bite 6 — Functions

> **Goal:** Decompose programs into functions like a professional, not a student.
>
> **Learn:**
> - Declaration (prototype) vs definition. Prototypes let you call before defining and let other files use your function.
> - Arguments are **passed by value** — the function gets a copy. Changing a parameter never changes the caller's variable. (This is why pointers exist — Phase 2.)
> - `void` parameters vs empty `()` (legacy, means unspecified — never use it).
>
> **Do:**
> 1. Write `max3(a,b,c)` where the prototype appears *before* `main` and the definition *after*.
> 2. Write `void attempt_swap(int a, int b)` that swaps its parameters. Call it from `main` — prove the swap didn't happen. Write down why. This failure is the cliffhanger for bite 12.
> 3. Refactor bite 5's multiplication table into `print_table(int rows, int cols)`.
>
> **Break it on purpose:** Remove the prototype but keep the definition after `main`. With `-Werror` this fails (`implicit declaration`). You've just learned why headers exist.
>
> **Done when:** You can explain "pass by value" in one sentence and demonstrate it with the failed swap.

## Bite 7 — Scope & Lifetime

> **Goal:** Understand where variables live and how long they last.
>
> **Learn:**
> - Three kinds: automatic (stack, dies at `}`), `static` local (persists across calls, scope still local), global/file-scope (lives forever, shared by all functions — use sparingly).
> - `static` on a *global/function* means "private to this file" — the root of C's module privacy. (File-level in bite 37.)
> - Shadowing: an inner `int x` hides the outer `int x`. Legal, dangerous.
>
> **Do:**
> 1. Write `int next_id(void)` with a `static int counter = 0;` returning `++counter`. Call it 5 times — IDs increment.
> 2. Remove `static`, run again — every call returns 1. Note the difference.
> 3. Print addresses of a local, a static, and a global variable with `%p`. Observe they live in different regions (preview of bite 23).
>
> **Break it on purpose:** Return a pointer to a local variable (`int *f(void){ int x = 5; return &x; }`). Read the compiler warning — clang/gcc often catch this. You've just written a dangling-pointer bug on purpose; never again by accident.
>
> **Done when:** You can fill in this table from memory: *kind → where it lives → when it dies*.

## Bite 8 — The Preprocessor & Multi-File Compile

> **Goal:** Split a program into files and drive the build by hand.
>
> **Learn:**
> - `#include` is literal text pasting. `<...>` = system paths, `"..."` = your paths.
> - Header guards (`#ifndef X / #define X / #endif`) prevent double-inclusion.
> - The real build: `gcc -c mathutil.c` + `gcc -c main.c` → two `.o` files → link them together. Headers describe; sources define.
>
> **Do:**
> 1. Write `mathutil.h` (prototypes for `max3`, `clamp`, `sum_digits`) with guards, `mathutil.c` (definitions), `main.c` (user).
> 2. Compile in three explicit steps: two `-c` runs, then link `gcc main.o mathutil.o -o app`.
> 3. Add `#define TABLE_MAX 10` in the header and use it in `main.c`.
>
> **Break it on purpose:** Remove the header guard, then include `mathutil.h` twice in `main.c`. Read the "redefinition" error. Add the guard back — bug immunized forever.
>
> **Done when:** You can build a 3-file program from a cold terminal, and explain the difference between a declaration and a definition.

## Bite 9 — Input Without Footguns

> **Goal:** Read input safely and survive garbage.
>
> **Learn:**
> - Raw `scanf("%s", buf)` is a buffer overflow with a smiley face. The professional pattern: `fgets` into a buffer, then parse with `sscanf`.
> - `fgets` keeps the `\n`. Deal with it.
> - `sscanf`'s return value = number of items successfully converted. Check it; it's your validation.
>
> **Do:**
> 1. Read a line into `char buf[64]`, strip the trailing newline (find it first).
> 2. Parse two integers with `sscanf(buf, "%d %d", &a, &b)`; if the return isn't 2, print "invalid input" and re-prompt.
> 3. Feed it: `3 4`, `3`, `abc`, `3 abc`, `99999999999999999` (overflow!). Handle or at least *observe* each.
>
> **Break it on purpose:** Type 80 characters at your 64-char buffer with `fgets` — nothing overflows (that's the point). Then imagine the same program with `scanf("%s")` and note what would happen.
>
> **Done when:** A garbage line can't crash or confuse your program; it just re-prompts.

## Bite 10 — ✅ Checkpoint: Number Cruncher

> **Goal:** First finished program. Everything from bites 1–9 in one place.
>
> **Do:**
> Build a menu-driven calculator:
> - Loop with `do/while` printing: `[a]dd [s]ub [m]ul [d]iv [p]ow [q]uit`.
> - Read the choice with `fgets` (accept `a` or `add`).
> - Read two operands with your validated `fgets`+`sscanf` helper.
> - Functions: `add`, `sub`, `mul`, `idiv` (reject division by zero with an error message, not a crash), `ipow` (loop-based, no `pow`).
> - Dispatch with a `switch`. Division by zero and bad input must never crash or corrupt.
> - ~150 lines, split into `calc.c` + `main.c` if it's getting long.
>
> **Break it on purpose:** Before declaring done: enter `q` mid-calculation, enter EOF (Ctrl-D), enter garbage operands. If any of these crashes, the checkpoint isn't done.
>
> **Done when:** Clean build under `-Wall -Wextra -Werror`, survives 5 minutes of deliberate abuse, committed with message `checkpoint 1: number cruncher`. **Do not start Phase 2 until this is true.**
