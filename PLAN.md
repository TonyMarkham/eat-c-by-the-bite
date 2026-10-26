# c-prep: C from Hello World to WebRTC Backend

> **How do you eat an elephant? One bite at a time.**
>
> This plan is a list of small bites. Each bite is 30–90 minutes of work:
> read a little, write a little, break a little, fix it, commit.
> You never "study C" in the abstract — every bite ends with code you wrote.

---

## The Rules

1. **One bite per sitting.** Don't binge. Momentum beats intensity.
2. **Never copy-paste from a tutorial without retyping it.** Your fingers learn, not your eyes.
3. **Break it on purpose.** After each bite, deliberately introduce a bug, observe the failure (compiler error, segfault, wrong output), then fix it. Predicting failure is the real skill.
4. **Commit after every bite.** `git commit -m "bite 12: pointer arithmetic on char arrays"` — your git log becomes your journal.
5. **You may skip *forward*, never backward.** If a later bite confuses you, that's the plan telling you which earlier bite to revisit.
6. **Compiler settings, day one:** `-Wall -Wextra -Werror -g -std=c17`. Treat warnings as errors early; it's cheaper than unlearning.

## Cadence

- **Default:** 5 bites/week → done in ~4 months.
- **Slow mode:** 2 bites/week → done in ~10 months. Still fine.
- Every phase ends with a **Checkpoint Project** — a small program you finish before moving on. No skipping checkpoints.

## Environment (Bite 0)

You're on Windows. Do this **once**, in this order:

1. Install **WSL2 + Ubuntu** (`wsl --install`). All systems/networking bites happen here — sockets, threads, and WebRTC tooling are Linux-first.
2. Inside WSL: `sudo apt install build-essential gdb valgrind cmake git pkg-config`
3. On Windows (optional): install **VS Code + C/C++ extension**, or Vim inside WSL — anything that runs `clangd` or IntelliSense.
4. `mkdir -p ~/c-prep && cd ~/c-prep && git init` — this repo mirrors this plan; keep one folder per bite: `bites/01-hello/`, `bites/02-vars/`, …

---

# Phase 1 — The Beak Full of Sand: Syntax (Bites 1–10)

*Goal: read simple C without fear. Zero pointers yet.*

📖 **Lesson plans:** [phases/phase-1-syntax.md](phases/phase-1-syntax.md)

| # | Bite | Do this |
|---|------|---------|
| 1 | Hello World | Write, compile with `-Wall -Wextra -Werror`, run. Delete the `return` statement — does it still compile? Why? |
| 2 | Variables & printf | All basic types (`char, int, long, float, double`), `sizeof`, format specifiers. Print a table of type → size. |
| 3 | Operators & precedence | Arithmetic, integer division surprise (`7/2`), `%`, increments, assignment vs `==`. Compute compound interest in a loop-free program. |
| 4 | if/else & switch | FizzBuzz. Then FizzBuzz with `switch` on `(n % 15)` — feel the difference. |
| 5 | Loops | `for`, `while`, `do/while`. Print a multiplication table. Break one with `break`/`continue` and predict output before running. |
| 6 | Functions | Declaration vs definition vs prototype. Write `int max3(int, int, int)` before `main`. Learn what the linker does with a missing function. |
| 7 | Scope & lifetime | Local vs `static` vs global. A function with a `static int` counter — call it 5 times, observe. |
| 8 | Preprocessor I | `#include`, header guards, `#define` constants. Split a program into `mathutil.h` + `mathutil.c` + `main.c` and compile all three. |
| 9 | Input | `fgets` + `sscanf` (NOT raw `scanf`). Read two numbers from stdin, print sum. Feed it "abc" and watch it fail — handle that. |
| 10 | **Checkpoint: number cruncher** | A menu-driven calculator loop (add/sub/mul/div, integer power, exit). Uses functions, switch, loops, input validation. ~150 lines. |

---

# Phase 2 — The Trunk: Arrays, Strings, Pointers (Bites 11–22)

*Goal: pointers stop being magic. This is the phase where most people quit — go slow.*

📖 **Lesson plans:** [phases/phase-2-pointers.md](phases/phase-2-pointers.md)

| # | Bite | Do this |
|---|------|---------|
| 11 | Arrays | Declare, iterate, pass to a function. Learn arrays decay to pointers when passed. Bounds: write past the end in a tiny program and see what (doesn't) happen. |
| 12 | First pointers | `&`, `*`, swap two ints via a function. Draw the boxes-and-arrows diagram on paper first. |
| 13 | Pointer arithmetic | `p + 1` moves by `sizeof(type)`. Walk an array with a pointer. Compare `arr[i]` and `*(arr+i)` in assembly-brained terms. |
| 14 | Pointers & functions | "Out parameters": `void minmax(int *a, int n, int *min, int *max)`. Return two things at once. |
| 15 | Strings as char arrays | `'\0'` terminator. Write your own `my_strlen`, `my_strcmp`, `my_strcpy` (with dest-size parameter). Test them against the real ones. |
| 16 | string.h + the dangers | `strlen/strcpy/strcat/sprintf` vs the safe `strncpy/snprintf`. Overflow: write the classic overflow, then fix it. |
| 17 | 2D arrays & arrays of strings | `char names[5][32]`. A tiny contact book: add, list, search. |
| 18 | Pointers to pointers | `char **argv` dissected. Print your own `main`'s argv tree with types annotated. |
| 19 | Structs | `struct`, `typedef`, `.` vs `->`, passing by value vs by pointer. A `struct point` + `distance()` function. |
| 20 | Structs + arrays + pointers | Array of structs. Rebuild the contact book as `struct contact { char name[32]; char phone[16]; }` with add/delete/list via functions taking pointers. |
| 21 | const & qualifiers | `const char *` vs `char *const` vs both. When function parameters should be const. |
| 22 | **Checkpoint: contact book v1** | Menu app, fixed-size array of 100 contacts, save nothing, crash-proof against bad input. All string handling hand-rolled or safe. |

---

# Phase 3 — The Heart: Memory (Bites 23–34)

*Goal: malloc/free fluency. Valgrind reports zero errors, always.*

📖 **Lesson plans:** [phases/phase-3-memory.md](phases/phase-3-memory.md)

| # | Bite | Do this |
|---|------|---------|
| 23 | The memory model | Stack vs heap vs static, drawn as one diagram. Run a program printing addresses of a local, a global, and a malloc'd block. |
| 24 | malloc/free basics | Allocate an int, use it, free it. Then: double-free, use-after-free, leak — trigger each one on purpose, run **valgrind**, read the report. |
| 25 | Dynamic arrays | Grow an array with `realloc`. Start at capacity 4, double when full. Build `intvec` (push/pop/get). |
| 26 | malloc + structs | Allocate a struct on the heap. Then a struct containing a pointer — who owns the pointed-to memory? Draw it. |
| 27 | Dynamic strings | `char *my_strdup(const char *)` without calling `strdup`. Then a growable string builder (`sb_append`, `sb_free`). |
| 28 | Linked list I | `struct node { int val; struct node *next; }`. Push front, print, free the whole list (the free-everything loop is the point). |
| 29 | Linked list II | Append, insert sorted, delete by value. Every operation: think about 3 cases — empty list, middle, head. |
| 30 | Hash map | Open-addressing table of string→int, fixed 64 buckets, FNV-1a hash. No chaining yet. |
| 31 | Function pointers | `int (*op)(int,int)`. A `calc` that takes an op pointer. Then an array of `{name, fn}` pairs — your first dispatch table (this concept returns in the WebRTC phase). |
| 32 | Callbacks + qsort | Sort the contacts by name using `qsort` with your own comparator. |
| 33 | Valgrind discipline | Take *any* previous checkpoint, run valgrind, fix every leak. Learn `--leak-check=full --show-leak-kinds=all`. Also compile with `-fsanitize=address,undefined` and rerun. |
| 34 | **Checkpoint: contact book v2** | Same app, but unlimited contacts (dynamic array), sorted insert, and **zero valgrind leaks**. Save/load contacts to a text file (preview of Phase 4). |

---

# Phase 4 — The Legs: Files, Tooling, Robustness (Bites 35–44)

*Goal: build real multi-file programs that don't fall over.*

📖 **Lesson plans:** [phases/phase-4-tooling.md](phases/phase-4-tooling.md)

| # | Bite | Do this |
|---|------|---------|
| 35 | File I/O | `fopen/fgets/fclose`. Read the contact file you wrote in bite 34. Handle: missing file, empty file, malformed line. |
| 36 | Binary I/O | `fwrite/fread` a struct array. Compare formats, corruption behavior. |
| 37 | Multi-file projects | Split contact book into `contact.c/h`, `storage.c/h`, `main.c`. Headers = interfaces. Learn why `.h` files have include guards forever. |
| 38 | Make | Write a Makefile from scratch for the multi-file build (not CMake yet). Understand targets, dependencies, `clean`. |
| 39 | gdb basics | Compile `-g`, break, step, print variables, print a struct, backtrace on a segfault. Crash a program on purpose and diagnose it in gdb. |
| 40 | Error handling conventions | Return codes vs errno. `perror`/`strerror`. Design your own convention: `int contact_add(contactbook_t*, const contact_t*)` returns 0/-1, document it. |
| 41 | Bitwise ops & hex | `& | ^ ~ << >>`, hex literals, masks. Parse a hex string like `0x1A2B` into bytes. (You'll need this for protocol parsing later.) |
| 42 | Endianness & serialization | Write a uint32 to a file both native and big-endian; read both back. This is exactly what RTP headers demand later. |
| 43 | CMake + testing | Convert the Makefile to CMake. Add a tiny test file (`assert`-based) that exercises `my_strcmp` etc. |
| 44 | **Checkpoint: contact book v3** | Multi-file, CMake, file persistence (text or binary), tests, valgrind-clean, gdb-navigable. This is your "finished C program" baseline. |

---

# Phase 5 — The Gut: Systems & Networking (Bites 45–58)

*Goal: think in bytes, sockets, and concurrency. Everything here is in WSL.*

📖 **Lesson plans:** [phases/phase-5-networking.md](phases/phase-5-networking.md)

| # | Bite | Do this |
|---|------|---------|
| 45 | Process & syscall model | `fork`, `exec`, `wait`, exit codes. A shell that runs one command (mini `system()`). |
| 46 | TCP client | `socket/connect/send/recv`. Fetch a page from `example.com:80` by hand — send a raw HTTP/1.1 GET, print raw response bytes. |
| 47 | TCP echo server | `socket/bind/listen/accept`. Handle one client at a time. Telnet into it with `nc`. |
| 48 | Robust recv | The loop: TCP gives you a *byte stream*, not messages. Implement "read exactly N bytes" and "read until \r\n\r\n". Internalize this — half of all protocol bugs live here. |
| 49 | Concurrent server: fork | Fork per connection. Two `nc` clients simultaneously. |
| 50 | Threads | `pthread_create/join/mutex`. A shared counter corrupted without a mutex, fixed with one. |
| 51 | poll/epoll | Single-threaded server handling many connections with `poll` (epoll after). This event loop pattern is *exactly* what a WebRTC backend's signaling layer uses. |
| 52 | UDP | `sendto/recvfrom`. Echo server + client. Note: no connections, no ordering, no delivery guarantee — remember this feeling. |
| 53 | Building a protocol | Design a tiny line-based protocol (`ADD 3 4` → `7`, `BYE`) on top of bite 51's server. Parse strictly; fuzz it with garbage from `nc`. |
| 54 | Binary protocol | Same server but binary: 1 byte type, 2 bytes length (big-endian), payload. Serialize/deserialize with bite 42 skills. |
| 55 | HTTP server v1 | Hand-rolled: parse request line + headers, route 2 paths, serve a static file, correct `Content-Length`. curl against it. |
| 56 | HTTP server v2 | Add threading, keep-alive, 404/405/500 handling, logging. |
| 57 | TLS awareness | Don't write TLS — *use* it. Link against OpenSSL, upgrade your client from bite 46 to `https`. Appreciate what you'll get for free later. |
| 58 | **Checkpoint: httpd** | A small threaded HTTP server in C: static files + a JSON endpoint (hand-built JSON is fine) + graceful shutdown. |

---

# Phase 6 — The Neck: Real-Time Media Concepts (Bites 59–68)

*Goal: the theory WebRTC is built on, learned by hand-rolling the primitives.*

📖 **Lesson plans:** [phases/phase-6-rt-media.md](phases/phase-6-rt-media.md)

| # | Bite | Do this |
|---|------|---------|
| 59 | Time in C | `clock_gettime(CLOCK_MONOTONIC)`, millisecond timing, a fixed-rate loop (e.g., tick every 20 ms). Pace: this is your RTP clock later. |
| 60 | Jitter & the network | Read about latency/jitter/packet loss *while* testing UDP over a real network. Use `nc` between two machines/tabs of WSL, observe out-of-order UDP by sending with random sleeps. |
| 61 | Sequencing | Add a sequence number + timestamp to bite 52's UDP protocol. On the receiver: reorder a small window, drop duplicates, measure the gap. **You just wrote a mini RTP receiver.** |
| 62 | Packetization | Slice a 64 KB buffer into 1200-byte payloads, mark last fragment with a flag byte. Reassemble at the receiver. |
| 63 | Audio, hands dirty | Record 3 s with `ffmpeg`/`arecord` to raw PCM 16-bit 48 kHz mono. Play it back. Compute RMS amplitude in C. |
| 64 | Crypto basics in C | OpenSSL: SHA-256 of a file, HMAC, AES-CTR encrypt/decrypt of a buffer. Just enough to read SRTP's requirements without flinching. |
| 65 | Randomness & keys | Cryptographic RNG (`RAND_bytes`), key exchange concept. Diffie–Hellman with OpenSSL `EVP` — do the handshake between two of your own processes. |
| 66 | STUN by hand | Read RFC 5389 sections 1–6 only. Write a C client that sends a raw **STUN Binding Request** to a public STUN server over UDP and parses the XOR-MAPPED-ADDRESS. **This is the first true WebRTC bite.** |
| 67 | DTLS conceptually | Read about the DTLS handshake (no implementation). Know the words: ClientHello, Certificate, SRTP keying material, `use_srtp`. |
| 68 | **Checkpoint: reorder engine** | A program that: sends numbered packets over UDP with jitter (separate sender/receiver processes), reorders, dedupes, reports loss %, and plays a PCM file back in-order. Ugly output OK; correct output required. |

---

# Phase 7 — The Whole Elephant: WebRTC Backend (Bites 69–80)

*Goal: understand WebRTC from the server's seat, then build one.*

📖 **Lesson plans:** [phases/phase-7-webrtc.md](phases/phase-7-webrtc.md)

**First, the map (bites 69–70).** WebRTC = signaling (your HTTP/WebSocket server) + ICE (find a path) + DTLS (authenticate) + SRTP (encrypted media). A *backend* does: signaling, STUN, TURN relay, optional media processing (echo/mix/record). Browsers do the heavy codec work — your C backend mostly shuttles offers/answers and relays packets.

| # | Bite | Do this |
|---|------|---------|
| 69 | WebRTC anatomy day | Draw the full connection flow on paper: signaling → offer/answer (SDP) → ICE candidates → STUN checks → DTLS → SRTP. Annotate which part is browser vs server. |
| 70 | Observe before building | Install Node's Pion example set (`pion/webrtc` examples) or `aiortc`. Run an echo demo, capture with Wireshark + tcpdump. *Identify your STUN packets and SRTP packets in the capture.* Now the theory has a face. |
| 71 | WebSocket server in C | Use **libwebsockets** (or a minimal hand-rolled WS implementation over bite 55's httpd — hard mode, but very instructive). Echo messages between two connected clients: **you now have a signaling channel.** |
| 72 | Signaling protocol | Define your own tiny JSON messages: `{"type":"offer"}`, `{"type":"answer"}`, `{"type":"candidate"}`. Relay them between peers through your server. Parse with a small C JSON lib (cJSON). |
| 73 | SDP literacy | Generate a fake offer in a browser page, print it server-side, parse out `m=`, `a=fingerprint:`, `a=candidate:` lines. No magic left: SDP is just lines. |
| 74 | ICE-lite server | Implement a **minimal ICE-lite** responder: answer STUN Binding requests on your media UDP port (extend bite 66), matching the `ufrag` you put in the SDP. Wireshark should show a successful binding. |
| 75 | DTLS handshake | Use OpenSSL's DTLSv1.2 APIs to complete the handshake with a browser on your media port. Use the SDP fingerprint to verify the cert. Hardest bite in the plan — take it in two sittings. |
| 76 | SRTP keying + send | Derive SRTP keys from the DTLS exporter, initialize **libsrtp**, and send an actual RTP packet ( PCM or a silent 20 ms frame ) to the browser. Hook up `<audio>` element proof-of-life if possible, else verify SRTP decrypt in Wireshark. |
| 77 | Media echo | Receive browser audio → decode or pass through → send back. Loopback: browser talks to your server, hears itself. For pass-through, strip/enforce correct RTP header fields; for decode, use a small codec lib or raw PCM if the browser sends `PCMU`. |
| 78 | TURN relay essentials | Understand when direct P2P fails (symmetric NAT). Either integrate **coturn** as a sidecar and configure it, or implement a minimal TURN Allocate/Permission subset for learning. Test a connection *through* it. |
| 79 | Hardening & ops | Reconnection, timeouts, resource cleanup (every socket, every SSL object, every srtp session), logging, graceful shutdown, valgrind on the server. Concurrent 2-peer test. |
| 80 | **Final checkpoint: the backend** | Your C WebRTC backend: WebSocket signaling between two browser tabs, ICE-lite + DTLS + SRTP audio loopback, clean shutdown, valgrind-clean, README that explains every layer in your own words. Ship it. Push it. It's a portfolio piece. |

---

## Milestone Summary

- ✅ **After Phase 2** — you can read any beginner C code.
- ✅ **After Phase 3** — you can write data structures and not leak memory.
- ✅ **After Phase 4** — you can build, debug, and organize real projects.
- ✅ **After Phase 5** — you can write network servers and think concurrently.
- ✅ **After Phase 6** — you understand real-time media and STUN from first principles.
- ✅ **After Phase 7** — you have written a WebRTC backend in C. The elephant is eaten.

## Rescue Rules (when stuck)

- Stuck > 30 min on a compile error → read the error aloud, line by line.
- Stuck > 1 day on a concept → do the bite's "do this" *wrong on purpose*, then compare against a reference implementation.
- Segfault you can't find → `valgrind` first, gdb second, rubber duck third.
- Lost motivation → shrink the bite. A bite can be "just get it to compile." Never zero.

## Reference Shelf (keep these open, don't read cover-to-cover)

- K&R, *The C Programming Language* — the classic; use as a reference, not a course.
- *Modern C* (Jens Gustedt) — free PDF, correct and current.
- Beej's Guide to Network Programming — for Phase 5.
- RFC 5389 (STUN), RFC 8445 (ICE), RFC 3711 (SRTP), RFC 8825+ (WebRTC overview) — for Phases 6–7. Read *sections*, not whole RFCs.
- `man 2 socket`, `man 3 pthread_create` — your real daily reference.
