# Phase 3 — The Heart: Memory (Bites 23–34)

*Goal: malloc/free fluency. Valgrind reports zero errors, always.*

From here on: **every** program you finish gets run under both
`valgrind --leak-check=full --show-leak-kinds=all` and `-fsanitize=address,undefined`.
Not because it's graded — because it's how you'll work for the rest of your C life.

---

## Bite 23 — The Memory Model

> **Goal:** See the whole map of a program's memory once, so every later bite has a home to live in.
>
> **Learn:**
> - Four regions: **text** (code), **data/BSS** (globals, statics — lives forever), **stack** (locals, grows/shrinks per call, limited ~MBs), **heap** (malloc's domain, grows on demand, *you* manage it).
> - Stack frames: every function call pushes a frame; return pops it. That's why locals die and why returning their address is a hanging offense.
> - The heap has no automatic cleanup. Ever. Every `malloc` is a promise you must keep with `free`.
>
> **Do:**
> 1. Write a program printing addresses of: a global, a static local, a local, a malloc'd block, and a string literal. Sort them mentally into the map.
> 2. Draw the four regions on paper with your observed addresses placed on them.
> 3. Demonstrate stack depth: a recursive function that prints its depth until it crashes (stack overflow). Note the crash address region.
>
> **Break it on purpose:** Return a pointer to a local (again, from bite 7) but this time print through it *after* the function returned, twice, with a `printf` in between. Watch the second value get stomped. Stack memory is recycled — seeing it is believing it.
>
> **Done when:** You can place any variable declaration into the correct region from memory, and you've watched stack memory get recycled.

## Bite 24 — malloc/free, First Contact

> **Goal:** Allocate, use, free — then meet all four classic heap crimes under supervision.
>
> **Learn:**
> - `void *malloc(size_t n)` returns `NULL` on failure — **always check**. `void *` converts implicitly to any object pointer.
> - `free(p)` returns the memory; `p` becomes dangling. `p = NULL;` after free turns "possible silent corruption" into "loud, debuggable crash."
> - The four crimes: **leak** (never freed), **use-after-free**, **double-free**, **uninitialized read**. Valgrind names each one explicitly.
>
> **Do:**
> 1. `int *p = malloc(sizeof *p);` — note the idiom `sizeof *p` (survives type changes). Check NULL, use, free, set NULL.
> 2. Allocate an array of 10 ints, initialize by loop (never assume zeros!), print, free.
> 3. Then, in four *separate* test programs: commit each of the four crimes on purpose. Run each under valgrind. Read every word of each report.
>
> **Break it on purpose:** That *was* the breaking exercise — four crimes, four reports. Screenshot or save the valgrind output into your repo; it's your catalog of future bugs.
>
> **Done when:** You can match each valgrind report line to its crime, and your real code always checks malloc's return.

## Bite 25 — Dynamic Arrays

> **Goal:** Build the growable array — the single most reused data structure in real C.
>
> **Learn:**
> - `realloc(p, newsize)` grows/shrinks, may *move* the block (returns a new pointer — the old one is dead). The idiom: `int *tmp = realloc(p, new); if (!tmp) { /* p still valid, handle */ } p = tmp;`
> - Growth strategy: capacity doubles (4 → 8 → 16…). Amortized O(1) push. Never grow by +1.
> - Struct-of-state: `struct { int *data; size_t len, cap; }`.
>
> **Do:**
> 1. Build `intvec`: `void iv_init(intvec *v); int iv_push(intvec *v, int x); size_t iv_len(const intvec *v); int iv_get(const intvec *v, size_t i); void iv_free(intvec *v);`
> 2. Growth rule: `cap == 0 ? 4 : cap * 2`.
> 3. Push 1000 values; `iv_get` a few; free. Then valgrind: zero leaks, zero errors.
>
> **Break it on purpose:** Write the classic realloc bug: `p = realloc(p, new);` and handle failure by... using `p`, which is now NULL and the old block leaked. Force a huge allocation to fail (or just reason it through in comments). Then fix it with the tmp-pointer idiom.
>
> **Done when:** intvec passes: push/len/get/free, grows past 4/8/16 correctly, valgrind-clean.

## Bite 26 — malloc + Structs + Ownership

> **Goal:** Allocate structs on the heap and answer THE question: who owns pointed-to members?
>
> **Learn:**
> - `contact_t *c = malloc(sizeof *c);` — the struct is yours; free it exactly once.
> - Ownership: if a struct contains `char *name` (a pointer), then freeing the struct does **not** free `name`. Someone owns each allocation. Draw an ownership arrow per pointer.
> - `calloc(n, size)` zero-initializes; `malloc` does not. For structs with pointers, `calloc` prevents garbage-pointer disasters.
>
> **Do:**
> 1. Heap-allocate a `point_t`, set fields, print, free — trivial on purpose.
> 2. `typedef struct { char *name; int age; } person_t;` with `person_t *person_create(const char *name, int age)` — it must strdup the name (bite 27's `my_strdup` or `strdup`). Write `person_destroy` that frees **both** allocations. Only free works correctly in pairs.
> 3. Create 3 persons in an array of pointers, destroy all 3. Valgrind: zero leaks.
>
> **Break it on purpose:** `free(p)` without freeing `p->name` → leak report. Then `free(p->name)` twice (via double person_destroy) → invalid free report. Both reports are now in your catalog.
>
> **Done when:** person_create/destroy are leak-free and you can state the ownership rule: *every pointer has exactly one owner, and the owner frees.*

## Bite 27 — Dynamic Strings

> **Goal:** Build strings whose length isn't known at compile time.
>
> **Learn:**
> - The str-builder pattern: struct `{ char *buf; size_t len, cap; }`, amortized doubling, always keep `buf` NUL-terminated.
> - `snprintf(NULL, 0, fmt, ...)` returns the length the output *would* need — the standard trick for allocating exactly enough.
>
> **Do:**
> 1. Write `char *my_strdup(const char *s)` — malloc `strlen+1`, copy, return. Test against real `strdup`.
> 2. Build `strbuf`: `sb_init`, `sb_append(strbuf*, const char*)`, `sb_appendf(strbuf*, const char *fmt, ...)` (using the snprintf(NULL,0) trick), `sb_free`.
> 3. Build a 1000-word sentence by appending in a loop; print; free. Valgrind-clean.
>
> **Break it on purpose:** Append without growing (comment out the growth check) — buffer overrun under ASan. Reinstate the check. The failure you caused is the exact reason the check exists.
>
> **Done when:** strbuf builds arbitrary-length strings, stays NUL-terminated, and never leaks.

## Bite 28 — Linked List I

> **Goal:** Your first pointer-to-structure data structure. Slow and careful.
>
> **Learn:**
> - `typedef struct node { int val; struct node *next; } node_t;` — the struct contains a pointer to its own type.
> - Empty list = `NULL` head. Every operation must handle: empty list, head node, middle.
> - Drawing first, always. Linked lists are pointer manipulation; code is just the drawing transcribed.
>
> **Do:**
> 1. `node_t *push_front(node_t *head, int val)` — allocate, set, return new head. Note it *returns* the new head; the caller reassigns: `head = push_front(head, 5);`
> 2. `void list_print(const node_t *head);` — walk with `for (const node_t *p = head; p; p = p->next)`.
> 3. `void list_free(node_t *head)` — walk and free each node. **The free loop is the entire lesson**: save `next` before freeing the current node, or you've use-after-freed your way off a cliff.
>
> **Break it on purpose:** In `list_free`, free the node *before* saving `next`. Crash under ASan/valgrind. Read it. Fix it. That bug will try to visit you for the rest of your career.
>
> **Done when:** Push 10, print 10 in reverse order (that's what push_front does), free all — valgrind reports `All heap blocks were freed`.

## Bite 29 — Linked List II

> **Goal:** The hard list operations: insert-middle, delete, and the pointer-to-pointer trick.
>
> **Learn:**
> - Deletion needs the *previous* node — that's why many people pass `node_t **head`.
> - The elegant trick: `node_t **pp = &head; while (*pp && (*pp)->val < x) pp = &(*pp)->next;` — `pp` points at *the pointer that points at the node*. Insert/delete become uniform, no special head case. This trick appears everywhere in systems C.
> - Sorted insert on a list = how you'll think about keeping ordered structures later.
>
> **Do:**
> 1. `node_t *list_append(node_t *head, int val)` — walk to the last node, link.
> 2. `node_t *list_insert_sorted(node_t *head, int val)` — first the naive version (track `prev`), then the `node_t **` version. Compare which was easier to get right.
> 3. `node_t *list_delete(node_t *head, int val)` — unlink and free exactly the matched node (all of them, or first — your choice, document it).
> 4. Full lifecycle test: insert sorted 7 values including duplicates, delete the head value, delete a middle value, delete a nonexistent value, free all. Valgrind.
>
> **Break it on purpose:** Delete the head node without updating `head` in the caller. Print. Then fix using the return-value or `**` approach.
>
> **Done when:** All operations pass the lifecycle test, valgrind-clean, and you've used `node_t **` at least once successfully.

## Bite 30 — Hash Map

> **Goal:** O(1) lookup with your own hands — the shape of every key-value store you'll ever meet.
>
> **Learn:**
> - Array of buckets + hash function + collision strategy. Open addressing: if slot is taken, walk to the next free slot.
> - FNV-1a (simple, good): `hash = (hash ^ byte) * 16777619` over the string's bytes.
> - Load factor matters: at 64 buckets with open addressing, keep entries < ~50; for this bite, fixed size is fine — resizing comes later if you want it.
>
> **Do:**
> 1. `#define HB 64` buckets of `struct { char *key; int val; int used; }`.
> 2. `int hm_put(hm *m, const char *key, int val)` — hash, probe (linear), overwrite if same key, insert if free, fail if full. The map **strdup**s the key (it owns it).
> 3. `int hm_get(const hm *m, const char *key, int *out)` and `void hm_free(hm *m)` — free every key.
> 4. Test: 30 keys, overwrite 5 of them, verify 5 overwrites took, lookup misses return -1, free → valgrind-clean.
>
> **Break it on purpose:** Hash function that returns 0 for everything (all keys collide into one probe chain) — still works, just slow. That's the whole trade-off of hashing in one experiment.
>
> **Done when:** put/get/overwrite/free all correct and leak-free.

## Bite 31 — Function Pointers

> **Goal:** Store *behavior* in variables. This is how servers dispatch, callbacks fire, and libraries feel object-oriented.
>
> **Learn:**
> - `int (*op)(int, int)` — a pointer to a function taking two ints. Call it: `op(3, 4)` or `(*op)(3, 4)` (same thing).
> - Functions decay to pointers like arrays do. `qsort`'s comparator parameter is exactly this.
> - Table-driven dispatch: array of `{name, fn}` — add a new operation by adding a row, not a case. This is the architecture of real servers (and of your WebRTC backend's message dispatch in Phase 7).
>
> **Do:**
> 1. Rewrite the checkpoint-1 calculator's dispatch as a table: `struct { const char *name; int (*fn)(int,int); } ops[] = ...` with a lookup loop.
> 2. Write `void apply(int *a, size_t n, int (*fn)(int))` that maps a function over an array. Use it with a doubler and a negator.
> 3. Add a `mod` operation to the table — note it required zero changes to the dispatch code.
>
> **Break it on purpose:** Call through a function pointer you never assigned (`int (*op)(int,int); op(1,2);`) — crash. ASan/valgrind will point at the wild call. NULL-check your function pointers in dispatch tables from now on.
>
> **Done when:** The table-driven calculator works and adding an operation touches exactly one line.

## Bite 32 — Callbacks with qsort

> **Goal:** Use the standard library's callback machinery, and sort your own data.
>
> **Learn:**
> - `void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));` — generic over element type via `void *` + size.
> - Comparator convention: `<0`, `0`, `>0` — and the classic safe idiom for ints: `(a > b) - (a < b)` (no overflow).
> - Comparing strings inside a comparator: cast to your struct type, then `strcmp` the fields.
>
> **Do:**
> 1. Sort an `int` array with `qsort` (write the comparator yourself).
> 2. Sort your contact book by name: comparator casts `const void *` to `const contact_t *`, then `strcmp(a->name, b->name)`.
> 3. Add a secondary sort key (phone) for ties.
>
> **Break it on purpose:** Return `a - b` from an int comparator and test with `INT_MAX` and `INT_MIN` — overflow makes the "sorted" array wrong. Then use the safe idiom. You've now been saved from a bug that ships in real products.
>
> **Done when:** Contacts sort correctly by name, then phone for ties, valgrind-clean.

## Bite 33 — Valgrind Discipline

> **Goal:** Turn memory-checking from an event into a habit.
>
> **Learn:**
> - The valgrind workflow: run → read *from the first error down* → fix → rerun. Errors cascade; the first one is usually the real one.
> - Key report lines: `Invalid write/read` (bounds or freed memory), `definitely lost` (leak, pointer gone), `indirectly lost` (owner freed but child didn't), `still reachable` (forgot to free but pointer still exists — still fix it).
> - ASan vs valgrind: ASan is faster and built into the compiler (compile-time flag); valgrind needs no recompile and catches more heap detail. Use both.
>
> **Do:**
> 1. Take contact book v1 (bite 22). Compile with `-fsanitize=address,undefined`, run your full abuse script.
> 2. Rebuild without sanitizers, run under `valgrind --leak-check=full --show-leak-kinds=all`. Fix every report.
> 3. Write a `Makefile` target `check:` that does both runs in sequence — from bite 38 you'll formalize this.
>
> **Break it on purpose:** Introduce one leak and one use-after-free into your (backed-up) contact book; find each with the corresponding tool in under 5 minutes. Being *good at finding* bugs is the deliverable here, not having none.
>
> **Done when:** Contact book v1 runs completely clean under both tools, and you can decode a valgrind report without Googling.

## Bite 34 — ✅ Checkpoint: Contact Book v2

> **Goal:** Rebuild the contact book with dynamic memory — same features, unlimited capacity, zero leaks.
>
> **Do:**
> - Storage: dynamic array of `contact_t *` (array of pointers — you choose the indirection; justify it in a comment) or array of structs that grow via realloc. Either is defensible; be consistent.
> - Features: add (sorted by name, using bite 29's insert thinking or qsort-on-demand), list, find, delete, **save/load to a text file** (one line per contact, `name;phone` — file I/O preview of Phase 4; `fgets` + `sscanf` with `;` parsing).
> - Every allocation freed: on delete, on exit, on load failure. Every function checks its malloc.
> - Sorted insert with `strcmp`.
>
> **Break it on purpose:** Your abuse script, expanded: load a file with a missing `;`, a 200-char name, an empty file, a nonexistent file. Then: add 200 contacts to force multiple reallocations mid-session. Nothing crashes; nothing leaks.
>
> **Done when:** Full session under valgrind: zero errors, zero leaks. Committed as `checkpoint 3: contact book v2`. **This is the halfway mark of becoming a real C programmer — notice it.**
