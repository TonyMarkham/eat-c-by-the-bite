/*
 * Bite 54 — Binary Protocol: length-prefixed frames
 *
 * Compile & run:
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/54-binary-protocol/server.c -o /tmp/b54srv
 *   /tmp/b54srv                          (listens on 127.0.0.1:48054)
 *   python3 bites/54-binary-protocol/client.py   (the cross-language test)
 *
 * ================================================================== *
 * THE PROTOCOL SPEC — frames, not lines.                             *
 *                                                                    *
 *   Transport : TCP.                                                 *
 *   Frame     : [ type: 1 byte ][ length: 2 bytes BIG-ENDIAN ]       *
 *               [ payload: exactly `length` bytes ]                  *
 *                                                                    *
 *   Client -> server:                                                *
 *     type 1 = ECHO  : payload arbitrary (<= MAX_PAYLOAD); server    *
 *                      replies type 1 with the same payload.         *
 *     type 2 = SUM   : payload EXACTLY 8 bytes = two u32 BE (a, b);  *
 *                      server replies type 2, 4 bytes = a+b (mod     *
 *                      2^32).                                        *
 *                                                                    *
 *   Server -> client:                                                *
 *     type 1, 2 : responses as above.                                *
 *     type 255  = ERR : payload is a human-readable message (UTF-8,  *
 *                       no newline). Connection STAYS OPEN.          *
 *                                                                    *
 *   Limits: MAX_PAYLOAD = 4096. A frame whose declared length exceeds *
 *   it is answered with ERR and that connection is CLOSED: we never   *
 *   allocate from an attacker-controlled length, and we do not try to *
 *   resync inside half-received payload garbage (closing is safe,     *
 *   simple, and unambiguous). The SERVER keeps serving everyone else. *
 * ================================================================== *
 *
 * WHAT THIS TEACHES (the "Learn" bullets):
 *  - THE canonical framing: fixed header, variable body. Read the header
 *    with read_full, then read EXACTLY `length` bytes with read_full.
 *    Two read_fulls = one frame, every time, regardless of how TCP
 *    chopped the bytes up. STUN and RTP packets have exactly this shape
 *    (STUN: type+len header, attributes; RTP: fixed header + payload).
 *  - All multi-byte fields in NETWORK byte order (big-endian) — bite 42's
 *    put/get u16/u32 in their natural habitat. get_u16be below is
 *    hand-rolled to make the byte math visible.
 *  - Length limits: reject length > MAX before allocating. A 65535
 *    length field must NOT make you malloc 64KB per connection, and a
 *    4-billion length field (in a 4-byte variant) must NOT malloc 4GB.
 *
 * BREAK IT ON PURPOSE:
 *   Send a frame with length = 65535 and no payload behind it (the
 *   Python client's 'oversize' test does exactly this). A server that
 *   TRUSTS the length tries to read 65535 bytes that never come -> it
 *   hangs, one dead connection per hostile client until memory/fds run
 *   out. This server never allocates from the length field: it answers
 *   ERR immediately and closes ONLY that connection — the 'server
 *   still serves after ERR' test reconnects and proves it. Declared
 *   length fields are advisory, never trusted: vulnerability class,
 *   met and closed.
 *
 * Done when: the Python client completes a full session of both message
 * types; malformed frames get ERR without killing the connection or the
 * server.
 */

/* -std=c17 is STRICT ISO C: POSIX APIs (poll, sigaction...) must be
 * asked for explicitly. This must precede ALL includes. */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define PORT 48054
#define HDR_LEN 3          /* 1 type byte + 2 length bytes */
#define MAX_PAYLOAD 4096   /* spec limit — enforced, never trusted */
#define TYPE_ECHO 1
#define TYPE_SUM 2
#define TYPE_ERR 255

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* ------------------------------------------------------------------ */
/* Byte-order helpers — bite 42's put/get, at their day job.          */
/* Unsigned char loads avoid any aliasing/alignment questions.        */
/* ------------------------------------------------------------------ */
static unsigned get_u16be(const unsigned char *p)
{
    return ((unsigned)p[0] << 8) | (unsigned)p[1];
}

static unsigned long get_u32be(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

static void put_u16be(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)((v >> 8) & 0xFF);
    p[1] = (unsigned char)(v & 0xFF);
}

static void put_u32be(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)((v >> 24) & 0xFF);
    p[1] = (unsigned char)((v >> 16) & 0xFF);
    p[2] = (unsigned char)((v >> 8) & 0xFF);
    p[3] = (unsigned char)(v & 0xFF);
}

static int send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Send one complete frame: header + payload. */
static int send_frame(int fd, unsigned char type, const unsigned char *payload,
                      unsigned len)
{
    unsigned char hdr[HDR_LEN];
    hdr[0] = type;
    put_u16be(hdr + 1, len);
    if (send_all(fd, hdr, sizeof hdr) < 0)
        return -1;
    if (len > 0 && send_all(fd, payload, len) < 0)
        return -1;
    return 0;
}

static int send_err(int fd, const char *msg)
{
    return send_frame(fd, TYPE_ERR, (const unsigned char *)msg,
                      (unsigned)strlen(msg));
}

/* ------------------------------------------------------------------ */
/* Per-connection state. The buffer grows as needed up to a hard cap  */
/* big enough for the largest LEGAL frame plus one oversized header   */
/* we may have to swallow (65535 payload + 3 header).                 */
/* ------------------------------------------------------------------ */
#define BUF_HARD_CAP (65535 + HDR_LEN)

struct client {
    int fd;
    unsigned char *buf;
    size_t len, cap;
};

static struct client *g_clients;
static size_t g_nclients, g_cap;

static int add_client(int fd)
{
    if (g_nclients == g_cap) {
        size_t ncap = g_cap ? g_cap * 2 : 8;
        struct client *tmp = realloc(g_clients, ncap * sizeof *tmp);
        if (tmp == NULL)
            return -1;
        g_clients = tmp;
        g_cap = ncap;
    }
    struct client *c = &g_clients[g_nclients];
    c->fd = fd;
    c->buf = NULL;
    c->len = c->cap = 0;
    g_nclients++;
    return 0;
}

static void close_client(size_t i)
{
    free(g_clients[i].buf); /* per-connection heap state dies with the conn */
    close(g_clients[i].fd);
    g_clients[i] = g_clients[g_nclients - 1];
    g_nclients--;
}

/* Handle ONE complete frame (payload already extracted). Returns 0 keep,
 * -1 close (send failure). */
static int handle_frame(struct client *c, unsigned char type,
                        const unsigned char *payload, unsigned len)
{
    switch (type) {
    case TYPE_ECHO:
        /* echo it straight back */
        return send_frame(c->fd, TYPE_ECHO, payload, len);

    case TYPE_SUM:
        if (len != 8) { /* strictness: exactly two u32s, no more, no less */
            send_err(c->fd, "SUM payload must be exactly 8 bytes");
            return 0;
        }
        {
            unsigned long a = get_u32be(payload);
            unsigned long b = get_u32be(payload + 4);
            unsigned char out[4];
            put_u32be(out, (a + b) & 0xFFFFFFFFUL); /* mod 2^32, per spec */
            return send_frame(c->fd, TYPE_SUM, out, 4);
        }

    default:
        /* Unknown types get ERR, not silence, not a close: the spec says
         * the connection survives protocol mistakes. */
        send_err(c->fd, "unknown frame type");
        return 0;
    }
}

/* Consume as many complete frames as the buffer holds. Returns 0 keep,
 * -1 close. This is a STREAM PARSER: bytes flow in arbitrary chunks,
 * frames are the unit we care about — the same loop shape you'll reuse
 * for RTP/STUN packets in Phase 6/7. */
static int process_buffer(struct client *c)
{
    for (;;) {
        if (c->len < HDR_LEN)
            break; /* not even a header yet: wait for more bytes */

        unsigned char type = c->buf[0];
        unsigned declared = get_u16be(c->buf + 1); /* NEVER trust this */

        if (declared > MAX_PAYLOAD) {
            /* The deliberate-bug case: a huge declared length with no
             * (or few) payload bytes. We NEVER allocate or wait for
             * `declared` bytes — waiting is the hang we are guarding
             * against (a hostile client declares 65535 and sends
             * nothing: a trusting server blocks forever). Answer ERR
             * at once and close this connection; the server itself
             * keeps serving every other connection. */
            send_err(c->fd, "payload too large (max 4096), closing");
            return -1;
        }

        if (c->len < HDR_LEN + declared)
            break; /* header ok, body incomplete: wait */

        /* dispatch the complete frame */
        if (handle_frame(c, type, c->buf + HDR_LEN, declared) < 0)
            return -1;

        memmove(c->buf, c->buf + HDR_LEN + declared,
                c->len - HDR_LEN - declared);
        c->len -= HDR_LEN + declared;
    }
    return 0;
}

int main(void)
{
    struct sigaction sa;
    int listen_fd;
    struct pollfd *pfds = NULL;
    size_t pfds_cap = 0;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }
    {
        int one = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    }
    {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof addr);
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(PORT);
        if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
            perror("bind");
            close(listen_fd);
            return 1;
        }
    }
    if (listen(listen_fd, 16) < 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

    fprintf(stderr, "[srv] binary protocol server on 127.0.0.1:%d\n", PORT);
    fprintf(stderr, "[srv] frame = [type:1][len:2 BE][payload]; "
                    "1=ECHO 2=SUM 255=ERR\n");

    while (!g_stop) {
        size_t need = g_nclients + 1;
        if (need > pfds_cap) {
            struct pollfd *tmp = realloc(pfds, need * sizeof *pfds);
            if (tmp == NULL)
                break;
            pfds = tmp;
            pfds_cap = need;
        }
        pfds[0].fd = listen_fd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        for (size_t i = 0; i < g_nclients; i++) {
            pfds[i + 1].fd = g_clients[i].fd;
            pfds[i + 1].events = POLLIN;
            pfds[i + 1].revents = 0;
        }

        int rc = poll(pfds, (nfds_t)need, 1000);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        if (pfds[0].revents & POLLIN) {
            int conn = accept(listen_fd, NULL, NULL);
            if (conn >= 0) {
                if (add_client(conn) < 0)
                    close(conn);
            }
        }

        int removed = 0;
        for (size_t i = g_nclients; i-- > 0 && !removed;) {
            short rev = pfds[i + 1].revents;
            if (!(rev & (POLLIN | POLLHUP | POLLERR)))
                continue;

            struct client *c = &g_clients[i];

            if (rev & (POLLHUP | POLLERR)) {
                close_client(i);
                removed = 1;
                break;
            }

            /* grow the buffer if the next recv needs room */
            if (c->len == c->cap) {
                size_t ncap = c->cap ? c->cap * 2 : 512;
                if (ncap > BUF_HARD_CAP)
                    ncap = BUF_HARD_CAP;
                unsigned char *tmp = realloc(c->buf, ncap);
                if (tmp == NULL) {
                    close_client(i); /* out of memory: drop this client */
                    removed = 1;
                    break;
                }
                c->buf = tmp;
                c->cap = ncap;
            }

            ssize_t n = recv(c->fd, c->buf + c->len, c->cap - c->len, 0);
            if (n == 0) {
                close_client(i);
                removed = 1;
                break;
            }
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN)
                    continue;
                close_client(i);
                removed = 1;
                break;
            }
            c->len += (size_t)n;

            if (process_buffer(c) < 0) { /* send-side failure */
                close_client(i);
                removed = 1;
                break;
            }
        }
    }

    fprintf(stderr, "[srv] shutting down\n");
    for (size_t i = 0; i < g_nclients; i++)
        close_client(i);
    free(g_clients);
    free(pfds);
    close(listen_fd);
    return 0;
}
