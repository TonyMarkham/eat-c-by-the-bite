/* bite 78 — turn_allocate.c: a minimal TURN Allocate client.
 *
 * COMPILE + RUN (inside WSL, from the repo root):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/78-turn/turn_allocate.c \
 *       -o /tmp/b78 -lcrypto
 *   timeout 5 /tmp/b78 127.0.0.1 3478            # no coturn -> clean error
 *   /tmp/b78 127.0.0.1 3478 myuser mypass        # with coturn running
 *
 * WHAT THIS TEACHES (see bites/78-turn/notes.md for the full story):
 *   TURN is STUN plus three ideas: (1) the Allocate request creates a relay
 *   binding; (2) long-term credentials use a CHALLENGE — the server 401s the
 *   first request and hands you REALM + NONCE; (3) the HMAC key for
 *   long-term creds is MD5(username:realm:password) — RFC 5389 s15.4 — NOT
 *   the raw password like ICE's short-term checks in bite 74.
 *
 * This implements only Allocate (+ the auth dance). Refresh/Permissions/
 * ChannelBind are not needed to understand what a relayed candidate IS; if
 * you want depth, they are the same attribute machinery.
 *
 * BREAK IT ON PURPOSE:
 *   - pass an expired time-limited username (e.g. "1600000000:me") with a
 *     coturn configured for use-auth-secret: 401 forever, clean errors —
 *     the exact failure the credential-expiry logic prevents (notes.md s2).
 *   - tamper one byte of the MESSAGE-INTEGRITY in the second Allocate
 *     (change the key below) and watch coturn 401 the retry as well.
 *
 * WITHOUT COTURN: connect() fails with ECONNREFUSED, or UDP sendtimes out —
 * both are reported as clean errors, exit code 2. That is the tested path
 * in this repo; running against a live coturn is the bite's hands-on step.
 */

#define _POSIX_C_SOURCE 200809L
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define STUN_MAGIC      0x2112A442u
#define STUN_HDR_LEN    20

#define TURN_ALLOCATE           0x0003
#define TURN_ALLOCATE_SUCCESS   0x0103
#define TURN_ALLOCATE_ERROR     0x0113

enum {
	ATTR_USERNAME          = 0x0006,
	ATTR_MESSAGE_INTEGRITY = 0x0008,
	ATTR_ERROR_CODE        = 0x0009,
	ATTR_LIFETIME          = 0x000D,
	ATTR_REALM             = 0x0014,
	ATTR_NONCE             = 0x0015,
	ATTR_REQUESTED_TRANSPORT = 0x0016,
	ATTR_XOR_RELAYED_ADDR  = 0x0017,   /* TURN's relayed candidate address */
	ATTR_XOR_MAPPED_ADDR   = 0x0020,   /* same code as XOR-RELAYED's STUN cousin */
};

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

static int find_attr(const unsigned char *m, size_t len, unsigned want,
		     const unsigned char **val, unsigned *alen)
{
	if (len < STUN_HDR_LEN)
		return 0;
	unsigned plen = (m[2] << 8) | m[3];
	size_t off = STUN_HDR_LEN, end = STUN_HDR_LEN + plen;
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

/* Long-term key (RFC 5389 s15.4): MD5(user ":" realm ":" pass). Note MD5 —
 * an STUN legacy choice; do not confuse with ICE short-term (raw password). */
static void long_term_key(const char *user, const char *realm, const char *pass,
			  unsigned char out[16])
{
	char buf[512];
	snprintf(buf, sizeof(buf), "%s:%s:%s", user, realm, pass);
	unsigned int l = 16;
	EVP_Digest(buf, strlen(buf), out, &l, EVP_md5(), NULL);
}

static void stun_hmac(const unsigned char *msg, size_t mi_off,
		      const unsigned char *key, size_t keylen,
		      unsigned char out[20])
{
	unsigned char tmp[STUN_HDR_LEN + 1024];
	memcpy(tmp, msg, mi_off);
	size_t plen = mi_off + 24 - STUN_HDR_LEN;
	tmp[2] = (unsigned char)(plen >> 8);
	tmp[3] = (unsigned char)(plen & 0xff);
	unsigned int olen = 20;
	HMAC(EVP_sha1(), key, (int)keylen, tmp, mi_off, out, &olen);
}

static size_t build_allocate(unsigned char *out, const unsigned char txid[12],
			     int with_auth, const char *user, const char *realm,
			     const unsigned char *nonce, unsigned nonce_len,
			     const unsigned char key[16])
{
	size_t off = STUN_HDR_LEN;
	out[0] = 0x00; out[1] = 0x03;            /* Allocate request */
	memcpy(out + 4, "\x21\x12\xa4\x42", 4);
	memcpy(out + 8, txid, 12);

	unsigned char rt[4] = { 0, 0, 0, 17 };   /* requested transport: UDP */
	put_attr(out, &off, ATTR_REQUESTED_TRANSPORT, rt, 4);

	if (with_auth) {
		put_attr(out, &off, ATTR_USERNAME,
			 (const unsigned char *)user, (unsigned)strlen(user));
		put_attr(out, &off, ATTR_REALM,
			 (const unsigned char *)realm, (unsigned)strlen(realm));
		put_attr(out, &off, ATTR_NONCE, nonce, nonce_len);
		size_t mi_off = off;
		off += 24;                       /* reserve MI */
		unsigned char mac[20];
		stun_hmac(out, mi_off, key, 16, mac);
		out[off - 24]     = 0x00;
		out[off - 24 + 1] = 0x08;
		out[off - 24 + 2] = 0x00;
		out[off - 24 + 3] = 20;
		memcpy(out + off - 20, mac, 20);
	}

	out[2] = (unsigned char)(((off - STUN_HDR_LEN) >> 8) & 0xff);
	out[3] = (unsigned char)((off - STUN_HDR_LEN) & 0xff);
	return off;
}

static void print_xor_address(const unsigned char *v, const char *what)
{
	if (v[1] != 0x01) {                      /* IPv4 only, as everywhere here */
		printf("  %s: (non-IPv4 family 0x%02x, skipped)\n", what, v[1]);
		return;
	}
	unsigned int ip = ((unsigned)v[4] << 24 | (unsigned)v[5] << 16 |
			   (unsigned)v[6] << 8 | v[7]) ^ STUN_MAGIC;
	unsigned short port = (unsigned short)(((v[2] << 8) | v[3]) ^ (STUN_MAGIC >> 16));
	struct in_addr a = { .s_addr = htonl(ip) };
	char s[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &a, s, sizeof(s));
	printf("  %s: %s:%u\n", what, s, port);
}

static int talk(int fd, const struct sockaddr_in *srv,
		unsigned char *req, size_t reqlen, unsigned char *resp,
		size_t cap, const unsigned char txid[12])
{
	sendto(fd, req, reqlen, 0, (const struct sockaddr *)srv, sizeof(*srv));
	for (;;) {
		ssize_t r = recvfrom(fd, resp, cap, 0, NULL, NULL);
		if (r <= 0)
			return -1;                   /* timeout */
		if ((size_t)r >= STUN_HDR_LEN + 12 &&
		    memcmp(resp + 8, txid, 12) == 0)
			return (int)r;               /* correlated by txid */
	}
}

int main(int argc, char **argv)
{
	const char *host = argc > 1 ? argv[1] : "127.0.0.1";
	int port = argc > 2 ? atoi(argv[2]) : 3478;
	const char *user = argc > 3 ? argv[3] : "c-prep-user";
	const char *pass = argc > 4 ? argv[4] : "c-prep-pass";
	setvbuf(stdout, NULL, _IOLBF, 0);

	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in srv;
	memset(&srv, 0, sizeof(srv));
	srv.sin_family = AF_INET;
	srv.sin_port = htons((unsigned short)port);
	if (inet_pton(AF_INET, host, &srv.sin_addr) != 1) {
		fprintf(stderr, "turn: bad address %s\n", host);
		return 2;
	}
	struct timeval tv = { 3, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	unsigned char req[1024], resp[2048];
	unsigned char txid[12];
	for (int i = 0; i < 12; i++)
		txid[i] = (unsigned char)(rand() & 0xff);

	/* ---- step 1: unauthenticated Allocate (expects 401 + realm + nonce) */
	size_t len = build_allocate(req, txid, 0, NULL, NULL, NULL, 0, NULL);
	printf("turn: -> Allocate (no credentials) to %s:%d\n", host, port);
	ssize_t r = talk(fd, &srv, req, len, resp, sizeof(resp), txid);
	if (r < 0) {
		fprintf(stderr, "turn: no response from %s:%d — is coturn "
			"running? (without a TURN server this client exits "
			"cleanly; that is the tested state here)\n", host, port);
		close(fd);
		return 2;
	}
	unsigned short type = (unsigned short)((resp[0] << 8) | resp[1]);
	if (type != TURN_ALLOCATE_ERROR) {
		fprintf(stderr, "turn: unexpected response type 0x%04x\n", type);
		close(fd);
		return 2;
	}
	const unsigned char *v; unsigned alen;
	unsigned short err = 0;
	if (find_attr(resp, (size_t)r, ATTR_ERROR_CODE, &v, &alen))
		err = (unsigned short)((v[2] & 0x07) * 100 + v[3]);
	char realm[128] = { 0 }, nonce[256] = { 0 };
	if (find_attr(resp, (size_t)r, ATTR_REALM, &v, &alen))
		memcpy(realm, v, alen < sizeof(realm) - 1 ? alen : sizeof(realm) - 1);
	if (find_attr(resp, (size_t)r, ATTR_NONCE, &v, &alen))
		memcpy(nonce, v, alen < sizeof(nonce) - 1 ? alen : sizeof(nonce) - 1);
	printf("turn: <- error %u, realm='%s', nonce=%u bytes\n",
	       err, realm, (unsigned)strlen(nonce));
	if (err != 401) {
		fprintf(stderr, "turn: expected 401 challenge\n");
		close(fd);
		return 2;
	}

	/* ---- step 2: authenticated Allocate with the long-term key ---- */
	unsigned char key[16];
	long_term_key(user, realm, pass, key);
	for (int i = 0; i < 12; i++)
		txid[i] = (unsigned char)(rand() & 0xff);
	len = build_allocate(req, txid, 1, user, realm,
			     (const unsigned char *)nonce,
			     (unsigned)strlen(nonce), key);
	printf("turn: -> Allocate (authenticated as '%s')\n", user);
	r = talk(fd, &srv, req, len, resp, sizeof(resp), txid);
	if (r < 0) {
		fprintf(stderr, "turn: timeout waiting for Allocate result\n");
		close(fd);
		return 2;
	}
	type = (unsigned short)((resp[0] << 8) | resp[1]);
	if (type == TURN_ALLOCATE_ERROR) {
		err = 0;
		if (find_attr(resp, (size_t)r, ATTR_ERROR_CODE, &v, &alen))
			err = (unsigned short)((v[2] & 0x07) * 100 + v[3]);
		fprintf(stderr, "turn: Allocate REJECTED with %u (wrong creds? "
			"expired time-limited username? — the break-it demo)\n", err);
		close(fd);
		return 1;
	}

	/* ---- step 3: show what your backend would put into the SDP ---- */
	printf("turn: ALLOCATE SUCCESS — relay binding created:\n");
	if (find_attr(resp, (size_t)r, ATTR_XOR_RELAYED_ADDR, &v, &alen) && alen == 8)
		print_xor_address(v, "relayed candidate (a=candidate ... typ relay)");
	if (find_attr(resp, (size_t)r, ATTR_XOR_MAPPED_ADDR, &v, &alen) && alen == 8)
		print_xor_address(v, "mapped (our reflexive address)");
	if (find_attr(resp, (size_t)r, ATTR_LIFETIME, &v, &alen) && alen == 4)
		printf("  lifetime: %u s (Refresh must arrive before expiry)\n",
		       ((unsigned)v[0] << 24 | (unsigned)v[1] << 16 |
			(unsigned)v[2] << 8 | v[3]));
	printf("turn: done. In production your signaling hands this relayed\n"
	       "      address to the peer as an ICE candidate (bite 72 relays it).\n");
	close(fd);
	return 0;
}
