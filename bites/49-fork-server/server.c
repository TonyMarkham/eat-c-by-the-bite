/*
 * Bite 49 — Concurrent Server: fork per Client
 *
 * Compile & run:
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/49-fork-server/server.c -o /tmp/b49
 *   /tmp/b49                      (listens on 127.0.0.1:48049, Ctrl-C to stop)
 *
 * Test it (second terminal):
 *   nc 127.0.0.1 48049            (open TWO of these at the same time!)
 *   # each line you type comes back prefixed with your connection number:
 *   #   [1] hello
 *   # Both clients live simultaneously — each is its own PROCESS.
 *   # Connect/disconnect rapidly 10+ times, then check for zombies:
 *   #   ps -o pid,ppid,stat,cmd | grep -E 'PID|Z'
 *   # (see BREAK IT ON PURPOSE for what you'd see without reaping)
 *
 * WHAT THIS TEACHES (the "Learn" bullets):
 *  - Per-connection fork(): the parent returns to accept() immediately, the
 *    CHILD serves one client. Simple and isolated (a crash in one client's
 *    handler kills only that child), but expensive: a whole process per
 *    client. Fine at learning scale; threads (bite 50) and poll (bite 51)
 *    scale further.
 *  - FD HYGIENE: fork() copies ALL file descriptors. The parent must close
 *    the connected socket (it goes back to accept), the child must close the
 *    listen socket (it will never accept). Miss one and the socket stays
 *    open in the other process: connections never fully close, the listen
 *    queue fills, resources leak. Leak you can't see is still a leak.
 *  - Zombie children return (bite 45): every fork()ed child that exits
 *    without being waited on becomes a zombie. Here we reap with
 *    signal(SIGCHLD, SIG_IGN) — the kernel auto-reaps for us.
 *
 * BREAK IT ON PURPOSE:
 *   Delete the SIGCHLD sigaction block below. Connect/disconnect 20 times
 *   (rapid-fire: `for i in $(seq 20); do echo hi | nc 127.0.0.1 48049; done`),
 *   then run:
 *       ps -o pid,ppid,stat,cmd | grep -E 'PID|Z'
 *   Watch zombies accumulate (stat Z / <defunct>) — 20 leaked PIDs and exit
 *   statuses. Restore the sigaction, repeat: zero zombies. Resource leaks
 *   you can SEE in a process list teach the best.
 *
 * Done when: two simultaneous clients echo correctly, no zombies, parent/
 * child fd hygiene is correct.
 */

/* -std=c17 is STRICT ISO C: POSIX APIs (sigaction, fork...) must be
 * asked for explicitly. This must precede ALL includes. */
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

#define PORT 48047 + 2 /* 48049: distinct high port per bite */

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* ------------------------------------------------------------------ */
/* read_line — byte-at-a-time "read until \n", copied from bite 48.    */
/* Every protocol line (and later every HTTP request) is built on it.  */
/* Returns 1 line ok, 0 clean EOF, -1 error, -2 line too long.         */
/* ------------------------------------------------------------------ */
static int read_line(int fd, char *buf, size_t max)
{
    size_t len = 0;
    for (;;) {
        char ch;
        ssize_t r = recv(fd, &ch, 1, 0);
        if (r == 0) {
            if (len == 0)
                return 0;
            buf[len] = '\0';
            return 1;
        }
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (len + 1 >= max) {
            buf[max - 1] = '\0';
            return -2;
        }
        buf[len++] = ch;
        if (ch == '\n') {
            buf[len] = '\0';
            return 1;
        }
    }
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

/* Runs IN THE CHILD. Echo every line back with the connection prefix. */
static void serve_connection(int conn, long conn_num)
{
    char line[256];
    char out[300];

    for (;;) {
        int rc = read_line(conn, line, sizeof line);
        if (rc == 0) {
            fprintf(stderr, "[srv] conn %ld: client closed\n", conn_num);
            return;
        }
        if (rc == -1) {
            perror("read_line");
            return;
        }
        if (rc == -2) {
            /* Oversized junk: answer, drop input until newline, go on.
             * A hostile client must not own the connection. */
            snprintf(out, sizeof out, "ERR line too long\n");
            send_all(conn, out, strlen(out));
            continue;
        }
        snprintf(out, sizeof out, "[%ld] %s", conn_num, line);
        if (send_all(conn, out, strlen(out)) < 0) {
            fprintf(stderr, "[srv] conn %ld: send failed\n", conn_num);
            return;
        }
    }
}

int main(void)
{
    struct sigaction sa;
    int listen_fd;
    long conn_num = 0;

    /* Ctrl-C -> clean shutdown (same trick as bite 47). */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) < 0) {
        perror("sigaction");
        return 1;
    }

    /* THE REAPING STRATEGY (Do item 3): SIGCHLD -> SIG_IGN tells the kernel
     * "never hand me zombie children, reap them yourself". One line instead
     * of a waitpid(WNOHANG) loop. The loop variant exists when you want to
     * KNOW about child exits (exit codes); here we don't. */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        perror("sigaction(SIGCHLD)");
        return 1;
    }

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

    fprintf(stderr, "[srv] fork-per-connection echo on 127.0.0.1:%d\n", PORT);

    for (;;) {
        int conn = accept(listen_fd, NULL, NULL);
        if (conn < 0) {
            if (errno == EINTR) {
                if (g_stop)
                    break;
                continue;
            }
            perror("accept");
            break;
        }

        conn_num++;

        /* fork: two processes return from this line. */
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            close(conn); /* no child will own it — don't leak */
            continue;
        }

        if (pid == 0) {
            /* ---- CHILD ----
             * FD HYGIENE, child side: we will never accept again, so the
             * listen socket must go. Keeping it would hold the port open
             * even if the parent died, and keep every future client's
             * EOF semantics confused. */
            close(listen_fd);
            serve_connection(conn, conn_num);
            close(conn);
            _exit(0); /* _exit in a forked child: no inherited-cleanup games */
        }

        /* ---- PARENT ----
         * FD HYGIENE, parent side: the parent must NOT keep the connected
         * socket. If it did, the socket would never close from the server
         * side even after the child exits (the parent still holds a copy),
         * and clients would hang waiting for FIN. */
        close(conn);

        /* No waitpid needed: SIGCHLD is SIG_IGN, kernel auto-reaps.
         * Parent returns to accept() immediately — that's the whole point. */
    }

    fprintf(stderr, "[srv] shutdown (%ld connections served)\n", conn_num);
    close(listen_fd);
    return 0;
}
