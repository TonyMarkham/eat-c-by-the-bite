/* bite 73 — SDP Literacy: parse an SDP offer into the fields a backend needs.
 *
 * COMPILE + RUN (inside WSL, from the repo root):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/73-sdp/sdp_parse.c -o /tmp/b73
 *   /tmp/b73 bites/73-sdp/sample-offer.sdp
 *   /tmp/b73 bites/73-sdp/sample-offer-corrupted.sdp   # <- the tripwire demo
 *   echo "not sdp at all" > /tmp/garbage.sdp && /tmp/b73 /tmp/garbage.sdp
 *
 * WHY THIS BITE EXISTS
 *   An SDP blob looks like noise until you read it as what it is: a list of
 *   `key=value` lines. Your backend will never *write* SDP (browsers do),
 *   but it must READ four things out of every offer/answer:
 *
 *     m=<media> <port> <proto> <fmt list>   -> what media, which transport
 *                                              (UDP/TLS/RTP/SAVPF means
 *                                              "DTLS-secured SRTP" — bites 75/76)
 *     a=fingerprint:<algo> <hex pairs>      -> SHA-256 of the peer's DTLS
 *                                              certificate. THE input to bite
 *                                              75's certificate check; also
 *                                              THE tripwire planted here.
 *     a=ice-ufrag / a=ice-pwd               -> STUN short-term credentials.
 *                                              The pwd is the HMAC key for the
 *                                              connectivity checks in bite 74.
 *     a=candidate:...                       -> where the peer can be reached;
 *                                              the server's candidates are what
 *                                              your ICE-lite responder listens
 *                                              on (bite 74).
 *
 *   Offer/answer semantics in one line: one side proposes (m= lines with
 *   payload types + setup:actpass), the other selects (setup:active/passive,
 *   subset of payload types). The backend mostly observes this exchange.
 *
 * BREAK IT ON PURPOSE:
 *   sample-offer-corrupted.sdp has one hex digit flipped (8E -> 0X). The
 *   parser must catch that: a fingerprint is a promise about a future
 *   handshake; garbage in the promise must fail at PARSE time, not at
 *   DTLS time 30 seconds later. KEEP this corrupted file — bite 75's server
 *   uses it as the negative test: hand it the corrupted fingerprint and the
 *   handshake must abort.
 */

#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE      2048
#define MAX_CANDS     16
#define FP_HEX_LEN    64        /* SHA-256 fingerprint = 64 hex chars = 32 bytes */

/* The struct bite 80 will carry from "SDP arrived" to "media flowing". */
typedef struct {
	char     media[16];               /* "audio" / "video"                      */
	int      port;                    /* m= port (9 = placeholder in WebRTC)    */
	char     proto[32];               /* "UDP/TLS/RTP/SAVPF"                    */
	char     fmtlist[64];             /* payload types offered: "0 111"         */
	char     fp_algo[16];             /* "sha-256"                              */
	unsigned char fp[32];             /* decoded certificate hash               */
	int      fp_have;                 /* 0 = no fingerprint parsed yet          */
	char     ice_ufrag[64];
	char     ice_pwd[128];
	struct {
		char foundation[32];
		char ip[48];
		int  port;
		char type[16];                /* host / srflx / relay / prflx          */
	} cand[MAX_CANDS];
	int      ncand;
} sdp_info_t;

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* Parse "a=fingerprint:sha-256 AB:CD:..". Returns 0 ok, -1 malformed.
 * Strictness here is deliberate: this value is later compared byte-for-byte
 * against a certificate hash, so it must be exactly 32 clean bytes. */
static int parse_fingerprint(const char *val, sdp_info_t *s)
{
	char algo[16] = { 0 };
	int a = 0;
	while (*val && !isspace((unsigned char)*val) && a < (int)sizeof(algo) - 1)
		algo[a++] = *val++;
	while (*val && isspace((unsigned char)*val))
		val++;

	unsigned char out[32];
	int nibbles = 0, byte = 0, outn = 0;
	for (const char *p = val; *p; p++) {
		if (*p == ':')
			continue;                    /* separators are cosmetic */
		if (outn >= 32)
			return -1;                   /* too long: not SHA-256 */
		int h = hexval(*p);
		if (h < 0)
			return -1;                   /* THE TRIPWIRE: non-hex char */
		byte = (byte << 4) | h;
		if (++nibbles == 2) {
			out[outn++] = (unsigned char)byte;
			nibbles = 0;
			byte = 0;
		}
	}
	if (nibbles != 0 || outn != 32)
		return -1;                           /* truncated fingerprint */
	snprintf(s->fp_algo, sizeof(s->fp_algo), "%s", algo);
	memcpy(s->fp, out, 32);
	s->fp_have = 1;
	return 0;
}

/* a=candidate:<foundation> <component> <transport> <priority> <ip> <port> typ <type> ... */
static void parse_candidate(const char *val, sdp_info_t *s)
{
	if (s->ncand >= MAX_CANDS)
		return;
	char foundation[32] = { 0 }, ip[48] = { 0 }, type[16] = { 0 };
	int port = 0;
	/* fields: foundation comp transport priority ip port [typ type ...] */
	if (sscanf(val, "%31s %*d %*s %*d %47s %d", foundation, ip, &port) < 3)
		return;
	const char *typ = strstr(val, "typ ");
	if (typ)
		sscanf(typ + 4, "%15s", type);
	snprintf(s->cand[s->ncand].foundation, sizeof(s->cand[0].foundation), "%s", foundation);
	snprintf(s->cand[s->ncand].ip, sizeof(s->cand[0].ip), "%s", ip);
	s->cand[s->ncand].port = port;
	snprintf(s->cand[s->ncand].type, sizeof(s->cand[0].type), "%s", type);
	s->ncand++;
}

/* sdp_extract: the function the "Do" list names. Feeds every line through the
 * same switch on the SDP line type character — SDP is just lines, remember. */
int sdp_extract(const char *sdp, size_t len, sdp_info_t *s)
{
	memset(s, 0, sizeof(*s));
	const char *p = sdp, *end = sdp + len;
	char line[MAX_LINE];

	while (p < end) {
		size_t n = 0;
		while (p < end && *p != '\n' && n < sizeof(line) - 1)
			line[n++] = *p++;
		if (p < end && *p == '\n')
			p++;
		line[n] = '\0';
		if (n && line[n - 1] == '\r')
			line[--n] = '\0';
		if (n < 2 || line[1] != '=')
			continue;                /* not key=value: skip silently (robustness) */

		const char *val = line + 2;
		switch (line[0]) {
		case 'm': {
			/* m=audio 9 UDP/TLS/RTP/SAVPF 0 111 */
			char media[16] = { 0 }, proto[32] = { 0 }, fmt[64] = { 0 };
			int port = 0;
			if (sscanf(val, "%15s %d %31s %63[^\r\n]", media, &port, proto, fmt) >= 3) {
				snprintf(s->media, sizeof(s->media), "%s", media);
				s->port = port;
				snprintf(s->proto, sizeof(s->proto), "%s", proto);
				snprintf(s->fmtlist, sizeof(s->fmtlist), "%s", fmt);
			} else {
				return -1;
			}
			break;
		}
		case 'a': {
			if (!strncmp(val, "fingerprint:", 12)) {
				if (parse_fingerprint(val + 12, s) != 0)
					return -1;   /* malformed fingerprint = hard error */
			} else if (!strncmp(val, "ice-ufrag:", 10)) {
				snprintf(s->ice_ufrag, sizeof(s->ice_ufrag), "%.63s", val + 10);
			} else if (!strncmp(val, "ice-pwd:", 8)) {
				snprintf(s->ice_pwd, sizeof(s->ice_pwd), "%.127s", val + 8);
			} else if (!strncmp(val, "candidate:", 10)) {
				parse_candidate(val + 10, s);
			}
			break;
		}
		default:
			break;                   /* v=, o=, s=, t=, c=, b= ... ignored */
		}
	}

	/* Post-conditions: an offer without credentials or a fingerprint cannot
	 * be used by ANY later bite, so fail now, loudly. */
	if (!s->fp_have || !s->ice_ufrag[0] || !s->ice_pwd[0])
		return -1;
	return 0;
}

static void print_info(const sdp_info_t *s, const char *path)
{
	printf("== %s\n", path);
	printf("media        : %-5s port=%-3d proto=%-20s fmts=%s\n",
	       s->media, s->port, s->proto, s->fmtlist);
	printf("fingerprint  : %s ", s->fp_algo);
	for (int i = 0; i < 32; i++)
		printf("%02X%s", s->fp[i], i == 31 ? "\n" : ":");
	printf("ice-ufrag    : %s   <- verified inside STUN USERNAME in bite 74\n", s->ice_ufrag);
	printf("ice-pwd      : %s   <- HMAC key for bite 74 checks / bite 79 secret\n", s->ice_pwd);
	printf("candidates   : %d\n", s->ncand);
	for (int i = 0; i < s->ncand; i++)
		printf("  [%d] %-3s %-15s %-5d (foundation %s)\n", i,
		       s->cand[i].type, s->cand[i].ip, s->cand[i].port,
		       s->cand[i].foundation);
	printf("where each field goes:\n");
	printf("  fingerprint  -> bite 75: SSL_get_peer_certificate + SHA-256 == this\n");
	printf("  ice-ufrag    -> bite 74: must appear in USERNAME 'ufrag:ufrag'\n");
	printf("  ice-pwd      -> bite 74: key of MESSAGE-INTEGRITY (HMAC-SHA1)\n");
	printf("  candidate ip -> bite 74: the UDP port your media socket serves\n");
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <file.sdp>\n", argv[0]);
		return 2;
	}
	FILE *f = fopen(argv[1], "rb");
	if (!f) {
		perror("fopen");
		return 2;
	}
	static char buf[64 * 1024];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';

	sdp_info_t s;
	if (sdp_extract(buf, n, &s) != 0) {
		fprintf(stderr, "== %s\nPARSE FAILED: malformed SDP "
			"(bad fingerprint hex / missing credentials / garbage)\n", argv[1]);
		return 1;
	}
	print_info(&s, argv[1]);
	return 0;
}
