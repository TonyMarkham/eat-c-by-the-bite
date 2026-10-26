/*
 * Bite 56 — HTTP Server v2: threaded, keep-alive, logged, robust
 *
 * Compile & run (run FROM the bite folder — it serves ./www):
 *   cd bites/56-http-server-v2
 *   gcc -Wall -Wextra -Werror -std=c17 -g -pthread server.c -o /tmp/b56
 *   /tmp/b56                    (listens on 127.0.0.1:48056, Ctrl-C = clean exit)
 *
 * Test it:
 *   curl -v http://127.0.0.1:48056/            # 200, note "Connection: keep-alive"
 *   curl -v http://127.0.0.1:48056/time        # 200
 *   curl -v http://127.0.0.1:48056/index.html  # static file
 *   curl -v http://127.0.0.1:48056/nope        # 404
 *   curl -v -X POST http://127.0.0.1:48056/    # 405
 *   # 5 parallel curls (Do item 4):
 *   for i in 1 2 3 4 5; do curl -s -o /dev/null -w "%{http_code}\n" \
 *        http://127.0.0.1:48056/ & done; wait
 *   # keep-alive reuse in one connection (50 requests, Do's break-it test):
 *   python3 -c "
 *   import http.client
 *   c = http.client.HTTPConnection('127.0.0.1', 48056)
 *   for i in range(50):
 *       c.request('GET', '/'); r = c.getresponse(); r.read()
 *   print('50 requests on ONE connection:', r.status)
 *   "
 *   # then: valgrind --leak-check=full /tmp/b56  (+ the python loop) -> all
 *   # fds closed, no leaks (the break-it exercise: any leak/hang here is a
 *   # keep-alive state-machine bug).
 *
 * WHAT THIS TEACHES (the "Learn" bullets):
 *  - KEEP-ALIVE is a state machine: one connection serves MANY requests.
 *    Loop: read headers -> (read body if declared) -> respond -> consume
 *    EXACTLY the bytes this request used -> repeat, until the client says
 *    "Connection: close", a timeout fires, or a hard request cap is hit.
 *    Every byte-position bookkeeping bug here = corrupted next request.
 *    This is how you'll model peer connections in Phase 7.
 *  - Thread-per-connection (bite 50) vs the poll loop (bite 51): this
 *    server uses THREADS because correctness is easier to see first
 *    (each thread owns one connection buffer, no cross-request state).
 *    poll/epoll wins at scale (10k sockets = 10k threads = death);
 *    correctness first, scale second.
 *  - LOGGING: one line per request — time, client, method, path, status.
 *    Grep-able, stderr, no colors, no excuses.
 *
 * Also new: 404/405/500 as real responses (500 = internal error, e.g. a
 * file that exists but can't be read), a request cap per connection,
 * Content-Length'd bodies read and discarded (so keep-alive stays in
 * sync), and graceful shutdown.
 *
 * GRACEFUL SHUTDOWN (Do item 3): the signal handler does ONLY
 * "g_stop = 1" on a volatile sig_atomic_t. accept() is interrupted
 * (no SA_RESTART), the loop checks the flag, the listen socket closes,
 * exit 0. (printf/ malloc/ free in a handler = undefined behavior;
 * don't.) The worker threads are detached — at exit() the kernel
 * closes every fd of the process; a production server would instead
 * set a flag the workers also check and then join them. Noted, not
 * built — that's a bite-58-sized upgrade.
 *
 * BREAK IT ON PURPOSE:
 *   Fire 50 rapid requests through ONE keep-alive connection (the python
 *   snippet above). If any response is wrong, any request hangs, or
 *   valgrind reports fds still open at exit — you broke the state
 *   machine. Classic ways to break it: forget to skip the request BODY
 *   (the next "request" starts mid-body), mis-count hdr_end by 2
 *   (forgetting the final \r\n\r\n), or reuse the buffer without the
 *   memmove. Introduce each bug once, watch it fail, fix it.
 *
 * Done when: 5 parallel curls pass, Ctrl-C exits cleanly with exit code 0,
 * valgrind-clean shutdown.
 */

/* -std=c17 is STRICT ISO C: POSIX APIs (sigaction, localtime_r...)
 * must be asked for explicitly. This must precede ALL includes. */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strncasecmp */
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define PORT 48056
#define IN_BUF_SIZE 16384   /* per-connection request buffer */
#define MAX_HEADERS 8192    /* refuse giant header blocks */
#define MAX_BODY 65536      /* bodies beyond this: 413 + close */
#define MAX_REQUESTS 100    /* hard cap per keep-alive connection */
#define RECV_TIMEOUT_SEC 10
#define WWW_DIR "www"
#define FILE_MAX (1u << 20)

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1; /* THE ONLY THING a handler should do */
}

/* shared stats — mutex-protected (bite 50's lesson, applied) */
static pthread_mutex_t g_stats_lock = PTHREAD_MUTEX_INITIALIZER;
static long g_requests_total = 0;
static long g_connections_total = 0;

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

static void now_str(char *out, size_t cap)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, cap, "%Y-%m-%d %H:%M:%S", &tm);
}

static void log_request(const char *ip, const char *method, const char *path,
                        int status)
{
    char ts[32];
    now_str(ts, sizeof ts);
    /* one line per request: time, client, method, path, status.
     * Grep-able:  grep ' 404 ' server.log */
    fprintf(stderr, "[%s] %s %s %s %d\n", ts, ip, method, path, status);
}

static void log_startup(void)
{
    char ts[32];
    now_str(ts, sizeof ts);
    fprintf(stderr, "[%s] httpd v2 listening on 127.0.0.1:%d (threads + keep-alive)\n",
            ts, PORT);
}

/* ------------------------------------------------------------------ */
/* Response building                                                   */
/* ------------------------------------------------------------------ */
static int send_response(int fd, int code, const char *reason,
                         const char *extra_hdr, const char *ctype,
                         const void *body, size_t blen, int keep_alive)
{
    char hdr[512];
    int hl = snprintf(hdr, sizeof hdr,
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %zu\r\n"
                      "%s"
                      "Connection: %s\r\n"
                      "\r\n",
                      code, reason, ctype, blen,
                      extra_hdr ? extra_hdr : "",
                      keep_alive ? "keep-alive" : "close");
    if (hl < 0 || (size_t)hl >= sizeof hdr)
        return -1;
    if (send_all(fd, hdr, (size_t)hl) < 0)
        return -1;
    if (blen > 0 && send_all(fd, body, blen) < 0)
        return -1;
    return 0;
}

static int send_error(int fd, int code, const char *reason,
                      const char *extra_hdr, int keep_alive)
{
    char body[256];
    int bl = snprintf(body, sizeof body,
                      "<html><body><h1>%d %s</h1></body></html>\n",
                      code, reason);
    return send_response(fd, code, reason, extra_hdr, "text/html",
                         body, (size_t)bl, keep_alive);
}

static const char *content_type_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot == NULL)
        return "application/octet-stream";
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0)
        return "text/html";
    if (strcmp(dot, ".css") == 0)
        return "text/css";
    if (strcmp(dot, ".txt") == 0)
        return "text/plain";
    if (strcmp(dot, ".png") == 0)
        return "image/png";
    return "application/octet-stream";
}

/* Serve www/<path>; returns 0 and sets *status, or -1 if the RESPONSE
 * ITSELF failed to send (that closes the connection). */
static int serve_static(int fd, const char *path, int *status, int keep_alive)
{
    char fullpath[512];
    if (snprintf(fullpath, sizeof fullpath, WWW_DIR "%s", path) >= (int)sizeof fullpath) {
        *status = 400;
        return send_error(fd, 400, "Bad Request", NULL, keep_alive);
    }
    FILE *f = fopen(fullpath, "rb");
    if (f == NULL) {
        *status = 404;
        return send_error(fd, 404, "Not Found", NULL, keep_alive);
    }
    char *body = malloc(FILE_MAX);
    if (body == NULL) {
        fclose(f);
        *status = 500;
        return send_error(fd, 500, "Internal Server Error", NULL, keep_alive);
    }
    size_t blen = fread(body, 1, FILE_MAX, f);
    int ok = !ferror(f) && !(blen == FILE_MAX && fgetc(f) != EOF);
    /* fopen succeeded but read failed (permissions?) = 500, a real
     * internal error — different from 404, and worth distinguishing. */
    fclose(f);
    int rc;
    if (ok) {
        *status = 200;
        rc = send_response(fd, 200, "OK", NULL, content_type_of(fullpath),
                           body, blen, keep_alive);
    } else {
        *status = 500;
        rc = send_error(fd, 500, "Internal Server Error", NULL, keep_alive);
    }
    free(body);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Per-connection state + the keep-alive state machine                 */
/* ------------------------------------------------------------------ */
struct conn {
    int fd;
    char ip[INET_ADDRSTRLEN];
    long id;
    char in[IN_BUF_SIZE]; /* byte buffer shared across requests: after each
                             request we consume EXACTLY its bytes */
    size_t len;
};

/* Find a header value (case-insensitive name) in the header block.
 * The block is NUL-terminated by the caller; each line ends in \r\n.
 * IMPORTANT: the FIRST line of hdrs may itself be a header (we are
 * called with everything AFTER the request line) — check it before
 * advancing past any \r\n. Returns 1 if found. */
static int get_header(const char *hdrs, const char *name,
                      char *out, size_t outcap)
{
    size_t nlen = strlen(name);
    const char *p = hdrs;

    for (;;) {
        if (p[0] == '\0')
            break; /* end of the (terminated) header block */

        if (strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
            const char *v = p + nlen + 1;
            while (*v == ' ' || *v == '\t')
                v++;
            size_t vlen = 0;
            while (v[vlen] != '\0' && v[vlen] != '\r' && v[vlen] != '\n' &&
                   vlen + 1 < outcap)
                vlen++;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return 1;
        }

        const char *nl = strstr(p, "\r\n");
        if (nl == NULL)
            break; /* last line had no terminator: malformed, not ours */
        p = nl + 2;
        if (p[0] == '\r' && p[1] == '\n')
            break; /* blank line = end of headers */
    }
    return 0;
}

/* Strict Content-Length: digits only. Returns >=0 or -1 if malformed. */
static long parse_content_length(const char *val)
{
    if (val == NULL || *val == '\0')
        return -1;
    char *end;
    errno = 0;
    long v = strtol(val, &end, 10);
    if (errno != 0 || end == val || *end != '\0' || v < 0)
        return -1;
    return v;
}

static void *worker_main(void *arg)
{
    struct conn *c = arg;
    int keep_alive = 1;

    /* the keep-alive read timeout: idle connections die politely */
    struct timeval tv = { .tv_sec = RECV_TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    for (int reqno = 0; reqno < MAX_REQUESTS && keep_alive && !g_stop; reqno++) {
        /* -- 1. wait for a full header block (\r\n\r\n) in c->in ------- */
        size_t hdr_end = 0;
        for (;;) {
            /* scan what we hold (the delimiter may straddle recvs) */
            for (size_t i = 0; i + 4 <= c->len; i++) {
                if (memcmp(c->in + i, "\r\n\r\n", 4) == 0) {
                    hdr_end = i + 4;
                    goto have_headers;
                }
            }
            if (c->len == sizeof c->in || c->len >= MAX_HEADERS) {
                send_error(c->fd, 400, "Bad Request", NULL, 0);
                goto done;
            }
            ssize_t n = recv(c->fd, c->in + c->len, sizeof c->in - c->len, 0);
            if (n == 0) {
                if (c->len == 0)
                    goto done; /* idle close: totally normal */
                goto done;     /* closed MID-request: abnormal, just drop */
            }
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (c->len == 0)
                        goto done; /* keep-alive idle timeout: normal */
                    send_error(c->fd, 408, "Request Timeout", NULL, 0);
                    goto done;
                }
                goto done;
            }
            c->len += (size_t)n;
        }
    have_headers:;

        /* -- 2. parse the request line -------------------------------- */
        char line[1024];
        char method[16] = "", path[512] = "", version[16] = "";
        {
            char *reqline_end = memchr(c->in, '\r', hdr_end);
            size_t ll = reqline_end ? (size_t)(reqline_end - c->in)
                                    : hdr_end - 2;
            if (ll >= sizeof line)
                ll = sizeof line - 1;
            memcpy(line, c->in, ll);
            line[ll] = '\0';

            char *sp1 = strchr(line, ' ');
            char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
            if (!sp1 || !sp2 || strchr(sp2 + 1, ' ')) {
                log_request(c->ip, "?", "?", 400);
                send_error(c->fd, 400, "Bad Request", NULL, 0);
                goto done; /* can't trust framing after garbage: close */
            }
            *sp1 = '\0';
            *sp2 = '\0';
            /* Strict parsing includes LENGTH: reject over-long tokens
             * instead of silently truncating them (snprintf would, and
             * -Wformat-truncation rightly flags it). */
            {
                size_t mlen = (size_t)(sp1 - line);
                size_t plen = (size_t)(sp2 - sp1 - 1);
                size_t vlen = strlen(sp2 + 1);
                if (mlen == 0 || mlen >= sizeof method || plen == 0 ||
                    plen >= sizeof path || vlen == 0 || vlen >= sizeof version) {
                    log_request(c->ip, "?", "?", 400);
                    send_error(c->fd, 400, "Bad Request", NULL, 0);
                    goto done;
                }
                memcpy(method, line, mlen);
                method[mlen] = '\0';
                memcpy(path, sp1 + 1, plen);
                path[plen] = '\0';
                memcpy(version, sp2 + 1, vlen);
                version[vlen] = '\0';
            }
        }

        /* -- 3. parse the headers we actually care about --------------- */
        char clbuf[32] = "", connhdr[32] = "";
        long body_len = 0;
        /* headers live between the request line and hdr_end */
        {
            /* NUL-terminate the header block in place for strstr use */
            char saved = c->in[hdr_end - 2];
            c->in[hdr_end - 2] = '\0';
            const char *hdrs = strchr(c->in, '\n');
            hdrs = hdrs ? hdrs + 1 : c->in;

            if (get_header(hdrs, "Content-Length", clbuf, sizeof clbuf))
                body_len = parse_content_length(clbuf);
            if (body_len < 0) {
                send_error(c->fd, 400, "Bad Request", NULL, 0);
                c->in[hdr_end - 2] = saved;
                goto done;
            }
            if (get_header(hdrs, "Connection", connhdr, sizeof connhdr))
                keep_alive = (strcmp(version, "HTTP/1.0") == 0)
                                 ? (strncasecmp(connhdr, "keep-alive", 10) == 0)
                                 : (strncasecmp(connhdr, "close", 5) != 0);
            c->in[hdr_end - 2] = saved; /* restore the byte we NUL'd */
        }

        /* -- 4. make sure the whole BODY is in the buffer -------------- */
        if (body_len > MAX_BODY) { /* don't buffer giant bodies: refuse */
            send_error(c->fd, 413, "Content Too Large", NULL, 0);
            goto done;
        }
        while (c->len < hdr_end + (size_t)body_len) {
            ssize_t n = recv(c->fd, c->in + c->len,
                             sizeof c->in - c->len, 0);
            if (n <= 0) {
                if (n < 0 && errno == EINTR)
                    continue;
                goto done; /* client vanished mid-body */
            }
            c->len += (size_t)n;
        }

        /* -- 5. route + respond ---------------------------------------- */
        int status;
        int cap_requests = (reqno == MAX_REQUESTS - 1); /* force close */
        int ka = keep_alive && !cap_requests;

        if (strstr(path, "..") != NULL || path[0] != '/') {
            log_request(c->ip, method, path, 400);
            send_error(c->fd, 400, "Bad Request", NULL, ka);
            if (!ka)
                goto done;
        } else if (strcmp(method, "GET") != 0) {
            log_request(c->ip, method, path, 405);
            send_error(c->fd, 405, "Method Not Allowed", "Allow: GET\r\n", ka);
            if (!ka)
                goto done;
        } else {
            int rc;
            if (strcmp(path, "/") == 0) {
                static const char page[] =
                    "<html><body><h1>bite 56: threaded httpd</h1>"
                    "<p>keep-alive is on; try /time and /index.html</p>"
                    "</body></html>\n";
                status = 200;
                rc = send_response(c->fd, 200, "OK", NULL, "text/html",
                                   page, sizeof page - 1, ka);
            } else if (strcmp(path, "/time") == 0) {
                char tbuf[128], body[192];
                time_t now = time(NULL);
                ctime_r(&now, tbuf);
                tbuf[strcspn(tbuf, "\n")] = '\0';
                int bl = snprintf(body, sizeof body,
                                  "<html><body>%s (unix %ld)</body></html>\n",
                                  tbuf, (long)now);
                status = 200;
                rc = send_response(c->fd, 200, "OK", NULL, "text/html",
                                   body, (size_t)bl, ka);
            } else {
                rc = serve_static(c->fd, path, &status, ka);
            }
            log_request(c->ip, method, path, status);
            if (rc < 0)
                goto done; /* response send failed: hang up */
            if (!ka)
                goto done;
        }

        /* -- 6. consume EXACTLY the bytes this request used ------------ */
        /* THE keep-alive step. Get this wrong and the next "request" is
         * parsed from the middle of a body. THE classic v2 bug. */
        size_t used = hdr_end + (size_t)body_len;
        memmove(c->in, c->in + used, c->len - used);
        c->len -= used;

        pthread_mutex_lock(&g_stats_lock);
        g_requests_total++;
        pthread_mutex_unlock(&g_stats_lock);
    }

done:
    close(c->fd);
    pthread_mutex_lock(&g_stats_lock);
    g_connections_total++;
    pthread_mutex_unlock(&g_stats_lock);
    free(c);
    return NULL;
}

int main(void)
{
    struct sigaction sa;
    int listen_fd;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: we WANT accept() interrupted */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

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
    if (listen(listen_fd, 64) < 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

    log_startup();

    while (!g_stop) {
        struct sockaddr_in cli;
        socklen_t clilen = sizeof cli;
        int conn = accept(listen_fd, (struct sockaddr *)&cli, &clilen);
        if (conn < 0) {
            if (errno == EINTR) {
                if (g_stop)
                    break; /* graceful shutdown: stop accepting */
                continue;
            }
            perror("accept");
            break;
        }

        struct conn *c = malloc(sizeof *c);
        if (c == NULL) {
            close(conn);
            continue;
        }
        c->fd = conn;
        c->len = 0;
        inet_ntop(AF_INET, &cli.sin_addr, c->ip, sizeof c->ip);

        pthread_t tid;
        if (pthread_create(&tid, NULL, worker_main, c) != 0) {
            perror("pthread_create");
            free(c);
            close(conn);
            continue;
        }
        pthread_detach(tid);
    }

    /* Graceful shutdown: stop accepting, close the listen socket, exit 0.
     * Worker threads are detached; exit() closes every fd in the process.
     * (A production server: set a stop flag workers check between
     * requests, join all threads, THEN exit — bite 58 does it properly
     * for its own scope.) */
    fprintf(stderr, "[srv] shutting down gracefully (exit 0)\n");
    close(listen_fd);
    return 0;
}
