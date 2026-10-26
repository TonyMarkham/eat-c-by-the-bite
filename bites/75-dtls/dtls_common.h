/* bite 75 — dtls_common.h/.c: shared DTLS setup used by dtls_server.c and
 * dtls_client.c.
 *
 * WHY THIS SPLIT: bite 80 will reuse these exact helpers, and the server /
 * client stay small enough to read top-to-bottom. This is bite 37's
 * "headers = interfaces" discipline applied to a subsystem.
 *
 * THE THREE FIDDLY DTLS-OVER-UDP THINGS THIS FILE SOLVES
 *   1. Cookie exchange (HelloVerify): DTLS servers must prove the client can
 *      receive at its claimed address before spending memory on it
 *      (anti-DoS). OpenSSL wants cookie generate/verify callbacks + the
 *      DTLSv1_listen state machine; we HMAC the peer address.
 *   2. Dgram BIO plumbing: UDP has no connection, so each peer needs a BIO
 *      whose "peer address" is pinned (BIO_dgram_set_peer) — the UDP
 *      equivalent of an accepted TCP socket.
 *   3. Retransmission timers: DTLS retransmits lost handshake flights
 *      itself, but only if you drive DTLSv1_handle_timeout() from your event
 *      loop. We expose dtls_handshake_loop() doing select() + timer.
 *
 * WHERE THE SDP FINGERPRINT PLUGS IN (the bite's exam)
 *   dtls_verify_peer_fingerprint() is called by the server right after the
 *   handshake: SHA-256 of the peer's certificate vs. the a=fingerprint:
 *   value parsed by bite 73. In production that value arrives via signaling;
 *   here the test double's fingerprint comes from client-fingerprint.txt
 *   (written by gen-cert.sh). A mismatch MUST abort the connection — see
 *   dtls_server.c's main for the negative test.
 */

#ifndef DTLS_COMMON_H
#define DTLS_COMMON_H

#include <openssl/ssl.h>
#include <netinet/in.h>

/* Create an SSL_CTX for DTLS 1.2 with our cert + key loaded, the DTLS-SRTP
 * profile advertised (the use_srtp extension — bite 76 consumes the keys),
 * and cookie callbacks installed (server side needs them for DTLSv1_listen).
 * is_server selects SSL_accept-side defaults; returns NULL on failure. */
SSL_CTX *dtls_ctx_create(const char *certfile, const char *keyfile, int is_server);

/* Run the DTLS handshake on an already-set-up SSL (BIO attached) to
 * completion, driving retransmission timers. Returns 1 ok, 0 failed. */
int dtls_handshake_loop(SSL *ssl, int is_server);

/* THE fingerprint check. expected_hex: 64 hex chars, colons optional
 * (bite 73's parsed SDP value). Returns 1 = match, 0 = mismatch, -1 = error. */
int dtls_verify_peer_fingerprint(SSL *ssl, const char *expected_hex);

/* Compute SHA-256 fingerprint of OUR certificate (client prints it so the
 * operator can spot-check against client-fingerprint.txt). Writes 64 hex
 * chars + NUL into out (cap >= 65). Returns 0 ok. */
int dtls_own_fingerprint(SSL_CTX *ctx, char *out, size_t cap);

/* Strip colons from a fingerprint hex string in place. */
void dtls_normalize_hex(char *hex);

#endif
