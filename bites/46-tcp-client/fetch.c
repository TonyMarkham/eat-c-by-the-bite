/*
 * Bite 46 — TCP Client: Speaking HTTP by Hand
 *
 * Compile & run:
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/46-tcp-client/fetch.c -o /tmp/b46
 *   /tmp/b46                        (fetch example.com:80, print raw bytes)
 *   /tmp/b46 example.com 80 /       (host, port, path — all overrideable)
 *
 * WHAT THIS TEACHES (the "Learn" bullets):
 *  - The client socket sequence: socket -> getaddrinfo -> connect ->
 *    send/recv -> close. getaddrinfo turns "example.com" into struct
 *    addrinfo candidates; connect picks the first one that works.
 *  - TCP is a BYTE STREAM with no message boundaries. One send() may arrive
 *    as two recv()s; two sends may arrive in one recv(). We send one request
 *    and recv() in a loop until 0 — the "0 means EOF" convention you will use
 *    in every server from here on.
 *  - HTTP/1.1 is just text over TCP. There is no magic layer: the request we
 *    type below IS the web.
 *
 * WHY THE TRAILING \r\n\r\n MATTERS:
 *   HTTP headers are terminated by a BLANK line. The server reads until it
 *   sees the empty line; without it, it keeps waiting for headers that never
 *   come. "GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close" (no
 *   final \r\n\r\n) is an unfinished sentence, and the server hangs on it.
 *
 * BREAK IT ON PURPOSE:
 *   Delete the final "\r\n\r\n" from the request below (leave one "\r\n")
 *   and run against any compliant server: the connection just... sits there.
 *   The server is still waiting for the rest of your headers. That hang is
 *   the whole lesson. Then note the SO_RCVTIMEO below: with a receive
 *   timeout the client can never hang forever — a habit from today on.
 *   (Restore the \r\n\r\n afterwards.)
 *
 * Done when: you fetched a real webpage with no HTTP library and can explain
 * why the trailing \r\n\r\n matters.
 */

/* -std=c17 is STRICT ISO C: glibc hides POSIX APIs (getaddrinfo among
 * them) unless the source asks for them. This must precede ALL includes. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>

/* Send every byte. send() may accept only part of a large buffer (kernel
 * socket buffer full); treat it like write() on a pipe: loop until done. */
static int send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue; /* interrupted by a signal: just retry */
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *host = (argc > 1) ? argv[1] : "example.com";
    const char *port = (argc > 2) ? argv[2] : "80";
    const char *path = (argc > 3) ? argv[3] : "/";
    struct addrinfo hints, *res = NULL, *rp;
    char request[512];
    int fd = -1;
    ssize_t n;
    long total = 0;

    /* ---- 1. Name -> address(es) -------------------------------------- */
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;     /* IPv4 or IPv6, whatever works */
    hints.ai_socktype = SOCK_STREAM; /* TCP */

    if (getaddrinfo(host, port, &hints, &res) != 0) {
        fprintf(stderr, "getaddrinfo failed for %s:%s\n", host, port);
        return 1;
    }

    /* ---- 2. socket + connect: try each candidate until one connects --- */
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0)
            continue; /* e.g. no IPv6 support: try the next candidate */

        /* CHECK EVERY RETURN VALUE (Do item 3). connect is the call that
         * actually fails in real life: wrong port, firewall, DNS lies. */
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break; /* connected */
        perror("connect");
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        fprintf(stderr, "could not connect to %s:%s\n", host, port);
        return 1;
    }

    /* ---- 3. The safety net: never hang forever on recv ---------------- */
    {
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) < 0)
            perror("setsockopt(SO_RCVTIMEO)");
        /* With this, a server that never answers makes recv fail with
         * EAGAIN/EWOULDBLOCK after 5s instead of blocking forever. */
    }

    /* ---- 4. Hand-write the HTTP request ------------------------------- */
    /* HTTP/1.1 REQUIRES a Host header (virtual hosting depends on it).
     * "Connection: close" asks the server to close after the response, so
     * our recv-loop-until-0 terminates naturally. The FINAL \r\n\r\n is the
     * end-of-headers marker — delete it and watch the server hang (see
     * BREAK IT ON PURPOSE). */
    snprintf(request, sizeof request,
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Connection: close\r\n"
             "\r\n",
             path, host);

    printf("--- sending request ----------------------------------\n%s",
           request);
    printf("--- raw response bytes --------------------------------\n");

    if (send_all(fd, request, strlen(request)) < 0) {
        perror("send");
        close(fd);
        return 1;
    }

    /* ---- 5. recv until EOF (0) ---------------------------------------- */
    /* Deliberately naive on purpose: one recv() may return any available
     * chunk. We don't care because we print a byte stream — but remember
     * this for bite 48, where "one recv = one message" breaks everything. */
    for (;;) {
        char buf[4096];
        n = recv(fd, buf, sizeof buf, 0);
        if (n == 0)
            break; /* 0 = orderly shutdown by peer = EOF */
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                fprintf(stderr, "\n[recv timed out after 5s — SO_RCVTIMEO "
                                "saved us from hanging forever]\n");
                break;
            }
            perror("recv");
            break;
        }
        /* fwrite, not printf: the body may contain NUL bytes or invalid
         * UTF-8; printf("%s") would stop at the first NUL. */
        fwrite(buf, 1, (size_t)n, stdout);
        total += n;
    }

    printf("\n--- end of response (%ld bytes) -----------------------\n", total);
    close(fd); /* releases the connection; the server sees EOF (recv == 0) */
    return 0;
}
