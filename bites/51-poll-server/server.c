/*
 * Bite 51 — poll/epoll: The Event Loop (one thread, many connections)
 *
 * Compile & run:
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/51-poll-server/server.c -o /tmp/b51
 *   /tmp/b51                      (listens on 127.0.0.1:48051, Ctrl-C to stop)
 *
 * Test it: open TEN nc clients at once and chat in all of them — one single
 * thread serves every one of them:
 *   for i in 1 2 3 4 5 6 7 8 9 10; do (sleep 20; echo bye) | nc 127.0.0.1 48051 & done
 *   # or just open several `nc 127.0.0.1 48051` terminals and type.
 *   # In htop/top you'll see ONE process at ~0% CPU while idle: the kernel
 *   # sleeps in poll() until SOMETHING is ready. That's the whole trick.
 *
 * WHAT THIS TEACHES (the "Learn" bullets):
 *  - Thread-per-client dies at scale: thousands of clients = thousands of
 *    stacks, pages of kernel memory, scheduler overhead. The EVENT LOOP
 *    inverts it: ONE thread waits on MANY fds and handles whichever is
 *    ready. This is the architecture of nginx, redis, libuv — and of the
 *    WebRTC signaling server you'll build in Phase 7.
 *  - poll(fds, nfds, timeout): an array of {fd, events, revents}. The
 *    kernel fills in revents for every fd that is ready. Simple, portable,
 *    fine to hundreds of connections. epoll scales further (per-fd cost is
 *    O(1) instead of scanning the array) — read `man 7 epoll` when you get
 *    there; the loop STRUCTURE you write here is identical, only the
 *    wait-and-ready mechanism changes (level- vs edge-triggered is the
 *    main new idea).
 *  - Per-connection state lives in YOUR struct (not in the OS): buffer,
 *    parse position, name, whatever the protocol needs. The kernel only
 *    tells you "fd is readable"; the rest is your data model.
 *
 * THE PATTERN (memorize this shape):
 *   setup listen socket
 *   loop:
 *     poll(fds)
 *     if listen fd readable -> accept, add client (fd + state) to array
 *     for each client fd readable -> read, process (never block!), close
 *       if dead; REMOVE dead entries from BOTH the poll array and the
 *       state array (swap-with-last keeps it O(1))
 *
 * BREAK IT ON PURPOSE:
 *   In close_client() below, "forget" the swap-with-last removal (comment
 *   it out, keep the close()). A closed fd whose poll entry stays in the
 *   array becomes IMMEDIATELY-ALWAYS-READY: poll returns instantly with
 *   POLLNVAL/POLLHUP forever, recv returns -1/0 forever — the loop spins
 *   at 100% CPU doing nothing. Watch it in `top` (the process eats a full
 *   core). Event-loop hygiene = removing dead fds PROMPTLY.
 *
 * Done when: 10 concurrent clients chat through one single-threaded event
 * loop, clean shutdown on Ctrl-C (close all fds).
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

#define PORT 48051
#define LISTEN_SLOT 0        /* index 0 of the poll array is the listen fd */
#define LINE_MAX_LEN 512     /* per-client echo buffer size */

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int sig)
{
    (void)sig;
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

/* Per-connection state — bite 25's growable-array pattern returns. */
struct client {
    int fd;
    char buf[LINE_MAX_LEN]; /* partial line bytes waiting for \n */
    size_t len;
};

static struct client *g_clients; /* dynamic array (your intvec, again) */
static size_t g_nclients;
static size_t g_cap;

static int add_client(int fd)
{
    if (g_nclients == g_cap) {
        size_t ncap = g_cap ? g_cap * 2 : 8;
        struct client *tmp = realloc(g_clients, ncap * sizeof *tmp);
        if (tmp == NULL) {
            fprintf(stderr, "[srv] out of memory\n");
            return -1;
        }
        g_clients = tmp;
        g_cap = ncap;
    }
    g_clients[g_nclients].fd = fd;
    g_clients[g_nclients].len = 0;
    g_nclients++;
    return 0;
}

/* Close a client and remove it from the array with SWAP-WITH-LAST:
 * O(1), order of clients doesn't matter. MUST happen for every dead fd —
 * see BREAK IT ON PURPOSE. */
static void close_client(size_t i)
{
    close(g_clients[i].fd);
    g_clients[i] = g_clients[g_nclients - 1]; /* move last into the hole */
    g_nclients--;
}

/* Consume complete lines out of the client's buffer and echo them back. */
static void echo_complete_lines(struct client *c)
{
    size_t start = 0;
    for (size_t i = 0; i < c->len; i++) {
        if (c->buf[i] == '\n') {
            send_all(c->fd, c->buf + start, i - start + 1); /* incl. \n */
            start = i + 1;
        }
    }
    if (start > 0) {
        memmove(c->buf, c->buf + start, c->len - start);
        c->len -= start;
    }
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

    /* The same 5-call server sequence as bites 47/49 — by now you should
     * be able to write this block from memory. */
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

    fprintf(stderr, "[srv] single-threaded poll() echo on 127.0.0.1:%d "
                    "(one thread serves ALL clients)\n", PORT);

    /* EVENT LOOP */
    while (!g_stop) {
        /* Rebuild the poll array every iteration: slot 0 = listen socket,
         * one slot per client. Rebuilding is O(n) but trivially correct —
         * incremental bookkeeping is an optimization, not a first step. */
        size_t need = g_nclients + 1;
        if (need > pfds_cap) {
            struct pollfd *tmp = realloc(pfds, need * sizeof *pfds);
            if (tmp == NULL) {
                fprintf(stderr, "[srv] out of memory\n");
                break;
            }
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

        /* THE WAIT: sleep until ANY fd is ready (or 1s passes, which is
         * just our chance to notice Ctrl-C). The process costs 0% CPU here. */
        int rc = poll(pfds, (nfds_t)need, 1000);
        if (rc < 0) {
            if (errno == EINTR)
                continue; /* signal arrived: loop, check g_stop */
            perror("poll");
            break;
        }

        /* Ready event 1: a new connection is pending. poll is LEVEL-
         * triggered, so a connection still waiting next iteration is seen
         * again — we never "miss" it. (With edge-triggered epoll this
         * would be a bug; that's the difference the man page talks about.) */
        if (pfds[0].revents & POLLIN) {
            int conn = accept(listen_fd, NULL, NULL);
            if (conn < 0) {
                if (errno != EINTR && errno != EAGAIN)
                    perror("accept");
            } else if (add_client(conn) < 0) {
                close(conn); /* can't track it -> can't serve it */
            } else {
                fprintf(stderr, "[srv] now serving %zu client%s\n",
                        g_nclients, g_nclients == 1 ? "" : "s");
            }
        }

        /* Ready event 2: some client has data (or died).
         * Subtlety: close_client() swaps the LAST client into slot i, which
         * would desync it from this iteration's pollfd array (pfds was
         * built before the swap). So after ANY removal we stop processing
         * this batch and rebuild+repoll next iteration — safe, because
         * poll is level-triggered: unread data is reported again. */
        int removed = 0;
        for (size_t i = g_nclients; i-- > 0 && !removed;) {
            short rev = pfds[i + 1].revents;

            if (!(rev & (POLLIN | POLLHUP | POLLERR)))
                continue; /* nothing to do for this client this round */

            struct client *c = &g_clients[i];
            ssize_t n = recv(c->fd, c->buf + c->len,
                             sizeof c->buf - c->len, 0);

            if (n == 0) {
                close_client(i); /* EOF: client gone -> REMOVE IT (see BREAK IT) */
                removed = 1;
                break;
            }
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN)
                    continue; /* spurious wakeup / interrupted: fine */
                close_client(i);
                removed = 1;
                break;
            }
            c->len += (size_t)n;

            if (c->len == sizeof c->buf) {
                /* No newline in a full buffer: a hostile 10 MB "line" must
                 * not grow our memory forever. Drop the client. */
                fprintf(stderr, "[srv] client %zu: line too long, dropped\n", i);
                close_client(i);
                removed = 1;
                break;
            }
            echo_complete_lines(c); /* only WHOLE lines go back out */
        }
    }

    /* CLEAN SHUTDOWN: every fd we own gets closed. In a server that keeps
     * more state, this is where you'd also free buffers, join threads, etc. */
    fprintf(stderr, "[srv] shutting down, closing %zu client connection(s)\n",
            g_nclients);
    for (size_t i = 0; i < g_nclients; i++)
        close(g_clients[i].fd);
    free(g_clients);
    free(pfds);
    close(listen_fd);
    return 0;
}
