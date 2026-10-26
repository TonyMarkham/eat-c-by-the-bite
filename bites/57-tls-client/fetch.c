/*
 * Bite 57 — TLS: Use It, Don't Write It (OpenSSL HTTPS client)
 *
 * Compile & run:
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/57-tls-client/fetch.c -o /tmp/b57 \
 *       -lssl -lcrypto
 *   /tmp/b57                                (fetch https://example.com/)
 *   /tmp/b57 example.com 443 /
 *   /tmp/b57 self-signed.badssl.com 443 /   (MUST FAIL — that's the feature)
 *
 * (Needs libssl-dev: `sudo apt install libssl-dev`.)
 *
 * WHAT THIS TEACHES (the "Learn" bullets):
 *  - TLS is a LAYER over your TCP connection, not a different API. Your
 *    socket dance (socket -> getaddrinfo -> connect) is IDENTICAL to
 *    bite 46; then SSL_new/SSL_set_fd/SSL_connect wrap it, and from that
 *    moment SSL_write/SSL_read REPLACE send/recv. That's the whole
 *    integration story: same bytes in, authenticated+encrypted bytes on
 *    the wire.
 *  - The handshake = certificates + key exchange, all inside SSL_connect.
 *    Your application's ONLY job in the handshake: VERIFY THE HOSTNAME.
 *    SSL_set1_host pins which certificate name we accept; skipping that
 *    check is exactly how man-in-the-middle happens.
 *  - "https://" in bite 46's client = this bite, applied. No HTTP library
 *    appeared; the only new code is TLS plumbing.
 *
 * BREAK IT ON PURPOSE:
 *   Comment out the two verification lines below (marked VERIFICATION),
 *   then fetch the bad-ssl host:
 *       /tmp/b57 self-signed.badssl.com 443 /
 *   It SUCCEEDS and hands you attacker-controlled bytes as if they were
 *   example.com. Horror. Restore the lines: the same fetch fails loudly
 *   with a verify error. You have now personally witnessed the
 *   vulnerability your Phase-7 server must never have.
 *
 * Done when: the hand-written HTTPS client fetches example.com, verifies
 * certs, and fails loudly on bad certs.
 */

/* -std=c17 is STRICT ISO C: POSIX APIs (getaddrinfo among them) must
 * be asked for explicitly. This must precede ALL includes. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static int send_all(SSL *ssl, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        int n = SSL_write(ssl, p, (int)len);
        if (n <= 0) {
            int err = SSL_get_error(ssl, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
                continue; /* nonblocking would retry; we're blocking, so
                             this "shouldn't happen" — but handle anyway */
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
    const char *port = (argc > 2) ? argv[2] : "443";
    const char *path = (argc > 3) ? argv[3] : "/";

    /* ---------- 1. TCP: identical to bite 46 ---------- */
    struct addrinfo hints, *res = NULL, *rp;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) {
        fprintf(stderr, "getaddrinfo failed for %s:%s\n", host, port);
        return 1;
    }
    int fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        fprintf(stderr, "could not connect (TCP) to %s:%s\n", host, port);
        return 1;
    }
    printf("[tcp] connected to %s:%s\n", host, port);

    /* ---------- 2. TLS context: the "policy" object ---------- */
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == NULL) {
        ERR_print_errors_fp(stderr);
        return 1;
    }
    /* VERIFICATION (part 1): fail the handshake unless the certificate
     * chain validates against our trusted roots. NULL callback = default
     * behavior = "abort handshake on failure". */
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    /* VERIFICATION (part 2, global): use the system CA store (the same
     * store curl and your browser use). Without trusted roots, every
     * cert fails — this loads /etc/ssl/certs. */
    if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
        ERR_print_errors_fp(stderr);
        return 1;
    }

    SSL *ssl = SSL_new(ctx);
    if (ssl == NULL) {
        ERR_print_errors_fp(stderr);
        return 1;
    }
    SSL_set_fd(ssl, fd);

    /* VERIFICATION (part 3, per-connection): pin the expected HOSTNAME.
     * A valid certificate for someone else is still an attack. This one
     * call is the difference between "encrypted" and "authenticated". */
    if (SSL_set1_host(ssl, host) != 1) {
        ERR_print_errors_fp(stderr);
        return 1;
    }
    /* SNI (server name indication): tell the server WHICH name we came
     * for — one IP can host many TLS sites. (Macro; returns 1 on ok.) */
    if (SSL_set_tlsext_host_name(ssl, host) != 1) {
        ERR_print_errors_fp(stderr);
        return 1;
    }

    /* ---------- 3. THE HANDSHAKE (certs + key exchange) ---------- */
    if (SSL_connect(ssl) != 1) {
        fprintf(stderr, "TLS handshake FAILED with %s — certificate "
                        "verification did its job (or the server is broken):\n",
                host);
        ERR_print_errors_fp(stderr);
        /* Show the verify result explicitly: this is what a hostile /
         * misconfigured cert looks like from the client side. */
        long vr = SSL_get_verify_result(ssl);
        fprintf(stderr, "verify result: %ld (%s)\n", vr,
                X509_verify_cert_error_string(vr));
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        return 1;
    }

    /* What did we negotiate? Print it — a habit for debugging TLS. */
    printf("[tls] version: %s\n", SSL_get_version(ssl));
    {
        const SSL_CIPHER *cipher = SSL_get_current_cipher(ssl);
        int bits;
        printf("[tls] cipher: %s (%d bits)\n",
               SSL_CIPHER_get_name(cipher),
               SSL_CIPHER_get_bits(cipher, &bits));
    }
    printf("[tls] certificate verification: %s\n",
           SSL_get_verify_result(ssl) == X509_V_OK
               ? "OK (chain + hostname verified)"
               : "FAILED — but handshake continued?! (should not happen)");

    /* ---------- 4. HTTP over TLS: same request as bite 46 ---------- */
    char request[512];
    snprintf(request, sizeof request,
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Connection: close\r\n"
             "\r\n",
             path, host);

    printf("--- sending request ----------------------------------\n%s"
           "--- raw decrypted response ----------------------------\n",
           request);

    if (send_all(ssl, request, strlen(request)) < 0) {
        fprintf(stderr, "SSL_write failed:\n");
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        return 1;
    }

    /* SSL_read loop until 0 — same "0 means EOF" contract as recv. */
    long total = 0;
    for (;;) {
        char buf[4096];
        int n = SSL_read(ssl, buf, sizeof buf);
        if (n == 0) /* SSL_ERROR_ZERO_RETURN: clean TLS shutdown */
            break;
        if (n < 0) {
            fprintf(stderr, "\nSSL_read failed (err=%d):\n",
                    SSL_get_error(ssl, n));
            ERR_print_errors_fp(stderr);
            break;
        }
        fwrite(buf, 1, (size_t)n, stdout);
        total += n;
    }

    printf("\n--- end (%ld bytes, decrypted) ------------------------\n", total);

    SSL_free(ssl);   /* sends close_notify (polite TLS goodbye) */
    SSL_CTX_free(ctx);
    close(fd);
    return 0;
}
