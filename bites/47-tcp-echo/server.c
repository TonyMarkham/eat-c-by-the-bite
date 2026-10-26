/*
 * Bite 47 — TCP Echo Server
 *
 * Compile & run:
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/47-tcp-echo/server.c -o /tmp/b47
 *   /tmp/b47                       (listens on 127.0.0.1:48047, Ctrl-C to stop)
 *
 * Test it (second terminal):
 *   nc 127.0.0.1 48047             (type a line, get it back; Ctrl-C to quit)
 *   echo hello | nc 127.0.0.1 48047
 *   # Two clients at once: run `nc` twice, type in #1, watch #2 hang —
 *   # this server serves ONE client at a time; #2 waits in the backlog.
 *
 * THE SERVER SEQUENCE — memorize these five calls:
 *   1. socket()        get a phone (an endpoint), not yet dialed
 *   2. setsockopt(SO_REUSEADDR)  see below — do it BEFORE bind
 *   3. bind()          claim address+port (our "phone number")
 *   4. listen()        mark the socket as willing to accept; backlog = queue
 *                      of completed-but-unaccepted connections
 *   5. accept()        block until a client arrives; returns a NEW socket
 *                      for that client. Listen socket != connection socket:
 *                      the listen socket is the lobby door, accept() hands
 *                      you a private room per client.
 *
 * WHY SO_REUSEADDR: after Ctrl-C, our closed connections sit in TIME_WAIT
 * for ~60s (TCP's "late packets from the old connection" protection). A new
 * bind() to the same port normally fails with EADDRINUSE during that window.
 * SO_REUSEADDR tells the kernel "I know what I'm doing, let me rebind".
 * Everyone meets "Address already in use" — here you meet it on purpose.
 *
 * BREAK IT ON PURPOSE:
 *   Delete the setsockopt(SO_REUSEADDR) block, run the server, kill it with
 *   Ctrl-C, and start it again IMMEDIATELY:
 *       bind: Address already in use
 *   Put the option back — instant restart works. (Variation: `ss -tan | grep
 *   48047` right after the kill shows the TIME_WAIT sockets that caused it.)
 *
 * Done when: echo works, the server restarts cleanly after Ctrl-C, and you
 * can write the 5-call server sequence from memory.
 */

/* -std=c17 is STRICT ISO C: glibc hides POSIX APIs (sigaction among
 * them) unless the source asks for them. This must precede ALL includes. */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define PORT 48047 /* high port: no root needed, unlikely to clash */

/* Signal handlers may run at (almost) any moment, so the compiler/cache
 * games get weird. The ONLY safe shared type here: volatile sig_atomic_t,
 * and the only safe action: set a flag. Nothing else. */
static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int sig)
{
    (void)sig; /* unused-parameter warning otherwise; we only need the kick */
    g_stop = 1;
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

int main(void)
{
    struct sigaction sa;
    int listen_fd;
    long served = 0;

    /* Install SIGINT handler WITHOUT SA_RESTART: we WANT accept() to fail
     * with EINTR when Ctrl-C arrives, so the loop can check g_stop and exit
     * cleanly instead of blocking forever in accept(). */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* deliberately no SA_RESTART */
    if (sigaction(SIGINT, &sa, NULL) < 0) {
        perror("sigaction");
        return 1;
    }

    /* 1. socket: AF_INET = IPv4, SOCK_STREAM = TCP, 0 = default protocol. */
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    /* 2. SO_REUSEADDR — must be set BEFORE bind() to have any effect. */
    {
        int one = 1;
        if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
                       &one, sizeof one) < 0) {
            perror("setsockopt(SO_REUSEADDR)");
            close(listen_fd);
            return 1;
        }
    }

    /* 3. bind: which address+port do we answer on? INADDR_ANY = all local
     *    interfaces; 48047 = our port, in NETWORK byte order (htons). */
    {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof addr);
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(PORT);
        if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
            perror("bind"); /* EADDRINUSE lives here — see BREAK IT ON PURPOSE */
            close(listen_fd);
            return 1;
        }
    }

    /* 4. listen: socket becomes passive. Backlog 8: up to 8 pending
     *    connections queue while we serve the current one. */
    if (listen(listen_fd, 8) < 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

    fprintf(stderr, "[srv] echo server listening on 127.0.0.1:%d (Ctrl-C to stop)\n",
            PORT);

    /* 5. accept loop — one client AT A TIME. While we echo client 1, client
     *    2 sits fully connected in the backlog queue (test this with two nc!). */
    for (;;) {
        struct sockaddr_in cli;
        socklen_t clilen = sizeof cli;
        char ip[INET_ADDRSTRLEN];
        int conn;

        conn = accept(listen_fd, (struct sockaddr *)&cli, &clilen);
        if (conn < 0) {
            if (errno == EINTR) {
                if (g_stop)
                    break; /* Ctrl-C: leave cleanly */
                continue;  /* other interrupts: retry */
            }
            perror("accept");
            break;
        }
        served++;
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof ip);
        fprintf(stderr, "[srv] connect #%ld from %s:%d\n",
                served, ip, (int)ntohs(cli.sin_port));

        /* Echo loop: read a chunk, send the same chunk back, until the
         * client closes (recv == 0). NOTE for bite 48: we echo BYTES, not
         * messages — a stream has no messages. */
        for (;;) {
            char buf[4096];
            ssize_t n = recv(conn, buf, sizeof buf, 0);
            if (n == 0)
                break; /* client closed: EOF */
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                perror("recv");
                break;
            }
            if (send_all(conn, buf, (size_t)n) < 0) {
                perror("send");
                break;
            }
        }
        fprintf(stderr, "[srv] disconnect #%ld\n", served);
        close(conn); /* every accept()ed socket must be closed exactly once */

        if (g_stop)
            break;
    }

    fprintf(stderr, "[srv] shutting down (served %ld clients)\n", served);
    close(listen_fd);
    return 0;
}
