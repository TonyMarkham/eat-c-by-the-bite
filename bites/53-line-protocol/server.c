/*
 * Bite 53 — Designing a Line Protocol (on the bite-51 event loop)
 *
 * Compile & run:
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/53-line-protocol/server.c -o /tmp/b53
 *   /tmp/b53                     (listens on 127.0.0.1:48053, Ctrl-C to stop)
 *
 * Test it:
 *   nc 127.0.0.1 48053
 *   then type:  ADD 3 4        -> SUM 7
 *               ECHO hi there  -> ECHO hi there
 *               WHO            -> WHO user5
 *               BYE            -> BYE  (connection closes)
 *   and abuse it: add, ADD 3, ADD 3 4 5, ADD x y, an empty line,
 *                 ADD 1 2 extra, a 300-character line, binary junk.
 *   Every answer must be an ERR line or a valid response; the server
 *   never dies, never hangs.
 *
 *   The split-command test (BREAK IT ON PURPOSE):
 *     (printf 'AD'; sleep 0.4; printf 'D 3 4\n') | nc 127.0.0.1 48053
 *   -> the two sends arrive SPLIT on the wire; read_line-style buffering
 *      reassembles them into "ADD 3 4" and you get "SUM 7". This is
 *      bite 48 paying rent.
 *
 * ================================================================== *
 * THE PROTOCOL SPEC — written BEFORE the code, as the bite demands.  *
 *                                                                    *
 *   Transport : TCP, one or more commands per connection.            *
 *   Framing   : one command per line, terminated by '\n' (a '\r'     *
 *               just before it is tolerated and stripped).           *
 *               Max line length: 256 bytes including the newline.    *
 *                                                                    *
 *   Commands (case-SENSITIVE — "add" is an unknown command):         *
 *     ADD <int> <int>   -> "SUM <int>\n"      (exactly 2 int args)    *
 *     ECHO <text...>    -> "ECHO <text>\n"   (text must be non-empty)*
 *     WHO               -> "WHO <name>\n"    (name assigned at conn) *
 *     BYE               -> "BYE\n" + close                            *
 *                                                                    *
 *   Errors: any malformed input gets exactly one line                *
 *     "ERR <reason>\n" and the connection STAYS OPEN. Lines longer   *
 *     than the max get one ERR; the rest of the overlong line is     *
 *     discarded up to the next newline, then serving continues.      *
 *                                                                    *
 *   Design principle: STRICT. Wrong arg count, unknown command,      *
 *   non-integer arguments, trailing garbage, empty line -> explicit  *
 *   ERR, never a guess. A protocol that guesses is a protocol that   *
 *   gets exploited.                                                  *
 * ================================================================== *
 *
 * WHAT THIS TEACHES:
 *  - Protocol design = commands + arguments + responses + errors + "what
 *    happens on garbage". You just wrote your first protocol spec.
 *  - Strict parsing with explicit error responses.
 *  - Per-connection state in a struct (name + partial-line buffer) —
 *    the event loop's data model, exactly like bite 51 promised.
 *  - Dispatch table (bite 31 returns!): commands as {name, fn} pairs —
 *    the same pattern that will route SDP/STUN messages in Phase 7.
 *
 * BREAK IT ON PURPOSE:
 *   Split a command across two sends: "AD" then "D 3 4\n" (the nc one-
 *   liner above). A naive one-recv-one-command server would see two
 *   commands ("AD", "D 3 4") and fail both. The buffered reader here
 *   handles it — verify by getting "SUM 7", then appreciate why
 *   bite 48 exists.
 *
 * Done when: the abuse script gets only well-formed responses and the
 * server never crashes or hangs.
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

#define PORT 48053
#define LINE_MAX_LEN 256 /* per the spec: max line length incl. '\n' */

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

static void reply(int fd, const char *text)
{
    send_all(fd, text, strlen(text));
}

/* ------------------------------------------------------------------ */
/* Per-connection state: the event loop's data model.                 */
/* ------------------------------------------------------------------ */
struct client {
    int fd;
    char name[24];          /* "user7" — assigned at connect, used by WHO */
    char buf[LINE_MAX_LEN]; /* partial line: bytes waiting for '\n' */
    size_t len;
    int dropping;           /* 1 = overlong line in flight: discard to '\n' */
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
    c->len = 0;
    c->dropping = 0;
    snprintf(c->name, sizeof c->name, "user%d", fd);
    g_nclients++;
    return 0;
}

static void close_client(size_t i)
{
    fprintf(stderr, "[srv] closing %s\n", g_clients[i].name);
    close(g_clients[i].fd);
    g_clients[i] = g_clients[g_nclients - 1]; /* swap-with-last */
    g_nclients--;
}

/* ------------------------------------------------------------------ */
/* Strict parsing helpers                                             */
/* ------------------------------------------------------------------ */

/* Parse an entire token as an integer — no trailing junk, no "3.5",
 * no "0x1". Strict means strtol must consume the WHOLE token. */
static int parse_int_strict(const char *tok, long *out)
{
    char *end;
    if (tok == NULL || *tok == '\0')
        return -1;
    errno = 0;
    *out = strtol(tok, &end, 10);
    if (errno != 0 || end == tok || *end != '\0')
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Command handlers. All return 0 = keep connection, -1 = close it.   */
/* raw = the whole line verbatim (after \r stripping) — handlers that */
/* need the exact remainder (ECHO) use it, everything else ignores it.*/
/* ------------------------------------------------------------------ */

static int cmd_add(struct client *c, int argc, char **argv, const char *raw)
{
    (void)raw;
    long a, b;
    char out[64];

    if (argc != 3) { /* exactly 2 args: "ADD 3", "ADD 3 4 5" -> ERR */
        reply(c->fd, "ERR ADD takes exactly 2 integer arguments\n");
        return 0;
    }
    if (parse_int_strict(argv[1], &a) < 0 || parse_int_strict(argv[2], &b) < 0) {
        reply(c->fd, "ERR ADD arguments must be integers\n");
        return 0;
    }
    snprintf(out, sizeof out, "SUM %ld\n", a + b);
    reply(c->fd, out);
    return 0;
}

static int cmd_echo(struct client *c, int argc, char **argv, const char *raw)
{
    char out[LINE_MAX_LEN + 16];

    (void)argv;
    /* raw is an UNTOUCHED copy of the line (handle_line copies before
     * strtok mutates it) — the only way to see the original spacing. */
    const char *rest = raw + strlen("ECHO");
    while (*rest == ' ' || *rest == '\t')
        rest++;
    if (argc < 2 || *rest == '\0') {
        reply(c->fd, "ERR ECHO requires text\n");
        return 0;
    }
    snprintf(out, sizeof out, "ECHO %s\n", rest); /* verbatim remainder */
    reply(c->fd, out);
    return 0;
}

static int cmd_who(struct client *c, int argc, char **argv, const char *raw)
{
    (void)argv, (void)raw;
    if (argc != 1) { /* WHO takes no arguments — strictness again */
        reply(c->fd, "ERR WHO takes no arguments\n");
        return 0;
    }
    char out[64];
    snprintf(out, sizeof out, "WHO %s\n", c->name);
    reply(c->fd, out);
    return 0;
}

static int cmd_bye(struct client *c, int argc, char **argv, const char *raw)
{
    (void)argv, (void)raw;
    if (argc != 1) {
        reply(c->fd, "ERR BYE takes no arguments\n");
        return 0; /* pedantic, but strict is strict */
    }
    reply(c->fd, "BYE\n");
    return -1; /* close */
}

/* THE DISPATCH TABLE (bite 31's function-pointer table, in its habitat).
 * Adding a command = one table row + one function. No if-chains. */
struct command {
    const char *name;
    int (*handler)(struct client *c, int argc, char **argv, const char *raw);
};

static const struct command g_commands[] = {
    { "ADD",  cmd_add  },
    { "ECHO", cmd_echo },
    { "WHO",  cmd_who  },
    { "BYE",  cmd_bye  },
};

/* Parse + dispatch one complete line (mutated in place). Returns
 * 0 = keep connection, -1 = close connection. */
static int handle_line(struct client *c, char *line)
{
    char *argv[16];
    int argc = 0;

    /* strip the trailing '\r' so CRLF clients work (spec: tolerated) */
    size_t L = strlen(line);
    if (L > 0 && line[L - 1] == '\n')
        line[--L] = '\0';
    if (L > 0 && line[L - 1] == '\r')
        line[--L] = '\0';

    if (L == 0) {
        reply(c->fd, "ERR empty line\n");
        return 0;
    }

    /* Keep an untouched copy: strtok() below mutates `line`, and ECHO
     * needs the original spacing of its remainder. */
    char rawcopy[LINE_MAX_LEN + 1];
    memcpy(rawcopy, line, L + 1);

    /* Tokenize on blanks. Multiple spaces BETWEEN tokens are tolerated
     * (the spec says so); command names and argument values are strictly
     * validated. Every protocol picks a side and documents it — ours
     * just did, in the spec block above. */
    char *tok = strtok(line, " \t");
    while (tok != NULL && argc < 16) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t");
    }
    if (tok != NULL) { /* more than 16 tokens */
        reply(c->fd, "ERR too many tokens\n");
        return 0;
    }

    for (size_t i = 0; i < sizeof g_commands / sizeof g_commands[0]; i++) {
        if (strcmp(argv[0], g_commands[i].name) == 0)
            return g_commands[i].handler(c, argc, argv, rawcopy);
    }
    reply(c->fd, "ERR unknown command (case-sensitive: ADD, ECHO, WHO, BYE)\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* The event loop (bite 51's shape, reused)                           */
/* ------------------------------------------------------------------ */

/* Feed buffered bytes through the line framer. Returns 0 keep / -1 close
 * (via BYE or fatal protocol abuse). */
static int process_buffer(struct client *c)
{
    for (;;) {
        if (c->dropping) {
            /* overlong line in flight: discard everything up to '\n' */
            char *nl = memchr(c->buf, '\n', c->len);
            if (nl == NULL) {
                c->len = 0; /* nothing but overlong bytes so far */
                return 0;
            }
            size_t skip = (size_t)(nl - c->buf) + 1;
            memmove(c->buf, nl + 1, c->len - skip);
            c->len -= skip;
            c->dropping = 0;
            continue;
        }

        char *nl = memchr(c->buf, '\n', c->len);
        if (nl == NULL)
            break; /* no complete line yet: wait for more bytes */

        size_t linelen = (size_t)(nl - c->buf) + 1; /* including '\n' */
        if (linelen > LINE_MAX_LEN) {
            /* Only possible via... it isn't: buffer is LINE_MAX_LEN. The
             * check documents the invariant; buffer-full handles the rest. */
            reply(c->fd, "ERR line too long (max 256)\n");
        } else {
            char line[LINE_MAX_LEN + 1];
            memcpy(line, c->buf, linelen);
            line[linelen] = '\0';
            if (handle_line(c, line) < 0) { /* BYE */
                c->len = 0;
                return -1;
            }
        }
        memmove(c->buf, nl + 1, c->len - linelen);
        c->len -= linelen;
    }

    if (c->len == sizeof c->buf) {
        /* buffer full, still no newline: hostile or broken client.
         * Answer once, then drop bytes until the next newline. */
        reply(c->fd, "ERR line too long (max 256), discarding\n");
        c->dropping = 1;
        c->len = 0;
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

    fprintf(stderr, "[srv] line protocol server on 127.0.0.1:%d\n", PORT);
    fprintf(stderr, "[srv] commands: ADD <int> <int> | ECHO <text> | WHO | BYE\n");

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
                else
                    fprintf(stderr, "[srv] %s connected (%zu clients)\n",
                            g_clients[g_nclients - 1].name, g_nclients);
            }
        }

        /* (removed flag + break: see bite 51 for the pollfd-desync reason) */
        int removed = 0;
        for (size_t i = g_nclients; i-- > 0 && !removed;) {
            short rev = pfds[i + 1].revents;
            if (!(rev & (POLLIN | POLLHUP | POLLERR)))
                continue;

            struct client *c = &g_clients[i];
            ssize_t n = recv(c->fd, c->buf + c->len,
                             sizeof c->buf - c->len, 0);

            if (n == 0) { /* EOF */
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

            if (process_buffer(c) < 0) { /* client said BYE */
                close_client(i);
                removed = 1;
                break;
            }
        }
    }

    fprintf(stderr, "[srv] shutting down, closing %zu connection(s)\n", g_nclients);
    for (size_t i = 0; i < g_nclients; i++)
        close(g_clients[i].fd);
    free(g_clients);
    free(pfds);
    close(listen_fd);
    return 0;
}
