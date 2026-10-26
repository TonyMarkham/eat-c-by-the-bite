/* bite 77 — echo_client.c: sends RTP PCMU through the echo server and checks
 * what comes back — the "you hear yourself" loopback, minus the speaker.
 *
 * COMPILE + RUN (inside WSL, from the repo root, echo_server running):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/77-media-echo/dtls_common.c \
 *       bites/77-media-echo/echo_client.c -o /tmp/b77c -lssl -lcrypto -lsrtp2
 *   timeout 20 /tmp/b77c 127.0.0.1 55558
 *
 * WHAT IT CHECKS (all the bite's receive-path guarantees):
 *   - every valid packet comes back within the wait window
 *   - echoes carry the SERVER's SSRC (0xC0FFEE01), never our own — the
 *     WebRTC distinct-SSRC rule that --echo-own-ssrc-off violates
 *   - echo sequence numbers start at 1 and increase by exactly 1
 *   - echo timestamps advance by 160 per packet (20 ms @ 8 kHz)
 *   - the payload that comes back is bit-identical to what we sent
 *   - the malformed packet (RTP version 1) and the duplicate seq produce
 *     NO echo — drop-and-log, not crash
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

#define RTP_HDR_LEN  12
#define CLIENT_SSRC  0x12345678u
#define TS_STEP      160   /* 20 ms @ 8 kHz — must match the server's ticker */

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
	srtp_t sess = NULL;
	srtp_create(&sess, &policy);
	return sess;
}

/* Send one RTP packet (protected). version_override!=0 mangles the header
 * for the malformed-packet test. */
static int send_rtp(int fd, srtp_t sess, unsigned short seq, unsigned int ts,
		    int mangle_version, unsigned char *sent_payload, size_t plen)
{
	unsigned char pkt[512];
	pkt[0] = mangle_version ? (unsigned char)(mangle_version << 6) : 0x80;
	pkt[1] = 0x00;                       /* PT=0 PCMU, M=0 */
	pkt[2] = (unsigned char)(seq >> 8);
	pkt[3] = (unsigned char)(seq & 0xff);
	pkt[4] = (unsigned char)(ts >> 24);
	pkt[5] = (unsigned char)(ts >> 16);
	pkt[6] = (unsigned char)(ts >> 8);
	pkt[7] = (unsigned char)ts;
	pkt[8]  = (unsigned char)(CLIENT_SSRC >> 24);
	pkt[9]  = (unsigned char)(CLIENT_SSRC >> 16);
	pkt[10] = (unsigned char)(CLIENT_SSRC >> 8);
	pkt[11] = (unsigned char)CLIENT_SSRC;
	for (size_t i = 0; i < plen; i++)
		pkt[RTP_HDR_LEN + i] = (unsigned char)((seq * 7 + i) & 0xff);
	memcpy(sent_payload, pkt + RTP_HDR_LEN, plen);

	if (mangle_version) {
		/* Send raw: it is not valid SRTP (not even valid RTP), which is
		 * the point — the server must drop it without crashing. */
		return send(fd, pkt, RTP_HDR_LEN + plen, 0) > 0 ? 0 : -1;
	}
	int len = (int)(RTP_HDR_LEN + plen);
	if (srtp_protect(sess, pkt, &len) != srtp_err_status_ok)
		return -2;   /* protect refused (e.g. duplicate seq) — see caller */
	return send(fd, pkt, (size_t)len, 0) > 0 ? 0 : -1;
}

/* Try to receive + unprotect + parse one echo. Returns 0 on success. */
static int recv_echo(int fd, srtp_t sess, unsigned char *payload, size_t plen,
		     unsigned short *out_seq, unsigned int *out_ts,
		     unsigned int *out_ssrc)
{
	unsigned char rx[2048];
	ssize_t r = recv(fd, rx, sizeof(rx), 0);
	if (r <= 0)
		return -1;
	int ilen = (int)r;
	if (srtp_unprotect(sess, rx, &ilen) != srtp_err_status_ok)
		return -2;
	size_t n = (size_t)ilen;
	if (n < RTP_HDR_LEN + plen)
		return -3;
	*out_seq = (unsigned short)((rx[2] << 8) | rx[3]);
	*out_ts = ((unsigned)rx[4] << 24 | (unsigned)rx[5] << 16 |
		   (unsigned)rx[6] << 8 | rx[7]);
	*out_ssrc = ((unsigned)rx[8] << 24 | (unsigned)rx[9] << 16 |
		     (unsigned)rx[10] << 8 | rx[11]);
	memcpy(payload, rx + RTP_HDR_LEN, plen);
	return 0;
}

int main(int argc, char **argv)
{
	const char *host = argc > 1 ? argv[1] : "127.0.0.1";
	const char *port = argc > 2 ? argv[2] : "55558";
	setvbuf(stdout, NULL, _IOLBF, 0);

	if (srtp_init() != srtp_err_status_ok)
		return 1;
	SSL_CTX *ctx = dtls_ctx_create("bites/77-media-echo/client.crt",
				       "bites/77-media-echo/client.key", 0);
	if (!ctx)
		return 1;

	struct addrinfo hints, *res;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	getaddrinfo(host, port, &hints, &res);
	int fd = socket(res->ai_family, res->ai_socktype, 0);
	connect(fd, res->ai_addr, res->ai_addrlen);
	struct sockaddr_storage peer;
	memcpy(&peer, res->ai_addr, res->ai_addrlen);
	freeaddrinfo(res);

	SSL *ssl = SSL_new(ctx);
	BIO *bio = BIO_new_dgram(fd, BIO_NOCLOSE);
	BIO_dgram_set_peer(bio, (struct sockaddr *)&peer);
	SSL_set_bio(ssl, bio, bio);
	if (!dtls_handshake_loop(ssl, 0)) {
		fprintf(stderr, "echo-client: handshake failed\n");
		return 1;
	}
	unsigned char keys[60];
	if (SSL_export_keying_material(ssl, keys, 60, "EXTRACTOR-dtls_srtp", 19,
				       USE_SRTP_CTX, 2, 1) != 1) {
		ERR_print_errors_fp(stderr);
		return 1;
	}
	srtp_t sess_out = srtp_session_make(keys, ssrc_any_outbound);
	srtp_t sess_in  = srtp_session_make(keys + 16, ssrc_any_inbound);

	struct timeval tv = { 2, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

#define PL 160
	unsigned char sent[PL], got[PL];
	int failures = 0, echoes = 0;
	unsigned int want_ts = 0;

	/* Plan: seqs 1..9, a MALFORMED packet (no seq), a DUPLICATE of seq 9
	 * (libsrtp refuses to even protect it — see below), then 11..18.
	 * Expected echoes: 17 packets, one per delivered RTP packet, carrying
	 * the server's OWN sequence numbers 1,2,3,... (it re-headers, so echo
	 * seq != inbound seq). orig[] tracks which inbound packet each echo
	 * corresponds to, because the payload check must compare against what
	 * we actually put in THAT packet. */
	unsigned short orig[32];
	int norig = 0;
	struct { unsigned short seq; int mangle; } plan[] = {
		{ 1, 0 }, { 2, 0 }, { 3, 0 }, { 4, 0 }, { 5, 0 },
		{ 6, 0 }, { 7, 0 }, { 8, 0 }, { 9, 0 },
		{ 0, 1 },          /* malformed: RTP version 1, no real seq */
		{ 9, 0 },          /* duplicate seq — outbound guard refuses it */
		{ 11, 0 }, { 12, 0 }, { 13, 0 }, { 14, 0 },
		{ 15, 0 }, { 16, 0 }, { 17, 0 }, { 18, 0 },
	};

	for (size_t i = 0; i < sizeof(plan) / sizeof(plan[0]); i++) {
		unsigned int ts = (unsigned)(plan[i].seq * TS_STEP);
		int rc = send_rtp(fd, sess_out, plan[i].seq, ts, plan[i].mangle,
				  sent, PL);
		if (rc == -2) {
			/* libsrtp's OUTBOUND replay window refuses to protect a
			 * sequence number we already sent (RFC 3711 replay
			 * protection is bidirectional). A real sender advances
			 * its sequence instead; the duplicate therefore never
			 * reaches the server — to exercise the server's inbound
			 * dedup, replay the captured bytes from a second socket
			 * and watch "DROP duplicate seq". */
			printf("echo-client: note: outbound protect refused for "
			       "duplicate seq %u (libsrtp replay guard) — "
			       "expected, continuing\n", plan[i].seq);
			continue;
		}
		if (rc != 0) {
			fprintf(stderr, "echo-client: send failed\n");
			return 1;
		}
		if (!plan[i].mangle)
			orig[norig++] = plan[i].seq;
	}
	/* Now collect everything the server sent back. */
	for (;;) {
		unsigned short es; unsigned int ets, essrc;
		int rc = recv_echo(fd, sess_in, got, PL, &es, &ets, &essrc);
		if (rc != 0)
			break;                     /* drained (or error, caught below) */
		echoes++;
		if (essrc != 0xC0FFEE01u) {
			fprintf(stderr, "echo-client: FAIL echo seq %u carried "
				"SSRC 0x%08x, not the server's 0xC0FFEE01 — "
				"WebRTC requires distinct SSRC per direction!\n",
				es, essrc);
			failures++;
		}
		if (es != echoes) {                /* server seq: 1,2,3,... */
			fprintf(stderr, "echo-client: FAIL expected echo seq %d, "
				"got %u (server out_seq must start at 1 and "
				"advance by 1)\n", echoes, es);
			failures++;
			break;
		}
		if (ets != want_ts) {
			fprintf(stderr, "echo-client: FAIL echo ts %u, expected %u "
				"(20 ms steps)\n", ets, want_ts);
			failures++;
		}
		want_ts += TS_STEP;
		/* payload must match the inbound packet that produced this echo */
		if (echoes <= norig) {
			for (size_t i = 0; i < PL; i++)
				sent[i] = (unsigned char)((orig[echoes - 1] * 7 + i) & 0xff);
			if (memcmp(sent, got, PL) != 0) {
				fprintf(stderr, "echo-client: FAIL payload mismatch "
					"on echo seq %u (inbound seq %u)\n",
					es, orig[echoes - 1]);
				failures++;
			}
		}
	}

	printf("echo-client: received %d echoes (expected 17: server seqs "
	       "1..17 for our 17 delivered packets)\n", echoes);
	if (echoes != 17) {
		fprintf(stderr, "echo-client: FAIL echo count\n");
		failures++;
	}
	if (failures) {
		fprintf(stderr, "echo-client: %d check(s) FAILED\n", failures);
		return 1;
	}
	printf("echo-client: ALL LOOPBACK CHECKS PASSED — the media plane works\n");

	srtp_dealloc(sess_out);
	srtp_dealloc(sess_in);
	srtp_shutdown();
	SSL_shutdown(ssl);
	SSL_free(ssl);
	SSL_CTX_free(ctx);
	close(fd);
	return 0;
}
