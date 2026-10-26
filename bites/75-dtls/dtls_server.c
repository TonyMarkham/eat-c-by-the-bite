/* bite 75 — dtls_server.c: complete a DTLS 1.2 handshake over UDP and verify
 * the peer certificate against the SDP fingerprint.
 *
 * COMPILE + RUN (inside WSL, from the repo root):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/75-dtls/dtls_common.c \
 *       bites/75-dtls/dtls_server.c -o /tmp/b75s -lssl -lcrypto
 *   bash bites/75-dtls/gen-cert.sh
 *   # terminal 1:
 *   /tmp/b75s [port]                        # default 55555
 *   # terminal 2 (the local test double for "a browser"):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/75-dtls/dtls_common.c \
 *       bites/75-dtls/dtls_client.c -o /tmp/b75c -lssl -lcrypto
 *   /tmp/b75c 127.0.0.1 55555
 *
 * NO BROWSER, ON PURPOSE: bites 75/76/77 need a DTLS peer that acts like a
 * browser (client role, shows a certificate, offers use_srtp). dtls_client.c
 * IS that peer — two processes over UDP loopback, same wire behavior as the
 * browser side. The fingerprint check is still fully wired: the expected
 * value is loaded from client-fingerprint.txt exactly as it would come from
 * the a=fingerprint: line parsed in bite 73.
 *
 * THE SITTING-1 FRICTION POINTS (from the bite plan, all encountered here):
 *   - DTLSv1_listen needs cookie callbacks + a dgram BIO, not SSL_accept.
 *   - The same UDP socket serves STUN (bite 74) + DTLS + SRTP; demux by
 *     first byte belongs in bite 80 — here we are pure DTLS.
 *   - Handshake retransmissions only happen if you call DTLSv1_handle_timeout
 *     from your own loop (see dtls_common.c).
 *
 * BREAK IT ON PURPOSE (the bite's core lesson):
 *   /tmp/b75s 55555 <64 hex chars that are NOT the client's fingerprint>
 *   ...then start the client. The handshake completes cryptographically —
 *   and the server MUST abort with FINGERPRINT MISMATCH. TLS-level success
 *   proves nothing about WHO you talked to; the fingerprint is the identity
 *   check WebRTC replaces the CA system with. Run it. Never skip it again.
 */

#define _POSIX_C_SOURCE 200809L
#include "dtls_common.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int udp_listen(int port)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	a.sin_port = htons((unsigned short)port);
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
		perror("bind");
		close(fd);
		return -1;
	}
	return fd;
}

static int load_expected_fingerprint(char *out, size_t cap, const char *override)
{
	if (override) {                       /* the break-it path: corrupted fp */
		snprintf(out, cap, "%s", override);
		return 0;
	}
	FILE *f = fopen("bites/75-dtls/client-fingerprint.txt", "r");
	if (!f) {
		fprintf(stderr, "dtls: cannot open client-fingerprint.txt "
			"(run bites/75-dtls/gen-cert.sh first)\n");
		return -1;
	}
	size_t n = fread(out, 1, cap - 1, f);
	fclose(f);
	while (n && (out[n - 1] == '\n' || out[n - 1] == '\r'))
		out[--n] = '\0';
	return 0;
}

int main(int argc, char **argv)
{
	int port = argc > 1 ? atoi(argv[1]) : 55555;
	const char *fp_override = argc > 2 ? argv[2] : NULL;

	/* Line-buffered stdout: logs stay visible when output goes to a file
	 * or pipe (you will be tailing these while debugging handshakes). */
	setvbuf(stdout, NULL, _IOLBF, 0);

	char expected[128];
	if (load_expected_fingerprint(expected, sizeof(expected), fp_override) != 0)
		return 1;

	SSL_CTX *ctx = dtls_ctx_create("bites/75-dtls/server.crt",
				       "bites/75-dtls/server.key", 1);
	if (!ctx)
		return 1;
	int fd = udp_listen(port);
	if (fd < 0) {
		SSL_CTX_free(ctx);
		return 1;
	}
	printf("dtls-server: UDP %d, waiting for a DTLS client...\n", port);

	/* ---- step 1: cookie exchange via DTLSv1_listen ----
	 * A brand-new SSL bound to the listen socket; DTLSv1_listen answers
	 * HelloVerify with a stateless cookie and only "returns" once a
	 * client that could receive at its address sends a second
	 * ClientHello with the right cookie. */
	SSL *ssl = SSL_new(ctx);
	if (!ssl) {
		fprintf(stderr, "dtls: SSL_new failed\n");
		return 1;
	}
	BIO *bio = BIO_new_dgram(fd, BIO_NOCLOSE);
	SSL_set_bio(ssl, bio, bio);

	/* OpenSSL 3 speaks BIO_ADDR here, not sockaddr — and BIO_ADDR is an
	 * opaque type, so it lives on the heap (BIO_ADDR_new/free). */
	BIO_ADDR *cli = BIO_ADDR_new();
	if (!cli) {
		fprintf(stderr, "dtls: BIO_ADDR_new failed\n");
		SSL_free(ssl);
		SSL_CTX_free(ctx);
		close(fd);
		return 1;
	}
	struct sockaddr_in cli_in;
	memset(&cli_in, 0, sizeof(cli_in));
	for (;;) {
		int r = DTLSv1_listen(ssl, cli);
		if (r == 1)
			break;                       /* validated client hello */
		int err = SSL_get_error(ssl, r);
		if (err == SSL_ERROR_WANT_READ)     /* cookie sent; wait for retry */
			continue;
		fprintf(stderr, "dtls: DTLSv1_listen failed (err=%d)\n", err);
		ERR_print_errors_fp(stderr);
		BIO_ADDR_free(cli);
		SSL_free(ssl);
		SSL_CTX_free(ctx);
		close(fd);
		return 1;
	}
	/* Pull the validated peer address out of the BIO_ADDR. */
	cli_in.sin_family = AF_INET;
	cli_in.sin_port = BIO_ADDR_rawport(cli);
	size_t iplen = 4;
	BIO_ADDR_rawaddress(cli, (unsigned char *)&cli_in.sin_addr.s_addr, &iplen);
	BIO_ADDR_free(cli);
	char ip[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &cli_in.sin_addr, ip, sizeof(ip));
	printf("dtls-server: cookie-validated client at %s:%d\n",
	       ip, ntohs(cli_in.sin_port));

	/* ---- step 2: pin the peer address onto a fresh dgram BIO ----
	 * From here every read/write goes to this one peer — the UDP analog
	 * of accept() having handed you a connected socket. */
	bio = BIO_new_dgram(fd, BIO_NOCLOSE);
	BIO_dgram_set_peer(bio, (struct sockaddr *)&cli_in);
	SSL_set_bio(ssl, bio, bio);          /* frees the old listen BIO */

	/* ---- step 3: full handshake with retransmission timers ---- */
	if (!dtls_handshake_loop(ssl, 1)) {
		fprintf(stderr, "dtls-server: handshake failed\n");
		SSL_shutdown(ssl);
		SSL_free(ssl);
		SSL_CTX_free(ctx);
		close(fd);
		return 1;
	}
	printf("dtls-server: handshake complete (%s, cipher %s)\n",
	       SSL_get_version(ssl), SSL_get_cipher_name(ssl));

	/* ---- step 4: THE security model — fingerprint check ----
	 * In the real system, `expected` came from the browser's SDP
	 * (a=fingerprint:, parsed by bite 73). Skip this check and any
	 * active MITM with a valid self-signed cert owns your media. */
	int fprc = dtls_verify_peer_fingerprint(ssl, expected);
	if (fprc != 1) {
		fprintf(stderr, "dtls-server: connection rejected\n");
		SSL_shutdown(ssl);
		SSL_free(ssl);
		SSL_CTX_free(ctx);
		close(fd);
		return 1;
	}
	printf("dtls-server: peer identity VERIFIED against SDP fingerprint\n");

	/* ---- step 5: prove the data channel ---- */
	char buf[512];
	int n = SSL_read(ssl, buf, sizeof(buf) - 1);
	if (n > 0) {
		buf[n] = '\0';
		printf("dtls-server: received \"%s\" (DTLS appdata on UDP!)\n", buf);
		SSL_write(ssl, "pong from c-prep server", 23);
	}

	/* ---- step 6: clean teardown (valgrind-clean, bite 79 preview) ---- */
	SSL_shutdown(ssl);
	SSL_free(ssl);
	SSL_CTX_free(ctx);
	close(fd);
	printf("dtls-server: done\n");
	return 0;
}
