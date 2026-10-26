# Phase 2 — The Trunk: Arrays, Strings, Pointers (Bites 11–22)

*Goal: pointers stop being magic. This is the phase where most people quit — go slow.*

Every bite in this phase should involve a pen and paper. Draw boxes and arrows
*before* writing code. If you can't draw it, you can't code it.

---

## Bite 11 — Arrays

> **Goal:** Use arrays and understand what silently happens when you pass them to functions.
>
> **Learn:**
> - `int a[10]` — contiguous memory, index 0–9, **no bounds checking**. Out-of-bounds is undefined behavior, not an error.
> - The big lie: when you pass an array to a function, what actually arrives is a pointer to the first element. `sizeof` inside the function is `sizeof(pointer)`, not the array size. That's why array-taking functions take a length parameter too.
> - Array initializers: `int a[3] = {1,2,3};`, `int a[] = {1,2,3};` (compiler counts), `{0}` zeros everything.
>
> **Do:**
> 1. Fill an array with squares, print it with a function `void print_ints(const int *a, size_t n)`.
> 2. Write `int sum(const int *a, size_t n)` and `int max_in(const int *a, size_t n)`.
> 3. In `main`, print `sizeof(a)` and `sizeof(a)/sizeof(a[0])`. Then print the same inside a function. Observe the lie in action.
>
> **Break it on purpose:** Write past the end: `a[10] = 42;` on a 10-element array. Run it — it probably "works" (UB often silently works). Compile with `-fsanitize=address` and run again. Now it screams. ASan is your always-on safety net from now on.
>
> **Done when:** You can explain why C functions that take arrays also take a length, and you've seen ASan catch your first out-of-bounds write.

## Bite 12 — First Pointers

> **Goal:** Demystify `&` and `*` with one diagram you draw yourself.
>
> **Learn:**
> - A pointer is a variable whose value is a memory address. `int *p = &x;` — read aloud as "p is a pointer to int, holding the address of x."
> - `&x` = address-of. `*p` = dereference (follow the arrow). Declaration `*` and expression `*` look identical but mean different things — this trips everyone once.
> - `NULL` is the "points to nothing" value. Dereferencing it = instant segfault (at least it's an *honest* crash).
>
> **Do:**
> 1. On paper: draw two boxes `x` and `p`, an arrow from `p` to `x`. Then write the code that matches the drawing.
> 2. Write `void swap(int *a, int *b)` and prove it works in `main`. Compare with bite 6's failed `attempt_swap` — same language, opposite outcome. *You now know why pointers exist.*
> 3. Print `p`, `&x`, `*p`, and `&p` — four different things. Label each in your notes.
>
> **Break it on purpose:** Declare `int *p;` without initializing, then `*p = 5;`. Run with ASan. Read the crash. uninitialized pointer = the most common segfault in human history; you've now met it under supervision.
>
> **Done when:** You can look at `int *p = &x; *p = 42;` and say exactly what memory changed.

## Bite 13 — Pointer Arithmetic

> **Goal:** Walk memory with pointers and understand why types matter to arithmetic.
>
> **Learn:**
> - `p + 1` advances by `sizeof(*p)` bytes, not 1 byte. Pointer arithmetic is typed.
> - `arr[i]` is *defined* as `*(arr + i)`. The subscript operator is pointer arithmetic in disguise.
> - `p - q` (same array) gives the element count between them (type `ptrdiff_t`, print with `%td`).
>
> **Do:**
> 1. Walk an array with only a pointer: `for (int *p = a; p < a + n; p++) printf("%d ", *p);`
> 2. Print addresses of `a[0]` through `a[4]` for an `int` array, then for a `char` array — see the stride difference.
> 3. Write `int *find(int *a, size_t n, int val)` returning a pointer to the match or `NULL`. Use it: `int *hit = find(...); if (hit) printf("at index %td\n", hit - a);`
>
> **Break it on purpose:** Add 5 to a pointer past the end of the array and *only print* through it (never dereference — that's allowed). Then dereference. Compare ASan's reaction.
>
> **Done when:** You can convert between `a[i]` and `*(a+i)` reflexively, and `find` works, returning `NULL` on miss.

## Bite 14 — Pointers as Out-Parameters

> **Goal:** Make functions return multiple results and understand "in/out" conventions.
>
> **Learn:**
> - Convention: `const T *in` = "I read this", `T *out` = "I write through this".
> - The caller must supply valid storage. The function must never write through a `const` pointer or leave an out-param unset on error.
> - Combined return+out pattern: `return 0/-1` for status, data via out-param.
>
> **Do:**
> 1. `void minmax(const int *a, size_t n, int *min, int *max)` — handle `n == 0` by writing nothing and documenting it.
> 2. `int parse_two_ints(const char *line, int *a, int *b)` — wraps `sscanf`, returns 0 on success, -1 on garbage. Use it in a tiny read-eval loop.
> 3. Rewrite bite 10's calculator input handling using this pattern.
>
> **Break it on purpose:** Call `minmax(vals, 5, NULL, &max);` if your implementation dereferences `min` without checking — crash. Decide: check for NULL, or document "must not be NULL"? Both are valid policies; being *consistent* is the skill.
>
> **Done when:** `parse_two_ints` rejects garbage without crashing and you can defend your NULL-policy choice.

## Bite 15 — Strings as char Arrays

> **Goal:** Understand that a C string is just a char array with a `'\0'` — nothing more.
>
> **Learn:**
> - `char s[6] = "hello";` stores 6 bytes: `h e l l o \0`. The terminator is *data*, not punctuation.
> - Every string function's correctness hinges on the terminator being present and in bounds.
> - `strlen` is O(n) — it walks to `\0`. There is no hidden length field.
>
> **Do:**
> 1. Write `size_t my_strlen(const char *s)` (walk to `\0`), `int my_strcmp(const char *a, const char *b)` (lexicographic, returns <0/0/>0), `char *my_strcpy(char *dst, size_t dstsz, const char *src)` (returns `dst`, never overruns).
> 2. Write a test harness: call each with 5 cases including empty string, and compare results against the real `strlen/strcmp` from `<string.h>`.
> 3. Declare `char s[5] = "hello";` — read the warning. Six bytes needed, five provided, terminator dropped. Count it on your fingers.
>
> **Break it on purpose:** Deliberately remove the `'\0'`: `char s[3] = {'a','b','c'};` then `printf("%s\n", s);` — observe the trailing garbage from whatever bytes follow. Undefined behavior doing its party trick.
>
> **Done when:** Your three functions pass all five tests, and you can explain what `sizeof s` vs `strlen(s)` gives for `char s[6] = "hello";`.

## Bite 16 — string.h and the Dangerous Half

> **Goal:** Use the standard string library — the safe variants.
>
> **Learn:**
> - The dangerous set: `strcpy`, `strcat`, `sprintf`, `gets` (removed from the language entirely — that's how dangerous).
> - The safe set: `strncpy` (still truncates without terminator — read the fine print!), `snprintf` (always terminates, returns needed length), `strncat`, `strtol` for numbers.
> - Rule of thumb: any function whose safety depends on the *caller's hope* is the wrong function.
>
> **Do:**
> 1. Copy strings with `snprintf(dst, sizeof dst, "%s", src);` — make this your default idiom.
> 2. Build `"Name: " + name + " (age N)"` into a fixed buffer with one `snprintf`; then deliberately overflow with a 100-char name and observe the truncation, not corruption.
> 3. Parse `"42"` and `"  42abc"` with `strtol` including its `endptr` — see exactly where conversion stopped.
>
> **Break it on purpose:** Use `strcpy` into a 16-byte buffer with a 40-byte string under ASan. Watch the stack-smash report. You've now seen the bug that powers half of all CVEs — in captivity.
>
> **Done when:** Your codebase contains zero calls to `strcpy/strcat/sprintf`, and you can explain `strncpy`'s missing-terminator trap.

## Bite 17 — 2D Arrays and Arrays of Strings

> **Goal:** Handle grids and lists of text.
>
> **Learn:**
> - `int grid[4][5]` — contiguous, row-major. `grid[i][j]` is `*(*(grid+i)+j)`.
> - Passing 2D arrays to functions requires the column count: `void f(int g[][5], size_t rows)`.
> - `char names[10][32]` = 10 strings × 31 chars + terminator each. Fixed-size but simple.
>
> **Do:**
> 1. Print a multiplication table into a 2D array first, then print from the array (not computed at print time).
> 2. Store 5 city names in `char names[5][32]`, print them numbered, find the longest.
> 3. Write `int longest_line(const char lines[][32], size_t n)` returning the index of the longest string.
>
> **Break it on purpose:** Access `grid[4][5]` (one past both dims) under ASan. Note that 2D arrays make it *easier* to wander off the end and that ASan still catches it.
>
> **Done when:** The city list works and you can state the memory layout of `char names[5][32]` in bytes.

## Bite 18 — Pointers to Pointers

> **Goal:** Read `char **` without flinching, starting with the most famous one: `argv`.
>
> **Learn:**
> - `char **argv` = a pointer to an array of pointers, each pointing to a string. Draw it: `argv` → `[ptr0, ptr1, ptr2, NULL]` → each ptr → a string.
> - `argv[argc]` is `NULL` by the standard.
> - Uses: modifying a pointer through a function (`void f(char **pp)`), 2D dynamic arrays (Phase 3), lists of strings.
>
> **Do:**
> 1. Write a program that prints `argc`, every `argv[i]`, and proves `argv[argc] == NULL`.
> 2. Write `void get_color(const char **out)` that assigns a string literal to `*out`. Understand why the pointer-to-pointer was needed (the callee must modify the *caller's* pointer).
> 3. Print `argv`'s own address, `argv[0]`'s value, and `argv[0][0]`'s value — three levels of indirection, three different numbers.
>
> **Break it on purpose:** Run your argv program with zero arguments, then `./prog one two "three four"`. Predict `argc` before each run. Quoting is the shell's job, not C's.
>
> **Done when:** You can draw `argv`'s full structure from memory, boxes and arrows.

## Bite 19 — Structs

> **Goal:** Bundle related data and pass it around correctly.
>
> **Learn:**
> - `struct point { double x; double y; };` — a template; `struct point p` makes one. `typedef` makes the name shorter.
> - Access: `p.x` for a struct, `p->x` for a pointer-to-struct (`p->x` is *exactly* `(*p).x`).
> - Structs are passed **by value** (copied!) — small structs by value is fine; anything bigger or mutable goes by pointer.
> - Padding: the compiler may insert gaps for alignment; `sizeof` a struct can exceed the sum of its members.
>
> **Do:**
> 1. `typedef struct { double x, y; } point_t;` plus `double dist(point_t a, point_t b)` and `void point_print(point_t p)`.
> 2. Write `void move(point_t *p, double dx, double dy)` — mutation through a pointer.
> 3. Print `sizeof(point_t)` then reorder/add members (`char tag;` in different positions) and watch `sizeof` change. Alignment is real.
>
> **Break it on purpose:** In `move`, write `p.x = ...` instead of `p->x = ...` — read the error, understand it says exactly what's wrong.
>
> **Done when:** `dist` and `move` both work, and you can explain when to pass a struct by value vs by pointer.

## Bite 20 — Arrays of Structs

> **Goal:** Build your first real "records database" in memory.
>
> **Learn:**
> - Arrays of structs are the workhorse data layout of C: cache-friendly, simple, no pointers needed yet.
> - `contacts[i].name` vs `contacts[i]->name` — array of structs vs array of *pointers to* structs. Know which you have.
> - Copying a struct copies all fields — including arrays inside it. Assignment works; no loops needed.
>
> **Do:**
> 1. `typedef struct { char name[32]; char phone[16]; } contact_t;`
> 2. `int contact_add(contact_t book[], size_t *count, size_t cap, const char *name, const char *phone);` — returns 0/-1 (full book = -1). Note the `size_t *count` out-param: the function must update the caller's count (bite 14 paying off).
> 3. `void contact_list(const contact_t book[], size_t count);` and `int contact_find(const contact_t book[], size_t count, const char *name);` (returns index or -1).
> 4. Seed 3 contacts in `main`, add one via input, list, find one.
>
> **Break it on purpose:** Add a 40-character name into the 32-char field using `strcpy`-thinking. Then do it correctly with `snprintf` and observe graceful truncation instead of corruption.
>
> **Done when:** Add/list/find all work through the function interfaces (no direct field poking in `main`).

## Bite 21 — const & Qualifiers

> **Goal:** Use `const` as documentation the compiler enforces.
>
> **Learn:**
> - Read declarations right-to-left: `const char *p` = pointer to const char (data protected); `char *const p` = const pointer to char (pointer pinned); `const char *const p` = both.
> - `const` in function parameters is a contract: "I will not modify what you give me."
> - `volatile` and `register` exist; you'll almost never need them. Know they exist, move on.
>
> **Do:**
> 1. Declare all three pointer kinds to the same variable; for each, try to change the data and the pointer — predict which lines fail to compile, then verify.
> 2. Retro-fit `const` onto every read-only parameter in bites 15–20's functions. Fix the cascade of errors.
> 3. Try to pass a `char[32]` to a function taking `const char *` (fine — safe direction), then the reverse through a `const char *` into a `char *` (needs a cast — and should make you suspicious).
>
> **Break it on purpose:** Cast away const: `char *p = (char*)"literal"; p[0] = 'X';` — the string literal is read-only memory; this crashes under ASan. Lesson: a cast is you telling the compiler "shut up, I know better" — have a good reason or don't cast.
>
> **Done when:** Every read-only parameter in your contact book is `const`, and you can read any `const` declaration aloud correctly.

## Bite 22 — ✅ Checkpoint: Contact Book v1

> **Goal:** A finished, crash-proof menu application using everything through bite 21.
>
> **Do:**
> - Fixed capacity: `contact_t book[100]; size_t count = 0;`
> - Menu loop (`do/while`): add, list, find (by name substring or exact), delete (swap-with-last trick), quit.
> - All I/O through the safe input pattern from bite 9.
> - All string writes via `snprintf`; all read-only params `const`; all mutation through the API functions — `main` never touches `.name` directly.
> - Delete: confirm with the user before removing; print "not found" when applicable.
>
> **Break it on purpose:** Before declaring done, abuse it: empty name, 40-char name, phone with letters, delete from empty book, find with empty string. Every abuse = a graceful message, never a crash.
>
> **Done when:** Clean `-Werror` build, survives the abuse list, committed as `checkpoint 2: contact book v1`. **Then, and only then, Phase 3.**
