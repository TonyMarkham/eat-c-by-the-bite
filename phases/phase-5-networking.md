# Phase 5 — The Gut: Systems & Networking (Bites 45–58)

*Goal: think in bytes, sockets, and concurrency. Everything here runs in WSL.*

Everything in this phase runs on Linux/WSL. `man` pages are your daily
companion: `man 2 socket`, `man 7 ip`, `man 3 pthread_create`.

---

## Bite 45 — Processes: fork/exec/wait

> **Goal:** Understand what a process is by creating and controlling them.
>
> **Learn:**
> - `fork()` returns twice: 0 in the child, child's PID in the parent. Same code, diverging paths.
> - `exec` replaces the current program image entirely — nothing after a successful `exec` runs.
> - `wait`/`waitpid` collect exit status — a finished, un-waited child becomes a zombie.
>
> **Do:**
> 1. Fork and have parent and child each print their PID and the relationship. Run it 5 times — race order will differ.
> 2. Mini-shell: fork + `execlp("ls", "ls", "-l", NULL)` + wait, print the exit status (`WEXITSTATUS`).
> 3. Extend it to accept a command line from the user and split it into args.
>
> **Break it on purpose:** Fork 5 children that never get waited on; check `ps` for zombies. Then wait for them all — zombies gone. (In Phase 5's servers you'll need `signal(SIGCHLD, SIG_IGN)` or a wait loop — this is why.)
>
> **Done when:** Your mini-shell runs `ls -l` and reports its exit code correctly.

## Bite 46 — TCP Client: Speaking HTTP by Hand

> **Goal:** Open a raw TCP connection and trade bytes with a real server.
>
> **Learn:**
> - The socket sequence for a client: `socket` → `getaddrinfo` (hostname → address) → `connect` → `send`/`recv` → `close`.
> - TCP is a **byte stream** with no message boundaries. What you send in one `send` may arrive in two `recv`s. This single fact causes half of all beginner network bugs.
> - HTTP/1.1 is just text over TCP. "Web magic" = bytes.
>
> **Do:**
> 1. Connect to `example.com:80`, send `GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n` by hand.
> 2. `recv` in a loop until 0 (EOF), print raw bytes — status line, headers, HTML body, all visible.
> 3. Check every return value: `socket`, `connect`, `send`, `recv`. Print `errno`-based errors with `perror` on failure.
>
> **Break it on purpose:** Send a request missing the final blank line — the server waits forever (it's still waiting for headers). Add a receive timeout (`setsockopt`, `SO_RCVTIMEO`) so your client can never hang forever. Timeouts: a habit from today.
>
> **Done when:** You've fetched a real webpage with no HTTP library, and can explain why the trailing `\r\n\r\n` matters.

## Bite 47 — TCP Echo Server

> **Goal:** The server side of the sequence: bind, listen, accept, serve.
>
> **Learn:**
> - Server sequence: `socket` → `setsockopt(SO_REUSEADDR)` (why: TIME_WAIT — you'll hit this error without it) → `bind` → `listen` → loop of `accept`.
> - `accept` returns a *new* socket per client. Listen socket ≠ connection socket.
> - One client at a time: while client 1 is served, client 2 waits in the backlog.
>
> **Do:**
> 1. Echo server: accept one connection, echo every byte back, until the client closes. Log connects/disconnects to stderr.
> 2. Test with `nc 127.0.0.1 7777` in another terminal.
> 3. Connect a second `nc` while the first is connected — confirm it hangs (then gets served when the first exits).
>
> **Break it on purpose:** Remove `SO_REUSEADDR`, kill the server with Ctrl-C, restart immediately — "Address already in use." Add the option back, restart works. Everyone meets this error; you met it on purpose.
>
> **Done when:** Echo works, the server restarts cleanly after Ctrl-C, and you can write the 5-call server sequence from memory.

## Bite 48 — Robust recv: Streams Are Not Messages

> **Goal:** Internalize the most important networking lesson in this entire plan.
>
> **Learn:**
> - `recv` returns *whatever is available* — half a message, two messages, one byte. Your protocol must define message boundaries itself.
> - Two canonical reads: **read exactly N** (loop until N total or EOF) and **read until delimiter** (accumulate until `\r\n\r\n`, bounded by a max).
> - Every protocol you'll ever parse (HTTP, your bite-53 protocol, SDP, STUN) is one of these two, or fixed-size fields + one of them.
>
> **Do:**
> 1. `int read_full(int fd, void *buf, size_t n)` — loop `recv`, accumulate, handle 0 (EOF) and -1 (error) distinctly.
> 2. `int read_line(int fd, char *buf, size_t max)` — byte-at-a-time accumulate until `\n`, enforce the max.
> 3. Prove the problem exists: a sender that does three `send`s ("hel", "lo wo", "rld\n") must arrive as one clean line to your `read_line` caller. Test it.
>
> **Break it on purpose:** Write a version that assumes one `recv` = one message. Test against the 3-part sender — observe truncation. Then fix with `read_full`. You'll never write the naive version again.
>
> **Done when:** `read_full` and `read_line` pass the split-sender test, and both handle EOF/error/overflow.

## Bite 49 — Concurrent Server: fork per Client

> **Goal:** Serve multiple clients at once — first concurrency model.
>
> **Learn:**
> - Per-connection `fork()`: parent returns to `accept` immediately, child handles one client. Simple, isolated, expensive (a process per client) — fine for learning scale.
> - The parent must **not** leak the connected socket; the child must **not** leak the listen socket. Each closes what it doesn't use.
> - Zombie children return: reap them (`SIGCHLD` → `SIG_IGN`, or `waitpid` in a loop with `WNOHANG`).
>
> **Do:**
> 1. Fork-per-connection echo server, using `read_line` from bite 48. Prefix each echoed line with the connection number.
> 2. Two `nc` clients simultaneously — both live, both echoed.
> 3. Add the reaping strategy; connect/disconnect 10 clients rapidly; `ps` shows zero zombies.
>
> **Break it on purpose:** Skip reaping, connect/disconnect 20 times, watch zombies accumulate in `ps`. Then fix. Resource leaks you can *see* in a process list teach the best.
>
> **Done when:** Two simultaneous clients echo correctly, no zombies, parent/child fd hygiene is correct.

## Bite 50 — Threads & Mutexes

> **Goal:** Shared-memory concurrency: what it costs, and why the mutex exists.
>
> **Learn:**
> - `pthread_create`/`pthread_join`; pass state via a malloc'd struct (bite 26 ownership rules apply).
> - Race condition: two threads `counter++` = read-modify-write interleaved = lost updates. Compile with `-pthread`.
> - `pthread_mutex_lock/unlock` around shared state. Rule: lock the *data*, not the code — decide per-structure what its lock protects.
> - Threads share memory: one bad write in one thread corrupts everyone. (Processes don't — trade-offs!)
>
> **Do:**
> 1. Global counter, 4 threads × 100,000 increments. Print the result: wrong, differently every run.
> 2. Add a mutex: 400,000, every run. Measure both timings.
> 3. Fork-per-connection echo server again — but threaded: `pthread` per client, a mutex-protected connection counter, printed on connect/disconnect.
>
> **Break it on purpose:** Unlock one path but not another (early return without unlock). Deadlock it on the second connection. Diagnose both — with helgrind (`valgrind --tool=helgrind`) if you're brave.
>
> **Done when:** You can reproduce a race on demand and fix it, and your threaded echo server handles 4 concurrent clients.

## Bite 51 — poll/epoll: The Event Loop

> **Goal:** One thread, many connections — the architecture of every serious server, including your WebRTC backend's.
>
> **Learn:**
> - Thread-per-client dies at scale (memory, context switching). The event loop: wait on *many* fds, handle whichever is ready.
> - `poll(fds, nfds, timeout)`: array of `{fd, events, revents}` — simple, portable, fine to hundreds of connections. `epoll` scales further (level vs edge-triggered — read the man page when you get there).
> - The pattern: single loop → ready events → nonblocking-ish handlers → repeat. State per connection lives in your own struct.
>
> **Do:**
> 1. Rewrite the echo server with `poll`: listen fd + dynamic array of client fds (your intvec! it grows!).
> 2. Handle: POLLIN on listen fd = accept; POLLIN on client = `read_line` + echo; POLLHUP/0-byte read = close and remove (swap-with-last on the array).
> 3. Stress it: 10 `nc` clients open simultaneously, each chatting independently. One thread. Watch `htop`: one process, tiny memory.
>
> **Break it on purpose:** Forget to remove a closed fd from the poll array → poll spins at 100% CPU on a dead fd. Watch `top` catch it. Event-loop hygiene = removing dead fds promptly.
>
> **Done when:** 10 concurrent clients chat through one single-threaded event loop, clean shutdown on Ctrl-C (close all fds).

## Bite 52 — UDP

> **Goal:** The connectionless protocol — and the one all WebRTC media actually rides on.
>
> **Learn:**
> - `socket(AF_INET, SOCK_DGRAM)` → `sendto`/`recvfrom` with per-packet addresses. No connect/accept (there *is* `connect` for UDP — it just pins a default peer).
> - Properties to burn into memory: no ordering, no delivery guarantee, no congestion control, but preserves message boundaries — each `sendto` = one `recvfrom` (dgram semantics!).
> - Every WebRTC media packet is a UDP datagram. Your server in Phase 7 is primarily a UDP program.
>
> **Do:**
> 1. UDP echo server + client. Send 10 datagrams, count replies back — run it 10 times, get 10 different counts. Loss, live.
> 2. Send a datagram larger than the MTU (e.g., 20,000 bytes) — observe fragmentation behavior and the ~64KB practical ceiling.
> 3. Send to a closed port — note the ICMP-unreachable surfacing as an error on a *later* call (UDP error handling is asynchronous and weird; just observe).
>
> **Break it on purpose:** Send datagrams in rapid bursts of 100 — count what comes back. Loss. Nothing you did wrong; that's UDP. Design protocols that tolerate it (bite 61).
>
> **Done when:** Echo works (mostly — and you can explain why "mostly" is correct UDP behavior).

## Bite 53 — Designing a Line Protocol

> **Goal:** Design, implement, and abuse a text protocol on your event-loop server.
>
> **Learn:**
> - Protocol design = commands, arguments, responses, errors, and *what happens on garbage*. Define it in a comment block before coding — your first protocol spec.
> - Strict parsing: wrong command count, unknown command, trailing garbage → explicit error response, never a guess.
> - State: your server tracks per-connection state (name? session?) in a struct — the event loop's data model.
>
> **Do:**
> 1. Spec (in comments): `ADD a b` → `SUM c` | `ERR msg`; `ECHO text...` → echo; `WHO` → your name; `BYE` → close. Max line 256.
> 2. Implement on the poll server using `read_line`. Dispatch via a table (bite 31 — told you it returns).
> 3. Abuse it from `nc`: lowercase commands, 300-char lines, 10 arguments, binary garbage (`\x00\xff`), empty lines. Server answers `ERR` and lives.
>
> **Break it on purpose:** Try to inject a command split across two `send`s (`AD` + `D 3 4`). Your `read_line` already handles it — verify, then appreciate why bite 48 exists.
>
> **Done when:** The abuse script gets only well-formed responses and the server never crashes or hangs.

## Bite 54 — Binary Protocol

> **Goal:** Frame-based binary messaging: length-prefixed, exactly how STUN and RTP packets are shaped.
>
> **Learn:**
> - Frame format: `[type:1][length:2 BE][payload]` — fixed header, variable body. Read header with `read_full`, then read exactly `length` bytes. This is *the* canonical framing.
> - All multi-byte fields: network byte order (bite 42's put/get in their natural habitat).
> - Length limits: reject length > your max before allocating — a 4-billion length field must not malloc 4 GB (this is a real vulnerability class).
>
> **Do:**
> 1. Spec in comments: type 1 = echo, 2 = sum-payload (two u32s → one u32 response), type 255 = error.
> 2. Implement framing with `read_full` + your `get_u16be/get_u32be`.
> 3. Python client for testing (10 lines: `struct.pack(">HI", ...)`) — cross-language proof your spec is real. Reply in the same framing.
>
> **Break it on purpose:** Send length = 65535 with no payload. If your server tries to read it all → hang. Enforce max payload, respond `ERR`, continue serving. Vulnerability class: met and closed.
>
> **Done when:** Python client completes a full session of both message types; malformed frames get `ERR` without killing the connection or the server.

## Bite 55 — HTTP Server v1

> **Goal:** Parse real HTTP requests and serve real responses — by hand.
>
> **Learn:**
> - Request shape: request line (`METHOD PATH VERSION`), headers until `\r\n\r\n`, optional body. Your `read_until_delimiter` from bite 48 is the parser's spine.
> - Minimal correct responses: status line, `Content-Length` (browsers/curl hang without it), `Connection: close` (for now).
> - 404 and 400 are features: a server that answers garbage with a proper error is a server.
>
> **Do:**
> 1. Parse: method, path, version; reject non-GET with 405; reject malformed with 400.
> 2. Route: `/` → hello HTML page; `/time` → `Date:` header value or computed time; anything else → 404 page.
> 3. Serve one static file from a `./www` directory — **path traversal check first**: reject any path containing `..` (the classic vulnerability, closed on day one).
> 4. Test with `curl -v`.
>
> **Break it on purpose:** Send a request with no final `\r\n\r\n` (netcat, then wait) — your server must time out, not hang forever. Send `GET /../../etc/passwd` — must be 400/404, never file contents. Both attacks, both survived.
>
> **Done when:** curl gets real pages, real 404s, and the traversal attempt gets rejected.

## Bite 56 — HTTP Server v2

> **Goal:** Production-shaped: threaded, keep-alive, logged, robust.
>
> **Learn:**
> - Keep-alive: loop reading requests on one connection until `Connection: close` or timeout — this is a state machine, and state machines are how you'll model peer connections in Phase 7.
> - Thread-per-connection (bite 50) vs your poll loop (bite 51): implement one, reason about the other in comments. Correctness first, scale second.
> - Logging: one line per request — time, client, method, path, status. To stderr, structured enough to grep.
>
> **Do:**
> 1. Add keep-alive with a read timeout (`SO_RCVTIMEO`, 10 s) and a hard request cap per connection.
> 2. Add the request log + a startup log ("listening on :8080").
> 3. Graceful shutdown: a flag the signal handler sets; the accept loop exits; all sockets close; exit 0. (Signals in C are their own rabbit hole — keep the handler to "set a `volatile sig_atomic_t` flag" and nothing else.)
> 4. Concurrent curl test: 5 parallel curls, all succeed.
>
> **Break it on purpose:** Send 50 rapid requests through one keep-alive connection with `curl`'s pipelining or a small Python client. Any leak/hang = a state-machine bug; valgrind will confirm fds are all closed.
>
> **Done when:** 5 parallel curls pass, Ctrl-C exits cleanly with exit code 0, valgrind-clean shutdown.

## Bite 57 — TLS: Use It, Don't Write It

> **Goal:** Wrap a connection in TLS with OpenSSL — and understand exactly why you never hand-roll crypto.
>
> **Learn:**
> - TLS = the layer that upgrades your plain TCP bytes into authenticated, encrypted bytes. Your code barely changes: `SSL_write`/`SSL_read` instead of `send`/`recv`.
> - Handshake = certificates + key exchange. The part you must get right as an *application*: verify the hostname (`SSL_set1_host`) — skipping verification is how MITM happens.
> - `https://` in bite 46's client = this bite, applied.
>
> **Do:**
> 1. `apt install libssl-dev`. Client: `TLS_client_method` → `SSL_CTX` → `SSL_set1_host` + `SSL_set1_host` verification → `SSL_connect` → fetch `https://example.com/` by hand.
> 2. Print the negotiated TLS version and cipher.
> 3. Check the certificate verification actually works: point it at a host with a bad cert (`self-signed.badssl.com`) — must *fail*. That failure is the feature.
>
> **Break it on purpose:** Comment out verification, fetch the bad-ssl host — succeeds (horror). Restore verification. You've now personally witnessed the vulnerability your Phase-7 server must never have.
>
> **Done when:** Hand-written HTTPS client fetches example.com, verifies certs, and fails loudly on bad certs.

## Bite 58 — ✅ Checkpoint: httpd

> **Goal:** The capstone of systems C. A small real server, ready to grow into a WebRTC backend.
>
> **Do:**
> - Your own HTTP server: static files (with traversal protection) + at least one JSON API endpoint (hand-built JSON — it's just strings and escaping) + 400/404/405/500 handling.
> - Concurrency: threaded or event-loop — your informed choice, justified in the README.
> - Keep-alive with timeouts, request logging, graceful shutdown, correct exit codes.
> - Tests: a script (shell or Python) driving `curl` through the happy paths and the abuse paths.
> - README: architecture sketch, endpoints, how to run, known limitations.
>
> **Break it on purpose:** Final abuse gauntlet: 20 parallel curls, slowloris-style slow headers (against your timeout), malformed request lines, giant headers, `..` traversal, SIGTERM mid-request. Nothing crashes, nothing hangs, nothing leaks (valgrind the whole run).
>
> **Done when:** The gauntlet passes and the README would let a stranger run it. Committed as `checkpoint 5: httpd`. **Everything after this builds directly on this server.**
