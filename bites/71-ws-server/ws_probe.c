/* bite 71 — ws_probe.c: a minimal WebSocket CLIENT, on purpose.
 *
 * FEATURE TEST MACRO: -std=c17 is strict ISO C; getaddrinfo/strdup etc. only
 * appear when we ask for POSIX (200809L) explicitly. Every bite-7x source
 * that uses POSIX APIs starts with this line.
 *
 * COMPILE + RUN (inside WSL, from the repo root, server already running):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/71-ws-server/ws_probe.c \
 *       -o /tmp/b71probe -lcrypto
 *   timeout 15 /tmp/b71probe 127.0.0.1 8081
 *
 * WHY THIS EXISTS
 *   The bite's "done when" is "two tabs echo through the server". A real
 *   browser is not needed to prove the relay — and hand-rolling the client
 *   side of the protocol is the fastest way to internalize what libwebsockets
 *   was doing for you on the server side. This is the documented local test
 *   double: one process opens TWO WS connections and checks A->B and B->A.
 *
 * WHAT YOU SEE HERE, PIECE BY PIECE (RFC 6455):
 *   1. HTTP upgrade request: GET + Upgrade: websocket + Sec-WebSocket-Key.
 *      Server must reply "101 Switching Protocols" with Sec-WebSocket-Accept
 *      = base64(SHA1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")).
 *      Bite 55 built the HTTP half; bite 64 the SHA-1 half.
 *   2. Frames: FIN/opcode byte, mask/len byte, optional extended length,
 *      4-byte masking key (client->server MUST be masked), payload.
 *   3. Close: opcode 8.
 *
 * BREAK IT ON PURPOSE:
 *   - Change the GUID in the accept computation (or drop a header): the
 *     server answers 400 and this probe prints the rejection — the upgrade
 *     handshake is strict by design.
 *   - Send the unmasked payload (flip USE_MASK to 0): a correct server MUST
 *     close the connection with a protocol error (RFC 6455 s5.1).
 *   - Run the probe against bite 72's server instead: it speaks the same WS
 *     transport but different (JSON) payloads — transport vs. application
 *     layering, right there.
 */
#define _POSIX_C_SOURCE 200809L

#include <openssl/sha.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define USE_MASK 1   /* flip to 0 to watch a server reject you (see BREAK IT) */

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

/* Read exactly n bytes — bite 48's discipline, again, because TCP is a stream. */
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

/* Read a full HTTP response header block terminated by \r\n\r\n. */
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

/* base64: 3 bytes -> 4 chars, '='-padded. Small enough to hand-roll and see. */
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

static int ws_connect(int *out_fd, const char *port)
{
	/* Sec-WebSocket-Key: 16 random bytes, base64. Entropy quality does not
	 * matter here — the key is a handshake check, not a secret. */
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
		fprintf(stderr, "probe: upgrade failed:\n%.1200s\n", hdr);
		close(fd);
		return -1;
	}

	/* Verify the accept token ourselves — this is exactly what a browser
	 * checks before treating the connection as a WebSocket. */
	unsigned char sha[SHA_DIGEST_LENGTH];
	char cat[64];
	snprintf(cat, sizeof(cat), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", keyb64);
	SHA1((unsigned char *)cat, strlen(cat), sha);
	char accept[32];
	b64_encode(sha, SHA_DIGEST_LENGTH, accept, sizeof(accept));
	if (!strstr(hdr, accept)) {
		fprintf(stderr, "probe: bad Sec-WebSocket-Accept (got none/'%.28s' mismatch)\n", accept);
		close(fd);
		return -1;
	}
	*out_fd = fd;
	return 0;
}

/* Send one unfragmented masked text frame. */
static int ws_send_text(int fd, const char *msg)
{
	size_t len = strlen(msg);
	unsigned char hdr[10];
	size_t hlen = 0;
	hdr[hlen++] = 0x81;              /* FIN=1, opcode=1 (text) */
	if (len < 126) {
		hdr[hlen] = (unsigned char)(0x80 * USE_MASK | len);
		hlen++;
	} else if (len < 65536) {
		hdr[hlen++] = (unsigned char)(0x80 * USE_MASK | 126);
		hdr[hlen++] = (unsigned char)(len >> 8);
		hdr[hlen++] = (unsigned char)(len & 0xff);
	} else {                         /* 127 => 8-byte extended length follows */
		hdr[hlen++] = (unsigned char)(0x80 * USE_MASK | 127);
		for (int i = 7; i >= 0; i--)
			hdr[hlen++] = (unsigned char)(((uint64_t)len >> (8 * i)) & 0xff);
	}
	unsigned char mask[4] = { 0xde, 0xad, 0xbe, 0xef };
	if (USE_MASK) {
		memcpy(hdr + hlen, mask, 4);
		hlen += 4;
	}
	if (send(fd, hdr, hlen, 0) != (ssize_t)hlen)
		return -1;
	unsigned char *payload = malloc(len + 1);
	if (!payload)
		return -1;
	memcpy(payload, msg, len);
	if (USE_MASK)
		for (size_t i = 0; i < len; i++)
			payload[i] ^= mask[i % 4];   /* XOR with cycling mask key, s5.3 */
	int rc = send(fd, payload, len, 0) == (ssize_t)len ? 0 : -1;
	free(payload);
	return rc;
}

/* Receive one complete (final) text message, transparently answering pings. */
static ssize_t ws_recv_text(int fd, char *out, size_t cap)
{
	size_t total = 0;
	for (;;) {
		unsigned char h[2];
		if (read_exact(fd, h, 2) != 0)
			return -1;
		int fin  = h[0] & 0x80;
		int op   = h[0] & 0x0f;
		uint64_t len = h[1] & 0x7f;
		if ((h[1] & 0x80)) {     /* server->client frames are never masked */
			fprintf(stderr, "probe: server sent masked frame (protocol error)\n");
			return -1;
		}
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
		if (op == 0x9) {         /* ping -> must answer pong (keepalive) */
			unsigned char pong[2] = { 0x8a, 0x00 };
			send(fd, pong, 2, 0);
			continue;
		}
		if (op == 0x8) {         /* close */
			fprintf(stderr, "probe: server sent close frame\n");
			return -1;
		}
		while (total < len && total + 1 < cap) {
			ssize_t r = recv(fd, out + total, (size_t)(len - total), 0);
			if (r <= 0)
				return -1;
			total += (size_t)r;
		}
		if (fin)
			break;
		/* continuation frames (opcode 0) just append — fragmentation done. */
	}
	out[total] = '\0';
	return (ssize_t)total;
}

static int ws_close(int fd)
{
	unsigned char frame[2] = { 0x88, 0x00 };  /* FIN, opcode 8, zero length */
	send(fd, frame, 2, 0);
	return close(fd);
}

int main(int argc, char **argv)
{
	const char *port = argc > 2 ? argv[2] : "8081";

	int fa, fb;
	if (ws_connect(&fa, port) != 0 || ws_connect(&fb, port) != 0) {
		fprintf(stderr, "probe: FAILED to open two websocket connections\n");
		return 1;
	}
	printf("probe: two connections established\n");

	char buf[65536];

	/* A -> B */
	if (ws_send_text(fa, "hello from A") != 0) {
		fprintf(stderr, "probe: send A failed\n");
		return 1;
	}
	if (ws_recv_text(fb, buf, sizeof(buf)) < 0 || strcmp(buf, "hello from A") != 0) {
		fprintf(stderr, "probe: FAILED: B did not receive A's message (got '%s')\n", buf);
		return 1;
	}
	printf("probe: PASS  A -> B relayed\n");

	/* B -> A */
	if (ws_send_text(fb, "hello from B") != 0 ||
	    ws_recv_text(fa, buf, sizeof(buf)) < 0 ||
	    strcmp(buf, "hello from B") != 0) {
		fprintf(stderr, "probe: FAILED: B -> A relay\n");
		return 1;
	}
	printf("probe: PASS  B -> A relayed\n");

	/* Sender must NOT hear its own message (relay, not echo). Bound the wait
	 * with a receive timeout so absence of a reply reads as EOF quickly. */
	struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
	setsockopt(fa, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if (ws_send_text(fa, "self-test") != 0 ||
	    ws_recv_text(fa, buf, sizeof(buf)) < 0) {
		printf("probe: PASS  sender does not hear itself\n");
	} else {
		fprintf(stderr, "probe: FAILED: A got its own message back: '%s'\n", buf);
		return 1;
	}

	ws_close(fa);
	ws_close(fb);
	printf("probe: all checks passed\n");
	return 0;
}
