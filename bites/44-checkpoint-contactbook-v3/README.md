# Contact Book v3 — Phase 4 checkpoint (bite 44)

The "finished C program" baseline of the c-prep plan: a multi-module,
CMake-built, tested, valgrind-clean contact book. Everything it does
was built across bites 1–43; this checkpoint is the assembly.

## Build

```sh
cmake -S . -B build          # configure (out-of-source: nothing lands in the source tree)
cmake --build build          # compile
./build/contactbook          # run (optional argument: path to your data file)
```

Build with sanitizers (recommended while developing):

```sh
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug && ./build-debug/contactbook
```

No-CMake fallback (what a Makefile used to do for you):

```sh
gcc -Wall -Wextra -Werror -std=c17 -g -o contactbook \
    contact.c storage.c strbuf.c util.c main.c
```

## Tests

```sh
ctest --test-dir build                       # assert-based suite, one function per module
ctest --test-dir build-debug                 # same suite under ASan + UBSan (must stay green)
```

## Usage

```
./build/contactbook [datafile]        # datafile defaults to contacts.txt

commands: 1 add  2 list  3 find  4 remove  5 sort  6 save  7 load  q quit
```

- Startup tries to load the datafile if it exists (quiet if absent).
- Saving writes the whole book; loading adds into the current book.
- `q` or Ctrl-D (EOF) quits cleanly. Garbage input is rejected, never crashes.

## File format (documented in `storage.h`)

Text, one contact per line: `name;phone\n`

```
Alice;555-0101
Bob;555-0102
```

Why text over binary (bites 36/42): diffable, debuggable with `cat`,
survives format drift, and machine-independent — binary struct dumps
carry padding bytes and native endianness. Failure cases, all handled
without crashing: missing file (error, errno preserved), empty file
(0 contacts, not an error), malformed / truncated / over-long lines
(skipped and counted). Known limitation: a name containing `;` or a
newline cannot round-trip (fixing needs escaping — deliberate exercise).

## Design notes (in my own words)

| module | responsibility |
|--------|----------------|
| `contact` | the data type + book operations (add/find/remove/sort). Doubled-capacity heap array; truncating copies so bad input can't overflow. |
| `storage` | the only module that knows the file format. Returns codes; never prints. |
| `strbuf`  | growable string builder (bite 27), reused by `storage_save` to assemble lines. |
| `util`    | logging (`log_error`/`log_info`), hex parsing (bite 41), big-endian put/get (bite 42). Follows us into Phase 5. |
| `main`    | UI only. The one layer allowed to talk to humans. |

**Error-handling convention** (designed in bite 40, obeyed everywhere):
`0 = success, negative = named cb_result code`; syscall failures leave
`errno` meaningful and are rendered only at the top with
`strerror(errno)`; low-level modules never print; failures leave state
unchanged (all-or-nothing). The contract lives at the top of `contact.h`.

## Abuse script (run these — the program must cope, never crash)

```sh
# EOF immediately (Ctrl-D simulation)
./build/contactbook </dev/null

# garbage menu input
printf 'garbage\n999\n\n2\nq\n' | ./build/contactbook

# garbage data files: missing, empty, malformed, absurdly long lines
./build/contactbook /tmp/no-such-file.txt          # quiet fresh start
: > /tmp/cb_abuse.txt && ./build/contactbook /tmp/cb_abuse.txt
printf 'not-a-contact\n;;;x\nok;555\n' > /tmp/cb_abuse.txt
{ head -c 400 /dev/zero | tr '\0' 'x'; } >> /tmp/cb_abuse.txt
./build/contactbook /tmp/cb_abuse.txt              # skips the bad, keeps 'ok'

# valgrind across a whole session (add 2, save, reload, quit)
valgrind --leak-check=full --show-leak-kinds=all ./build/contactbook /tmp/cb_val.txt <<'EOF'
1
Alice
555-0101
1
Bob
555-0102
6
7
q
EOF
# Expected: "All heap blocks were freed - no leaks are possible", 0 errors.
```

## BREAK IT ON PURPOSE

Hand this project (README only) to a fresh you-tomorrow. Whatever they
get wrong is a documentation bug — fix the README. Then: flip an
expected value in `tests.c` (watch the red), plant a double-free in
`contact.c` and diagnose it with gdb + `bt`, and re-run valgrind —
the tools from bites 38–40 are the maintenance kit for everything below.
