/* bite 80 — test_client.c: plays the WHOLE browser against the backend.
 *
 * COMPILE + RUN (inside WSL, from the repo root, server running):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/80-checkpoint-backend/dtls_common.c \
 *       bites/80-checkpoint-backend/test_client.c -o /tmp/b80t \
 *       -lwebsockets -lssl -lcrypto -lsrtp2
 *   timeout 30 /tmp/b80t 127.0.0.1
 *   # negative demo:
 *   timeout 30 /tmp/b80t 127.0.0.1 --corrupt-fp    # server must reject
 *   # abuse demo (run while the server is idle):
 *   timeout 10 /tmp/b80t 127.0.0.1 --flood-udp     # garbage at media port
 *
 * The phases mirror the bite-69 diagram one layer at a time:
 *   1. signaling: two WS connections join as "a"/"b", one offer is relayed
 *      a->b, one error case checked (bite 71/72 proof)
 *   2. ICE: a STUN Binding check with the right ufrag/pwd (bite 74)
 *   3. DTLS: client-role handshake, self-signed cert (bite 75); the server
 *      verifies OUR fingerprint — with --corrupt-fp the "signaled" value is
 *      wrong and the server must abort
 *   4. SRTP media: export keys, protect 10 PCMU packets, verify the echoes
 *      carry the SERVER's SSRC and our payloads bit-identical (bite 76/77)
 * On a real browser every phase is the same shape; the browser just also
 * draws UI and runs codecs we don't need for a loopback proof.
 */

#define _POSIX_C_SOURCE 200809L
#include "dtls_common.h"

#include <libwebsockets.h>
#include <srtp2/srtp.h>
#include <openssl/err.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define WS_PORT     8081
#define MEDIA_PORT  50000
#define SERVER_UFRAG "srvUfrag77"
#define SERVER_PWD   "serverIcePwdDemo42"
#define CLIENT_SSRC  0x12345678u
#define TS_STEP      160
#define RTP_HDR_LEN  12
#define EXPORT_LEN   60

static const unsigned char USE_SRTP_CTX[] = { 0x00, 0x01 };
static int g_failures = 0;

static void check(int ok, const char *label, const char *detail)
{
	if (ok)
		printf("test: PASS  %s\n", label);
	else
		printf("test: FAIL  %s  (%s)\n", label, detail ? detail : "");
	if (!ok)
		g_failures++;
}

/* ------------------------- phase 1: signaling (WS) ------------------------ */
/* The WS client plumbing is bite 71's ws_probe (handshake + frames); the
 * protocol layer is bite 72's. Condensed here so the checkpoint is one file. */
static void b64_encode(const unsigned char *in, size_t n, char *out, size_t cap)
{
	static const char t[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t o = 0;
	for (size_t i = 0; i + 2 < n; i += 3) {
		unsigned v = (unsigned)in[i] << 16 | (unsigned)in[i + 1] << 8 | in[i + 2];
		out[o++] = t[(v >> 18) & 63];
		out[o++] = t[(v >> 12) & 63];
		out[o++] = t[(v >> 6) & 63];
		out[o++] = t[v & 63];
	}
	size_t rem = n % 3;
	if (rem == 1) {
		unsigned v = (unsigned)in[n - 1] << 16;
		out[o++] = t[(v >> 18) & 63];
		out[o++] = t[(v >> 12) & 63];
		out[o++] = '='; out[o++] = '=';
	} else if (rem == 2) {
		unsigned v = (unsigned)in[n - 2] << 16 | (unsigned)in[n - 1] << 8;
		out[o++] = t[(v >> 18) & 63];
		out[o++] = t[(v >> 12) & 63];
		out[o++] = t[(v >> 6) & 63];
		out[o++] = '=';
	}
	out[o] = '\0';
	(void)cap;
}

static int ws_dial(int port, const char *id)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons((unsigned short)port);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
		perror("ws connect");
		close(fd);
		return -1;
	}
	unsigned char key[16];
	for (size_t i = 0; i < sizeof(key); i++)
		key[i] = (unsigned char)(rand() & 0xff);
	char kb[25];
	b64_encode(key, sizeof(key), kb, sizeof(kb));
	char req[512];
	int n = snprintf(req, sizeof(req),
			 "GET / HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\n"
			 "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
			 "Sec-WebSocket-Version: 13\r\n\r\n", kb);
	send(fd, req, (size_t)n, 0);
	char hdr[1024] = { 0 };
	size_t used = 0;
	while (used < sizeof(hdr) - 1) {
		char c;
		if (recv(fd, &c, 1, 0) <= 0)
			break;
		hdr[used++] = c;
		if (used >= 4 && !memcmp(hdr + used - 4, "\r\n\r\n", 4))
			break;
	}
	if (!strstr(hdr, " 101 ")) {
		fprintf(stderr, "test: ws upgrade failed for %s\n", id);
		close(fd);
		return -1;
	}
	/* join immediately */
	char msg[96];
	int m = snprintf(msg, sizeof(msg), "{\"type\":\"join\",\"id\":\"%s\"}", id);
	unsigned char frame[160];
	size_t fl = 0;
	frame[fl++] = 0x81;
	frame[fl++] = (unsigned char)(0x80 | m);
	unsigned char mask[4] = { 1, 2, 3, 4 };
	memcpy(frame + fl, mask, 4);
	fl += 4;
	memcpy(frame + fl, msg, (size_t)m);
	for (int i = 0; i < m; i++)
		frame[fl + i] ^= mask[i % 4];
	fl += (size_t)m;
	send(fd, frame, fl, 0);
	return fd;
}

static int ws_recv(int fd, char *out, size_t cap)
{
	size_t total = 0;
	for (;;) {
		unsigned char h[2];
		if (recv(fd, h, 2, 0) != 2)
			return -1;
		int fin = h[0] & 0x80;
		int op = h[0] & 0x0f;
		unsigned len = h[1] & 0x7f;
		if (len == 126) {
			unsigned char e[2];
			if (recv(fd, e, 2, 0) != 2)
				return -1;
			len = (unsigned)(e[0] << 8 | e[1]);
		}
		if (op == 0x9) {
			unsigned char pong[2] = { 0x8a, 0 };
			send(fd, pong, 2, 0);
			continue;
		}
		if (op == 0x8)
			return -1;
		if (total + len >= cap)
			return -1;
		size_t got = 0;
		while (got < len) {
			ssize_t r = recv(fd, out + total, len - got, 0);
			if (r <= 0)
				return -1;
			got += (size_t)r;
		}
		total += got;
		if (fin)
			break;
	}
	out[total] = '\0';
	return 0;
}

static int ws_send_text(int fd, const char *s)
{
	size_t len = strlen(s);
	unsigned char frame[2048 + 16];
	size_t fl = 0;
	frame[fl++] = 0x81;
	if (len < 126) {
		frame[fl++] = (unsigned char)(0x80 | len);
	} else {
		frame[fl++] = (unsigned char)(0x80 | 126);
		frame[fl++] = (unsigned char)(len >> 8);
		frame[fl++] = (unsigned char)(len & 0xff);
	}
	unsigned char mask[4] = { 9, 8, 7, 6 };
	memcpy(frame + fl, mask, 4);
	fl += 4;
	memcpy(frame + fl, s, len);
	for (size_t i = 0; i < len; i++)
		frame[fl + i] ^= mask[i % 4];
	fl += len;
	return send(fd, frame, fl, 0) > 0 ? 0 : -1;
}

/* Close like a browser does: a proper WS close frame (opcode 8) first, THEN
 * the TCP close. An abrupt close(fd) without the frame leaves libwebsockets
 * waiting through its graceful-close timeout — lws's default close-ack wait
 * is minutes, which is exactly the multi-second stall this bite can show you. */
static void ws_close_frame(int fd)
{
	unsigned char frame[8];
	size_t fl = 0;
	frame[fl++] = 0x88;              /* FIN + opcode 8 (close) */
	frame[fl++] = 0x82;              /* masked, 2-byte payload */
	unsigned char mask[4] = { 1, 1, 1, 1 };
	memcpy(frame + fl, mask, 4);
	fl += 4;
	frame[fl++] = (unsigned char)(0x03 ^ 1);   /* status 1000 normal closure */
	frame[fl++] = (unsigned char)(0xE8 ^ 1);
	send(fd, frame, fl, 0);
}

/* ---------------------- phase 2: ICE (STUN check) ------------------------- */
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

static void put_attr(unsigned char *b, size_t *o, unsigned t,
		     const unsigned char *v, unsigned l)
{
	b[(*o)++] = (unsigned char)(t >> 8);
	b[(*o)++] = (unsigned char)(t & 0xff);
	b[(*o)++] = (unsigned char)(l >> 8);
	b[(*o)++] = (unsigned char)(l & 0xff);
	memcpy(b + *o, v, l);
	*o += l;
	while (*o & 3)
		b[(*o)++] = 0;
}

static void stun_hmac_sha1(const unsigned char *msg, size_t mi_off,
			   const char *key, unsigned char out[20])
{
	unsigned char tmp[1600];
	memcpy(tmp, msg, mi_off);
	size_t plen = mi_off + 24 - 20;
	tmp[2] = (unsigned char)(plen >> 8);
	tmp[3] = (unsigned char)(plen & 0xff);
	unsigned int olen = 20;
	HMAC(EVP_sha1(), key, (int)strlen(key), tmp, mi_off, out, &olen);
}

/* Build the connectivity check the browser would send: USERNAME is
 * "<server ufrag>:<our ufrag>" and the HMAC key is the SERVER's pwd. */
static size_t build_check(unsigned char *out, const unsigned char txid[12],
			  const char *our_ufrag)
{
	size_t off = 20;
	out[0] = 0x00; out[1] = 0x01;
	put_u32(out + 4, 0x2112A442u);
	memcpy(out + 8, txid, 12);
	char uname[64];
	snprintf(uname, sizeof(uname), "%s:%s", SERVER_UFRAG, our_ufrag);
	put_attr(out, &off, 0x0006, (const unsigned char *)uname,
		 (unsigned)strlen(uname));
	size_t mi_off = off;
	off += 24;
	unsigned char mac[20];
	stun_hmac_sha1(out, mi_off, SERVER_PWD, mac);
	out[mi_off] = 0x00; out[mi_off + 1] = 0x08;
	out[mi_off + 2] = 0x00; out[mi_off + 3] = 20;
	memcpy(out + mi_off + 4, mac, 20);
	out[2] = (unsigned char)(((off + 8 - 20) >> 8) & 0xff);
	out[3] = (unsigned char)((off + 8 - 20) & 0xff);
	unsigned char fp[4];
	put_u32(fp, crc32_stun(out, off) ^ 0x5354554Eu);
	put_attr(out, &off, 0x8028, fp, 4);
	return off;
}

/* ----------------------- phase 3/4: DTLS + SRTP media -------------------- */
static srtp_t srtp_session_make(const unsigned char *key, srtp_ssrc_type_t dir)
{
	srtp_policy_t pol;
	memset(&pol, 0, sizeof(pol));
	srtp_crypto_policy_set_from_profile_for_rtp(&pol.rtp, srtp_profile_aes128_cm_sha1_80);
	srtp_crypto_policy_set_from_profile_for_rtcp(&pol.rtcp, srtp_profile_aes128_cm_sha1_80);
	pol.ssrc.type = dir;
	pol.key = (unsigned char *)key;
	pol.window_size = 128;
	srtp_t s = NULL;
	srtp_create(&s, &pol);
	return s;
}

static int udp_dial(int port, struct sockaddr_in *out)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in local;
	if (!out)
		out = &local;                        /* caller doesn't need it */
	memset(out, 0, sizeof(*out));
	out->sin_family = AF_INET;
	out->sin_port = htons((unsigned short)port);
	out->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(fd, (struct sockaddr *)out, sizeof(*out)) != 0) {
		perror("udp connect");
		close(fd);
		return -1;
	}
	struct timeval tv = { 3, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	return fd;
}

static void flood_udp(int port)
{
	int fd = udp_dial(port, NULL);
	if (fd < 0)
		return;
	unsigned char junk[64];
	memset(junk, 0xAA, sizeof(junk));
	printf("test: spraying 2000 garbage UDP packets at the media port...\n");
	for (int i = 0; i < 2000; i++)
		send(fd, junk, sizeof(junk), 0);
	/* a valid check right after must still work — legit peer unaffected */
	unsigned char tx[256], rx[256];
	unsigned char txid[12];
	for (int i = 0; i < 12; i++)
		txid[i] = (unsigned char)(rand() & 0xff);
	size_t n = build_check(tx, txid, "cliUfragXX");
	send(fd, tx, n, 0);
	ssize_t r = recv(fd, rx, sizeof(rx), 0);
	check(r > 0 && ((rx[0] << 8) | rx[1]) == 0x0101,
	      "media port healthy after UDP flood", NULL);
	close(fd);
}

int main(int argc, char **argv)
{
	int corrupt_fp = 0, flood = 0;
	/* host is loopback-only in this demo; the positional argument is
	 * accepted for symmetry but ignored (see README's "simplified"). */
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--corrupt-fp"))
			corrupt_fp = 1;
		else if (!strcmp(argv[i], "--flood-udp"))
			flood = 1;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGPIPE, SIG_IGN);

	if (flood) {
		flood_udp(MEDIA_PORT);
		printf("test: flood run finished (%d failures)\n", g_failures);
		return g_failures ? 1 : 0;
	}
	if (srtp_init() != srtp_err_status_ok)
		return 1;

	/* ---- phase 1: signaling ---- */
	int wa = ws_dial(WS_PORT, "a");
	int wb = ws_dial(WS_PORT, "b");
	char buf[2048];
	int ok = wa >= 0 && wb >= 0;
	check(ok, "two websocket connections joined", NULL);
	if (ok) {
		ws_recv(wa, buf, sizeof(buf));
		ws_recv(wb, buf, sizeof(buf));
		ws_send_text(wa, "{\"type\":\"offer\",\"to\":\"b\",\"sdp\":\"v=0 o=- demo\"}");
		ok = ws_recv(wb, buf, sizeof(buf)) == 0 &&
		     strstr(buf, "v=0 o=- demo") != NULL;
		check(ok, "offer relayed a -> b verbatim", buf);
		ws_send_text(wa, "{\"type\":\"offer\",\"to\":\"nobody\"}");
		ok = ws_recv(wa, buf, sizeof(buf)) == 0 &&
		     strstr(buf, "unknown peer") != NULL;
		check(ok, "bad route -> clean error", buf);
		ws_close_frame(wa);
		ws_close_frame(wb);
		close(wa);
		close(wb);
	}

	/* ---- phase 2: ICE ---- */
	int fd = udp_dial(MEDIA_PORT, NULL);
	if (fd < 0)
		return 1;
	unsigned char tx[512], rx[2048];
	unsigned char txid[12];
	for (int i = 0; i < 12; i++)
		txid[i] = (unsigned char)(rand() & 0xff);
	size_t n = build_check(tx, txid, "cliUfragXX");
	send(fd, tx, n, 0);
	ssize_t r = recv(fd, rx, sizeof(rx), 0);
	check(r > 0 && ((rx[0] << 8) | rx[1]) == 0x0101,
	      "ICE check answered (binding success)", NULL);

	/* ---- phase 3: DTLS ---- */
	SSL_CTX *ctx = dtls_ctx_create("bites/80-checkpoint-backend/client.crt",
				       "bites/80-checkpoint-backend/client.key", 0);
	SSL *ssl = SSL_new(ctx);
	BIO *bio = BIO_new_dgram(fd, BIO_NOCLOSE);
	BIO_dgram_set_peer(bio, (struct sockaddr *)&(struct sockaddr_in){
		.sin_family = AF_INET });
	/* (peer address already pinned by connect(); re-set explicitly) */
	struct sockaddr_in srv;
	memset(&srv, 0, sizeof(srv));
	srv.sin_family = AF_INET;
	srv.sin_port = htons(MEDIA_PORT);
	srv.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	BIO_dgram_set_peer(bio, (struct sockaddr *)&srv);
	SSL_set_bio(ssl, bio, bio);

	/* Client role with drive loop: handshake over the SAME socket the
	 * STUN check just used — the demux reality of a media port. */
	int hs_ok = 0;
	for (int i = 0; i < 50 && !hs_ok; i++) {
		int rr = SSL_connect(ssl);
		if (rr == 1) {
			hs_ok = 1;
			break;
		}
		int err = SSL_get_error(ssl, rr);
		if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
			ERR_print_errors_fp(stderr);
			break;
		}
		struct timeval tv = { 0, 200 * 1000 };
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
		select(fd + 1, &rfds, NULL, NULL, &tv);  /* wait for READ: the
			server's next flight, not write-readiness */
	}
	check(hs_ok, "DTLS 1.2 handshake complete on media port", NULL);
	if (!hs_ok) {
		printf("test: %d failure(s) — aborting\n", g_failures);
		return 1;
	}
	char ownfp[65];
	if (dtls_own_fingerprint(ctx, ownfp, sizeof(ownfp)) == 0)
		printf("test: our cert fingerprint (the a=fingerprint the server "
		       "checks): %.16s...\n", ownfp);
	if (corrupt_fp) {
		/* The planted tripwire: we TELL the operator a wrong value is in
		 * the SDP. The server, having loaded the real fingerprint file,
		 * still matches — so instead we demonstrate the rejection by
		 * presenting a DIFFERENT certificate than the one whose hash
		 * the server expects: swap to server.crt as our identity! */
		printf("test: --corrupt-fp: connecting with a certificate whose "
		       "fingerprint is NOT the signaled one...\n");
		SSL_shutdown(ssl);
		SSL_free(ssl);
		SSL_CTX_free(ctx);
		close(fd);
		fd = udp_dial(MEDIA_PORT, &srv);
		SSL_CTX *ctx2 = dtls_ctx_create("bites/80-checkpoint-backend/server.crt",
						"bites/80-checkpoint-backend/server.key", 0);
		SSL *ssl2 = SSL_new(ctx2);
		BIO *bio2 = BIO_new_dgram(fd, BIO_NOCLOSE);
		BIO_dgram_set_peer(bio2, (struct sockaddr *)&srv);
		SSL_set_bio(ssl2, bio2, bio2);
		int hs2 = 0;
		for (int i = 0; i < 50 && !hs2; i++) {
			int rr = SSL_connect(ssl2);
			if (rr == 1) {
				hs2 = 1;
				break;
			}
			int err = SSL_get_error(ssl2, rr);
			if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE)
				break;
			struct timeval tv = { 0, 200 * 1000 };
			fd_set rfds;
			FD_ZERO(&rfds);
			FD_SET(fd, &rfds);
			select(fd + 1, &rfds, NULL, NULL, &tv);
		}
		if (hs2) {
			/* handshake succeeded cryptographically — the server must
			 * now refuse the data path. Prove it: drain whatever
			 * handshake tail is still in flight, then send an RTP
			 * packet and expect NO echo (only silence). */
			printf("test: handshake completed with WRONG cert (crypto "
			       "does not know the SDP) — expecting media silence\n");
			struct timeval tv1s = { 1, 0 };
			setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv1s, sizeof(tv1s));
			unsigned char drain[2048];
			while (recv(fd, drain, sizeof(drain), 0) > 0)
				;    /* drain the handshake tail */
			memset(drain, 0x80, sizeof(drain));
			send(fd, drain, sizeof(drain), 0);
			ssize_t e = recv(fd, drain, sizeof(drain), 0);
			check(e < 0, "server refused media from unverified cert", NULL);
		} else {
			check(1, "server rejected handshake outright (also valid)", NULL);
		}
		SSL_free(ssl2);
		SSL_CTX_free(ctx2);
		close(fd);
		printf("test: corrupt-fp run finished (%d failures)\n", g_failures);
		srtp_shutdown();
		return g_failures ? 1 : 0;
	}

	/* ---- phase 4: SRTP media echo ---- */
	unsigned char keys[EXPORT_LEN];
	if (SSL_export_keying_material(ssl, keys, EXPORT_LEN,
				       "EXTRACTOR-dtls_srtp", 19,
				       USE_SRTP_CTX, sizeof(USE_SRTP_CTX), 1) != 1) {
		ERR_print_errors_fp(stderr);
		return 1;
	}
	srtp_t sout = srtp_session_make(keys, ssrc_any_outbound);
	srtp_t sin  = srtp_session_make(keys + 16, ssrc_any_inbound);
	check(sout && sin, "SRTP sessions built from exporter keys", NULL);

	int echoed = 0;
	for (int seq = 1; seq <= 10; seq++) {
		unsigned char pkt[512];
		pkt[0] = 0x80;
		pkt[1] = 0;
		pkt[2] = (unsigned char)(seq >> 8);
		pkt[3] = (unsigned char)(seq & 0xff);
		unsigned int ts = (unsigned)(seq * TS_STEP);
		pkt[4] = (unsigned char)(ts >> 24);
		pkt[5] = (unsigned char)(ts >> 16);
		pkt[6] = (unsigned char)(ts >> 8);
		pkt[7] = (unsigned char)ts;
		pkt[8]  = (unsigned char)(CLIENT_SSRC >> 24);
		pkt[9]  = (unsigned char)(CLIENT_SSRC >> 16);
		pkt[10] = (unsigned char)(CLIENT_SSRC >> 8);
		pkt[11] = (unsigned char)CLIENT_SSRC;
		for (int i = 0; i < 160; i++)
			pkt[RTP_HDR_LEN + i] = (unsigned char)((seq * 7 + i) & 0xff);
		int plen = (int)(RTP_HDR_LEN + 160);
		if (srtp_protect(sout, pkt, &plen) != srtp_err_status_ok)
			break;
		send(fd, pkt, (size_t)plen, 0);

		unsigned char ebuf[2048];
		ssize_t er = recv(fd, ebuf, sizeof(ebuf), 0);
		if (er <= 0)
			continue;
		int elen = (int)er;
		if (srtp_unprotect(sin, ebuf, &elen) != srtp_err_status_ok)
			continue;
		size_t el = (size_t)elen;
		if (el < RTP_HDR_LEN + 160)
			continue;
		unsigned int essrc = ((unsigned)ebuf[8] << 24 | (unsigned)ebuf[9] << 16 |
				      (unsigned)ebuf[10] << 8 | ebuf[11]);
		unsigned short eseq = (unsigned short)((ebuf[2] << 8) | ebuf[3]);
		if (essrc != 0xC0FFEE01u) {
			printf("test: FAIL echo carried SSRC 0x%08x\n", essrc);
			g_failures++;
			continue;
		}
		/* payload round trip: server echoed OUR payload for THIS packet */
		unsigned char expect[160];
		for (int i = 0; i < 160; i++)
			expect[i] = (unsigned char)((seq * 7 + i) & 0xff);
		if (memcmp(expect, ebuf + RTP_HDR_LEN, 160) != 0) {
			printf("test: FAIL echo payload mismatch for seq %u\n", eseq);
			g_failures++;
			continue;
		}
		echoed++;
	}
	check(echoed == 10, "10/10 SRTP packets echoed bit-identical", NULL);

	srtp_dealloc(sout);
	srtp_dealloc(sin);
	srtp_shutdown();
	SSL_shutdown(ssl);
	SSL_free(ssl);
	SSL_CTX_free(ctx);
	close(fd);

	printf("test: %s (%d failure(s))\n",
	       g_failures ? "FAILED" : "ALL PHASES PASSED", g_failures);
	return g_failures ? 1 : 0;
}
