/*
 * Bite 52 — UDP: sendto/recvfrom, the connectionless protocol
 *
 * Compile & run (two terminals, or background the server):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/52-udp/server.c -o /tmp/b52srv
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/52-udp/client.c  -o /tmp/b52cli
 *   /tmp/b52srv &                  (listens on UDP 127.0.0.1:48052)
 *   /tmp/b52cli                    (send 10 datagrams, count the replies)
 *   /tmp/b52cli burst              (100 rapid datagrams -> observe LOSS)
 *   /tmp/b52cli big                (one 20000-byte datagram -> fragmentation)
 *   /tmp/b52cli closed             (send to a dead port -> async ICMP error)
 *   kill %1                        (always stop the server when done)
 *
 * WHAT THIS TEACHES (the "Learn" bullets):
 *  - UDP has NO connect/accept. socket(AF_INET, SOCK_DGRAM), bind (server),
 *    then every packet travels with its own address: recvfrom tells you
 *    WHO sent it, sendto sends to a WHO. (There IS a connect() for UDP —
 *    it just pins a default peer so you can use send/recv; see 'closed'.)
 *  - Properties to burn into memory: no ordering, no delivery guarantee,
 *    no congestion control — but PRESERVED MESSAGE BOUNDARIES. Each
 *    sendto() arrives as exactly one recvfrom() (or not at all). Compare
 *    with TCP: byte stream, guaranteed order, can lose the connection.
 *    Every WebRTC media packet is a UDP datagram; Phase 7's server is
 *    primarily a UDP program.
 *  - UDP errors are ASYNCHRONOUS and weird: an ICMP "port unreachable"
 *    coming back from the network surfaces as an error on a LATER recv
 *    call (and only on a *connected* UDP socket), not on the sendto that
 *    triggered it. The 'closed' mode lets you observe exactly that.
 *
 * BREAK IT ON PURPOSE:
 *   `burst` mode: 100 datagrams fired as fast as the loop allows, then we
 *   count what comes back. On loopback you'll typically get most of them
 *   (the kernel is fast and the socket buffer is deep); over a real
 *   network or with a slow receiver you would lose plenty. Nothing you
 *   did was wrong — THAT IS UDP. Protocols on top must tolerate loss
 *   (retransmit, sequence numbers — that's exactly bite 61's lesson).
 *
 * Done when: echo works "mostly" — and you can explain why "mostly" is
 * correct UDP behavior.
 */

/* -std=c17 is STRICT ISO C: POSIX APIs must be asked for explicitly.
 * This must precede ALL includes. */
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

#define PORT 48052
#define MAX_DGRAM 65536 /* theoretical max UDP payload ~65507; round buffer up */

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

int main(void)
{
    struct sigaction sa;
    int fd;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    /* SOCK_DGRAM = UDP. One socket, no listen/accept, no per-client state. */
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof addr);
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(PORT);
        if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
            perror("bind");
            close(fd);
            return 1;
        }
    }

    fprintf(stderr, "[srv] UDP echo server on 127.0.0.1:%d (Ctrl-C to stop)\n"
                    "[srv] NO connections: every datagram carries its own sender\n",
            PORT);

    for (;;) {
        char buf[MAX_DGRAM];
        struct sockaddr_in peer;
        socklen_t peerlen = sizeof peer;
        char ip[INET_ADDRSTRLEN];

        /* recvfrom = recv + "and tell me who sent it". THIS is the UDP
         * equivalent of accept(): per-datagram addressing. */
        ssize_t n = recvfrom(fd, buf, sizeof buf, 0,
                             (struct sockaddr *)&peer, &peerlen);
        if (n < 0) {
            if (errno == EINTR) {
                if (g_stop)
                    break;
                continue;
            }
            perror("recvfrom");
            break;
        }

        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        fprintf(stderr, "[srv] %zd bytes from %s:%d%s\n",
                n, ip, (int)ntohs(peer.sin_port),
                n > 1400 ? "  (fragmented on the wire!)" : "");

        /* Echo straight back to whoever sent it. NOTE the message
         * boundary preservation: what arrived as one datagram leaves as
         * one datagram. No \r\n framing needed, no read_full needed —
         * boundaries are the ONE thing UDP gives you for free. */
        if (sendto(fd, buf, (size_t)n, 0,
                   (struct sockaddr *)&peer, peerlen) < 0) {
            /* e.g. ECONNREFUSED if a previous ICMP error was cached */
            perror("sendto"); /* log and keep serving: one bad packet is
                                 not a reason to die */
        }
    }

    fprintf(stderr, "[srv] bye\n");
    close(fd);
    return 0;
}
