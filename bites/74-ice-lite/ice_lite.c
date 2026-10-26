/* bite 74 — ICE-lite Responder: answer STUN connectivity checks on the media port.
 *
 * COMPILE + RUN (inside WSL, from the repo root):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/74-ice-lite/ice_lite.c \
 *       -o /tmp/b74 -lcrypto
 *   # terminal 1:
 *   /tmp/b74 --port 50000
 *   # terminal 2 (the local test double for "a browser peer"):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/74-ice-lite/check_client.c \
 *       -o /tmp/b74client -lcrypto
 *   timeout 10 /tmp/b74client 127.0.0.1 50000
 *   # negative demo (the break-it, done FOR you by the flag):
 *   /tmp/b74 --port 50000 --bad-mac &   then rerun the client: check 1 must FAIL
 *
 * WHY THIS EXISTS
 *   After signaling (bites 71-73) both sides know *where* to talk: the SDP's
 *   a=candidate lines. But knowing an address is not being reachable — ICE
 *   proves reachability with STUN Binding requests that carry credentials.
 *   Your backend is an ICE-LITE agent (RFC 8445 s2.5): it never originates
 *   checks, it only answers them. That answer is the first packet your
 *   server sends that a browser will accept as "this peer exists".
 *
 * THE RFC DETAILS THAT MAKE OR BREAK THIS BITE
 *   - Request from the browser: USERNAME = "<server ufrag>:<browser ufrag>"
 *     (RFC 8445 s7.2.2: "username formed by concatenating the username
 *     fragment provided by the PEER ... with the username fragment of the
 *     agent sending the request"). The password used to HMAC the request is
 *     OUR ice-pwd (the peer's password, from the sender's perspective).
 *     => we validate with our own pwd, and sign our response with the same
 *     pwd. Getting this wrong is the #1 ICE bug; the test client will tell you.
 *   - MESSAGE-INTEGRITY input: the STUN message UP TO AND INCLUDING the
 *     MESSAGE-INTEGRITY attribute header, with the header's length field
 *     temporarily adjusted to end at that attribute (FINGERPRINT, which
 *     trails it, is excluded). This footgun is the bite's real exam.
 *   - Failures must get error RESPONSES (400/401), never silence: silence
 *     breaks ICE timers and you cannot tell "dropped" from "wrong".
 *   - Demux: this same UDP port later carries DTLS (bite 75) and SRTP
 *     (bites 76/77). STUN is recognized by first byte 0x00..0x03 AND magic
 *     cookie 0x2112A442 at offset 4.
 *
 * BREAK IT ON PURPOSE:
 *   --bad-mac: the responder signs responses with a WRONG key. The client's
 *   check of the response MESSAGE-INTEGRITY fails -> "ICE would never
 *   connect". Now remove the flag: the exact same code path succeeds. That
 *   asymmetry is what a browser experiences when your HMAC input is wrong —
 *   except the browser shows you nothing but "icegatheringstate: failed",
 *   which is why we built the test double that CAN report the difference.
 */

#define _POSIX_C_SOURCE 200809L
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ------------------------------ STUN constants ------------------------------ */
#define STUN_MAGIC            0x2112A442u
#define STUN_HDR_LEN          20
#define STUN_METHOD_BINDING   0x001
#define STUN_TYPE_REQUEST     0x000
#define STUN_TYPE_INDICATION  0x010
#define STUN_SUCCESS(resp)    ((resp) | 0x100)   /* 0x001 -> 0x101 */
#define STUN_ERROR(resp)      ((resp) | 0x110)   /* 0x001 -> 0x111 */

enum {
	ATTR_MAPPED_ADDRESS      = 0x0001,
	ATTR_USERNAME            = 0x0006,
	ATTR_MESSAGE_INTEGRITY   = 0x0008,
	ATTR_ERROR_CODE          = 0x0009,
	ATTR_XOR_MAPPED_ADDRESS  = 0x0020,
	ATTR_PRIORITY            = 0x0024,
	ATTR_USE_CANDIDATE       = 0x0025,
	ATTR_FINGERPRINT         = 0x8028,
	ATTR_ICE_CONTROLLED      =0x8029,
};

/* Default credentials — in bite 80 these come from the SDP this server
 * "would have sent" (a=ice-ufrag / a=ice-pwd, bite 73). */
#define SERVER_UFRAG "srvUfrag77"
#define SERVER_PWD   "serverIcePwdDemo42"   /* >= 24 chars per RFC 8445 s16.4 */

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* -------------------------------- crc32 (for FINGERPRINT) -------------------- */
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

/* --------------------------- attribute walk helpers ------------------------- */
typedef struct {
	unsigned short type;
	const unsigned char *value;
	unsigned short len;
} attr_t;

/* Iterate TLVs starting at offset 20. Returns index+1 of found attr, 0 if none. */
static int find_attr(const unsigned char *msg, size_t len, unsigned short want, attr_t *out)
{
	size_t off = STUN_HDR_LEN;
	unsigned short plen = (unsigned short)((msg[2] << 8) | msg[3]);
	size_t end = STUN_HDR_LEN + plen;
	if (end > len)
		end = len;
	int idx = 0;
	while (off + 4 <= end) {
		unsigned short type = (unsigned short)((msg[off] << 8) | msg[off + 1]);
		unsigned short alen = (unsigned short)((msg[off + 2] << 8) | msg[off + 3]);
		if (off + 4 + alen > end)
			break;                        /* truncated: caller's problem */
		attr_t a = { type, msg + off + 4, alen };
		if (type == want && out) {
			*out = a;
			return idx + 1;
		}
		idx++;
		off += 4 + (size_t)((alen + 3) & ~3u);  /* value is 4-byte padded */
	}
	return 0;
}

/* HMAC-SHA1 over the STUN message for MESSAGE-INTEGRITY. RFC 5389 s15.4:
 * "The text used as input to HMAC is the STUN message, including the header,
 * up to and including the attribute PRECEDING the MESSAGE-INTEGRITY attribute."
 * So the input EXCLUDES the MI attribute itself (header AND value), but the
 * header's length field is adjusted to end at the (virtual) end of the MI
 * attribute, i.e. mi_attr_off + 24. Getting either half wrong = silent 401s.
 * This footgun is the bite's real exam. */
static void stun_hmac_sha1(const unsigned char *msg, size_t mi_attr_off,
			   const char *key, unsigned char out[20])
{
	unsigned char tmp[STUN_HDR_LEN + 1500];
	memcpy(tmp, msg, mi_attr_off);
	size_t plen = mi_attr_off + 24 - STUN_HDR_LEN;   /* MI attr = 4 hdr + 20 val */
	tmp[2] = (unsigned char)((plen >> 8) & 0xff);
	tmp[3] = (unsigned char)(plen & 0xff);
	unsigned int outlen = 20;
	HMAC(EVP_sha1(), key, (int)strlen(key), tmp, mi_attr_off, out, &outlen);
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
	while (*off & 3)                      /* pad to 4 bytes with zeros */
		buf[(*off)++] = 0;
}

static void put_u32(unsigned char *p, unsigned int v)
{
	p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}

/* XOR-MAPPED-ADDRESS (0x0020): port ^ magic>>16, ip ^ magic. */
static void put_xor_mapped(unsigned char *buf, size_t *off, const struct sockaddr_in *sa)
{
	unsigned char val[8] = { 0, 0x01, 0, 0, 0, 0, 0, 0 };
	unsigned short port = (unsigned short)ntohs(sa->sin_port);
	unsigned int ip = ntohl(sa->sin_addr.s_addr);
	unsigned short xport = (unsigned short)(port ^ (STUN_MAGIC >> 16));
	val[2] = (unsigned char)(xport >> 8);
	val[3] = (unsigned char)(xport & 0xff);
	put_u32(val + 4, ip ^ STUN_MAGIC);
	put_attr(buf, off, ATTR_XOR_MAPPED_ADDRESS, val, 8);
}

/* Build a full Binding response/error with MESSAGE-INTEGRITY (+FINGERPRINT).
 * good_mac=0 for the --bad-mac break-it mode. Returns final packet length. */
static size_t build_response(unsigned char *out, unsigned short resp_type,
			     const unsigned char txid[12],
			     const struct sockaddr_in *mapped,
			     unsigned short err_num,
			     int bad_mac)
{
	size_t off = 0;

	/* header (length patched at the end) */
	out[0] = (unsigned char)(resp_type >> 8);
	out[1] = (unsigned char)(resp_type & 0xff);
	/* magic + txid */
	put_u32(out + 4, STUN_MAGIC);
	memcpy(out + 8, txid, 12);
	off = STUN_HDR_LEN;

	if (resp_type == STUN_ERROR(STUN_METHOD_BINDING)) {
		/* ERROR-CODE value: 2 reserved bytes, class digit, number mod 100 */
		char *reason = (err_num == 401) ? (char *)"Unauthorized" : (char *)"Bad Request";
		size_t rl = strlen(reason);
		unsigned char ecattr[4 + 128];
		ecattr[0] = ecattr[1] = 0;
		ecattr[2] = (unsigned char)(err_num / 100);   /* class: 4xx -> 4 */
		ecattr[3] = (unsigned char)(err_num % 100);   /* code:  01 for 401 */
		memcpy(ecattr + 4, reason, rl);
		put_attr(out, &off, ATTR_ERROR_CODE, ecattr, (unsigned short)(4 + rl));
	} else {
		put_xor_mapped(out, &off, mapped);
	}

	/* MESSAGE-INTEGRITY: we must know its end offset before we can sign.
	 * Reserve 24 bytes (4 header + 20 HMAC), sign, then fill. */
	size_t mi_off = off;
	unsigned char zero[20] = { 0 };
	put_attr(out, &off, ATTR_MESSAGE_INTEGRITY, zero, 20);

	unsigned char mac[20];
	if (bad_mac) {
		/* The deliberate break: sign with a key that is not ours.
		 * A conforming peer must reject this response. */
		stun_hmac_sha1(out, mi_off, "not-the-real-password", mac);
	} else {
		stun_hmac_sha1(out, mi_off, SERVER_PWD, mac);
	}
	memcpy(out + mi_off + 4, mac, 20);

	/* FINGERPRINT (0x8028): CRC32 of everything before it, XORed with the
	 * ASCII magic "STUN". The CRC covers the bytes AS TRANSMITTED, so the
	 * length field must already hold its FINAL value (including this
	 * FINGERPRINT attribute: +8 bytes) when it runs. The HMAC above was
	 * unaffected because stun_hmac_sha1 rewrites the length in its private
	 * copy — that asymmetry is worth a long stare; it bites everyone once. */
	out[2] = (unsigned char)(((off + 8 - STUN_HDR_LEN) >> 8) & 0xff);
	out[3] = (unsigned char)((off + 8 - STUN_HDR_LEN) & 0xff);
	unsigned char fp[4];
	put_u32(fp, crc32_stun(out, off) ^ 0x5354554Eu);
	put_attr(out, &off, ATTR_FINGERPRINT, fp, 4);
	return off;
}

/* ------------------------------- packet demux ------------------------------- */
/* STUN's first byte is 0x00..0x03 (message class/method bits); DTLS records
 * start with 0x14..0x17; RTPv2 starts with 0x80. Combined with the magic
 * cookie at offset 4, this one function is the whole media-port demux. */
static int is_stun_packet(const unsigned char *b, size_t len)
{
	return len >= STUN_HDR_LEN &&
	       (b[0] & 0xC0) == 0 &&   /* top two bits zero (RFC 5389 s6) */
	       (b[4] << 24 | b[5] << 16 | b[6] << 8 | b[7]) == STUN_MAGIC;
}

int main(int argc, char **argv)
{
	int port = 50000;
	int bad_mac = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--port") && i + 1 < argc)
			port = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--bad-mac"))
			bad_mac = 1;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("socket");
		return 1;
	}
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons((unsigned short)port);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		perror("bind");
		return 1;
	}
	printf("ice-lite: listening on UDP %d (ufrag '%s', ICE-lite = answer-only)\n",
	       port, SERVER_UFRAG);
	if (bad_mac)
		printf("ice-lite: *** --bad-mac mode: responses are signed WRONG (break-it demo)\n");

	unsigned char rx[2048], tx[2048];
	unsigned long n_ok = 0, n_err = 0, n_dropped = 0;

	while (!g_stop) {
		struct sockaddr_in from;
		socklen_t fromlen = sizeof(from);
		ssize_t r = recvfrom(fd, rx, sizeof(rx), 0,
				     (struct sockaddr *)&from, &fromlen);
		if (r <= 0)
			break;
		size_t len = (size_t)r;

		/* ---- the demux point bite 75 builds on ---- */
		if (!is_stun_packet(rx, len)) {
			n_dropped++;
			printf("ice-lite: non-STUN packet (%zu bytes, first byte 0x%02x)"
			       " — DTLS/SRTP arrive here in bites 75/76\n", len, rx[0]);
			continue;
		}

		unsigned short type = (unsigned short)((rx[0] << 8) | rx[1]);
		/* message class lives in bits 4..5 of the first byte-ish fields:
		 * request=0x000, indication=0x010 on the wire pattern 0x00xx/0x01xx */
		unsigned short cls = (unsigned short)(type & 0x0110);
		if (cls == STUN_TYPE_INDICATION) {
			printf("ice-lite: STUN indication (keepalive) — no response per RFC\n");
			continue;
		}
		if (cls != STUN_TYPE_REQUEST) {
			printf("ice-lite: not a request (type 0x%04x), ignoring\n", type);
			continue;
		}

		const unsigned char *txid = rx + 8;
		char fromstr[64];
		inet_ntop(AF_INET, &from.sin_addr, fromstr, sizeof(fromstr));

		/* --- credential check 1: USERNAME must carry OUR ufrag --- */
		attr_t user;
		size_t resp_len;
		if (!find_attr(rx, len, ATTR_USERNAME, &user)) {
			resp_len = build_response(tx, STUN_ERROR(STUN_METHOD_BINDING), txid,
						  &from, 400, 0);
			printf("ice-lite: %s:%d -> 400 (no USERNAME)\n", fromstr, ntohs(from.sin_port));
			sendto(fd, tx, resp_len, 0, (struct sockaddr *)&from, fromlen);
			n_err++;
			continue;
		}
		char uname[128] = { 0 };
		size_t un = user.len < sizeof(uname) - 1 ? user.len : sizeof(uname) - 1;
		memcpy(uname, user.value, un);
		if (!strstr(uname, SERVER_UFRAG)) {
			/* USERNAME is "<srvUfrag>:<peerUfrag>"; the first part must be us. */
			resp_len = build_response(tx, STUN_ERROR(STUN_METHOD_BINDING), txid,
						  &from, 401, 0);
			printf("ice-lite: %s:%d -> 401 (USERNAME '%s' lacks our ufrag)\n",
			       fromstr, ntohs(from.sin_port), uname);
			sendto(fd, tx, resp_len, 0, (struct sockaddr *)&from, fromlen);
			n_err++;
			continue;
		}

		/* --- credential check 2: MESSAGE-INTEGRITY --- */
		attr_t mi;
		if (!find_attr(rx, len, ATTR_MESSAGE_INTEGRITY, &mi)) {
			resp_len = build_response(tx, STUN_ERROR(STUN_METHOD_BINDING), txid,
						  &from, 401, 0);
			printf("ice-lite: %s:%d -> 401 (no MESSAGE-INTEGRITY)\n",
			       fromstr, ntohs(from.sin_port));
			sendto(fd, tx, resp_len, 0, (struct sockaddr *)&from, fromlen);
			n_err++;
			continue;
		}
		unsigned char expect[20];
		/* RFC 5389 s15.4: HMAC input ends at the attribute PRECEDING
		 * MESSAGE-INTEGRITY, i.e. it excludes the MI attribute TLV itself.
		 * mi.value - 4 = start of the MI attribute header. */
		size_t mi_off = (size_t)(mi.value - rx) - 4;
		stun_hmac_sha1(rx, mi_off, SERVER_PWD, expect);
#ifdef DEBUG
		fprintf(stderr, "ice-lite: mi_attr_off=%zu (HMAC input ends here)\n", mi_off);
		fprintf(stderr, "ice-lite: expect:");
		for (int i = 0; i < 20; i++) fprintf(stderr, " %02x", expect[i]);
		fprintf(stderr, "\nice-lite: got   :");
		for (int i = 0; i < 20; i++) fprintf(stderr, " %02x", mi.value[i]);
		fprintf(stderr, "\nice-lite: pkt:");
		for (size_t i = 0; i < mi_off; i++) fprintf(stderr, " %02x", rx[i]);
		fprintf(stderr, "\n");
#endif
		if (memcmp(expect, mi.value, 20) != 0) {
			resp_len = build_response(tx, STUN_ERROR(STUN_METHOD_BINDING), txid,
						  &from, 401, 0);
			printf("ice-lite: %s:%d -> 401 (HMAC mismatch — wrong pwd or HMAC input)\n",
			       fromstr, ntohs(from.sin_port));
			sendto(fd, tx, resp_len, 0, (struct sockaddr *)&from, fromlen);
			n_err++;
			continue;
		}

		/* --- success --- */
		resp_len = build_response(tx, STUN_SUCCESS(STUN_METHOD_BINDING), txid,
					  &from, 0, bad_mac);
		sendto(fd, tx, resp_len, 0, (struct sockaddr *)&from, fromlen);
		n_ok++;
		printf("ice-lite: %s:%d -> Binding success (txid %02x%02x%02x%02x...)"
		       "  [browser ICE pair would now be 'valid']\n",
		       fromstr, ntohs(from.sin_port),
		       txid[0], txid[1], txid[2], txid[3]);
	}

	printf("ice-lite: stats: %lu successful checks, %lu rejected, %lu non-STUN dropped\n",
	       n_ok, n_err, n_dropped);
	close(fd);
	return 0;
}
