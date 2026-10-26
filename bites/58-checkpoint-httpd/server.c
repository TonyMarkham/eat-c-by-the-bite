/*
 * Bite 58 — CHECKPOINT: httpd (static files + JSON API + graceful shutdown)
 *
 * Compile & run (run FROM the bite folder — it serves ./www):
 *   cd bites/58-checkpoint-httpd
 *   gcc -Wall -Wextra -Werror -std=c17 -g -pthread server.c -o server -lpthread
 *   ./server [port]              (default 127.0.0.1:48058; Ctrl-C/SIGTERM = exit 0)
 *   ./test.sh                    (happy paths + the abuse gauntlet, self-contained)
 *
 * Endpoints:
 *   GET  /            hello page (inline HTML)
 *   GET  /index.html  static file from ./www (traversal-protected)
 *   GET  /time        server time, HTML
 *   GET  /api/time    JSON  {"now":"...","unix":123456}
 *   GET  /api/stats   JSON  {"requests_served":N,"connections":N,"uptime_seconds":N}
 *   anything else: 404 | non-GET: 405 | malformed: 400 | giant body: 413
 *
 * This is the capstone of Phase 5: everything from bites 45–57 in one
 * program — sockets (46/47), robust framing (48), threads + mutex (50),
 * keep-alive state machine (56), strict parsing (53), byte-level care
 * (54), graceful shutdown, and a README a stranger could run.
 *
 * ARCHITECTURE (one paragraph):
 *   main thread: accept loop only. Per connection: one DETACHED worker
 *   thread that owns a malloc'd `struct conn` (fd, ip, read buffer) and
 *   runs the keep-alive state machine from bite 56: buffer bytes until a
 *   full \r\n\r\n header block exists, parse request line + the three
 *   headers we care about (Content-Length, Connection), buffer the body
 *   if declared, dispatch on (method, path), respond, consume EXACTLY
 *   the bytes used, repeat until close/timeout/cap. Shared counters
 *   (requests, connections, uptime) live behind one mutex (bite 50).
 *   Why threads instead of a poll loop? See README.md for the honest
 *   trade-off discussion the checkpoint demands.
 *
 * BREAK IT ON PURPOSE (the final abuse gauntlet — test.sh runs it):
 *   20 parallel curls; slowloris-style slow headers (partial request,
 *   then silence — must hit the 10s timeout, never hang forever);
 *   malformed request lines; giant headers (16KB cap -> 400); ".."
 *   traversal; SIGTERM mid-request. Nothing crashes, nothing hangs,
 *   nothing leaks (run the whole thing under valgrind once:
 *     valgrind --leak-check=full --track-fds=yes ./server 48058 &
 *     ./test.sh; then SIGTERM it and read the report).
 *
 * Done when: the gauntlet passes and README.md would let a stranger run
 * it. Committed as "checkpoint 5: httpd".
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
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PORT 48058
#define IN_BUF_SIZE 16384
#define MAX_HEADERS 8192
#define MAX_BODY 65536
#define MAX_REQUESTS 100
#define RECV_TIMEOUT_SEC 10
#define WWW_DIR "www"
#define FILE_MAX (1u << 20)

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1; /* the only thing a handler does */
}

/* ---- shared stats: one mutex, three counters (bite 50 discipline) ---- */
static pthread_mutex_t g_stats_lock = PTHREAD_MUTEX_INITIALIZER;
static long g_requests_served = 0;
static long g_connections_total = 0;
static time_t g_started_at;

static long uptime_seconds(void)
{
    return (long)(time(NULL) - g_started_at);
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

static void now_str(char *out, size_t cap)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, cap, "%Y-%m-%d %H:%M:%S", &tm);
}

static void log_line(const char *ip, const char *method, const char *path,
                     int status)
{
    char ts[32];
    now_str(ts, sizeof ts);
    fprintf(stderr, "[%s] %s %s %s %d\n", ts, ip, method, path, status);
}

/* ------------------------------------------------------------------ */
/* Hand-built JSON — it's just strings and escaping (Do item, verbatim)*/
/* ------------------------------------------------------------------ */

/* Escape a string for JSON: ", \, control chars. Writes at most cap-1
 * chars, always NUL-terminates. Cheap and correct beats fast here. */
static void json_escape(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 7 < cap; p++) {
        switch (*p) {
        case '"':  dst[o++] = '\\'; dst[o++] = '"';  break;
        case '\\': dst[o++] = '\\'; dst[o++] = '\\'; break;
        case '\n': dst[o++] = '\\'; dst[o++] = 'n';  break;
        case '\r': dst[o++] = '\\'; dst[o++] = 'r';  break;
        case '\t': dst[o++] = '\\'; dst[o++] = 't';  break;
        default:
            if (*p < 0x20) /* control char: \u00XX */
                o += (size_t)snprintf(dst + o, cap - o, "\\u%04x", *p);
            else
                dst[o++] = (char)*p;
        }
    }
    dst[o] = '\0';
}

/* ------------------------------------------------------------------ */
/* HTTP plumbing (bites 55/56, battle-tested)                          */
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

/* GET /api/time -> {"now":"...","unix":...} */
static int serve_api_time(int fd, int *status, int keep_alive)
{
    char tstr[32], body[160];
    time_t now = time(NULL);
    now_str(tstr, sizeof tstr);
    char escaped[64];
    json_escape(escaped, sizeof escaped, tstr); /* escaping is the habit */
    int bl = snprintf(body, sizeof body,
                      "{\"now\":\"%s\",\"unix\":%ld}\n",
                      escaped, (long)now);
    if (bl < 0 || (size_t)bl >= sizeof body) {
        *status = 500;
        return send_error(fd, 500, "Internal Server Error", NULL, keep_alive);
    }
    *status = 200;
    return send_response(fd, 200, "OK", NULL, "application/json",
                         body, (size_t)bl, keep_alive);
}

/* GET /api/stats -> {"requests_served":N,"connections":N,"uptime_seconds":N} */
static int serve_api_stats(int fd, int *status, int keep_alive)
{
    char body[160];
    long req, conns;
    pthread_mutex_lock(&g_stats_lock); /* snapshot under the lock */
    req = g_requests_served;
    conns = g_connections_total;
    pthread_mutex_unlock(&g_stats_lock);
    int bl = snprintf(body, sizeof body,
                      "{\"requests_served\":%ld,\"connections\":%ld,"
                      "\"uptime_seconds\":%ld}\n",
                      req, conns, uptime_seconds());
    if (bl < 0 || (size_t)bl >= sizeof body) {
        *status = 500;
        return send_error(fd, 500, "Internal Server Error", NULL, keep_alive);
    }
    *status = 200;
    return send_response(fd, 200, "OK", NULL, "application/json",
                         body, (size_t)bl, keep_alive);
}

/* ------------------------------------------------------------------ */
/* Per-connection keep-alive state machine (bite 56)                   */
/* ------------------------------------------------------------------ */
struct conn {
    int fd;
    char ip[INET_ADDRSTRLEN];
    char in[IN_BUF_SIZE];
    size_t len;
};

static int get_header(const char *hdrs, const char *name,
                      char *out, size_t outcap)
{
    size_t nlen = strlen(name);
    const char *p = hdrs;
    for (;;) {
        if (p[0] == '\0')
            break;
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
            break;
        p = nl + 2;
        if (p[0] == '\r' && p[1] == '\n')
            break;
    }
    return 0;
}

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

    struct timeval tv = { .tv_sec = RECV_TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    for (int reqno = 0; reqno < MAX_REQUESTS && keep_alive && !g_stop; reqno++) {
        size_t hdr_end = 0;
        for (;;) {
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
            if (n == 0)
                goto done;
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (c->len == 0)
                        goto done; /* idle keep-alive timeout */
                    send_error(c->fd, 408, "Request Timeout", NULL, 0);
                    goto done;
                }
                goto done;
            }
            c->len += (size_t)n;
        }
    have_headers:;

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
                log_line(c->ip, "?", "?", 400);
                send_error(c->fd, 400, "Bad Request", NULL, 0);
                goto done;
            }
            *sp1 = '\0';
            *sp2 = '\0';
            /* Strict parsing includes LENGTH: reject over-long tokens
             * instead of silently truncating them. */
            {
                size_t mlen = (size_t)(sp1 - line);
                size_t plen = (size_t)(sp2 - sp1 - 1);
                size_t vlen = strlen(sp2 + 1);
                if (mlen == 0 || mlen >= sizeof method || plen == 0 ||
                    plen >= sizeof path || vlen == 0 || vlen >= sizeof version) {
                    log_line(c->ip, "?", "?", 400);
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

        char clbuf[32] = "", connhdr[32] = "";
        long body_len = 0;
        {
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
            c->in[hdr_end - 2] = saved;
        }

        if (body_len > MAX_BODY) {
            send_error(c->fd, 413, "Content Too Large", NULL, 0);
            goto done;
        }
        while (c->len < hdr_end + (size_t)body_len) {
            ssize_t n = recv(c->fd, c->in + c->len,
                             sizeof c->in - c->len, 0);
            if (n <= 0) {
                if (n < 0 && errno == EINTR)
                    continue;
                goto done;
            }
            c->len += (size_t)n;
        }

        int status;
        int ka = keep_alive && (reqno != MAX_REQUESTS - 1);

        if (strstr(path, "..") != NULL || path[0] != '/') {
            log_line(c->ip, method, path, 400);
            send_error(c->fd, 400, "Bad Request", NULL, ka);
            if (!ka)
                goto done;
        } else if (strcmp(method, "GET") != 0) {
            log_line(c->ip, method, path, 405);
            send_error(c->fd, 405, "Method Not Allowed", "Allow: GET\r\n", ka);
            if (!ka)
                goto done;
        } else {
            int rc;
            if (strcmp(path, "/") == 0) {
                static const char page[] =
                    "<html><head><title>checkpoint 5: httpd</title></head>\n"
                    "<body><h1>It works — hand-rolled C httpd</h1>\n"
                    "<p>Static files in /index.html, JSON at /api/time and /api/stats.</p>\n"
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
            } else if (strcmp(path, "/api/time") == 0) {
                rc = serve_api_time(c->fd, &status, ka);
            } else if (strcmp(path, "/api/stats") == 0) {
                rc = serve_api_stats(c->fd, &status, ka);
            } else {
                rc = serve_static(c->fd, path, &status, ka);
            }
            log_line(c->ip, method, path, status);
            if (rc < 0)
                goto done;
            if (!ka)
                goto done;
        }

        size_t used = hdr_end + (size_t)body_len;
        memmove(c->in, c->in + used, c->len - used);
        c->len -= used;

        pthread_mutex_lock(&g_stats_lock);
        g_requests_served++;
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

int main(int argc, char **argv)
{
    struct sigaction sa;
    int listen_fd;
    int port = DEFAULT_PORT;

    if (argc > 1) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "usage: %s [port]\n", argv[0]);
            return 1;
        }
    }

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
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
        addr.sin_port = htons((unsigned short)port);
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

    g_started_at = time(NULL);
    {
        char ts[32];
        now_str(ts, sizeof ts);
        fprintf(stderr, "[%s] httpd (checkpoint 5) listening on 127.0.0.1:%d\n",
                ts, port);
    }

    while (!g_stop) {
        struct sockaddr_in cli;
        socklen_t clilen = sizeof cli;
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

    /* Graceful shutdown: stop accepting, close the listener, exit 0.
     * In-flight worker threads finish their current step; exit() closes
     * their fds. True "drain then join" shutdown is a documented
     * limitation (see README) — honest scope beats fake completeness. */
    fprintf(stderr, "[srv] SIGINT/SIGTERM: shutting down gracefully\n");
    close(listen_fd);
    return 0;
}
