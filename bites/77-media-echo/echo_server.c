/* bite 77 — echo_server.c: full-duplex media loopback. Receive the peer's
 * SRTP, unprotect, parse the RTP header, RE-HEADER it with OUR OWN
 * SSRC/seq/timestamps, protect, send back.
 *
 * COMPILE + RUN (inside WSL, from the repo root):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/77-media-echo/dtls_common.c \
 *       bites/77-media-echo/echo_server.c -o /tmp/b77s -lssl -lcrypto -lsrtp2
 *   bash bites/77-media-echo/gen-cert.sh      # once
 *   # terminal 1:
 *   /tmp/b77s [port]                          # default 55558
 *   # terminal 2 (browser test double):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/77-media-echo/dtls_common.c \
 *       bites/77-media-echo/echo_client.c -o /tmp/b77c -lssl -lcrypto -lsrtp2
 *   timeout 20 /tmp/b77c 127.0.0.1 55558
 *
 * WHY RE-HEADER INSTEAD OF RELAYING BLINDLY (the bite's core lesson)
 *   WebRTC requires DISTINCT SSRC per direction. If we echoed the browser's
 *   packet with the browser's SSRC intact, the browser would see its own
 *   SSRC arriving and get confused (RTCP/reporting breaks; some stacks drop
 *   it outright). Also OUR outbound seq/timestamps must advance on OUR
 *   clock: passive echo (one packet out per packet in) is what we do here;
 *   real servers RE-PACE from a jitter buffer (bite 59's 20 ms ticker) so
 *   output timing survives network jitter — noted where the code would
 *   change.
 *
 * ROBUSTNESS (the bite's "never crash" list): malformed RTP, wrong payload
 * type, duplicate sequence numbers, unknown SSRC — each drops + logs.
 * The echo_client sends a malformed packet and a duplicate ON PURPOSE.
 *
 * BREAK IT ON PURPOSE:
 *   /tmp/b77s 55558 --echo-own-ssrc-off
 *   echoes with the PEER's SSRC left in place — watch the client report the
 *   violation ("echo carried the peer's SSRC"). WebRTC SSRC discipline,
 *   learned audibly (well, visibly — there is no speaker on this loopback).
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
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define RTP_HDR_LEN     12
#define RTP_PT_PCMU     0
#define SERVER_SSRC     0xC0FFEE01u   /* ours; the peer's stays the peer's */
#define TS_STEP         160           /* 20 ms @ 8 kHz — bite 59's ticker    */

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
	if (srtp_create(&sess, &policy) != srtp_err_status_ok)
		return NULL;
	return sess;
}

/* --- minimal STUN Binding responder for ICE keepalives (bite 74 lite) --- */
static unsigned int crc32_stun(const unsigned char *d, size_t n)
{
	unsigned int c = 0xFFFFFFFFu;
	for (size_t i = 0; i < n; i++) {
		c ^= d[i];
		for (int b = 0; b < 8; b++)
			c = (c >> 1) ^ (0xEDB88320u & (unsigned)-(int)(c & 1));
	}
	return ~c;
}
static void put_u32(unsigned char *p, unsigned int v)
{ p[0]=(unsigned char)(v>>24); p[1]=(unsigned char)(v>>16); p[2]=(unsigned char)(v>>8); p[3]=(unsigned char)v; }

static size_t stun_keepalive_response(unsigned char *out, const unsigned char *req,
				      const struct sockaddr_in *from)
{
	/* Same recipe as bite 74 (HMAC-SHA1 with our ice-pwd); abbreviated:
	 * XOR-MAPPED-ADDRESS + MESSAGE-INTEGRITY + FINGERPRINT. */
	const char *pwd = "serverIcePwdDemo42";
	size_t off = 20;
	out[0] = 0x01; out[1] = 0x01;             /* Binding success */
	put_u32(out + 4, 0x2112A442u);
	memcpy(out + 8, req + 8, 12);             /* echo txid */
	/* XOR-MAPPED-ADDRESS */
	unsigned char xm[8] = { 0, 0x01, 0, 0, 0, 0, 0, 0 };
	unsigned short port = (unsigned short)ntohs(from->sin_port);
	unsigned int ip = ntohl(from->sin_addr.s_addr);
	unsigned short xport = (unsigned short)(port ^ (0x2112A442u >> 16));
	xm[2] = (unsigned char)(xport >> 8);
	xm[3] = (unsigned char)(xport & 0xff);
	put_u32(xm + 4, ip ^ 0x2112A442u);
	out[off++]=0x00; out[off++]=0x20; out[off++]=0; out[off++]=8;
	memcpy(out+off, xm, 8); off += 8;
	/* MESSAGE-INTEGRITY (RFC 5389 s15.4: input ends BEFORE the MI attr) */
	size_t mi_off = off;
	off += 24;
	unsigned char tmp[512];
	memcpy(tmp, out, mi_off);
	size_t plen = mi_off + 24 - 20;
	tmp[2] = (unsigned char)(plen >> 8);
	tmp[3] = (unsigned char)(plen & 0xff);
	unsigned int maclen = 20;
	unsigned char mac[20];
	HMAC(EVP_sha1(), pwd, (int)strlen(pwd), tmp, mi_off, mac, &maclen);
	out[off++] = 0x00; out[off++] = 0x08; out[off++] = 0; out[off++] = 20;
	memcpy(out + off, mac, 20); off += 20;
	/* FINGERPRINT with FINAL length field already patched */
	out[2] = (unsigned char)(((off + 8 - 20) >> 8) & 0xff);
	out[3] = (unsigned char)((off + 8 - 20) & 0xff);
	unsigned char fp[4];
	put_u32(fp, crc32_stun(out, off) ^ 0x5354554Eu);
	out[off++]=0x80; out[off++]=0x28; out[off++]=0; out[off++]=4;
	memcpy(out+off, fp, 4); off += 4;
	return off;
}

int main(int argc, char **argv)
{
	int port = 55558;
	int keep_peer_ssrc = 0;   /* the break-it flag */
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--port") && i + 1 < argc)
			port = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--echo-own-ssrc-off"))
			keep_peer_ssrc = 1;
		else if (argv[i][0] != '-')
			port = atoi(argv[i]);   /* positional, like bites 75/76 */
	}
	setvbuf(stdout, NULL, _IOLBF, 0);

	if (srtp_init() != srtp_err_status_ok)
		return 1;
	SSL_CTX *sctx = dtls_ctx_create("bites/77-media-echo/server.crt",
					"bites/77-media-echo/server.key", 1);
	if (!sctx)
		return 1;

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
	printf("echo-server: UDP %d, waiting for the DTLS client...\n", port);

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
		fprintf(stderr, "echo-server: handshake failed\n");
		return 1;
	}
	printf("echo-server: DTLS ready; deriving SRTP keys\n");

	unsigned char keys[60];
	if (SSL_export_keying_material(ssl, keys, 60, "EXTRACTOR-dtls_srtp", 19,
				       USE_SRTP_CTX, 2, 1) != 1) {
		ERR_print_errors_fp(stderr);
		return 1;
	}
	srtp_t sess_out = srtp_session_make(keys + 16, ssrc_any_outbound);
	srtp_t sess_in  = srtp_session_make(keys, ssrc_any_inbound);
	if (!sess_out || !sess_in)
		return 1;

	/* Echo state: our OWN outbound counters (bite 59's clock), the last
	 * inbound sequence (bite 61's dedup), and the peer SSRC registry. */
	unsigned short out_seq = 1;
	unsigned int out_ts = 0;
	unsigned short last_in_seq = 0;
	unsigned int peer_ssrc = 0;
	int have_peer = 0;
	unsigned long echoed = 0, dropped = 0, stun_keepalives = 0, dtls_after = 0;

	unsigned char rx[2048], tx[2048];
	/* Run until the client goes quiet — a short bounded demo. */
	for (int i = 0; i < 200; i++) {
		struct timeval tv = { 1, 0 };
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		struct sockaddr_in from;
		socklen_t fl = sizeof(from);
		ssize_t r = recvfrom(fd, rx, sizeof(rx), 0,
				     (struct sockaddr *)&from, &fl);
		if (r <= 0)
			continue;
		size_t len = (size_t)r;

		/* ---- the media-port demux (bite 74/75 discipline) ---- */
		if ((rx[0] & 0xC0) == 0 && (rx[4] << 24 | rx[5] << 16 |
					    rx[6] << 8 | rx[7]) == 0x2112A442u) {
			size_t rl = stun_keepalive_response(tx, rx, &from);
			sendto(fd, tx, rl, 0, (struct sockaddr *)&from, fl);
			stun_keepalives++;
			continue;
		}
		if (rx[0] >= 0x14 && rx[0] <= 0x17) {
			/* DTLS record after the handshake (usually close_notify
			 * or a rekey). In bite 80 these bytes are fed to the
			 * peer's SSL object; in this single-peer demo we count
			 * and ignore them — the session is already up. */
			dtls_after++;
			continue;
		}
		if ((rx[0] & 0xC0) != 0x80) {          /* not RTP v2 at all */
			printf("echo-server: DROP non-RTP garbage (first byte 0x%02x)\n", rx[0]);
			dropped++;
			continue;
		}

		/* ---- inbound: SRTP -> RTP ---- */
		int ilen = (int)len;    /* libsrtp2's API predates size_t */
		if (srtp_unprotect(sess_in, rx, &ilen) != srtp_err_status_ok) {
			printf("echo-server: DROP unprotect failure (wrong key? "
			       "corrupt packet?)\n");
			dropped++;
			continue;
		}
		size_t plen = (size_t)ilen;

		/* ---- RTP header parse (bite 42's serialization, final form) ----
		 *  0                   1                   2                   3
		 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
		 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
		 * |V=2|P|X|  CC   |M|     PT      |       sequence number         |
		 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
		 * |                           timestamp                           |
		 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
		 * |           synchronization source (SSRC) identifier            |
		 * +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
		 */
		unsigned version = (unsigned)(rx[0] >> 6);
		unsigned padding = (unsigned)((rx[0] >> 5) & 1);
		unsigned cc      = (unsigned)(rx[0] & 0x0F);
		unsigned ext     = (unsigned)((rx[0] >> 4) & 1);
		unsigned marker  = (unsigned)((rx[1] >> 7) & 1);
		unsigned pt      = (unsigned)(rx[1] & 0x7F);
		unsigned short seq = (unsigned short)((rx[2] << 8) | rx[3]);
		unsigned int ts = ((unsigned)rx[4] << 24 | (unsigned)rx[5] << 16 |
				   (unsigned)rx[6] << 8 | rx[7]);
		unsigned int ssrc = ((unsigned)rx[8] << 24 | (unsigned)rx[9] << 16 |
				     (unsigned)rx[10] << 8 | rx[11]);

		if (version != 2) {
			printf("echo-server: DROP malformed RTP (version=%u)\n", version);
			dropped++;
			continue;
		}
		if (pt != RTP_PT_PCMU) {
			printf("echo-server: DROP unexpected payload type %u\n", pt);
			dropped++;
			continue;
		}
		if (plen < RTP_HDR_LEN + (size_t)cc * 4) {
			printf("echo-server: DROP truncated RTP (len=%zu)\n", plen);
			dropped++;
			continue;
		}
		if (!have_peer || ssrc != peer_ssrc) {
			/* unknown/changed SSRC: log it, accept it. A real server
			 * enforces the SDP-declared SSRCs; a relay logs + learns. */
			printf("echo-server: %sSSRC 0x%08x (seq=%u ts=%u M=%u ext=%u pad=%u)\n",
			       have_peer ? "SSRC CHANGE -> new " : "first packet from ",
			       ssrc, seq, ts, marker, ext, padding);
			peer_ssrc = ssrc;
			have_peer = 1;
		}
		if (seq == last_in_seq) {
			printf("echo-server: DROP duplicate seq=%u (replay?)\n", seq);
			dropped++;
			continue;
		}
		/* (A full receiver would also reorder within a window and track
		 * loss — bite 61/68 built exactly that engine; revisit it.) */
		last_in_seq = seq;

		/* ---- outbound: RE-HEADER + protect + send (the ECHO) ----
		 * Passive pacing: one out per in. Real servers re-pace from a
		 * jitter buffer on their own 20 ms ticker — that change lives
		 * exactly here (a timer + a packet queue instead of this
		 * immediate bounce). */
		size_t payload_len = plen - RTP_HDR_LEN;
		tx[0] = 0x80;
		tx[1] = (unsigned char)RTP_PT_PCMU;
		tx[2] = (unsigned char)(out_seq >> 8);
		tx[3] = (unsigned char)(out_seq & 0xff);
		tx[4] = (unsigned char)(out_ts >> 24);
		tx[5] = (unsigned char)(out_ts >> 16);
		tx[6] = (unsigned char)(out_ts >> 8);
		tx[7] = (unsigned char)out_ts;
		unsigned int out_ssrc = keep_peer_ssrc ? ssrc : SERVER_SSRC;
		tx[8]  = (unsigned char)(out_ssrc >> 24);
		tx[9]  = (unsigned char)(out_ssrc >> 16);
		tx[10] = (unsigned char)(out_ssrc >> 8);
		tx[11] = (unsigned char)out_ssrc;
		memcpy(tx + RTP_HDR_LEN, rx + RTP_HDR_LEN, payload_len);

		size_t tlen = RTP_HDR_LEN + payload_len;
		int tlen_i = (int)tlen;
		if (srtp_protect(sess_out, tx, &tlen_i) != srtp_err_status_ok) {
			printf("echo-server: protect failed\n");
			dropped++;
			continue;
		}
		sendto(fd, tx, (size_t)tlen_i, 0, (struct sockaddr *)&from, fl);
		printf("echo-server: echo seq %u -> %u (payload %zu bytes, "
		       "out_ts=%u, ssrc=0x%08x)\n",
		       seq, out_seq, payload_len, out_ts, out_ssrc);
		out_seq++;
		out_ts += TS_STEP;
		echoed++;
	}

	printf("echo-server: stats: %lu echoed, %lu dropped, %lu STUN keepalives, "
	       "%lu post-handshake DTLS records\n",
	       echoed, dropped, stun_keepalives, dtls_after);
	srtp_dealloc(sess_out);
	srtp_dealloc(sess_in);
	srtp_shutdown();
	SSL_shutdown(ssl);
	SSL_free(ssl);
	SSL_CTX_free(sctx);
	close(fd);
	return 0;
}
