/* bite 76 — srtp_server.c: derive SRTP keys from DTLS, init libsrtp2, and
 * push a REAL encrypted RTP packet to the peer.
 *
 * COMPILE + RUN (inside WSL, from the repo root):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/76-srtp/dtls_common.c \
 *       bites/76-srtp/srtp_server.c -o /tmp/b76s -lssl -lcrypto -lsrtp2
 *   bash bites/76-srtp/gen-cert.sh          # once
 *   # terminal 1:
 *   /tmp/b76s [port] [--swap-keys]          # default 55557
 *   # terminal 2 (the browser test double):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/76-srtp/dtls_common.c \
 *       bites/76-srtp/srtp_client.c -o /tmp/b76c -lssl -lcrypto -lsrtp2
 *   /tmp/b76c 127.0.0.1 55557
 *
 * THE KEY RECIPE (RFC 5764 s4.2 — follow it LITERALLY):
 *   SSL_export_keying_material(
 *       ssl, out, 60, "EXTRACTOR-dtls_srtp", 19, client_use_srtp_value, 2, 1)
 *   The 60 bytes are, IN ORDER:
 *     [ 0..16)  client_write_key   (16)  — client encrypts with this
 *     [16..32)  server_write_key   (16)  — server encrypts with this
 *     [32..46)  client_write_salt  (14)
 *     [46..60)  server_write_salt  (14)
 *   libsrtp wants key||salt (30 bytes) per session, so:
 *     server outbound session = server_write_key + server_write_salt
 *     server inbound session  = client_write_key + client_write_salt
 *   The exporter CONTEXT is the use_srtp extension value exactly as the
 *   CLIENT offered it: 2 bytes per profile, {0x00,0x01} for
 *   SRTP_AES128_CM_SHA1_80. (If the client had offered two profiles, the
 *   context would be 4 bytes — the context is the offered list, verbatim.)
 *
 * BREAK IT ON PURPOSE — the #1 SRTP setup bug:
 *   /tmp/b76s 55557 --swap-keys
 *   encrypts OUTBOUND with the CLIENT's key. The peer decrypts garbage /
 *   drops, everything else looks fine, no error anywhere — direction mixups
 *   are SILENT. Only external verification (our client's explicit check)
 *   catches it. Swap back and the packet arrives. Bite 76's exact scenario.
 */

#define _POSIX_C_SOURCE 200809L
#include "dtls_common.h"

#include <srtp2/srtp.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <openssl/err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define EXPORT_LEN   60   /* 16 + 16 + 14 + 14, see header comment */
#define RTP_PT_PCMU  0
#define RTP_HDR_LEN  12

/* use_srtp extension value the CLIENT offers (client.c sends the same). */
static const unsigned char USE_SRTP_CTX[] = { 0x00, 0x01 };

/* Derive + print the keys. Returns 0 ok. */
static int export_srtp_keys(SSL *ssl, unsigned char *out60)
{
	if (SSL_export_keying_material(ssl, out60, EXPORT_LEN,
				       "EXTRACTOR-dtls_srtp", 19,
				       USE_SRTP_CTX, sizeof(USE_SRTP_CTX), 1) != 1) {
		fprintf(stderr, "srtp: export_keying_material failed (use_srtp "
			"not negotiated?)\n");
		ERR_print_errors_fp(stderr);
		return -1;
	}
	printf("srtp: exported %d bytes:\n", EXPORT_LEN);
	printf("  client_write_key : ");
	for (int i = 0; i < 16; i++) printf("%02x", out60[i]);
	printf("\n  server_write_key : ");
	for (int i = 16; i < 32; i++) printf("%02x", out60[i]);
	printf("\n  client_salt      : ");
	for (int i = 32; i < 46; i++) printf("%02x", out60[i]);
	printf("\n  server_salt      : ");
	for (int i = 46; i < 60; i++) printf("%02x", out60[i]);
	printf("\n");
	return 0;
}

/* Build an srtp session around one 30-byte key||salt. */
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
	policy.key = (unsigned char *)key;   /* 16 key + 14 salt = 30 bytes */
	policy.window_size = 128;
	policy.next = NULL;
	srtp_t sess = NULL;
	if (srtp_create(&sess, &policy) != srtp_err_status_ok) {
		fprintf(stderr, "srtp: srtp_create failed\n");
		return NULL;
	}
	return sess;
}

/* RTP packet: v=2, PT=PCMU, 160-byte payload = 20 ms of 8 kHz silence. */
static size_t build_rtp(unsigned char *pkt, unsigned short seq, unsigned int ts,
			unsigned int ssrc)
{
	pkt[0] = 0x80;                       /* V=2, P=0, X=0, CC=0 */
	pkt[1] = (unsigned char)RTP_PT_PCMU; /* M=0, PT=0 (PCMU) */
	pkt[2] = (unsigned char)(seq >> 8);
	pkt[3] = (unsigned char)(seq & 0xff);
	pkt[4] = (unsigned char)(ts >> 24);
	pkt[5] = (unsigned char)(ts >> 16);
	pkt[6] = (unsigned char)(ts >> 8);
	pkt[7] = (unsigned char)ts;
	pkt[8]  = (unsigned char)(ssrc >> 24);
	pkt[9]  = (unsigned char)(ssrc >> 16);
	pkt[10] = (unsigned char)(ssrc >> 8);
	pkt[11] = (unsigned char)ssrc;
	memset(pkt + RTP_HDR_LEN, 0, 160);   /* 20 ms @ 8 kHz, 8-bit samples */
	return RTP_HDR_LEN + 160;
}

int main(int argc, char **argv)
{
	int port = 55557;
	int swap_keys = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--port") && i + 1 < argc)
			port = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--swap-keys"))
			swap_keys = 1;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);

	if (srtp_init() != srtp_err_status_ok) {
		fprintf(stderr, "srtp: srtp_init failed\n");
		return 1;
	}

	SSL_CTX *sctx = dtls_ctx_create("bites/76-srtp/server.crt",
					"bites/76-srtp/server.key", 1);
	if (!sctx)
		return 1;

	/* media port (pure DTLS here; bite 80 adds the STUN/DTLS/SRTP demux) */
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	a.sin_port = htons((unsigned short)port);
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
		perror("bind");
		return 1;
	}
	printf("srtp-server: UDP %d, waiting for the DTLS client...\n", port);

	/* cookie exchange + handshake — identical plumbing to bite 75 */
	SSL *ssl = SSL_new(sctx);
	BIO *bio = BIO_new_dgram(fd, BIO_NOCLOSE);
	SSL_set_bio(ssl, bio, bio);
	BIO_ADDR *cli = BIO_ADDR_new();
	while (DTLSv1_listen(ssl, cli) != 1) {
		if (SSL_get_error(ssl, -1) != SSL_ERROR_WANT_READ)
			break;
	}
	struct sockaddr_in cli_in;
	memset(&cli_in, 0, sizeof(cli_in));
	cli_in.sin_family = AF_INET;
	cli_in.sin_port = BIO_ADDR_rawport(cli);
	size_t iplen = 4;
	BIO_ADDR_rawaddress(cli, (unsigned char *)&cli_in.sin_addr.s_addr, &iplen);
	BIO_ADDR_free(cli);

	bio = BIO_new_dgram(fd, BIO_NOCLOSE);
	BIO_dgram_set_peer(bio, (struct sockaddr *)&cli_in);
	SSL_set_bio(ssl, bio, bio);
	if (!dtls_handshake_loop(ssl, 1)) {
		fprintf(stderr, "srtp-server: handshake failed\n");
		return 1;
	}
	/* (fingerprint check omitted here for brevity — bite 75 owns it;
	 * bite 80 re-wires it. This bite is about the KEYS.) */
	printf("srtp-server: DTLS handshake complete; deriving SRTP keys\n");

	unsigned char keys[EXPORT_LEN];
	if (export_srtp_keys(ssl, keys) != 0)
		return 1;

	/* Sessions: outbound protects with OUR (server) write key; inbound
	 * unprotected with the CLIENT's. --swap-keys deliberately breaks the
	 * outbound side to demo the silent failure. */
	const unsigned char *out_key = swap_keys ? keys : keys + 16;
	const unsigned char *in_key  = swap_keys ? keys + 16 : keys;
	if (swap_keys)
		printf("srtp-server: *** --swap-keys: encrypting with the CLIENT's key"
		       " (the silent direction bug)\n");

	srtp_t sess_out = srtp_session_make(out_key, ssrc_any_outbound);
	srtp_t sess_in  = srtp_session_make(in_key,  ssrc_any_inbound);
	if (!sess_out || !sess_in)
		return 1;

	/* ---- the first media packet: 20 ms of PCMU silence ---- */
	unsigned char pkt[512];
	size_t len = build_rtp(pkt, 1 /*seq*/, 0 /*ts*/, 0xDEADBEEFu);
	printf("srtp-server: RTP plaintext: seq=1 ts=0 ssrc=0xdeadbeef pt=0 len=%zu\n", len);
	int plen = (int)len;
	if (srtp_protect(sess_out, pkt, &plen) != srtp_err_status_ok) {
		fprintf(stderr, "srtp: protect failed\n");
		return 1;
	}
	len = (size_t)plen;
	/* The payload is now AES-CTR encrypted + HMAC-SHA1 authenticated
	 * (10-byte auth tag appended, per the SHA1_80 profile). */
	printf("srtp-server: SRTP on wire (%zu bytes): ", (size_t)len + 10);
	for (int i = 0; i < 16; i++) printf("%02x", pkt[i]);
	printf("...\n");
	sendto(fd, pkt, len, 0, (struct sockaddr *)&cli_in, sizeof(cli_in));
	printf("srtp-server: sent. Now waiting for the peer's packet to decrypt...\n");

	/* ---- receive + unprotect the client's RTP ---- */
	unsigned char rx[512];
	struct sockaddr_in from;
	socklen_t fl = sizeof(from);
	ssize_t r = recvfrom(fd, rx, sizeof(rx), 0, (struct sockaddr *)&from, &fl);
	if (r <= 0 || (rx[0] & 0xC0) != 0x80) {
		fprintf(stderr, "srtp-server: no/invalid RTP received\n");
		return 1;
	}
	size_t rlen = (size_t)r;
	int urx = (int)rlen;
	if (srtp_unprotect(sess_in, rx, &urx) != srtp_err_status_ok) {
		fprintf(stderr, "srtp-server: UNPROTECT FAILED — wrong key "
			"direction? (bite 76's lesson)\n");
		return 1;
	}
	rlen = (size_t)urx;
	unsigned short seq = (unsigned short)((rx[2] << 8) | rx[3]);
	unsigned int ts = ((unsigned)rx[4] << 24 | (unsigned)rx[5] << 16 |
			   (unsigned)rx[6] << 8 | rx[7]);
	unsigned int ssrc = ((unsigned)rx[8] << 24 | (unsigned)rx[9] << 16 |
			     (unsigned)rx[10] << 8 | rx[11]);
	printf("srtp-server: decrypted peer RTP: seq=%u ts=%u ssrc=0x%08x "
	       "payload[0]=0x%02x — SRTP round trip COMPLETE\n",
	       seq, ts, ssrc, rx[RTP_HDR_LEN]);

	/* cleanup: every resource exactly once (bite 79 preview) */
	srtp_dealloc(sess_out);
	srtp_dealloc(sess_in);
	srtp_shutdown();
	SSL_shutdown(ssl);
	SSL_free(ssl);
	SSL_CTX_free(sctx);
	close(fd);
	return 0;
}
