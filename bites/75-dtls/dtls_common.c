/* bite 75 — dtls_common.c: implementation of the shared DTLS helpers.
 * Read dtls_common.h first — the WHY comments live there and here. */

#define _POSIX_C_SOURCE 200809L
#include "dtls_common.h"

#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/srtp.h>
#include <ctype.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* Cookie secret: in a real server this must be random per process and kept
 * for the process lifetime (cookies signed with a rotating secret would
 * strand in-flight handshakes). Generated once, below. */
static unsigned char g_cookie_secret[32];
static int g_cookie_secret_ready = 0;

static void ensure_cookie_secret(void)
{
	if (!g_cookie_secret_ready) {
		if (RAND_bytes(g_cookie_secret, (int)sizeof(g_cookie_secret)) != 1) {
			fprintf(stderr, "dtls: RAND_bytes failed for cookie secret\n");
			exit(1);
		}
		g_cookie_secret_ready = 1;
	}
}

/* RFC 6347: cookie = HMAC(peer address). Cheap, stateless, unforgeable.
 * NOTE: BIO_dgram_get_peer fills a sockaddr and RETURNS its length — HMAC
 * must cover exactly those bytes, not the trailing (uninitialized) slack of
 * a sockaddr_storage, or generate/verify will disagree. */
static int cookie_gen(SSL *ssl, unsigned char *cookie, unsigned int *cookie_len)
{
	(void)ssl;
	ensure_cookie_secret();
	struct sockaddr_storage ss;
	socklen_t sl = sizeof(ss);
	memset(&ss, 0, sizeof(ss));
	int got = BIO_dgram_get_peer(SSL_get_rbio(ssl), (struct sockaddr *)&ss);
#ifdef DEBUG
	fprintf(stderr, "dtls: cookie_gen: BIO_dgram_get_peer -> %d (family %d)\n",
		got, got > 0 ? ((struct sockaddr *)&ss)->sa_family : -1);
#endif
	if (got <= 0)
		return 0;
	if ((socklen_t)got < sl)
		sl = (socklen_t)got;
	unsigned int maclen = 0;
	HMAC(EVP_sha256(), g_cookie_secret, (int)sizeof(g_cookie_secret),
	     (unsigned char *)&ss, sl, cookie, &maclen);
	*cookie_len = maclen;
	return 1;
}

static int cookie_verify(SSL *ssl, const unsigned char *cookie, unsigned int cookie_len)
{
	(void)ssl;
	ensure_cookie_secret();
	struct sockaddr_storage ss;
	socklen_t sl = sizeof(ss);
	memset(&ss, 0, sizeof(ss));
	int got = BIO_dgram_get_peer(SSL_get_rbio(ssl), (struct sockaddr *)&ss);
	if (got <= 0)
		return 0;
	if ((socklen_t)got < sl)
		sl = (socklen_t)got;
	unsigned char mac[EVP_MAX_MD_SIZE];
	unsigned int maclen = 0;
	HMAC(EVP_sha256(), g_cookie_secret, (int)sizeof(g_cookie_secret),
	     (unsigned char *)&ss, sl, mac, &maclen);
#ifdef DEBUG
	fprintf(stderr, "dtls: cookie_verify: peer->%d len=%u\n", got, cookie_len);
#endif
	return cookie_len == maclen && memcmp(cookie, mac, maclen) == 0;
}

/* The DTLS-SRTP profile BOTH ends must agree on — bite 76 derives its keys
 * for exactly this profile. Registering it here sends the use_srtp extension
 * in the handshake, which is why SSL_export_keying_material works later. */
#define SRTP_PROFILE "SRTP_AES128_CM_SHA1_80"

/* "No CA" callback: see the comment at its use below. */
static int verify_accept_any_cert(int preverify_ok, X509_STORE_CTX *ctx)
{
	(void)preverify_ok;
	(void)ctx;
	return 1;   /* the fingerprint match (after the handshake) is the real check */
}

SSL_CTX *dtls_ctx_create(const char *certfile, const char *keyfile, int is_server)
{
	ensure_cookie_secret();
	SSL_CTX *ctx = SSL_CTX_new(is_server ? DTLS_server_method()
					     : DTLS_client_method());
	if (!ctx) {
		fprintf(stderr, "dtls: SSL_CTX_new failed\n");
		return NULL;
	}
	/* TLS 1.3-style security applied to DTLS 1.2: our certs are ECDSA
	 * P-256, so this suite list always matches. */
	SSL_CTX_set_cipher_list(ctx, "HIGH:!aNULL:!MD5");

	if (SSL_CTX_use_certificate_file(ctx, certfile, SSL_FILETYPE_PEM) != 1 ||
	    SSL_CTX_use_PrivateKey_file(ctx, keyfile, SSL_FILETYPE_PEM) != 1) {
		fprintf(stderr, "dtls: cannot load %s/%s (run gen-cert.sh first)\n",
			certfile, keyfile);
		SSL_CTX_free(ctx);
		return NULL;
	}
	/* WebRTC REQUIRES mutual authentication: the browser shows its cert,
	 * we show ours, and the SDP fingerprints vouch for both. There is no
	 * CA in WebRTC — so the X.509 chain check is replaced by "always
	 * accept the chain, then match the SHA-256 fingerprint yourself"
	 * (dtls_verify_peer_fingerprint). This callback is that "accept".
	 * Requiring a cert (FAIL_IF_NO_PEER_CERT) is NOT negotiable. */
	SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
			   verify_accept_any_cert);

	if (SSL_CTX_set_tlsext_use_srtp(ctx, SRTP_PROFILE) != 0) {
		fprintf(stderr, "dtls: cannot set SRTP profile %s\n", SRTP_PROFILE);
		SSL_CTX_free(ctx);
		return NULL;
	}

	if (is_server) {
		SSL_CTX_set_cookie_generate_cb(ctx, cookie_gen);
		SSL_CTX_set_cookie_verify_cb(ctx, cookie_verify);
		/* DTLS handshake timers, tuned for loopback but sane in general. */
		SSL_CTX_set_timeout(ctx, 10);   /* seconds; retransmit window */
	}
	return ctx;
}

/* Drive the handshake: select() on the socket until readable or until
 * OpenSSL's DTLS timer says "retransmit now". This is the loop every
 * hand-rolled DTLS server needs — browsers on lossy links depend on it. */
int dtls_handshake_loop(SSL *ssl, int is_server)
{
	(void)is_server;
	int fd = SSL_get_fd(ssl);
	for (;;) {
		int r = is_server ? SSL_accept(ssl) : SSL_connect(ssl);
		if (r == 1)
			return 1;                      /* handshake complete */
		int err = SSL_get_error(ssl, r);
		if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
			/* Wait for either a retransmitted flight or the DTLS
			 * retransmit timer. DTLSv1_get_timeout tells us when
			 * OpenSSL wants to retransmit; if it has nothing
			 * pending we fall back to a plain 5 s cap. */
			struct timeval tv = { 5, 0 };
			if (DTLSv1_get_timeout(ssl, &tv) != 1) {
				tv.tv_sec = 5;
				tv.tv_usec = 0;
			}
			fd_set rfds;
			FD_ZERO(&rfds);
			FD_SET(fd, &rfds);
			select(fd + 1, &rfds, NULL, NULL, &tv);
			/* If the timer expired without data, kick retransmission. */
			if (!FD_ISSET(fd, &rfds)) {
				if (DTLSv1_handle_timeout(ssl) < 0)
					return 0;
			}
			continue;
		}
		fprintf(stderr, "dtls: handshake failed (err=%d)\n", err);
		ERR_print_errors_fp(stderr);
		return 0;
	}
}

/* SHA-256 of the peer certificate, compared against bite 73's SDP value. */
int dtls_verify_peer_fingerprint(SSL *ssl, const char *expected_hex)
{
	X509 *cert = SSL_get1_peer_certificate(ssl);   /* new name in OpenSSL 3 */
	if (!cert) {
		fprintf(stderr, "dtls: peer sent no certificate (mitm guard tripped)\n");
		return -1;
	}
	unsigned char md[EVP_MAX_MD_SIZE];
	unsigned int mdlen = 0;
	if (!X509_digest(cert, EVP_sha256(), md, &mdlen)) {
		X509_free(cert);
		return -1;
	}
	char got[65];
	for (unsigned int i = 0; i < mdlen; i++)
		snprintf(got + 2 * i, 3, "%02x", md[i]);
	got[64] = '\0';

	char norm[128];
	snprintf(norm, sizeof(norm), "%s", expected_hex);
	dtls_normalize_hex(norm);

	int rc = strcmp(got, norm) == 0 ? 1 : 0;
	printf("dtls: peer cert fingerprint (SHA-256): %s\n", got);
	printf("dtls: expected (from SDP/bite 73)   : %s\n", norm);
	if (rc != 1)
		fprintf(stderr, "dtls: FINGERPRINT MISMATCH — aborting (this is the "
			"MITM you exist to prevent; see bite 75 'break it on purpose')\n");
	X509_free(cert);       /* every SSL object/child freed exactly once */
	return rc;
}

int dtls_own_fingerprint(SSL_CTX *ctx, char *out, size_t cap)
{
	X509 *cert = SSL_CTX_get0_certificate(ctx);
	if (!cert)
		return -1;
	unsigned char md[EVP_MAX_MD_SIZE];
	unsigned int mdlen = 0;
	if (!X509_digest(cert, EVP_sha256(), md, &mdlen) || mdlen != 32)
		return -1;
	if (cap < 65)
		return -1;
	for (int i = 0; i < 32; i++)
		snprintf(out + 2 * i, 3, "%02x", md[i]);
	out[64] = '\0';
	return 0;
}

void dtls_normalize_hex(char *hex)
{
	/* SDP fingerprints are case-insensitive hex (browsers emit uppercase,
	 * we emit lowercase): fold to lowercase before comparing, and strip
	 * the colon separators. */
	char *w = hex;
	for (char *r = hex; *r; r++) {
		if (*r == ':')
			continue;
		*w++ = (char)tolower((unsigned char)*r);
	}
	*w = '\0';
}
