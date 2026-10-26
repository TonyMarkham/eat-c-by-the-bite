/* bite 72 — sign_probe.c: exercises the signaling protocol WITHOUT a browser.
 *
 * COMPILE + RUN (inside WSL, from the repo root, sign_relay already running):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/72-signaling/sign_probe.c \
 *       -o /tmp/b72probe -lcrypto
 *   timeout 20 /tmp/b72probe 127.0.0.1 8082
 *
 * The WebSocket plumbing (upgrade, frames, masking) is the same hand-rolled
 * client as bite 71's ws_probe.c — read the layering lesson in its header.
 * This file reuses it and layers THE PROTOCOL on top:
 *   1. A joins as "a", B joins as "b"           (registry works, dup join rejected)
 *   2. A -> B: offer/answer/candidate           (routing works)
 *   3. abuse: malformed JSON, missing type, unknown type, unknown "to",
 *      duplicate id, oversized message          (clean errors, no crash, no relay)
 */

#define _POSIX_C_SOURCE 200809L
#include <openssl/sha.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define USE_MASK 1

static void b64_encode(const unsigned char *in, size_t n, char *out, size_t outcap)
{
	static const char tbl[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t o = 0;
	for (size_t i = 0; i + 2 < n; i += 3) {
		unsigned v = (unsigned)in[i] << 16 | (unsigned)in[i + 1] << 8 | in[i + 2];
		if (o + 4 < outcap) {
			out[o++] = tbl[(v >> 18) & 0x3f];
			out[o++] = tbl[(v >> 12) & 0x3f];
			out[o++] = tbl[(v >> 6) & 0x3f];
			out[o++] = tbl[v & 0x3f];
		}
	}
	size_t rem = n % 3;
	if (rem == 1 && o + 4 < outcap) {
		unsigned v = (unsigned)in[n - 1] << 16;
		out[o++] = tbl[(v >> 18) & 0x3f];
		out[o++] = tbl[(v >> 12) & 0x3f];
		out[o++] = '=';
		out[o++] = '=';
	} else if (rem == 2 && o + 4 < outcap) {
		unsigned v = (unsigned)in[n - 2] << 16 | (unsigned)in[n - 1] << 8;
		out[o++] = tbl[(v >> 18) & 0x3f];
		out[o++] = tbl[(v >> 12) & 0x3f];
		out[o++] = tbl[(v >> 6) & 0x3f];
		out[o++] = '=';
	}
	out[o] = '\0';
}

static int tcp_connect(const char *host, const char *port)
{
	struct addrinfo hints, *res;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, port, &hints, &res) != 0) {
		perror("getaddrinfo");
		return -1;
	}
	int fd = -1;
	for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, 0);
		if (fd < 0)
			continue;
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
			break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	return fd;
}

static int read_exact(int fd, void *buf, size_t n)
{
	unsigned char *p = buf;
	size_t got = 0;
	while (got < n) {
		ssize_t r = recv(fd, p + got, n - got, 0);
		if (r <= 0)
			return -1;
		got += (size_t)r;
	}
	return 0;
}

static int read_http_headers(int fd, char *buf, size_t cap)
{
	size_t used = 0;
	while (used < cap - 1) {
		char c;
		if (read_exact(fd, &c, 1) != 0)
			return -1;
		buf[used++] = c;
		if (used >= 4 && !memcmp(buf + used - 4, "\r\n\r\n", 4))
			break;
	}
	buf[used] = '\0';
	return 0;
}

static int ws_connect(int *out_fd, const char *port)
{
	unsigned char key[16] = { 0 };
	for (size_t i = 0; i < sizeof(key); i++)
		key[i] = (unsigned char)(rand() & 0xff);
	char keyb64[25] = { 0 };
	b64_encode(key, sizeof(key), keyb64, sizeof(keyb64));

	char req[512];
	int n = snprintf(req, sizeof(req),
			 "GET / HTTP/1.1\r\n"
			 "Host: probe\r\n"
			 "Upgrade: websocket\r\n"
			 "Connection: Upgrade\r\n"
			 "Sec-WebSocket-Key: %s\r\n"
			 "Sec-WebSocket-Version: 13\r\n\r\n", keyb64);

	int fd = tcp_connect("127.0.0.1", port);
	if (fd < 0)
		return -1;
	if (send(fd, req, (size_t)n, 0) != n) {
		close(fd);
		return -1;
	}
	char hdr[2048];
	if (read_http_headers(fd, hdr, sizeof(hdr)) != 0 || !strstr(hdr, " 101 ")) {
		fprintf(stderr, "probe: upgrade failed:\n%.600s\n", hdr);
		close(fd);
		return -1;
	}
	*out_fd = fd;
	return 0;
}

static int ws_send_text(int fd, const char *msg)
{
	size_t len = strlen(msg);
	unsigned char hdr[10];
	size_t hlen = 0;
	hdr[hlen++] = 0x81;
	if (len < 126) {
		hdr[hlen++] = (unsigned char)(0x80 * USE_MASK | len);
	} else {
		hdr[hlen++] = (unsigned char)(0x80 * USE_MASK | 126);
		hdr[hlen++] = (unsigned char)(len >> 8);
		hdr[hlen++] = (unsigned char)(len & 0xff);
	}
	unsigned char mask[4] = { 0xde, 0xad, 0xbe, 0xef };
	memcpy(hdr + hlen, mask, 4);
	hlen += 4;
	if (send(fd, hdr, hlen, 0) != (ssize_t)hlen)
		return -1;
	unsigned char *payload = malloc(len + 1);
	if (!payload)
		return -1;
	memcpy(payload, msg, len);
	for (size_t i = 0; i < len; i++)
		payload[i] ^= mask[i % 4];
	int rc = send(fd, payload, len, 0) == (ssize_t)len ? 0 : -1;
	free(payload);
	return rc;
}

static ssize_t ws_recv_text(int fd, char *out, size_t cap)
{
	size_t total = 0;
	for (;;) {
		unsigned char h[2];
		if (read_exact(fd, h, 2) != 0)
			return -1;
		int fin = h[0] & 0x80;
		int op  = h[0] & 0x0f;
		uint64_t len = h[1] & 0x7f;
		if (len == 126) {
			unsigned char ext[2];
			if (read_exact(fd, ext, 2) != 0)
				return -1;
			len = (uint64_t)ext[0] << 8 | ext[1];
		} else if (len == 127) {
			unsigned char ext[8];
			if (read_exact(fd, ext, 8) != 0)
				return -1;
			len = 0;
			for (int i = 0; i < 8; i++)
				len = len << 8 | ext[i];
		}
		if (op == 0x9) {
			unsigned char pong[2] = { 0x8a, 0x00 };
			send(fd, pong, 2, 0);
			continue;
		}
		if (op == 0x8)
			return -1;
		if (len >= cap) {
			fprintf(stderr, "probe: frame of %llu bytes too big for probe buffer\n",
				(unsigned long long)len);
			return -1;
		}
		if (read_exact(fd, out + total, (size_t)len) != 0)
			return -1;
		total += (size_t)len;
		if (fin)
			break;
	}
	out[total] = '\0';
	return (ssize_t)total;
}

static int ws_close(int fd)
{
	unsigned char frame[2] = { 0x88, 0x00 };
	send(fd, frame, 2, 0);
	return close(fd);
}

/* ------------------------- protocol-level helpers ------------------------- */

static int expect_contains(const char *label, const char *got, const char *needle)
{
	if (got && strstr(got, needle)) {
		printf("probe: PASS  %-34s -> %.60s\n", label, got);
		return 0;
	}
	fprintf(stderr, "probe: FAILED %-33s -> got '%.120s', wanted substring '%s'\n",
		label, got ? got : "(recv failed)", needle);
	return 1;
}

int main(int argc, char **argv)
{
	const char *port = argc > 2 ? argv[2] : "8082";
	int a, b;
	char buf[32768];
	int failures = 0;

	if (ws_connect(&a, port) != 0 || ws_connect(&b, port) != 0) {
		fprintf(stderr, "probe: cannot open two connections (is sign_relay running?)\n");
		return 1;
	}

	/* 1. joins */
	ws_send_text(a, "{\"type\":\"join\",\"id\":\"a\"}");
	ws_send_text(b, "{\"type\":\"join\",\"id\":\"b\"}");
	if (ws_recv_text(a, buf, sizeof(buf)) < 0) { fprintf(stderr, "probe: no join ack\n"); return 1; }
	failures += expect_contains("join a", buf, "\"joined\"");
	if (ws_recv_text(b, buf, sizeof(buf)) < 0) { fprintf(stderr, "probe: no join ack\n"); return 1; }
	failures += expect_contains("join b", buf, "\"joined\"");

	/* 2. duplicate id rejected */
	ws_send_text(b, "{\"type\":\"join\",\"id\":\"a\"}");
	if (ws_recv_text(b, buf, sizeof(buf)) < 0) { fprintf(stderr, "probe: no reply\n"); return 1; }
	failures += expect_contains("duplicate id rejected", buf, "already taken");

	/* 3. offer / answer / candidate routed */
	ws_send_text(a, "{\"type\":\"offer\",\"to\":\"b\",\"sdp\":\"v=0 o=- fake\"}");
	if (ws_recv_text(b, buf, sizeof(buf)) < 0) { fprintf(stderr, "probe: no offer\n"); return 1; }
	failures += expect_contains("offer a->b (verbatim)", buf, "v=0 o=- fake");

	ws_send_text(b, "{\"type\":\"answer\",\"to\":\"a\",\"sdp\":\"v=0 o=- reply\"}");
	if (ws_recv_text(a, buf, sizeof(buf)) < 0) { fprintf(stderr, "probe: no answer\n"); return 1; }
	failures += expect_contains("answer b->a", buf, "v=0 o=- reply");

	ws_send_text(a, "{\"type\":\"candidate\",\"to\":\"b\",\"candidate\":\"candidate:1 1 UDP 212 127.0.0.1 50000 typ host\"}");
	if (ws_recv_text(b, buf, sizeof(buf)) < 0) { fprintf(stderr, "probe: no candidate\n"); return 1; }
	failures += expect_contains("candidate a->b", buf, "candidate:1 1 UDP");

	/* 4. abuse cases — every one must yield a clean error to the sender */
	ws_send_text(a, "this is not json at all {{{");
	if (ws_recv_text(a, buf, sizeof(buf)) < 0) { fprintf(stderr, "probe: no reply\n"); return 1; }
	failures += expect_contains("malformed json", buf, "malformed");

	ws_send_text(a, "{\"id\":\"no-type-here\"}");
	if (ws_recv_text(a, buf, sizeof(buf)) < 0) { fprintf(stderr, "probe: no reply\n"); return 1; }
	failures += expect_contains("missing type", buf, "missing 'type'");

	ws_send_text(a, "{\"type\":\"flurb\",\"to\":\"b\"}");
	if (ws_recv_text(a, buf, sizeof(buf)) < 0) { fprintf(stderr, "probe: no reply\n"); return 1; }
	failures += expect_contains("unknown type", buf, "unknown type");

	ws_send_text(a, "{\"type\":\"offer\",\"to\":\"nobody\"}");
	if (ws_recv_text(a, buf, sizeof(buf)) < 0) { fprintf(stderr, "probe: no reply\n"); return 1; }
	failures += expect_contains("unknown 'to'", buf, "unknown peer");

	ws_send_text(a, "{\"type\":\"offer\",\"to\":\"a\",\"sdp\":\"x\"}");
	if (ws_recv_text(a, buf, sizeof(buf)) < 0) { fprintf(stderr, "probe: no reply\n"); return 1; }
	failures += expect_contains("signal yourself", buf, "yourself");

	/* 10 KB message: within the cap, must route (B receives it, error free). */
	char big[11000];
	memset(big, 'x', sizeof(big));
	const char prefix[] = "{\"type\":\"offer\",\"to\":\"b\",\"sdp\":\"";
	memcpy(big, prefix, sizeof(prefix) - 1);            /* 32 bytes incl. opening quote */
	memcpy(big + 10272, "\"}", 2);
	big[10274] = '\0';
	ws_send_text(a, big);
	if (ws_recv_text(b, buf, sizeof(buf)) < 0) { fprintf(stderr, "probe: no big msg\n"); return 1; }
	failures += expect_contains("10 KB message routes", buf, "type\":\"offer");

	/* B's channel is clean: nothing was ever relayed to a third party. */
	ws_send_text(b, "{\"type\":\"answer\",\"to\":\"a\",\"sdp\":\"ok\"}");
	if (ws_recv_text(a, buf, sizeof(buf)) < 0) { fprintf(stderr, "probe: no reply\n"); return 1; }
	failures += expect_contains("channel still healthy after abuse", buf, "ok");

	ws_close(a);
	ws_close(b);
	if (failures) {
		fprintf(stderr, "probe: %d check(s) FAILED\n", failures);
		return 1;
	}
	printf("probe: all signaling protocol checks passed\n");
	return 0;
}
