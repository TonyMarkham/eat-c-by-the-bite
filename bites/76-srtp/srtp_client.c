/* bite 76 — srtp_client.c: the browser-side test double that proves the
 * server's first SRTP packet is genuinely encrypted with the DTLS-derived
 * keys — and answers with its own packet to prove the reverse direction.
 *
 * COMPILE + RUN (inside WSL, from the repo root, srtp_server running):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/76-srtp/dtls_common.c \
 *       bites/76-srtp/srtp_client.c -o /tmp/b76c -lssl -lcrypto -lsrtp2
 *   /tmp/b76c 127.0.0.1 55557
 *
 * The client must call the exporter with the SAME label and the SAME context
 * (its own use_srtp offer) — then both sides hold identical key blocks and
 * every byte below the handshake is symmetric.
 */

#define _POSIX_C_SOURCE 200809L
#include "dtls_common.h"

#include <srtp2/srtp.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <openssl/err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define EXPORT_LEN  60
#define RTP_HDR_LEN 12

static const unsigned char USE_SRTP_CTX[] = { 0x00, 0x01 };

static srtp_t srtp_session_make(const unsigned char *key, srtp_ssrc_type_t dir)
{
	srtp_policy_t policy;
	memset(&policy, 0, sizeof(policy));
	srtp_crypto_policy_set_from_profile_for_rtp(&policy.rtp,
						    srtp_profile_aes128_cm_sha1_80);
	srtp_crypto_policy_set_from_profile_for_rtcp(&policy.rtcp,
						     srtp_profile_aes128_cm_sha1_80);
	policy.ssrc.type  = dir;
	policy.ssrc.value = 0;
	policy.key = (unsigned char *)key;
	policy.window_size = 128;
	policy.next = NULL;
	srtp_t sess = NULL;
	if (srtp_create(&sess, &policy) != srtp_err_status_ok)
		return NULL;
	return sess;
}

int main(int argc, char **argv)
{
	const char *host = argc > 1 ? argv[1] : "127.0.0.1";
	const char *port = argc > 2 ? argv[2] : "55557";
	setvbuf(stdout, NULL, _IOLBF, 0);

	if (srtp_init() != srtp_err_status_ok) {
		fprintf(stderr, "srtp: srtp_init failed\n");
		return 1;
	}
	SSL_CTX *ctx = dtls_ctx_create("bites/76-srtp/client.crt",
				       "bites/76-srtp/client.key", 0);
	if (!ctx)
		return 1;

	struct addrinfo hints, *res;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	if (getaddrinfo(host, port, &hints, &res) != 0) {
		fprintf(stderr, "srtp-client: bad address\n");
		return 1;
	}
	int fd = socket(res->ai_family, res->ai_socktype, 0);
	if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
		perror("connect");
		return 1;
	}
	struct sockaddr_storage peer;
	memcpy(&peer, res->ai_addr, res->ai_addrlen);
	freeaddrinfo(res);

	SSL *ssl = SSL_new(ctx);
	BIO *bio = BIO_new_dgram(fd, BIO_NOCLOSE);
	BIO_dgram_set_peer(bio, (struct sockaddr *)&peer);
	SSL_set_bio(ssl, bio, bio);

	if (!dtls_handshake_loop(ssl, 0)) {
		fprintf(stderr, "srtp-client: handshake failed\n");
		return 1;
	}
	printf("srtp-client: DTLS handshake complete; deriving SRTP keys\n");

	unsigned char keys[EXPORT_LEN];
	if (SSL_export_keying_material(ssl, keys, EXPORT_LEN,
				       "EXTRACTOR-dtls_srtp", 19,
				       USE_SRTP_CTX, sizeof(USE_SRTP_CTX), 1) != 1) {
		ERR_print_errors_fp(stderr);
		return 1;
	}
	printf("srtp-client: keys exported (same 60 bytes as the server — "
	       "verify against its log)\n");

	/* Client's sessions are the mirror image of the server's. */
	srtp_t sess_out = srtp_session_make(keys, ssrc_any_outbound);
	srtp_t sess_in  = srtp_session_make(keys + 16, ssrc_any_inbound);
	if (!sess_out || !sess_in)
		return 1;

	/* ---- receive the server's first SRTP packet ---- */
	struct timeval tv = { 3, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	unsigned char rx[512];
	ssize_t r = recv(fd, rx, sizeof(rx), 0);
	if (r <= 0) {
		fprintf(stderr, "srtp-client: TIMEOUT waiting for SRTP packet — "
			"if the server ran with --swap-keys, this is the silent "
			"direction bug in action\n");
		return 1;
	}
	size_t rlen = (size_t)r;
	printf("srtp-client: received %zu encrypted bytes, first: %02x %02x\n",
	       rlen, rx[0], rx[1]);
	int urx = (int)rlen;
	if (srtp_unprotect(sess_in, rx, &urx) != srtp_err_status_ok) {
		fprintf(stderr, "srtp-client: UNPROTECT FAILED\n");
		return 1;
	}
	rlen = (size_t)urx;
	unsigned short seq = (unsigned short)((rx[2] << 8) | rx[3]);
	unsigned int ts = ((unsigned)rx[4] << 24 | (unsigned)rx[5] << 16 |
			   (unsigned)rx[6] << 8 | rx[7]);
	unsigned int ssrc = ((unsigned)rx[8] << 24 | (unsigned)rx[9] << 16 |
			     (unsigned)rx[10] << 8 | rx[11]);
	int silent = 1;
	for (int i = RTP_HDR_LEN; i < (int)rlen; i++)
		if (rx[i] != 0)
			silent = 0;
	printf("srtp-client: DECRYPTED: seq=%u ts=%u ssrc=0x%08x pt=%u len=%zu "
	       "payload all-zero(silence)=%s\n",
	       seq, ts, ssrc, rx[1] & 0x7f, rlen, silent ? "yes" : "no");
	if (seq != 1 || ts != 0 || !silent) {
		fprintf(stderr, "srtp-client: packet content mismatch\n");
		return 1;
	}

	/* ---- answer with our own RTP packet (reverse direction proof) ---- */
	unsigned char pkt[512];
	pkt[0] = 0x80;
	pkt[1] = 0;                          /* PT=0 PCMU */
	pkt[2] = 0; pkt[3] = 7;              /* seq=7 */
	pkt[4] = 0; pkt[5] = 0; pkt[6] = 6;  pkt[7] = 64;   /* ts = 1600 */
	pkt[8] = 0x12; pkt[9] = 0x34; pkt[10] = 0x56; pkt[11] = 0x78;
	memset(pkt + RTP_HDR_LEN, 0xAB, 160); /* recognizable payload */
	size_t len = RTP_HDR_LEN + 160;
	int plen = (int)len;
	if (srtp_protect(sess_out, pkt, &plen) != srtp_err_status_ok) {
		fprintf(stderr, "srtp-client: protect failed\n");
		return 1;
	}
	len = (size_t)plen;
	send(fd, pkt, len, 0);
	printf("srtp-client: sent protected RTP seq=7 ssrc=0x12345678\n");

	srtp_dealloc(sess_out);
	srtp_dealloc(sess_in);
	srtp_shutdown();
	SSL_shutdown(ssl);
	SSL_free(ssl);
	SSL_CTX_free(ctx);
	close(fd);
	printf("srtp-client: done\n");
	return 0;
}
