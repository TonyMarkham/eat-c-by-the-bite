# Checkpoint 5 — httpd

A small threaded HTTP server in C, the capstone of Phase 5. It exists to
prove you can turn sockets, framing, threads, and protocol parsing into a
program a stranger could run — and that everything after this (WebRTC
signaling in Phase 7) builds directly on it.

## Run it

```bash
cd bites/58-checkpoint-httpd
gcc -Wall -Wextra -Werror -std=c17 -g -pthread server.c -o server
./server            # listens on 127.0.0.1:48058
./server 49123      # or any port
./test.sh           # happy paths + abuse gauntlet, starts its own server
```

## Architecture (the honest sketch)

```
                 ┌────────────────────────────────────────────┐
  curl/nc  ───▶  │ main thread: accept() loop                 │
                 │   └─ per connection: malloc'd struct conn  │
                 │       pthread_create (detached) ───────────┼──▶ worker thread
                 └────────────────────────────────────────────┘    │ keep-alive loop:
                                                                    │  1. buffer until \r\n\r\n
                                                                    │  2. parse request line + headers
                                                                    │  3. buffer declared body
                                                                    │  4. dispatch(method, path)
                                                                    │  5. respond (Content-Length!)
                                                                    │  6. consume exact bytes used
                                                                    └─ close on close/timeout/cap
```

- **I/O model:** thread-per-connection (bite 50), keep-alive state
  machine per thread (bite 56). Chosen over a poll/epoll event loop
  (bite 51) **on purpose**: for this scale, thread-per-connection is the
  easier code to make *correct* — each connection owns its buffer, so no
  shared request-parsing state exists to get wrong. The event loop wins
  at thousands of sockets; when this server grows into the Phase-7
  signaling layer, that migration is the first thing on the roadmap.
- **Shared state:** exactly three counters (requests served, connections
  total, start time) behind one `pthread_mutex_t` — the bite-50 rule
  "lock the data, not the code", applied to the smallest possible data.
- **Framing:** all reads are buffered accumulations until
  `\r\n\r\n`/declared length — never "one recv = one request" (bite 48).
- **JSON:** hand-built strings with an escaping helper for `"`, `\`, and
  control characters. No library; JSON is just text with rules.
- **Graceful shutdown:** SIGINT/SIGTERM handler sets one
  `volatile sig_atomic_t` flag; accept() is interrupted (no
  `SA_RESTART`), the loop exits, the listen socket closes, exit 0.
  Worker threads are detached and their fds are closed by exit(); a
  full "stop accepting, drain, join all threads" shutdown is listed
  under Limitations.

## Endpoints

| Route          | Method | Response                                             |
|----------------|--------|------------------------------------------------------|
| `/`            | GET    | 200 HTML hello page                                  |
| `/index.html`  | GET    | 200 static file from `./www/`                        |
| `/time`        | GET    | 200 HTML, server local time                          |
| `/api/time`    | GET    | 200 JSON `{"now":"...","unix":123}`                  |
| `/api/stats`   | GET    | 200 JSON `{"requests_served":N,"connections":N,"uptime_seconds":N}` |
| anything else  | GET    | 404                                                  |
| any route      | POST/DELETE/… | 405 (+ `Allow: GET`)                          |
| garbage request line / oversized headers | any | 400                        |
| body > 64 KB   | any    | 413                                                  |
| file exists but unreadable | GET | 500                                         |

## Tests

`./test.sh` builds the server, starts it on `${PORT:-48058}`, and drives:

1. **Happy paths** — every route above, plus JSON body assertions.
2. **Errors** — 404, 405 on POST and DELETE.
3. **Abuse gauntlet:**
   - `../../etc/passwd` traversal with `curl --path-as-is` → 400,
   - malformed request line via `nc` → server survives,
   - a 20 KB header block → server survives,
   - slowloris-lite (half a request, then silence) → no early response,
     no crash,
   - 20 parallel curls → all 200,
   - 50 requests over ONE keep-alive connection (python3) → all 200.
4. **Graceful shutdown** — SIGTERM mid-traffic must exit with code 0.

## Known limitations (documented, not hidden)

- **No drain-and-join shutdown:** workers are detached; SIGTERM stops
  the accept loop and exits while in-flight responses may be cut. Fine
  for the checkpoint; the Phase-7 backend will do it properly.
- **GET only** for routes; bodies are read and discarded (keep-alive
  correctness), never interpreted.
- **No chunked transfer encoding, no HEAD, no conditional requests**,
  no directory listing, no MIME table beyond a handful of types.
- **Static file size cap** is 1 MB; larger files get a 500 instead of a
  streaming implementation.
- **Traversal protection is a substring check** (`..` in path → 400).
  Correct here; real servers normalize the path and verify containment
  under the document root instead.
- **HTTP only** — TLS (bite 57) is not wired into this server yet.
- Paths are used verbatim after the `..` check; `%XX` URL decoding is
  not implemented (a file literally named `a%20b.txt` is not reachable).

## What this checkpoint proves

You can go from "I have a port number" to a server that parses real
HTTP, serves real files, talks JSON, handles concurrent clients, survives
deliberate abuse, shuts down cleanly, and passes a test suite — in
portable C, with zero warnings under `-Wall -Wextra -Werror`, and with
every design trade-off written down.
