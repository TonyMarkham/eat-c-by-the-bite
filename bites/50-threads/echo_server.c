/*
 * Bite 50 — Threads: threaded echo server (fork-per-client, but with threads)
 *
 * Compile & run:
 *   gcc -Wall -Wextra -Werror -std=c17 -g -pthread bites/50-threads/echo_server.c -o /tmp/b50srv
 *   /tmp/b50srv                    (listens on 127.0.0.1:48050, Ctrl-C to stop)
 *
 * Test it: open FOUR nc clients at once (all four should be live and echoed):
 *   nc 127.0.0.1 48050             (type anything; it comes back echoed)
 *
 * WHAT THIS TEACHES (the "Learn" bullets):
 *  - Thread-per-connection: like bite 49's fork(), but threads share memory
 *    and cost ~10x less to create. State passed to the thread goes through
 *    a malloc'd struct (bite 26 ownership rules: exactly one owner, the
 *    worker thread, frees it).
 *  - Shared state needs a mutex: the connection counter lives in the shared
 *    address space, so connect/disconnect updates from many threads must be
 *    locked (bite 50's counter_race.c shows what happens otherwise).
 *  - Threads are detached here: we don't need their return values, and a
 *    detached thread reaps itself. (Un-joined, un-detached threads leak.)
 *
 * BREAK IT ON PURPOSE:
 *   1) Unlock one path but not another: make an early `return` inside the
 *      locked region in worker_main() (e.g. return right after a failed
 *      send_all without unlocking). The NEXT connection's lock() blocks
 *      forever — a self-inflicted deadlock. Diagnose with gdb: `thread
 *      apply all bt` shows one thread parked in pthread_mutex_lock.
 *   2) Deadlock it on the second connection: lock the mutex TWICE in the
 *      same thread (pthread_mutex_lock is not recursive) — same symptom,
 *      different cause. valgrind --tool=helgrind names both mistakes.
 *
 * Done when: the threaded echo server handles 4 concurrent clients.
 */

/* -std=c17 is STRICT ISO C: POSIX APIs must be asked for explicitly.
 * This must precede ALL includes. */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define PORT 48050

/* ---- shared state: everything NOT protected by the mutex is a bug ---- */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static long g_connections_total = 0; /* protected by g_lock */
static long g_connections_active = 0; /* protected by g_lock */

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

/* The malloc'd state handed to the thread. Heap, not stack: the creating
 * thread's stack frame dies when accept-loop iteration ends, but the
 * worker is still running. Ownership: created by main, freed by worker. */
struct conn {
    int fd;
    long id;
};

/* One thread per connection. Echo raw bytes until the client closes. */
static void *worker_main(void *arg)
{
    struct conn *c = arg;

    fprintf(stderr, "[srv] conn %ld hello (active: %ld)\n",
            c->id, g_connections_active); /* read w/o lock: benign for a log,
                                             but SEE the comment below */

    for (;;) {
        char buf[4096];
        ssize_t n = recv(c->fd, buf, sizeof buf, 0);
        if (n == 0)
            break; /* client closed */
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("recv");
            break;
        }
        if (send_all(c->fd, buf, (size_t)n) < 0)
            break;
    }

    /* All shared-state updates must hold the mutex. If you insert an early
     * `return` INSIDE a locked region above/below, you create the deadlock
     * described in BREAK IT ON PURPOSE: the mutex stays locked forever and
     * every future connection hangs on lock(). */
    pthread_mutex_lock(&g_lock);
    g_connections_active--;
    fprintf(stderr, "[srv] conn %ld bye   (active: %ld, total served: %ld)\n",
            c->id, g_connections_active, g_connections_total);
    pthread_mutex_unlock(&g_lock);

    close(c->fd);
    free(c); /* the worker owns the struct — it frees what it was given */
    return NULL;
}

int main(void)
{
    struct sigaction sa;
    int listen_fd;

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

    fprintf(stderr, "[srv] threaded echo server on 127.0.0.1:%d "
                    "(open 4 nc clients at once)\n", PORT);

    for (;;) {
        struct sockaddr_in cli;
        socklen_t clilen = sizeof cli;
        struct conn *c;
        pthread_t tid;

        int conn = accept(listen_fd, (struct sockaddr *)&cli, &clilen);
        if (conn < 0) {
            if (errno == EINTR) {
                if (g_stop)
                    break;
                continue;
            }
            perror("accept");
            break;
        }

        c = malloc(sizeof *c); /* lives on the heap so the worker can reach it */
        if (c == NULL) {
            fprintf(stderr, "[srv] out of memory, dropping connection\n");
            close(conn);
            continue;
        }
        c->fd = conn;

        /* Assign id + count under the mutex: this shared counter is exactly
         * what counter_race.c proved you can't update unprotected. */
        pthread_mutex_lock(&g_lock);
        c->id = ++g_connections_total;
        g_connections_active++;
        pthread_mutex_unlock(&g_lock);

        /* Detached: the thread reaps itself when worker_main returns.
         * (If we wanted join(), we'd have to keep every pthread_t — and
         * then we'd need a shutdown path that joins them all. Detached is
         * the simple, correct choice for this bite.) */
        if (pthread_create(&tid, NULL, worker_main, c) != 0) {
            perror("pthread_create");
            free(c); /* nobody else owns it yet */
            close(conn);
            continue;
        }
        pthread_detach(tid);
        /* main does NOT close conn: the worker owns that fd now. */
    }

    fprintf(stderr, "[srv] shutdown (total connections: %ld)\n",
            g_connections_total);
    close(listen_fd);
    return 0;
}
