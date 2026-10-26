/* bite 75 — dtls_client.c: the browser-side test double for the DTLS server.
 *
 * COMPILE + RUN (inside WSL, from the repo root):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/75-dtls/dtls_common.c \
 *       bites/75-dtls/dtls_client.c -o /tmp/b75c -lssl -lcrypto
 *   /tmp/b75c [host] [port]          # server must be running first
 *
 * WHAT "acting like a browser" means here:
 *   - DTLS CLIENT role (browsers act as DTLS client when their SDP said
 *     a=setup:actpass and the server answered setup:passive/active).
 *   - presents ITS OWN self-signed certificate (client.crt) — this is the
 *     cert whose SHA-256 hash lives in the SDP's a=fingerprint: line; the
 *     server verifies it (see the server's step 4).
 *   - offers the use_srtp extension (bite 76 needs it to export SRTP keys).
 * On a real browser you'd additionally verify the SERVER's cert against the
 * server's SDP fingerprint; we print our cert's fingerprint instead so the
 * operator can see the values that would be exchanged in SDP.
 */

#define _POSIX_C_SOURCE 200809L
#include "dtls_common.h"

#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int udp_connect(const char *host, const char *port,
		       struct sockaddr_storage *out)
{
	struct addrinfo hints, *res;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	if (getaddrinfo(host, port, &hints, &res) != 0) {
		fprintf(stderr, "dtls-client: bad address %s:%s\n", host, port);
		return -1;
	}
	int fd = -1;
	for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, 0);
		if (fd < 0)
			continue;
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
			memcpy(out, ai->ai_addr, ai->ai_addrlen);
			break;
		}
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	return fd;
}

int main(int argc, char **argv)
{
	const char *host = argc > 1 ? argv[1] : "127.0.0.1";
	const char *port = argc > 2 ? argv[2] : "55555";
	setvbuf(stdout, NULL, _IOLBF, 0);

	SSL_CTX *ctx = dtls_ctx_create("bites/75-dtls/client.crt",
				       "bites/75-dtls/client.key", 0);
	if (!ctx)
		return 1;

	struct sockaddr_storage peer;
	int fd = udp_connect(host, port, &peer);
	if (fd < 0) {
		fprintf(stderr, "dtls-client: cannot reach %s:%s\n", host, port);
		SSL_CTX_free(ctx);
		return 1;
	}

	SSL *ssl = SSL_new(ctx);
	if (!ssl) {
		SSL_CTX_free(ctx);
		return 1;
	}
	/* connect()ed UDP socket + dgram BIO with pinned peer: exactly the
	 * plumbing a browser uses for its DTLS-over-UDP media channel. */
	BIO *bio = BIO_new_dgram(fd, BIO_NOCLOSE);
	BIO_dgram_set_peer(bio, (struct sockaddr *)&peer);
	SSL_set_bio(ssl, bio, bio);

	char ownfp[65];
	if (dtls_own_fingerprint(ctx, ownfp, sizeof(ownfp)) == 0)
		printf("dtls-client: OUR cert fingerprint (would be sent as "
		       "a=fingerprint:sha-256 %s:%s)\n",
		       "sha-256", ownfp);

	printf("dtls-client: starting DTLS handshake with %s:%s...\n", host, port);
	if (!dtls_handshake_loop(ssl, 0)) {
		fprintf(stderr, "dtls-client: handshake failed\n");
		SSL_free(ssl);
		SSL_CTX_free(ctx);
		close(fd);
		return 1;
	}
	printf("dtls-client: handshake complete (%s, cipher %s)\n",
	       SSL_get_version(ssl), SSL_get_cipher_name(ssl));

	/* A real browser would now ALSO hash the server's certificate and
	 * compare it to the server SDP's fingerprint. Left as the reader's
	 * exercise — the helper exists: dtls_verify_peer_fingerprint(). */

	SSL_write(ssl, "ping from c-prep client", 23);
	char buf[512];
	int n = SSL_read(ssl, buf, sizeof(buf) - 1);
	if (n > 0) {
		buf[n] = '\0';
		printf("dtls-client: received \"%s\"\n", buf);
	}

	SSL_shutdown(ssl);
	SSL_free(ssl);
	SSL_CTX_free(ctx);
	close(fd);
	printf("dtls-client: done\n");
	return 0;
}
