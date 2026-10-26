/*
 * Bite 52 — UDP client: four experiments in one binary
 *
 * Compile & run (start the server first):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/52-udp/client.c -o /tmp/b52cli
 *   /tmp/b52cli            ping   : 10 sequential datagrams, count replies
 *   /tmp/b52cli burst      : 100 rapid datagrams, count replies (LOSS!)
 *   /tmp/b52cli big        : one 20000-byte datagram (fragmentation)
 *   /tmp/b52cli closed     : datagram to a dead port (async ICMP error)
 *
 * Each mode demonstrates one "Do" item of the bite. See server.c for the
 * commentary on UDP's contract: no ordering, no delivery guarantee, but
 * datagram boundaries preserved.
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
#include <sys/time.h>
#include <unistd.h>

#define PORT 48052
#define DEAD_PORT 48099 /* nothing listens here — that's the point */
#define MAX_DGRAM_SAFE 65536 /* theoretical max UDP datagram size */

static int make_socket(void)
{
    struct sockaddr_in addr;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        exit(1);
    }

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    /* 127.0.0.1: loopback. The kernel routes it without any real network. */
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        fprintf(stderr, "inet_pton failed\n");
        exit(1);
    }
    addr.sin_port = htons(0); /* port 0 = let the kernel pick a free
                                 (ephemeral) client port. The SERVER owns
                                 48052; if the client bound the same port
                                 this bind would fail with EADDRINUSE. */
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        exit(1);
    }
    return fd;
}

static void set_recv_timeout(int fd, int ms)
{
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

/* ping: one datagram out, wait for ITS echo, repeat. Sequential means the
 * socket buffer never fills — this is the "network is fine" baseline. */
static void mode_ping(int fd)
{
    struct sockaddr_in dst;
    int replies = 0;

    memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dst.sin_port = htons(PORT);

    for (int i = 0; i < 10; i++) {
        char msg[64], buf[2048];
        snprintf(msg, sizeof msg, "ping %d", i);
        /* No connect() here: every sendto carries its own destination —
         * that's the connectionless deal. */
        if (sendto(fd, msg, strlen(msg), 0, (struct sockaddr *)&dst, sizeof dst) < 0) {
            perror("sendto");
            continue;
        }

        ssize_t n = recvfrom(fd, buf, sizeof buf, 0, NULL, NULL);
        if (n >= 0) {
            replies++;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* timed out: the datagram was lost or is still in flight.
             * With no ACK protocol, we just shrug — that's UDP. */
        } else {
            perror("recvfrom");
        }
    }
    printf("ping   : %d/10 replies. Run me 10 times — expect variance on a\n"
           "         real network (loopback losses are rare but possible\n"
           "         under load). 'Mostly' is correct UDP behavior.\n", replies);
}

/* burst: 100 datagrams fired as fast as possible, THEN drain replies.
 * The burst can overflow the kernel receive buffer on the server side —
 * packets beyond the buffer are DROPPED SILENTLY by the OS. That is loss
 * you did nothing to cause: exactly what your RTP media will face. */
static void mode_burst(int fd)
{
    struct sockaddr_in dst;
    int replies = 0;

    memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dst.sin_port = htons(PORT);

    set_recv_timeout(fd, 500);

    for (int i = 0; i < 100; i++) {
        char msg[64];
        snprintf(msg, sizeof msg, "burst %d", i);
        if (sendto(fd, msg, strlen(msg), 0, (struct sockaddr *)&dst, sizeof dst) < 0)
            perror("sendto");
    }
    for (;;) {
        char buf[2048];
        ssize_t n = recvfrom(fd, buf, sizeof buf, 0, NULL, NULL);
        if (n < 0)
            break; /* timeout: drain complete */
        replies++;
    }
    printf("burst  : %d/100 replies came back. The rest were dropped by the\n"
           "       network stack (buffer overflow / scheduling) — nobody\n"
           "       told you, no error was raised. Design for this (bite 61).\n",
           replies);
}

/* big: one 20000-byte datagram. Loopback MTU is 65536 so no IP
 * fragmentation is strictly forced here, but on real Ethernet (MTU 1500)
 * this datagram would be split into ~14 fragments, and ONE lost fragment
 * discards the WHOLE datagram. Practical max payload: ~65507 bytes
 * (65535 - IP header - UDP header). */
static void mode_big(int fd)
{
    struct sockaddr_in dst;
    static char payload[20000];
    char buf[MAX_DGRAM_SAFE];
    ssize_t n;

    memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dst.sin_port = htons(PORT);

    memset(payload, 'A', sizeof payload);
    printf("big    : sending %zu-byte datagram (over classic 1500 MTU)...\n",
           sizeof payload);
    if (sendto(fd, payload, sizeof payload, 0,
               (struct sockaddr *)&dst, sizeof dst) < 0) {
        perror("sendto"); /* e.g. EMSGSIZE beyond ~64KB */
        return;
    }
    set_recv_timeout(fd, 1000);
    n = recvfrom(fd, buf, sizeof buf, 0, NULL, NULL);
    if (n < 0) {
        printf("         no echo within 1s: some fragment was lost ->\n"
               "         the WHOLE datagram was discarded. All-or-nothing.\n");
    } else {
        printf("         echo came back: %zd bytes intact (boundaries kept).\n", n);
    }
}

/* closed: send to a port with no listener. UDP's weird error model: the
 * ICMP "port unreachable" comes back ASYNCHRONOUSLY and is delivered to
 * the socket on a LATER call — and only because we connect()ed, which
 * pins the peer and makes the kernel route ICMP errors to us. On an
 * unconnected socket this error is silently discarded by default! */
static void mode_closed(void)
{
    struct sockaddr_in dst;
    char msg[] = "are you there?";
    char buf[256];

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return;
    }
    memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dst.sin_port = htons(DEAD_PORT);

    if (connect(fd, (struct sockaddr *)&dst, sizeof dst) < 0) {
        perror("connect"); /* for UDP this is local bookkeeping only */
        close(fd);
        return;
    }
    printf("closed : sending to dead port %d (connected UDP socket)...\n",
           DEAD_PORT);
    if (send(fd, msg, strlen(msg), 0) < 0) {
        /* The send itself usually SUCCEEDS — the error hasn't happened
         * yet as far as this machine knows. */
        perror("send");
    }
    set_recv_timeout(fd, 2000);
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    printf("       : recv -> %zd, errno=%d (%s)\n"
           "         ^ the ICMP unreachable from the FIRST send surfaced\n"
           "         on this LATER call. Asynchronous error delivery:\n"
           "         another reason UDP protocols need their own ACKs.\n",
           n, errno, n < 0 ? strerror(errno) : "none");
    close(fd);
}

int main(int argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "ping";

    if (strcmp(mode, "closed") == 0) {
        mode_closed();
        return 0;
    }

    int fd = make_socket();
    if (strcmp(mode, "burst") == 0)
        mode_burst(fd);
    else if (strcmp(mode, "big") == 0)
        mode_big(fd);
    else
        mode_ping(fd);
    close(fd);
    return 0;
}
