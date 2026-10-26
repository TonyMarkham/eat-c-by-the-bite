/*
 * Bite 55 — HTTP Server v1: hand-rolled request parsing and responses
 *
 * Compile & run (run FROM the bite folder — it serves ./www):
 *   cd bites/55-http-server-v1
 *   gcc -Wall -Wextra -Werror -std=c17 -g server.c -o /tmp/b55
 *   /tmp/b55                    (listens on 127.0.0.1:48055, Ctrl-C to stop)
 *
 * Test it (curl -v shows request AND response headers):
 *   curl -v http://127.0.0.1:48055/              # hello page (200)
 *   curl -v http://127.0.0.1:48055/time          # current time (200)
 *   curl -v http://127.0.0.1:48055/index.html    # static file from ./www
 *   curl -v http://127.0.0.1:48055/nope          # 404
 *   curl -v -X POST http://127.0.0.1:48055/      # 405 (Allow: GET)
 *   curl -v --path-as-is http://127.0.0.1:48055/../../etc/passwd   # 400
 *
 * WHAT THIS TEACHES (the "Learn" bullets):
 *  - Request shape: a request LINE ("METHOD SP PATH SP VERSION"), then
 *    headers until the FIRST EMPTY line (\r\n\r\n), then an optional
 *    body. Our bite-48 reader is the parser's spine: we accumulate bytes
 *    until \r\n\r\n — never trusting that one recv() = one request.
 *  - Minimal CORRECT responses: status line + Content-Length + reason.
 *    Content-Length is not optional: curl/browsers read exactly that
 *    many bytes; without it they hang waiting for EOF that (with
 *    keep-alive) never comes.
 *  - 404 and 400 are FEATURES: a server that answers garbage with a
 *    proper error is a server. A server that crashes on garbage is an
 *    incident report.
 *
 * SECURITY ON DAY ONE — PATH TRAVERSAL:
 *   GET /../../etc/passwd is the classic. The check is one strstr:
 *   reject any path containing ".." BEFORE touching the filesystem.
 *   (Real servers normalize the path and compare against a root; we
 *   take the sledgehammer because it's correct and it's one line.)
 *
 * BREAK IT ON PURPOSE:
 *   1) Send a request with NO final \r\n\r\n and then just wait:
 *        nc 127.0.0.1 48055
 *        GET / HTTP/1.1\r\nHost: x\r\n        (then press Enter twice max,
 *                                              leave the blank line OFF)
 *      A naive server waits forever for headers that never end. THIS
 *      server has SO_RCVTIMEO (5s): it times out, logs, and closes.
 *      Never hang on hostile input.
 *   2) GET /../../etc/passwd — must be 400, NEVER file contents.
 *      (curl normalizes ../ out of the URL; use --path-as-is or nc.)
 *
 * Done when: curl gets real pages, real 404s, and the traversal attempt
 * gets rejected.
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
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define PORT 48055
#define HDR_MAX 8192
#define FILE_MAX (1u << 20) /* 1 MB static-file ceiling */
#define WWW_DIR "www"
#define RECV_TIMEOUT 5

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

/* ------------------------------------------------------------------ */
/* Reading a request: "read until \r\n\r\n", bounded — bite 48's       */
/* read-until-delimiter, applied to real HTTP.                        */
/* Returns 0 = got headers (*hdr_len set), -1 = overflow/error,       */
/*         1 = timeout (hostile or dead client), 2 = client closed.   */
/* ------------------------------------------------------------------ */
static int read_headers(int fd, char *buf, size_t cap, size_t *hdr_len)
{
    size_t len = 0;
    for (;;) {
        /* rescan the WHOLE buffer each round: the \r\n\r\n may straddle
         * two recv()s — the classic boundary case */
        for (size_t i = 0; i + 4 <= len; i++) {
            if (memcmp(buf + i, "\r\n\r\n", 4) == 0) {
                *hdr_len = i + 4;
                return 0;
            }
        }
        if (len == cap)
            return -1; /* headers too big: refuse, don't grow forever */
        ssize_t n = recv(fd, buf + len, cap - len, 0);
        if (n == 0)
            return 2; /* client hung up */
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 1; /* SO_RCVTIMEO fired — the anti-slowloris net */
            return -1;
        }
        len += (size_t)n;
    }
}

/* One response builder for every route and error: status line,
 * Content-Length (curl hangs without it!), Connection: close. */
static int send_response(int fd, int code, const char *reason,
                         const char *extra_hdr,
                         const char *ctype, const void *body, size_t blen)
{
    char hdr[512];
    int hl = snprintf(hdr, sizeof hdr,
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %zu\r\n"
                      "%s"
                      "Connection: close\r\n"
                      "\r\n",
                      code, reason, ctype, blen,
                      extra_hdr ? extra_hdr : "");
    if (hl < 0 || (size_t)hl >= sizeof hdr)
        return -1;
    if (send_all(fd, hdr, (size_t)hl) < 0)
        return -1;
    if (blen > 0 && send_all(fd, body, blen) < 0)
        return -1;
    return 0;
}

/* Small HTML error pages — 404/400/405 are features, make them real. */
static int send_error(int fd, int code, const char *reason, const char *extra_hdr)
{
    char body[256];
    int bl = snprintf(body, sizeof body,
                      "<html><body><h1>%d %s</h1></body></html>\n",
                      code, reason);
    return send_response(fd, code, reason, extra_hdr, "text/html",
                         body, (size_t)bl);
}

/* Content-Type by extension — tiny table, good enough for the bite. */
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

/* Serve www/<path> — ONLY after the traversal check has passed.
 * Always sets *status_out (404/400/500/200) before returning. */
static int serve_static(int fd, const char *path, long *status_out)
{
    char fullpath[512];
    FILE *f;

    if (snprintf(fullpath, sizeof fullpath, WWW_DIR "%s", path) >= (int)sizeof fullpath) {
        *status_out = 400;
        return send_error(fd, 400, "Bad Request", NULL);
    }

    f = fopen(fullpath, "rb");
    if (f == NULL) {
        *status_out = 404; /* not found is normal life for a web server */
        return send_error(fd, 404, "Not Found", NULL);
    }

    /* read the whole file (bounded) */
    char *body = malloc(FILE_MAX);
    if (body == NULL) {
        fclose(f);
        *status_out = 500;
        return send_error(fd, 500, "Internal Server Error", NULL);
    }
    size_t blen = fread(body, 1, FILE_MAX, f);
    int truncated = ferror(f) || (blen == FILE_MAX && fgetc(f) != EOF);
    fclose(f);

    int rc;
    if (truncated) {
        *status_out = 500;
        rc = send_error(fd, 500, "Internal Server Error", NULL);
    } else {
        *status_out = 200;
        rc = send_response(fd, 200, "OK", NULL, content_type_of(fullpath),
                           body, blen);
    }
    free(body);
    return rc;
}

/* Handle ONE connection (v1: one request, then close). */
static void handle_connection(int fd)
{
    char buf[HDR_MAX];
    size_t hdr_len = 0;
    char method[16], path[512], version[16];
    long status;
    int rc = read_headers(fd, buf, sizeof buf, &hdr_len);

    if (rc == 1) {
        fprintf(stderr, "[srv] timeout waiting for request headers\n");
        return; /* close without a response — we never got a full request */
    }
    if (rc != 0) {
        fprintf(stderr, "[srv] read error (%s)\n",
                rc == 2 ? "client closed" : "overflow/error");
        if (rc == -1)
            send_error(fd, 400, "Bad Request", NULL);
        return;
    }
    buf[hdr_len < sizeof buf ? hdr_len : sizeof buf - 1] = '\0';

    /* ---- parse the request line -------------------------------- */
    char *line_end = strstr(buf, "\r\n");
    if (line_end == NULL) { /* impossible after read_headers, but belt+braces */
        send_error(fd, 400, "Bad Request", NULL);
        return;
    }
    *line_end = '\0';

    {
        /* exactly three blank-separated tokens, else 400 */
        char *sp1 = strchr(buf, ' ');
        char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
        if (!sp1 || !sp2 || strchr(sp2 + 1, ' ')) {
            fprintf(stderr, "[srv] malformed request line: '%.60s'\n", buf);
            send_error(fd, 400, "Bad Request", NULL);
            return;
        }
        *sp1 = '\0';
        *sp2 = '\0';
        /* Strict parsing includes LENGTH: method/path/version have max
         * sizes. Copy with explicit bounds and reject instead of
         * silently truncating (a truncated path is a wrong path). */
        {
            size_t mlen = (size_t)(sp1 - buf);
            size_t plen = (size_t)(sp2 - sp1 - 1);
            size_t vlen = strlen(sp2 + 1);
            if (mlen == 0 || mlen >= sizeof method || plen == 0 ||
                plen >= sizeof path || vlen == 0 || vlen >= sizeof version) {
                fprintf(stderr, "[srv] malformed request line: '%.60s'\n", buf);
                send_error(fd, 400, "Bad Request", NULL);
                return;
            }
            memcpy(method, buf, mlen);
            method[mlen] = '\0';
            memcpy(path, sp1 + 1, plen);
            path[plen] = '\0';
            memcpy(version, sp2 + 1, vlen);
            version[vlen] = '\0';
        }
    }

    if (strncmp(version, "HTTP/1.", 7) != 0) {
        send_error(fd, 400, "Bad Request", NULL);
        return;
    }
    if (strcmp(method, "GET") != 0) {
        send_error(fd, 405, "Method Not Allowed", "Allow: GET\r\n");
        status = 405;
        fprintf(stderr, "[srv] %s %s -> %ld\n", method, path, status);
        return;
    }

    /* ---- traversal check BEFORE anything touches the disk ------- */
    if (strstr(path, "..") != NULL || path[0] != '/') {
        fprintf(stderr, "[srv] BLOCKED traversal attempt: %s\n", path);
        send_error(fd, 400, "Bad Request", NULL);
        status = 400;
        fprintf(stderr, "[srv] %s %s -> %ld\n", method, path, status);
        return;
    }

    /* ---- routing ------------------------------------------------- */
    if (strcmp(path, "/") == 0) {
        static const char page[] =
            "<html><head><title>bite 55</title></head>\n"
            "<body><h1>Hello from a hand-rolled C HTTP server</h1>\n"
            "<p>No libraries. Just bytes on a socket.</p>\n"
            "<p>Try /time and /index.html</p></body></html>\n";
        rc = send_response(fd, 200, "OK", NULL, "text/html",
                           page, sizeof page - 1);
        status = 200;
    } else if (strcmp(path, "/time") == 0) {
        char tbuf[128], body[192];
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(tbuf, sizeof tbuf, "%Y-%m-%d %H:%M:%S", &tm);
        int bl = snprintf(body, sizeof body,
                          "<html><body>server time: %s (unix %ld)</body></html>\n",
                          tbuf, (long)now);
        rc = send_response(fd, 200, "OK", NULL, "text/html", body, (size_t)bl);
        status = 200;
    } else {
        rc = serve_static(fd, path, &status); /* 200 / 404 / 500 */
    }

    fprintf(stderr, "[srv] %s %s -> %ld\n", method, path, status);
    if (rc < 0)
        fprintf(stderr, "[srv] send failed\n");
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

    /* The 5-call server sequence — you know this by heart by now. */
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

    fprintf(stderr, "[srv] HTTP/1.1 server v1 on http://127.0.0.1:%d/ "
                    "(serving ./%s/, Ctrl-C to stop)\n", PORT, WWW_DIR);

    while (!g_stop) {
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

        /* per-connection receive timeout: the anti-"wait forever" net */
        struct timeval tv = { .tv_sec = RECV_TIMEOUT, .tv_usec = 0 };
        setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

        handle_connection(conn);
        close(conn); /* v1: Connection: close, always */
    }

    fprintf(stderr, "[srv] bye\n");
    close(listen_fd);
    return 0;
}
