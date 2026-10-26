/* bite 74 — check_client.c: the local test double for "a browser's ICE agent".
 *
 * COMPILE + RUN (inside WSL, from the repo root, ice_lite already running):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/74-ice-lite/check_client.c \
 *       -o /tmp/b74client -lcrypto
 *   timeout 10 /tmp/b74client 127.0.0.1 50000
 *
 * A full ICE agent gathers candidates, pairs them, paces checks (timer Ta),
 * nominates... none of which is needed to exercise an ICE-LITE server: lite
 * servers only ever see inbound Binding requests, so this client plays the
 * browser's role directly:
 *   check 1: well-formed request, correct ufrag, correct pwd -> success,
 *            response's XOR-MAPPED-ADDRESS and MESSAGE-INTEGRITY verified.
 *   check 2: correct ufrag, WRONG password                  -> 401 error.
 *   check 3: USERNAME missing our ufrag                     -> 401 error.
 *   check 4: STUN request with no attributes                -> 400 error.
 * If you started the server with --bad-mac, check 1 fails at the client's
 * verification of the RESPONSE integrity — exactly what a browser would do.
 */

#define _POSIX_C_SOURCE 200809L
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define STUN_MAGIC            0x2112A442u
#define STUN_HDR_LEN          20
#define SERVER_UFRAG "srvUfrag77"
#define SERVER_PWD   "serverIcePwdDemo42"

enum {
	ATTR_USERNAME           = 0x0006,
	ATTR_MESSAGE_INTEGRITY  = 0x0008,
	ATTR_ERROR_CODE         = 0x0009,
	ATTR_XOR_MAPPED_ADDRESS = 0x0020,
	ATTR_PRIORITY           = 0x0024,
	ATTR_FINGERPRINT        = 0x8028,
	ATTR_ICE_CONTROLLING    = 0x802A,
};

static unsigned int crc32_stun(const unsigned char *data, size_t len)
{
	unsigned int crc = 0xFFFFFFFFu;
	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int b = 0; b < 8; b++)
			crc = (crc >> 1) ^ (0xEDB88320u & (unsigned)-(int)(crc & 1));
	}
	return ~crc;
}

static void put_attr(unsigned char *buf, size_t *off, unsigned short type,
		     const unsigned char *val, unsigned short len)
{
	buf[(*off)++] = (unsigned char)(type >> 8);
	buf[(*off)++] = (unsigned char)(type & 0xff);
	buf[(*off)++] = (unsigned char)(len >> 8);
	buf[(*off)++] = (unsigned char)(len & 0xff);
	memcpy(buf + *off, val, len);
	*off += len;
	while (*off & 3)
		buf[(*off)++] = 0;
}

static void put_u32(unsigned char *p, unsigned int v)
{
	p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}

/* HMAC-SHA1 for MESSAGE-INTEGRITY. RFC 5389 s15.4: the input is the message
 * up to (NOT including) the MI attribute, with the header length field
 * adjusted to end at the MI attribute (mi_attr_off + 24). */
static void stun_hmac_sha1(const unsigned char *msg, size_t mi_attr_off,
			   const char *key, unsigned char out[20])
{
	unsigned char tmp[STUN_HDR_LEN + 1500];
	memcpy(tmp, msg, mi_attr_off);
	size_t plen = mi_attr_off + 24 - STUN_HDR_LEN;
	tmp[2] = (unsigned char)((plen >> 8) & 0xff);
	tmp[3] = (unsigned char)(plen & 0xff);
	unsigned int outlen = 20;
	HMAC(EVP_sha1(), key, (int)strlen(key), tmp, mi_attr_off, out, &outlen);
}

static int find_attr(const unsigned char *msg, size_t len, unsigned short want,
		     const unsigned char **val, unsigned short *alen)
{
	unsigned short plen = (unsigned short)((msg[2] << 8) | msg[3]);
	size_t off = STUN_HDR_LEN, end = STUN_HDR_LEN + plen;
	if (end > len)
		end = len;
	while (off + 4 <= end) {
		unsigned short type = (unsigned short)((msg[off] << 8) | msg[off + 1]);
		unsigned short l = (unsigned short)((msg[off + 2] << 8) | msg[off + 3]);
		if (off + 4 + l > end)
			return 0;
		if (type == want) {
			*val = msg + off + 4;
			*alen = l;
			return 1;
		}
		off += 4 + (size_t)((l + 3) & ~3u);
	}
	return 0;
}

/* Build one Binding request. bad_pwd!=0 uses a deliberately wrong password
 * (the browser equivalent of an SDP pwd mismatch). */
static size_t build_request(unsigned char *out, const unsigned char txid[12],
			    const char *username, const char *pwd, int bad_pwd)
{
	size_t off = STUN_HDR_LEN;
	out[0] = 0x00; out[1] = 0x01;            /* Binding request */
	put_u32(out + 4, STUN_MAGIC);
	memcpy(out + 8, txid, 12);

	put_attr(out, &off, ATTR_USERNAME,
		 (const unsigned char *)username, (unsigned short)strlen(username));
	unsigned char prio[4];
	put_u32(prio, 0x6e0001ff);               /* host-candidate priority, any value ok */
	put_attr(out, &off, ATTR_PRIORITY, prio, 4);
	unsigned char ctrl[8] = { 0 };
	put_u32(ctrl, 0x00000001);               /* ICE-CONTROLLING tiebreaker */
	put_attr(out, &off, ATTR_ICE_CONTROLLING, ctrl, 8);

	size_t mi_off = off;
	unsigned char zero[20] = { 0 };
	put_attr(out, &off, ATTR_MESSAGE_INTEGRITY, zero, 20);
	unsigned char mac[20];
	/* Sign over the message up to (not including) the MI attribute. */
	stun_hmac_sha1(out, mi_off, bad_pwd ? "wrong-password-on-purpose" : pwd, mac);
	memcpy(out + mi_off + 4, mac, 20);

	/* Patch the length field to its FINAL value (including the upcoming
	 * FINGERPRINT attr: +8) before the CRC runs — the CRC covers bytes as
	 * the receiver sees them. (Same trap the server side comments about.) */
	out[2] = (unsigned char)(((off + 8 - STUN_HDR_LEN) >> 8) & 0xff);
	out[3] = (unsigned char)((off + 8 - STUN_HDR_LEN) & 0xff);
	unsigned char fp[4];
	put_u32(fp, crc32_stun(out, off) ^ 0x5354554Eu);
	put_attr(out, &off, ATTR_FINGERPRINT, fp, 4);
	return off;
}

static int wait_response(int fd, unsigned char *rx, size_t cap, const unsigned char txid[12])
{
	for (;;) {
		ssize_t r = recv(fd, rx, cap, 0);
		if (r <= 0)
			return -1;                       /* timeout: silence IS a result */
		if (memcmp(rx + 8, txid, 12) != 0)
			continue;                        /* stale packet; correlate by txid */
		return (int)r;
	}
}

static int verify_response(const unsigned char *rx, size_t len, int *is_error)
{
	/* 1. FINGERPRINT CRC — proves we dissect the same way Wireshark would.
	 * RFC 5389 s15.5: CRC32 of the message up to, but NOT including, the
	 * FINGERPRINT attribute itself (fp points at its value, so the input
	 * ends right before the 4-byte attribute header). */
	const unsigned char *fp; unsigned short fplen;
	if (find_attr(rx, len, ATTR_FINGERPRINT, &fp, &fplen)) {
		unsigned int want = crc32_stun(rx, (size_t)(fp - 4 - rx)) ^ 0x5354554Eu;
#ifdef DEBUG
		fprintf(stderr, "client: resp(%zu):", len);
		for (size_t i = 0; i < len; i++) fprintf(stderr, " %02x", rx[i]);
		fprintf(stderr, "\nclient: crc_off=%zu want=0x%08x got=0x%08x\n",
			(size_t)(fp - 4 - rx), want,
			((unsigned)fp[0] << 24 | (unsigned)fp[1] << 16 |
			 (unsigned)fp[2] << 8 | fp[3]));
#endif
		unsigned int got = ((unsigned)fp[0] << 24 | (unsigned)fp[1] << 16 |
				    (unsigned)fp[2] << 8 | fp[3]);
		if (want != got) {
			fprintf(stderr, "client: FINGERPRINT mismatch (0x%08x vs 0x%08x)\n",
				want, got);
			return -1;
		}
	}
	/* 2. message class */
	unsigned short type = (unsigned short)((rx[0] << 8) | rx[1]);
	*is_error = (type & 0x0110) == 0x0110;

	/* 3. MESSAGE-INTEGRITY of the response (same key as the request).
	 * mi points at the HMAC value; the attribute header starts at mi-4,
	 * and the HMAC input ends right before that header. */
	const unsigned char *mi; unsigned short milen;
	if (find_attr(rx, len, ATTR_MESSAGE_INTEGRITY, &mi, &milen) && milen == 20) {
		unsigned char expect[20];
		stun_hmac_sha1(rx, (size_t)(mi - rx) - 4, SERVER_PWD, expect);
		if (memcmp(expect, mi, 20) != 0) {
			fprintf(stderr, "client: response MESSAGE-INTEGRITY mismatch "
				"(server signed with wrong key?)\n");
			return -1;
		}
	}
	return 0;
}

/* One full transaction: build, send, wait, verify, judge. */
static int one_check(int fd, int port, const char *label, const char *username,
		     const char *pwd, int bad_pwd, int expect_success)
{
	unsigned char tx[512], rx[2048];
	unsigned char txid[12];
	for (int i = 0; i < 12; i++)
		txid[i] = (unsigned char)(rand() & 0xff);
	size_t len = build_request(tx, txid, username, pwd, bad_pwd);

	struct sockaddr_in srv;
	memset(&srv, 0, sizeof(srv));
	srv.sin_family = AF_INET;
	srv.sin_port = htons((unsigned short)port);
	inet_pton(AF_INET, "127.0.0.1", &srv.sin_addr);

	if (sendto(fd, tx, len, 0, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
		perror("sendto");
		return 1;
	}
	int is_error = 0;
	ssize_t r = wait_response(fd, rx, sizeof(rx), txid);
	if (r < 0) {
		fprintf(stderr, "client: %-28s -> TIMEOUT (silence breaks ICE timers!)\n", label);
		return 1;
	}
	if (verify_response(rx, (size_t)r, &is_error) != 0)
		return 1;

	int ok = expect_success ? !is_error : is_error;
	if (ok) {
		printf("client: PASS %-28s -> %s\n", label,
		       is_error ? "error response (as expected)" : "success response");
		/* decode XOR-MAPPED-ADDRESS on success for the teaching moment */
		if (!is_error) {
			const unsigned char *xm; unsigned short xl;
			if (find_attr(rx, (size_t)r, ATTR_XOR_MAPPED_ADDRESS, &xm, &xl) && xl == 8) {
				unsigned int ip = ((unsigned)xm[4] << 24 | (unsigned)xm[5] << 16 |
						   (unsigned)xm[6] << 8 | xm[7]) ^ STUN_MAGIC;
				unsigned short xport = (unsigned short)((xm[2] << 8 | xm[3]) ^ (STUN_MAGIC >> 16));
				struct in_addr a = { .s_addr = htonl(ip) };
				char s[INET_ADDRSTRLEN];
				inet_ntop(AF_INET, &a, s, sizeof(s));
				printf("client:       XOR-MAPPED-ADDRESS %s:%u — server saw our real address\n",
				       s, xport);
			}
		}
		return 0;
	}
	fprintf(stderr, "client: FAIL %-28s -> unexpected %s response\n", label,
		is_error ? "error" : "success");
	return 1;
}

int main(int argc, char **argv)
{
	int port = argc > 2 ? atoi(argv[2]) : 50000;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("socket");
		return 1;
	}
	/* Every transaction gets a bounded wait: a real ICE agent retransmits
	 * (RTO backoff); a test double just gives up and reports. */
	struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	int failures = 0;
	char uname[64];

	/* check 1: the happy path — USERNAME "srvUfrag77:cliUfragXX", our pwd */
	snprintf(uname, sizeof(uname), "%s:cliUfragXX", SERVER_UFRAG);
	failures += one_check(fd, port, "valid check", uname, SERVER_PWD, 0, 1);

	/* check 2: wrong password -> server must 401 us */
	failures += one_check(fd, port, "wrong pwd", uname, SERVER_PWD, 1, 0);

	/* check 3: USERNAME without our ufrag -> server must 401 us */
	failures += one_check(fd, port, "wrong ufrag", "otherUfrag:cliUfragXX",
			      SERVER_PWD, 0, 0);

	/* check 4: request with NO attributes at all -> 400 */
	{
		unsigned char tx[64], rx[2048];
		unsigned char txid[12];
		for (int i = 0; i < 12; i++)
			txid[i] = (unsigned char)(rand() & 0xff);
		tx[0] = 0x00; tx[1] = 0x01;
		put_u32(tx + 4, STUN_MAGIC);
		memcpy(tx + 8, txid, 12);
		tx[2] = 0; tx[3] = 0;   /* zero attributes */
		struct sockaddr_in sa;
		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_port = htons((unsigned short)port);
		inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
		sendto(fd, tx, STUN_HDR_LEN, 0, (struct sockaddr *)&sa, sizeof(sa));
		ssize_t r = wait_response(fd, rx, sizeof(rx), txid);
		int is_error = 0;
		if (r > 0)
			verify_response(rx, (size_t)r, &is_error);
		if (r > 0 && is_error) {
			printf("client: PASS %-28s -> error response (as expected)\n",
			       "empty request");
		} else {
			fprintf(stderr, "client: FAIL %-28s\n", "empty request");
			failures++;
		}
	}

	close(fd);
	if (failures) {
		fprintf(stderr, "client: %d check(s) FAILED\n", failures);
		return 1;
	}
	printf("client: all ICE connectivity checks passed\n");
	return 0;
}
