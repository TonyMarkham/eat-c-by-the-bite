/*
 * Bite 48 — Robust recv: Streams Are Not Messages
 *
 * Compile & run (self-testing — it forks sender processes and checks results):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/48-robust-recv/robust_recv.c -o /tmp/b48
 *   /tmp/b48
 * (Uses loopback TCP on an ephemeral port; no network needed.)
 *
 * THE MOST IMPORTANT NETWORKING LESSON IN THIS PLAN:
 *   TCP gives you a BYTE STREAM. recv() returns "whatever is available":
 *   half a message, two messages, or a single byte — never promise yourself
 *   that one recv() == one message. Your protocol must define message
 *   boundaries itself, and every protocol you will ever parse (HTTP, the
 *   bite-53 line protocol, SDP, STUN) is built from exactly two reads:
 *     1. read exactly N bytes            -> fixed-size fields, binary headers
 *     2. read until a delimiter          -> lines, HTTP headers
 *   (or fixed-size fields + one of those — see bite 54's [type][len][body]).
 *
 * WHAT THIS PROGRAM DOES:
 *   - implements read_full(fd, buf, n)   — "read exactly N"
 *   - implements read_line(fd, buf, max) — "read until \n, bounded"
 *   - forks real TCP senders that send in PIECES ("hel", "lo wo", "rld\n")
 *     and proves both functions reassemble the pieces correctly
 *   - runs the NAIVE version (one recv = one message) against the same
 *     sender so you SEE the truncation bug with your own eyes
 *
 * BREAK IT ON PURPOSE:
 *   The naive demo at the end IS the deliberate bug: a sender that does
 *   three sends must arrive as one clean line, but naive_recv() returns
 *   whatever first chunk happened to land ("hel") and CALLS IT A MESSAGE.
 *   Run the program a few times — the naive result varies with timing,
 *   the read_line result never does. Then fix: read_full. You will never
 *   write the naive version again.
 *
 * Done when: read_full and read_line pass the split-sender test, and both
 * handle EOF/error/overflow.
 */

/* -std=c17 is STRICT ISO C: POSIX APIs must be asked for explicitly.
 * This must precede ALL includes. */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h> /* nanosleep */
#include <unistd.h>

/* ================================================================== */
/* THE TWO CANONICAL READS                                            */
/* ================================================================== */

/*
 * read_full — read EXACTLY n bytes or fail.
 *   Returns 1 : success, n bytes in buf
 *           0 : clean EOF before n bytes arrived (buffer has a PARTIAL
 *               read whose length we can't report with this signature —
 *               if a caller needs that, it should loop recv() itself)
 *          -1 : hard error (errno set)
 * EINTR is retried: a signal arriving mid-read is not a data error.
 */
int read_full(int fd, void *buf, size_t n)
{
    char *p = buf;
    size_t got = 0;

    while (got < n) {
        ssize_t r = recv(fd, p + got, n - got, 0);
        if (r == 0)
            return 0; /* peer closed the stream mid-message */
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        got += (size_t)r; /* r may be ANY amount: 1 byte, half, or all */
    }
    return 1;
}

/*
 * read_line — read bytes until '\n' (inclusive) or the buffer is full.
 *   Returns  1 : a line was read; buf is NUL-terminated and contains the
 *                '\n' if there was one
 *            0 : clean EOF with NO bytes at all (stream over, no more lines)
 *           -1 : hard error
 *           -2 : line longer than max-1 (overflow: the honest failure)
 * A stream that ends mid-line (EOF with partial data) yields that partial
 * line — protocol designers must decide whether that's legal; here it is.
 */
int read_line(int fd, char *buf, size_t max)
{
    size_t len = 0;

    for (;;) {
        char ch;
        ssize_t r = recv(fd, &ch, 1, 0); /* byte-at-a-time: simple, a bit slow.
                                            Real servers buffer (bite 51+). */
        if (r == 0) {
            if (len == 0)
                return 0;   /* EOF before any byte: clean end of stream */
            buf[len] = '\0';
            return 1;       /* EOF terminated a partial line */
        }
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (len + 1 >= max) { /* need room for ch AND the NUL */
            buf[max - 1] = '\0';
            return -2;        /* overflow — do NOT silently truncate */
        }
        buf[len++] = ch;
        if (ch == '\n') {
            buf[len] = '\0';
            return 1;
        }
    }
}

/* ================================================================== */
/* THE NAIVE BUG, PRESERVED FOR POSTERITY                             */
/* ================================================================== */

/* Assumes one recv == one message. WRONG. Kept here so you can watch it
 * fail against the split sender. This exact bug produces "truncated
 * JSON", "random half-requests", and 3am pages everywhere. */
static void naive_recv_assumes_message(int fd, char *buf, size_t max)
{
    ssize_t n = recv(fd, buf, max - 1, 0);
    if (n < 0)
        n = 0;
    buf[n] = '\0';
}

/* ================================================================== */
/* Test harness: fork a child that connects and sends in pieces       */
/* ================================================================== */

typedef struct {
    const char *chunks[8]; /* what the sender sends, one send() per chunk */
    int nchunks;
    int delay_us;          /* pause between chunks (encourages separate recvs) */
} script_t;

static struct sockaddr_in g_bound_addr; /* set before fork, inherited by child */

/* Create a real loopback TCP connection; the CHILD plays the sender using
 * `script`, the parent gets the connected fd to test against. */
static int open_sender(const script_t *s)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int conn;
    socklen_t addrlen;
    pid_t pid;

    if (listen_fd < 0) {
        perror("socket");
        return -1;
    }

    memset(&g_bound_addr, 0, sizeof g_bound_addr);
    g_bound_addr.sin_family = AF_INET;
    g_bound_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    g_bound_addr.sin_port = 0; /* port 0 = kernel picks a free port */

    if (bind(listen_fd, (struct sockaddr *)&g_bound_addr, sizeof g_bound_addr) < 0 ||
        listen(listen_fd, 1) < 0) {
        perror("bind/listen");
        close(listen_fd);
        return -1;
    }
    addrlen = sizeof g_bound_addr;
    getsockname(listen_fd, (struct sockaddr *)&g_bound_addr, &addrlen);

    pid = fork();
    if (pid < 0) {
        perror("fork");
        close(listen_fd);
        return -1;
    }

    if (pid == 0) {
        /* ---- child: connect and send the script, then die ---- */
        int cfd, i;
        close(listen_fd); /* child doesn't listen — fd hygiene (bite 49!) */
        cfd = socket(AF_INET, SOCK_STREAM, 0);
        if (cfd < 0 ||
            connect(cfd, (struct sockaddr *)&g_bound_addr,
                    sizeof g_bound_addr) < 0)
            _exit(1);
        for (i = 0; i < s->nchunks; i++) {
            size_t len = strlen(s->chunks[i]);
            if (s->delay_us > 0) {
                /* nanosleep, not usleep: usleep was dropped from POSIX
                 * 2008 — and -std=c17 hides it. A 200809L-portable sleep
                 * takes a {seconds, nanoseconds} pair. */
                struct timespec ts = { .tv_sec = 0,
                                       .tv_nsec = s->delay_us * 1000L };
                nanosleep(&ts, NULL);
            }
            /* loop until the whole chunk is accepted (send may be partial) */
            {
                size_t off = 0;
                while (off < len) {
                    ssize_t w = send(cfd, s->chunks[i] + off, len - off, 0);
                    if (w <= 0)
                        _exit(1);
                    off += (size_t)w;
                }
            }
        }
        close(cfd); /* sender closes -> EOF for the tester */
        _exit(0);
    }

    /* ---- parent: accept the child's connection ---- */
    addrlen = sizeof g_bound_addr;
    conn = accept(listen_fd, NULL, &addrlen);
    close(listen_fd); /* parent is done with the lobby door */
    if (conn < 0) {
        perror("accept");
        return -1;
    }
    return conn;
}

static void finish_sender(int fd)
{
    int status;
    close(fd);
    waitpid(-1, &status, 0); /* reap the sender child (no zombies allowed) */
}

static int g_failures;

static void report(const char *name, int ok, const char *detail)
{
    printf("%-38s %s%s%s\n", name, ok ? "PASS" : "FAIL",
           detail && detail[0] ? "  (" : "",
           detail && detail[0] ? detail : "");
    if (!ok)
        g_failures++;
}

static void test_read_line_reassembles_split_sends(void)
{
    script_t s = { { "hel", "lo wo", "rld\n" }, 3, 120000 };
    char buf[64];
    int rc = -99;
    int fd = open_sender(&s);
    char detail[96]; /* big enough for a full 63-byte line + quotes */

    if (fd < 0) { report("read_line: 3 sends -> 1 line", 0, "no connection"); return; }
    rc = read_line(fd, buf, sizeof buf);
    snprintf(detail, sizeof detail, "got '%s'", buf);
    report("read_line: 3 sends -> 1 line",
           rc == 1 && strcmp(buf, "hello world\n") == 0, detail);
    finish_sender(fd);
}

static void test_read_full_exact(void)
{
    script_t s = { { "abcde", "fghi" }, 2, 100000 };
    char buf[16];
    int fd = open_sender(&s);

    if (fd < 0) { report("read_full: exact 9 bytes", 0, "no connection"); return; }
    memset(buf, '?', sizeof buf);
    {
        int rc = read_full(fd, buf, 9);
        buf[9] = '\0';
        report("read_full: exact 9 bytes",
               rc == 1 && memcmp(buf, "abcdefghi", 9) == 0, buf);
    }
    finish_sender(fd);
}

static void test_read_full_eof(void)
{
    script_t s = { { "short" }, 1, 0 };
    char buf[16];
    int fd = open_sender(&s);

    if (fd < 0) { report("read_full: EOF != error", 0, "no connection"); return; }
    /* First drain the 5 real bytes, THEN the next read must hit EOF (0),
     * which is a DIFFERENT outcome from -1 (error). Confusing the two is
     * how servers spin or hang on half-closed connections. */
    read_full(fd, buf, 5);
    {
        int rc = read_full(fd, buf, 1);
        report("read_full: EOF after data -> 0 (not -1)", rc == 0, NULL);
    }
    finish_sender(fd);
}

static void test_read_line_eof_partial(void)
{
    script_t s = { { "no newline here" }, 1, 0 };
    char buf[64];
    int fd = open_sender(&s);

    if (fd < 0) { report("read_line: EOF ends a partial line", 0, "no connection"); return; }
    {
        int rc = read_line(fd, buf, sizeof buf);
        report("read_line: EOF ends a partial line",
               rc == 1 && strcmp(buf, "no newline here") == 0, buf);
    }
    finish_sender(fd);
}

static void test_read_line_overflow(void)
{
    /* 200 'x' then '\n': must be refused, not silently truncated into a
     * "valid" short line — silent truncation corrupts every protocol. */
    char longline[220];
    script_t s;
    char buf[64];
    int fd, rc;

    memset(longline, 'x', 200);
    longline[200] = '\n';
    longline[201] = '\0';
    s.chunks[0] = longline;
    s.nchunks = 1;
    s.delay_us = 0;

    fd = open_sender(&s);
    if (fd < 0) { report("read_line: overflow -> -2", 0, "no connection"); return; }
    rc = read_line(fd, buf, sizeof buf);
    report("read_line: overflow -> -2", rc == -2, NULL);
    finish_sender(fd);
}

static void test_naive_recv_fails(void)
{
    script_t s = { { "hel", "lo wo", "rld\n" }, 3, 200000 };
    char buf[64];
    int fd = open_sender(&s);

    if (fd < 0) { report("naive recv: truncates (known bug)", 0, "no connection"); return; }
    naive_recv_assumes_message(fd, buf, sizeof buf);
    /* The point is not a strict assertion — scheduling varies. Show what
     * actually arrived and explain why it is untrustworthy either way:
     *  - "hel"        -> truncated message (the classic bug)
     *  - "hello world\n" -> PASSES today, breaks tomorrow under load.
     * Timing-dependent code that "usually works" is the worst kind. */
    printf("%-38s DEMO  naive single recv got: '%s'\n"
           "    ^ if that is the whole line it was LUCK, not correctness;\n"
           "      try again / raise the delay and watch it truncate\n",
           "naive recv: truncates (known bug)", buf);
    finish_sender(fd);
}

int main(void)
{
    printf("bite 48: proving that TCP is a byte stream, not a message bus\n\n");

    test_read_line_reassembles_split_sends();
    test_read_full_exact();
    test_read_full_eof();
    test_read_line_eof_partial();
    test_read_line_overflow();
    test_naive_recv_fails();

    printf("\n%s (%d failure%s)\n",
           g_failures == 0 ? "all tests passed" : "FAILURES PRESENT",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
