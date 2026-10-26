# Phase 4 — The Legs: Files, Tooling, Robustness (Bites 35–44)

*Goal: build real multi-file programs that don't fall over.*

This phase is about moving from "code that works" to "projects that last":
proper files, proper builds, real debugging, deliberate error handling.

---

## Bite 35 — File I/O

> **Goal:** Read and write text files defensively.
>
> **Learn:**
> - `fopen` modes: `"r"`, `"w"` (truncate!), `"a"`. It returns `NULL` on failure — check it and report *why* with `perror`.
> - `fgets` works on files exactly as on stdin — same buffer discipline.
> - Every `fopen` pairs with exactly one `fclose` on **every** exit path, including error paths. This is why C code has careful `goto cleanup` patterns — you'll meet them here first.
>
> **Do:**
> 1. Write the contacts to a file: one line per contact, `name;phone\n`. Use `fprintf`.
> 2. Read it back with `fgets` + `sscanf(line, "%31[^;];%15s", name, phone)` — note `%[^;]`, the scanset conversion.
> 3. Round-trip test: save 5 contacts, load into a fresh book, list — identical output.
> 4. Handle: missing file, empty file, malformed line (no `;`), truncated last line. Each = skip + warn, never crash.
>
> **Break it on purpose:** Open a file for write, "forget" `fclose`, kill the process — observe the missing/empty file. Buffered writes live in memory until flushed; `fclose`/`fflush` is when they hit disk.
>
> **Done when:** Round-trip works and all four failure cases produce warnings instead of crashes.

## Bite 36 — Binary I/O

> **Goal:** Understand text vs binary formats and when each is right.
>
> **Learn:**
> - `fwrite(&c, sizeof c, 1, f)` dumps raw bytes — fast and exact, but fragile across compilers/platforms (padding! endianness! — bite 42).
> - Binary files aren't human-readable or diffable; text formats win on debuggability.
> - `fread`'s return value = items actually read; less than requested = EOF or error. Check it.
>
> **Do:**
> 1. Save the contact book as binary: count first, then structs. Load it back. Round-trip test.
> 2. Hex-dump your binary file (`xxd`) and find your own contact names in the raw bytes — a small moment of seeing through the machine.
> 3. Compare file sizes: text vs binary for the same data.
>
> **Break it on purpose:** Change one byte in the binary file with a hex editor / `printf '\x00' | dd of=...`. Load it — silent corruption or garbage. Binary formats trust you; that's their danger.
>
> **Done when:** Both formats round-trip, and you can argue text-vs-binary for a given use case in three sentences.

## Bite 37 — Multi-File Projects

> **Goal:** Organize code the way real projects do: modules with interfaces.
>
> **Learn:**
> - A module = `foo.h` (interface: types, prototypes, constants) + `foo.c` (implementation). Consumers include the header; the `.c` file is private.
> - Header rules: guards always; headers include *only what the interface needs*; no global variables in headers.
> - `static` on file-scope functions/variables = module-private. Your module's internal helpers never appear in the header.
>
> **Do:**
> 1. Split contact book v2: `contact.h/contact.c` (the data + operations), `storage.h/storage.c` (save/load), `main.c` (UI only).
> 2. Make all internal helpers `static`. Verify `main.c` compiles knowing *nothing* about internals.
> 3. Rebuild by hand: compile each `.c` to `.o`, link. Touch one file, recompile only it — feel what build systems automate.
>
> **Break it on purpose:** Remove a prototype from a header that `main.c` needs — linker error (`undefined reference`). Add a function *definition* to a header included twice without guards — redefinition error. Both classic; both now familiar.
>
> **Done when:** Four files, clean interfaces, `main.c` reads like a menu, internals invisible.

## Bite 38 — Make

> **Goal:** Automate builds correctly — understanding what make actually computes.
>
> **Learn:**
> - A rule = `target: dependencies` + recipe (must be a **tab**). Make rebuilds only what's stale, based on file timestamps.
> - Correct dependency tracking: every `.o` depends on its `.c` *and every header it includes*. Missing header deps = the classic "I changed the .h but nothing rebuilt" trap.
> - Variables (`CC`, `CFLAGS`), pattern rules (`%.o: %.c`), and `clean`/`check` phony targets.
>
> **Do:**
> 1. Write the Makefile by hand for the 4-file project: explicit rules first, then refactor to pattern rules + variables.
> 2. Add `check:` running valgrind + ASan builds (bite 33's workflow, formalized).
> 3. `make clean && make` from scratch; then `touch contact.h && make` — verify both `.o` files that depend on it rebuild.
>
> **Break it on purpose:** Remove `contact.h` from one `.o`'s dependencies, touch the header, `make` — the stale object silently links in. Re-add the dep, watch make fix it. Now you'll never ship a Makefile without header deps.
>
> **Done when:** `make`, `make check`, `make clean` all work and incremental builds are provably correct.

## Bite 39 — gdb

> **Goal:** Debug instead of guess. This bite saves you hundreds of future hours.
>
> **Learn:**
> - Core commands: `break`, `run`, `next` (step over), `step` (step into), `print`, `bt` (backtrace), `continue`, `quit`.
> - `print` understands structs, arrays (`p arr@5`), and expressions. `display` auto-prints each step.
> - A segfault under gdb: crash → `bt` → top frame is (usually) your bug.
>
> **Do:**
> 1. Set a breakpoint in `contact_add`, run, `print *c`, `next` through it. Watch the fields fill in.
> 2. Debug the *whole* add→save→load cycle: breakpoint in storage, inspect the file line string before parsing.
> 3. Learn conditional breakpoints: `break contact_add if count == 99` — catch the "book full" case live.
>
> **Break it on purpose:** Add `free(c->name); free(c->name);` somewhere reachable, run under gdb, get the crash, `bt` to the exact line. Do the same for a NULL-pointer deref. Diagnosis speed is now a skill you own.
>
> **Done when:** You can go from "it crashes somewhere" to "this line, this value" in under 2 minutes for a bug you planted.

## Bite 40 — Error Handling Conventions

> **Goal:** Design your program's error story instead of improvising it.
>
> **Learn:**
> - The dominant C convention: return `0`/`NULL`/non-negative on success; `-1`/`NULL` on failure; set `errno` when using system calls; report with `perror`/`strerror(errno)`.
> - Failure must *propagate*: low-level code returns codes; the top level (main/UI) decides what the human sees. Never `printf` an error 5 layers down.
> - Design decision to make once, use everywhere: your project's convention (e.g., "all functions return int, 0 = ok, negative = -errno").
>
> **Do:**
> 1. Write your convention as a comment block at the top of `contact.h`. This is your first API documentation.
> 2. Audit every function in the project against it; fix the stragglers.
> 3. Add a `log_error(const char *fmt, ...)` helper using `stdarg.h` (`va_list`) — your first variadic function, one consistent error channel.
>
> **Break it on purpose:** Make one low-level function `printf` its error directly instead of returning a code. Trigger it. Observe how the UI now double-reports and can't recover. Feel the reason conventions exist.
>
> **Done when:** Every failure path returns a code, only `main` prints user-facing errors, and the convention is written down in the header.

## Bite 41 — Bitwise Ops & Hex

> **Goal:** Become fluent in bits — mandatory for every protocol in Phases 6–7.
>
> **Learn:**
> - `& | ^ ~ << >>`. Masks isolate fields: `(x >> 4) & 0x0F`. Shifts on signed ints are a minefield — shift unsigned types.
> - Hex is the native language of protocols: one hex digit = 4 bits.
> - Setting/clearing/testing a flag bit: `x |= bit; x &= ~bit; if (x & bit)`.
>
> **Do:**
> 1. Write `void print_bits(uint32_t x)` (loop, masks, no libraries). Verify against `0x00000000`, `0xFFFFFFFF`, `0xDEADBEEF`.
> 2. Write `int parse_hex_nibble(char c)` and `uint8_t parse_hex_byte(const char *s)` — `"1A"` → `0x1A`. Reject garbage.
> 3. Flag practice: `uint32_t flags` with 4 named flag bits; functions to set/clear/test/print them.
>
> **Break it on purpose:** Shift a signed negative int right and compare across compilers/values (`gcc -fsanitize=undefined` flags implementation-defined behavior). Then redo it `unsigned` — clean. Signed shifting: banned in your code from today.
>
> **Done when:** `parse_hex_byte("1A")` returns 26, rejects `"G1"`, and `print_bits` matches known values.

## Bite 42 — Endianness & Serialization

> **Goal:** Turn integers into bytes and back, independent of machine architecture. RTP headers (bite 76) are exactly this skill.
>
> **Learn:**
> - Little-endian (x86): least significant byte first. Network byte order = **big-endian**. `htonl/htons/ntohl/ntohs` convert.
> - Portable serialization = shift-and-mask by hand: `(v >> 24) & 0xFF`, byte at a time. Never `fwrite` an `int` and call it a format.
>
> **Do:**
> 1. Determine your machine's endianness by inspecting bytes of `uint32_t x = 1` (the classic trick). Print a verdict.
> 2. Write `void put_u16be(uint8_t *p, uint16_t v)` and `uint16_t get_u16be(const uint8_t *p)`; same for u32. These four functions follow you into Phase 7.
> 3. Serialize a small record (`u32 id, u16 len, char payload[]`) to a file in big-endian; read it back; verify.
>
> **Break it on purpose:** `fwrite(&x, sizeof x, 1, f)` raw, read on the same machine — works. Reason through (in comments) what breaks if the file crosses to a big-endian machine or the struct gains padding. That reasoning *is* the lesson.
>
> **Done when:** put/get functions round-trip every value from 0 to 65535 (test loop!), and you can explain network byte order in one sentence.

## Bite 43 — CMake + Tests

> **Goal:** Modern builds and a repeatable test suite.
>
> **Learn:**
> - CMake's shape: `cmake_minimum_required`, `project`, `add_executable(app file1 file2...)`. Out-of-source builds: `cmake -B build && cmake --build build`.
> - Tests don't need a framework at this scale: a `tests.c` with `assert`s and a non-zero exit on failure. A test that can fail loudly > a framework you avoid using.
> - Why both make and CMake: make teaches you what builds really are; CMake is what real projects use (and every library you `find_package` later speaks it).
>
> **Do:**
> 1. `CMakeLists.txt` for the project with `-Wall -Wextra -Werror -g` and ASan in a Debug config.
> 2. `tests.c`: assert `my_strcmp`, `parse_hex_byte`, `put_u16be/get_u16be`, strbuf, intvec, hash map. Target the *functions*, not the UI.
> 3. Wire `ctest` or a `check` target; get a green run into your routine.
>
> **Break it on purpose:** Flip one expected value in a test. Watch the suite fail loudly, with the failing function named. A test suite's value = the confidence of its red.
>
> **Done when:** `cmake --build build && ctest` runs green, and the Makefile era is officially over.

## Bite 44 — ✅ Checkpoint: Contact Book v3

> **Goal:** The finished artifact of your "core C" education. Multi-module, tested, documented, clean.
>
> **Do:**
> - Modules: `contact`, `storage`, `strbuf` (reused!), `util` (logging, hex, endian), `main`. Each with a documented header.
> - Persistence: your choice of format — but the choice documented in `storage.h` with its failure cases.
> - Test suite covering every module's functions. `ctest` green under ASan build.
> - `README.md` in the project: build instructions, usage, design notes in your own words.
> - Full abuse script: garbage files, huge inputs, EOF mid-menu, valgrind across the whole session.
>
> **Break it on purpose:** Hand the project to a friend (or a fresh you tomorrow) with only the README. Whatever they get wrong is a documentation bug — fix it.
>
> **Done when:** Strangers could build and use it from the README. Committed as `checkpoint 4: contact book v3`. **You now write real C. Phase 5 leaves "toy programs" behind.**
