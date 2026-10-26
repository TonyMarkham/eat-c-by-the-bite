/* bite 80 — server.c: THE BACKEND. One process, four planes:
 *
 *   [signaling]   WebSocket on tcp/8081 — JSON join/offer/answer/candidate
 *                 relay with a peer registry (bites 71+72, condensed)
 *   [ICE]         UDP 50000 answers STUN Binding checks with short-term
 *                 credentials (bite 74) — an ICE-lite agent
 *   [security]    same UDP port demuxes DTLS 1.2 (bite 75): cookie exchange,
 *                 handshake, certificate fingerprint verified against the
 *                 SDP value (client-fingerprint.txt stands in for a=fingerprint:)
 *   [media]       SRTP (bite 76 keys from the DTLS exporter) audio echo —
 *                 receive PCMU, re-header with OUR SSRC/seq/ts, return (bite 77)
 *
 * COMPILE + RUN (inside WSL, from the repo root):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/80-checkpoint-backend/dtls_common.c \
 *       bites/80-checkpoint-backend/server.c -o /tmp/b80 \
 *       -lwebsockets -lssl -lcrypto -lsrtp2
 *   bash bites/80-checkpoint-backend/gen-cert.sh          # once
 *   /tmp/b80                                              # default ports
 *   # full loopback without a browser:
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/80-checkpoint-backend/test_client.c \
 *       -o /tmp/b80t -lwebsockets -lssl -lcrypto -lsrtp2
 *   timeout 30 /tmp/b80t 127.0.0.1
 *
 * EVENT LOOP SHAPE (the bite-51 lesson, finished — with one honest twist):
 *   libwebsockets wants to own its poll loop, and a hand-rolled foreign loop
 *   around it proved stall-prone (lws_service() blocked for tens of seconds
 *   once WS connections churned — see git history of this bite). So the
 *   signaling plane runs its classic `lws_service(ctx, 200)` loop in a
 *   dedicated THREAD (bite 50), while the media plane (poll over UDP +
 *   signalfd, DTLS timers, SRTP echo) runs the bite-51 pattern on the main
 *   thread. There is deliberately NO shared mutable state: the peer registry
 *   is touched only by the lws thread; stats counters are per-plane and
 *   merged at shutdown — no locks, no races, easy to reason about.
 *
 * BREAK IT ON PURPOSE (the final gauntlet, all reproducible):
 *   - send garbage UDP at 50000 (test_client --flood-udp): demux drops it,
 *     the DTLS session (if any) is untouched, memory flat.
 *   - start the server, then run test_client --corrupt-fp: the handshake
 *     completes and the server REJECTS on fingerprint mismatch (bite 75's
 *     tripwire, now wired to a "signaled" value).
 *   - kill the test client mid-handshake: the session is torn down on idle
 *     timeout, stats stay correct, fds stable.
 *   - SIGTERM: no accepting, peers closed, final stats, exit 0. Valgrind:
 *     valgrind --leak-check=full --error-exitcode=9 /tmp/b80 &  (then churn).
 */

#define _POSIX_C_SOURCE 200809L
#include "dtls_common.h"

#include <libwebsockets.h>
#include <cjson/cJSON.h>
#include <srtp2/srtp.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ----------------------------- configuration ------------------------------ */
#define WS_PORT        8081
#define MEDIA_PORT     50000
#define SERVER_UFRAG   "srvUfrag77"
#define SERVER_PWD     "serverIcePwdDemo42"
#define SERVER_SSRC    0xC0FFEE01u
#define TS_STEP        160
#define IDLE_TIMEOUT_S 30
#define MAX_PEERS      8
#define MAX_MSG        (16 * 1024)
#define RX_BUF_SIZE    (4 * 1024)
#define EXPORT_LEN     60
#define RTP_HDR_LEN    12

static const unsigned char USE_SRTP_CTX[] = { 0x00, 0x01 };

/* ------------------------------- tiny logging ----------------------------- */
static long long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static long long g_start_ms;
static unsigned long g_ws_msgs_relayed, g_ice_checks_ok, g_ice_rejected;
static unsigned long g_rtp_echoed, g_rtp_dropped, g_udp_dropped;

static void logline(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	char ts[32];
	time_t t = time(NULL);
	strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&t));
	printf("[%s] ", ts);
	vprintf(fmt, ap);
	printf("\n");
	fflush(stdout);
	va_end(ap);
}

/* ================= 1. SIGNALING PLANE (bites 71 + 72) ===================== */
typedef struct {
	char   acc[MAX_MSG + 1];
	size_t len;
	int    overflow;
} sess_t;

static struct {
	struct lws *wsi;
	char id[64];
	int  active;
} g_peers[MAX_PEERS];

static int peer_by_wsi(struct lws *wsi)
{
	for (int i = 0; i < MAX_PEERS; i++)
		if (g_peers[i].active && g_peers[i].wsi == wsi)
			return i;
	return -1;
}
static int peer_by_id(const char *id)
{
	for (int i = 0; i < MAX_PEERS; i++)
		if (g_peers[i].active && strcmp(g_peers[i].id, id) == 0)
			return i;
	return -1;
}

static void ws_send(struct lws *to, const char *json)
{
	static unsigned char buf[MAX_MSG + LWS_PRE];
	size_t len = strlen(json);
	if (len > MAX_MSG)
		len = MAX_MSG;
	memcpy(buf + LWS_PRE, json, len);
	lws_write(to, buf + LWS_PRE, len, LWS_WRITE_TEXT);
}

static void ws_error(struct lws *to, const char *reason)
{
	char buf[160];
	snprintf(buf, sizeof(buf), "{\"type\":\"error\",\"reason\":\"%s\"}", reason);
	ws_send(to, buf);
}

/* Route offer/answer/candidate verbatim between registered peers.
 * (Identical protocol to bite 72; see that file for the full spec block.) */
static void handle_signal(struct lws *wsi, const char *raw, size_t rawlen)
{
	int from = peer_by_wsi(wsi);
	cJSON *msg = cJSON_ParseWithLength(raw, rawlen);
	if (!msg) {
		ws_error(wsi, "malformed json");
		return;
	}
	cJSON *type = cJSON_GetObjectItemCaseSensitive(msg, "type");
	if (!cJSON_IsString(type) || !type->valuestring[0]) {
		ws_error(wsi, "missing 'type'");
		cJSON_Delete(msg);
		return;
	}
	if (!strcmp(type->valuestring, "join")) {
		cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
		if (!cJSON_IsString(id) || !id->valuestring[0]) {
			ws_error(wsi, "join needs 'id'");
		} else if (peer_by_id(id->valuestring) >= 0) {
			ws_error(wsi, "id already taken");
		} else if (from < 0) {
			for (int i = 0; i < MAX_PEERS; i++) {
				if (!g_peers[i].active) {
					snprintf(g_peers[i].id, sizeof(g_peers[i].id),
						 "%.63s", id->valuestring);
					g_peers[i].wsi = wsi;
					g_peers[i].active = 1;
					logline("signal: peer '%s' joined", g_peers[i].id);
					char ok[96];
					snprintf(ok, sizeof(ok),
						 "{\"type\":\"joined\",\"id\":\"%.63s\"}",
						 g_peers[i].id);
					ws_send(wsi, ok);
					break;
				}
			}
		} else {
			ws_error(wsi, "already joined");
		}
	} else if (!strcmp(type->valuestring, "offer") ||
		   !strcmp(type->valuestring, "answer") ||
		   !strcmp(type->valuestring, "candidate")) {
		cJSON *to = cJSON_GetObjectItemCaseSensitive(msg, "to");
		int dst = (cJSON_IsString(to) && to->valuestring[0]) ?
			  peer_by_id(to->valuestring) : -1;
		if (from < 0)
			ws_error(wsi, "join first");
		else if (dst < 0)
			ws_error(wsi, "unknown peer");
		else {
			logline("signal: relaying '%s' %s -> %s",
				type->valuestring, g_peers[from].id, g_peers[dst].id);
			/* relay verbatim (see bite 72 for why we don't re-serialize) */
			static unsigned char buf[MAX_MSG + LWS_PRE];
			size_t n = rawlen < MAX_MSG ? rawlen : MAX_MSG;
			memcpy(buf + LWS_PRE, raw, n);
			lws_write(g_peers[dst].wsi, buf + LWS_PRE, n, LWS_WRITE_TEXT);
			g_ws_msgs_relayed++;
		}
	} else {
		ws_error(wsi, "unknown type");
	}
	cJSON_Delete(msg);
}

static int callback_signal(struct lws *wsi, enum lws_callback_reasons reason,
			   void *user, void *in, size_t len)
{
	sess_t *s = (sess_t *)user;
	switch (reason) {
	case LWS_CALLBACK_RECEIVE: {
		size_t avail = (size_t)(MAX_MSG - s->len);
		if (s->overflow || len > avail) {
			if (!s->overflow)
				ws_error(wsi, "message too large");
			s->overflow = 1;
		} else {
			memcpy(s->acc + s->len, in, len);
			s->len += len;
		}
		if (lws_is_final_fragment(wsi)) {
			s->acc[s->len] = '\0';
			if (!s->overflow)
				handle_signal(wsi, s->acc, s->len);
			s->len = 0;
			s->overflow = 0;
		}
		break;
	}
	case LWS_CALLBACK_CLOSED: {
		int i = peer_by_wsi(wsi);
		if (i >= 0) {
			logline("signal: peer '%s' left", g_peers[i].id);
			g_peers[i].active = 0;
			g_peers[i].wsi = NULL;
		}
		break;
	}
	default:
		break;
	}
	return 0;
}

static const struct lws_protocols protocols[] = {
	{ "signal", callback_signal, sizeof(sess_t), RX_BUF_SIZE, 0, NULL, 0 },
	LWS_PROTOCOL_LIST_TERM
};

/* The signaling plane runs on its own thread (bite 50): libwebsockets owns
 * its poll loop, and wrapping it in a foreign loop stalled the media plane
 * for tens of seconds (documented in the file header). Shared state design:
 * the peer registry below is touched ONLY from this thread; the media plane
 * never reads it. Counters are per-plane and merged at shutdown. */
static volatile sig_atomic_t g_running = 1;

struct ws_thread_args {
	struct lws_context *ctx;
	volatile sig_atomic_t *running;
};

static void *ws_service_thread(void *arg)
{
	struct ws_thread_args *a = (struct ws_thread_args *)arg;
	while (*a->running)
		lws_service(a->ctx, 200);   /* bite 71's proven loop shape */
	return NULL;
}

/* ================== 2-4. MEDIA PLANE (bites 74 + 75 + 76/77) ============== */
typedef struct {
	int              in_use;
	struct sockaddr_in peer;
	SSL             *ssl;          /* DTLS session (bite 75)            */
	int              dtls_up;
	int              fingerprint_ok;
	unsigned char    keys[EXPORT_LEN];
	srtp_t           sess_out;     /* SRTP sessions (bite 76)           */
	srtp_t           sess_in;
	/* echo state (bite 77) */
	unsigned short   out_seq;
	unsigned int     out_ts;
	unsigned short   last_in_seq;
	long long        last_seen_ms;
} media_t;

static media_t g_media;
static SSL_CTX *g_sctx;

static void media_reset(const char *why)
{
	if (!g_media.in_use)
		return;
	logline("media: session torn down (%s)", why);
	if (g_media.sess_out) {
		srtp_dealloc(g_media.sess_out);
		g_media.sess_out = NULL;
	}
	if (g_media.sess_in) {
		srtp_dealloc(g_media.sess_in);
		g_media.sess_in = NULL;
	}
	if (g_media.ssl) {
		SSL_shutdown(g_media.ssl);
		SSL_free(g_media.ssl);
		g_media.ssl = NULL;
	}
	memset(&g_media, 0, sizeof(g_media));
}

/* --- STUN machinery: verbatim from bite 74 (read that file for the RFC
 * citations; the footgun commentary there is the exam) --- */
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

static int find_attr(const unsigned char *m, size_t len, unsigned want,
		     const unsigned char **val, unsigned *alen)
{
	if (len < 20)
		return 0;
	unsigned plen = (m[2] << 8) | m[3];
	size_t off = 20, end = 20 + plen;
	if (end > len)
		end = len;
	while (off + 4 <= end) {
		unsigned t = (m[off] << 8) | m[off + 1];
		unsigned l = (m[off + 2] << 8) | m[off + 3];
		if (off + 4 + l > end)
			return 0;
		if (t == want) {
			*val = m + off + 4;
			*alen = l;
			return 1;
		}
		off += 4 + ((l + 3) & ~3u);
	}
	return 0;
}

static void stun_hmac_sha1(const unsigned char *msg, size_t mi_attr_off,
			   const char *key, unsigned char out[20])
{
	unsigned char tmp[1600];
	memcpy(tmp, msg, mi_attr_off);
	size_t plen = mi_attr_off + 24 - 20;
	tmp[2] = (unsigned char)(plen >> 8);
	tmp[3] = (unsigned char)(plen & 0xff);
	unsigned int olen = 20;
	HMAC(EVP_sha1(), key, (int)strlen(key), tmp, mi_attr_off, out, &olen);
}

/* Full ICE-lite Binding response (bite 74's build_response, unchanged). */
static size_t build_stun_response(unsigned char *out, unsigned short resp_type,
				  const unsigned char txid[12],
				  const struct sockaddr_in *mapped,
				  unsigned short err_num)
{
	size_t off = 20;
	out[0] = (unsigned char)(resp_type >> 8);
	out[1] = (unsigned char)(resp_type & 0xff);
	put_u32(out + 4, 0x2112A442u);
	memcpy(out + 8, txid, 12);

	if (resp_type == 0x0111) {
		char *reason = (err_num == 401) ? (char *)"Unauthorized" : (char *)"Bad Request";
		size_t rl = strlen(reason);
		unsigned char ec[4 + 64];
		ec[0] = ec[1] = 0;
		ec[2] = (unsigned char)(err_num / 100);
		ec[3] = (unsigned char)(err_num % 100);
		memcpy(ec + 4, reason, rl);
		out[off++]=0x00; out[off++]=0x09; out[off++]=(unsigned char)((4+rl)>>8);
		out[off++]=(unsigned char)((4+rl)&0xff);
		memcpy(out+off, ec, 4+rl); off += 4+rl;
		while (off & 3) out[off++]=0;
	} else {
		unsigned char xm[8] = { 0, 0x01, 0, 0, 0, 0, 0, 0 };
		unsigned short port = (unsigned short)ntohs(mapped->sin_port);
		unsigned int ip = ntohl(mapped->sin_addr.s_addr);
		unsigned short xport = (unsigned short)(port ^ (0x2112A442u >> 16));
		xm[2] = (unsigned char)(xport >> 8);
		xm[3] = (unsigned char)(xport & 0xff);
		put_u32(xm + 4, ip ^ 0x2112A442u);
		out[off++]=0x00; out[off++]=0x20; out[off++]=0; out[off++]=8;
		memcpy(out+off, xm, 8); off += 8;
	}

	size_t mi_off = off;
	off += 24;
	unsigned char mac[20];
	stun_hmac_sha1(out, mi_off, SERVER_PWD, mac);
	out[mi_off]=0x00; out[mi_off+1]=0x08;
	out[mi_off+2]=0x00; out[mi_off+3]=20;
	memcpy(out + mi_off + 4, mac, 20);

	out[2] = (unsigned char)(((off + 8 - 20) >> 8) & 0xff);
	out[3] = (unsigned char)((off + 8 - 20) & 0xff);
	unsigned char fp[4];
	put_u32(fp, crc32_stun(out, off) ^ 0x5354554Eu);
	out[off++]=0x80; out[off++]=0x28; out[off++]=0; out[off++]=4;
	memcpy(out+off, fp, 4); off += 4;
	return off;
}

static void ice_handle_packet(int fd, const unsigned char *rx, size_t len,
			      const struct sockaddr_in *from)
{
	unsigned short type = (unsigned short)((rx[0] << 8) | rx[1]);
	if ((type & 0x0110) == 0x010) {          /* indication = keepalive */
		return;
	}
	const unsigned char *v; unsigned alen;
	if (!find_attr(rx, len, 0x0006, &v, &alen)) {
		g_ice_rejected++;
		unsigned char tx[256];
		size_t n = build_stun_response(tx, 0x0111, rx + 8, from, 400);
		sendto(fd, tx, n, 0, (const struct sockaddr *)from, sizeof(*from));
		return;
	}
	char uname[128] = { 0 };
	memcpy(uname, v, alen < sizeof(uname) - 1 ? alen : sizeof(uname) - 1);
	if (!strstr(uname, SERVER_UFRAG)) {
		g_ice_rejected++;
		unsigned char tx[256];
		size_t n = build_stun_response(tx, 0x0111, rx + 8, from, 401);
		sendto(fd, tx, n, 0, (const struct sockaddr *)from, sizeof(*from));
		logline("ice: 401 for USERNAME '%.32s'", uname);
		return;
	}
	const unsigned char *mi; unsigned milen;
	if (!find_attr(rx, len, 0x0008, &mi, &milen) || milen != 20) {
		g_ice_rejected++;
		unsigned char tx[256];
		size_t n = build_stun_response(tx, 0x0111, rx + 8, from, 401);
		sendto(fd, tx, n, 0, (const struct sockaddr *)from, sizeof(*from));
		return;
	}
	unsigned char expect[20];
	stun_hmac_sha1(rx, (size_t)(mi - rx) - 4, SERVER_PWD, expect);
	if (memcmp(expect, mi, 20) != 0) {
		g_ice_rejected++;
		unsigned char tx[256];
		size_t n = build_stun_response(tx, 0x0111, rx + 8, from, 401);
		sendto(fd, tx, n, 0, (const struct sockaddr *)from, sizeof(*from));
		logline("ice: 401 (HMAC mismatch)");
		return;
	}
	unsigned char tx[256];
	size_t n = build_stun_response(tx, 0x0101, rx + 8, from, 0);
	sendto(fd, tx, n, 0, (const struct sockaddr *)from, sizeof(*from));
	g_ice_checks_ok++;
	if (!g_media.in_use) {
		memset(&g_media, 0, sizeof(g_media));
		g_media.in_use = 1;
		g_media.peer = *from;
	}
	logline("ice: binding success for USERNAME '%.32s' — path validated", uname);
}

/* --- DTLS on the media socket: start/continue the handshake --- */
static char g_expected_fp[128];

static void dtls_start(int fd, const struct sockaddr_in *from)
{
	media_reset("new peer replacing stale session");
	memset(&g_media, 0, sizeof(g_media));
	g_media.in_use = 1;
	g_media.peer = *from;

	g_media.ssl = SSL_new(g_sctx);
	BIO *bio = BIO_new_dgram(fd, BIO_NOCLOSE);
	BIO_dgram_set_peer(bio, (const struct sockaddr *)from);
	SSL_set_bio(g_media.ssl, bio, bio);
	logline("dtls: ClientHello from peer — handshake begins");
	/* The accept is driven from the main loop (dtls_continue). */
}

/* Returns 1 when the handshake (and fingerprint check) is complete.
 * The BIO owns the socket; fd is only here for signature symmetry. */
static int dtls_continue(int fd)
{
	(void)fd;
	static unsigned long calls = 0;
	calls++;
	int r = SSL_accept(g_media.ssl);
#ifdef DEBUG
	fprintf(stderr, "[%ld] dtls: SSL_accept call #%lu -> %d err=%d\n",
		(long)time(NULL), calls, r,
		r == 1 ? 0 : SSL_get_error(g_media.ssl, r));
#endif
	if (r == 1) {
		logline("dtls: handshake complete (%s, %s)",
			SSL_get_version(g_media.ssl), SSL_get_cipher_name(g_media.ssl));
		int fprc = dtls_verify_peer_fingerprint(g_media.ssl, g_expected_fp);
		if (fprc != 1) {
			logline("dtls: REJECTED — fingerprint mismatch (MITM guard)");
			media_reset("fingerprint mismatch");
			return 0;
		}
		g_media.fingerprint_ok = 1;
		g_media.dtls_up = 1;
		/* ---- bite 76: exporter -> libsrtp ---- */
		if (SSL_export_keying_material(g_media.ssl, g_media.keys, EXPORT_LEN,
					       "EXTRACTOR-dtls_srtp", 19,
					       USE_SRTP_CTX, sizeof(USE_SRTP_CTX), 1) != 1) {
			logline("srtp: key export failed");
			media_reset("key export failed");
			return 0;
		}
		srtp_policy_t pol;
		memset(&pol, 0, sizeof(pol));
		srtp_crypto_policy_set_from_profile_for_rtp(&pol.rtp, srtp_profile_aes128_cm_sha1_80);
		srtp_crypto_policy_set_from_profile_for_rtcp(&pol.rtcp, srtp_profile_aes128_cm_sha1_80);
		pol.ssrc.type = ssrc_any_outbound;
		pol.key = g_media.keys + 16;         /* server write key+salt */
		pol.window_size = 128;
		srtp_create(&g_media.sess_out, &pol);
		memset(&pol, 0, sizeof(pol));
		srtp_crypto_policy_set_from_profile_for_rtp(&pol.rtp, srtp_profile_aes128_cm_sha1_80);
		srtp_crypto_policy_set_from_profile_for_rtcp(&pol.rtcp, srtp_profile_aes128_cm_sha1_80);
		pol.ssrc.type = ssrc_any_inbound;
		pol.key = g_media.keys;              /* client write key+salt */
		pol.window_size = 128;
		srtp_create(&g_media.sess_in, &pol);
		logline("srtp: sessions ready — media echo is LIVE");
		return 1;
	}
	int err = SSL_get_error(g_media.ssl, r);
	if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
		return 0;                            /* keep waiting for flights */
	logline("dtls: handshake failed (err=%d)", err);
	media_reset("handshake failed");
	return 0;
}

/* --- the echo (bite 77), unchanged in spirit --- */
static void media_echo_rtp(int fd, unsigned char *rx, size_t len,
			   const struct sockaddr_in *from)
{
	if (!g_media.dtls_up) {
		g_rtp_dropped++;
		return;                              /* media before DTLS = drop */
	}
	int ilen = (int)len;
	if (srtp_unprotect(g_media.sess_in, rx, &ilen) != srtp_err_status_ok) {
		logline("media: DROP unprotect failure");
		g_rtp_dropped++;
		return;
	}
	size_t plen = (size_t)ilen;
	if (plen < RTP_HDR_LEN + 1 || (rx[0] >> 6) != 2) {
		g_rtp_dropped++;
		return;
	}
	unsigned pt = rx[1] & 0x7f;
	unsigned short seq = (unsigned short)((rx[2] << 8) | rx[3]);
	if (pt != 0) {
		logline("media: DROP unexpected payload type %u", pt);
		g_rtp_dropped++;
		return;
	}
	if (seq == g_media.last_in_seq) {
		logline("media: DROP duplicate seq=%u", seq);
		g_rtp_dropped++;
		return;
	}
	g_media.last_in_seq = seq;

	size_t payload_len = plen - RTP_HDR_LEN;
	unsigned char tx[2048];
	tx[0] = 0x80;
	tx[1] = (unsigned char)pt;
	tx[2] = (unsigned char)(g_media.out_seq >> 8);
	tx[3] = (unsigned char)(g_media.out_seq & 0xff);
	tx[4] = (unsigned char)(g_media.out_ts >> 24);
	tx[5] = (unsigned char)(g_media.out_ts >> 16);
	tx[6] = (unsigned char)(g_media.out_ts >> 8);
	tx[7] = (unsigned char)g_media.out_ts;
	tx[8]  = (unsigned char)(SERVER_SSRC >> 24);
	tx[9]  = (unsigned char)(SERVER_SSRC >> 16);
	tx[10] = (unsigned char)(SERVER_SSRC >> 8);
	tx[11] = (unsigned char)SERVER_SSRC;
	memcpy(tx + RTP_HDR_LEN, rx + RTP_HDR_LEN, payload_len);
	size_t tlen = RTP_HDR_LEN + payload_len;
	int tlen_i = (int)tlen;
	if (srtp_protect(g_media.sess_out, tx, &tlen_i) == srtp_err_status_ok)
		sendto(fd, tx, (size_t)tlen_i, 0,
		       (const struct sockaddr *)from, sizeof(*from));
	g_media.out_seq++;
	g_media.out_ts += TS_STEP;
	g_rtp_echoed++;
	logline("media: echoed seq %u -> %u (%zu-byte payload, out_ts=%u)",
		seq, g_media.out_seq - 1, payload_len, g_media.out_ts - TS_STEP);
}

/* --- the demux: the whole point of sharing one UDP port ---
 * CRITICAL MECHANICS: we MSG_PEEK to classify, and only consume the
 * datagram ourselves for STUN/RTP/garbage. DTLS datagrams are left in the
 * kernel buffer for the dgram BIO to consume — recvfrom'ing them first
 * would hand OpenSSL an empty socket and the handshake would stall forever
 * (this is exactly the bite-75 friction, now in its final form). */
static void media_handle_packet(int fd)
{
	unsigned char peek[16];
	struct sockaddr_in from;
	socklen_t fl = sizeof(from);
	ssize_t pr = recvfrom(fd, peek, sizeof(peek), MSG_PEEK | MSG_TRUNC,
			      (struct sockaddr *)&from, &fl);
	if (pr < 8)
		return;
#ifdef DEBUG
	fprintf(stderr, "[%ld] demux: first=0x%02x from port %d\n", (long)time(NULL),
		peek[0], ntohs(from.sin_port));
#endif
	if (g_media.in_use)
		g_media.last_seen_ms = now_ms();

	if ((peek[0] & 0xC0) == 0 &&
	    (peek[4] << 24 | peek[5] << 16 | peek[6] << 8 | peek[7]) == 0x2112A442u) {
		unsigned char rx[2048];
		ssize_t r = recvfrom(fd, rx, sizeof(rx), 0,
				     (struct sockaddr *)&from, &fl);
		if (r > 0)
			ice_handle_packet(fd, rx, (size_t)r, &from);
		return;
	}
	if (peek[0] >= 0x14 && peek[0] <= 0x17) {                /* DTLS */
		if (!g_media.in_use || !g_media.ssl ||
		    memcmp(&from.sin_addr, &g_media.peer.sin_addr, 4) != 0 ||
		    from.sin_port != g_media.peer.sin_port) {
			if (peek[0] == 0x16)              /* a fresh ClientHello */
				dtls_start(fd, &from);
			else {
				unsigned char junk[2048]; /* non-CH from stranger */
				recvfrom(fd, junk, sizeof(junk), 0, NULL, NULL);
				g_udp_dropped++;
				return;
			}
		}
		if (!g_media.dtls_up)
			dtls_continue(fd);   /* the BIO consumes the datagram */
		else {
			/* post-handshake DTLS record (close_notify etc.) */
			char sink[64];
			SSL_read(g_media.ssl, sink, sizeof(sink));
		}
		return;
	}
	if ((peek[0] & 0xC0) == 0x80) {                          /* RTP/SRTP */
		unsigned char rx[2048];
		ssize_t r = recvfrom(fd, rx, sizeof(rx), 0,
				     (struct sockaddr *)&from, &fl);
		if (r > 0)
			media_echo_rtp(fd, rx, (size_t)r, &from);
		return;
	}
	unsigned char junk[2048];                                /* garbage */
	recvfrom(fd, junk, sizeof(junk), 0, NULL, NULL);
	g_udp_dropped++;
}

/* ================================== main ================================== */
static int media_listen(int port)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	a.sin_port = htons((unsigned short)port);
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
		perror("bind media");
		close(fd);
		return -1;
	}
	int fl = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	return fd;
}

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	g_start_ms = now_ms();

	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	int sigfd = signalfd(-1, &mask, 0);
	signal(SIGPIPE, SIG_IGN);

	if (srtp_init() != srtp_err_status_ok) {
		fprintf(stderr, "srtp_init failed\n");
		return 1;
	}
	g_sctx = dtls_ctx_create("bites/80-checkpoint-backend/server.crt",
				 "bites/80-checkpoint-backend/server.key", 1);
	if (!g_sctx)
		return 1;
	FILE *f = fopen("bites/80-checkpoint-backend/client-fingerprint.txt", "r");
	if (!f) {
		fprintf(stderr, "run bites/80-checkpoint-backend/gen-cert.sh first\n");
		return 1;
	}
	size_t fn = fread(g_expected_fp, 1, sizeof(g_expected_fp) - 1, f);
	fclose(f);
	while (fn && (g_expected_fp[fn - 1] == '\n' || g_expected_fp[fn - 1] == '\r'))
		g_expected_fp[--fn] = '\0';
	dtls_normalize_hex(g_expected_fp);

	int mfd = media_listen(MEDIA_PORT);
	if (mfd < 0)
		return 1;

	struct lws_context_creation_info info;
	memset(&info, 0, sizeof(info));
	info.port = WS_PORT;
	info.protocols = protocols;
	info.gid = info.uid = -1;
	struct lws_context *lwsctx = lws_create_context(&info);
	if (!lwsctx) {
		fprintf(stderr, "lws_create_context failed (port %d busy?)\n", WS_PORT);
		return 1;
	}
	logline("backend: signaling ws/%d, media udp/%d (ufrag '%s')",
		WS_PORT, MEDIA_PORT, SERVER_UFRAG);
	logline("backend: expecting peer fingerprint %.16s... (from SDP)", g_expected_fp);

	/* ---- the signaling thread (bite 50) ----
	 * libwebsockets owns its poll loop; fighting that from a foreign loop
	 * caused multi-second stalls. Its own thread, its own counters. */
	pthread_t ws_thread;
	struct ws_thread_args args = { lwsctx, &g_running };
	if (pthread_create(&ws_thread, NULL, ws_service_thread, &args) != 0) {
		fprintf(stderr, "pthread_create failed\n");
		return 1;
	}

	while (g_running) {
		struct pollfd pfds[2];
		pfds[0].fd = mfd;
		pfds[0].events = POLLIN;
		pfds[1].fd = sigfd;
		pfds[1].events = POLLIN;
		/* 20 ms tick: the bite-59 pacing quantum. */
		int rc = poll(pfds, 2, 20);
		if (rc < 0 && errno != EINTR)
			break;

		if (pfds[1].revents & POLLIN) {
			/* signalfd footgun: a plain `while (read(...) == size)`
			 * drain loop BLOCKS when the queue is empty — signalfd
			 * has no EOF, unlike a pipe. One read per poll round,
			 * and the poll loop re-checks the flag. */
			struct signalfd_siginfo si;
			if (read(sigfd, &si, sizeof(si)) == sizeof(si)) {
				logline("backend: signal %u — graceful shutdown begins",
					si.ssi_signo);
				g_running = 0;   /* stops both planes */
			}
		}
		if (pfds[0].revents & POLLIN)
			media_handle_packet(mfd);

		/* DTLS retransmission timer (bite 75's lesson, wired for real) */
		if (g_media.in_use && g_media.ssl && !g_media.dtls_up) {
			struct timeval tv;
			if (DTLSv1_get_timeout(g_media.ssl, &tv) == 1) {
				long long due = (long long)tv.tv_sec * 1000 +
						tv.tv_usec / 1000;
				if (due <= 20)
					DTLSv1_handle_timeout(g_media.ssl);
			}
		}
		/* idle media session sweep (bite 79) */
		if (g_media.in_use && now_ms() - g_media.last_seen_ms >
		    (long long)IDLE_TIMEOUT_S * 1000 && g_media.last_seen_ms) {
			media_reset("idle timeout");
		}
	}

	/* ---- graceful shutdown: everything freed exactly once ---- */
	logline("backend: stopping the signaling thread...");
	g_running = 0;
	lws_cancel_service(lwsctx);   /* wake the service loop so join is quick */
	pthread_join(ws_thread, NULL);
	logline("backend: closing media session and signaling peers...");
	media_reset("shutdown");
	lws_context_destroy(lwsctx);
	close(mfd);
	close(sigfd);
	SSL_CTX_free(g_sctx);
	srtp_shutdown();
	logline("backend: final stats — ws_relayed=%lu ice_ok=%lu ice_rej=%lu "
		"rtp_echoed=%lu rtp_dropped=%lu udp_garbage=%lu uptime=%llds",
		g_ws_msgs_relayed, g_ice_checks_ok, g_ice_rejected,
		g_rtp_echoed, g_rtp_dropped, g_udp_dropped,
		(now_ms() - g_start_ms) / 1000);
	return 0;
}
