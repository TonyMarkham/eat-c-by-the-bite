/*
 * Bite 66 — STUN by hand: your first true WebRTC wire protocol
 *
 * We speak raw RFC 5389 STUN over UDP to a public server and learn the
 * (IP, port) that the INTERNET sees us on. This discovery is the core of
 * ICE: each candidate pair gets checked using exactly these packets.
 *
 * STUN message header (20 bytes):
 *
 *      0                   1                   2                   3
 *      0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *     ++-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     |0|     STUN Message Type       |         Message Length        |
 *     ++-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     |                Magic Cookie  =  0x2112A442                    |
 *     ++-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     |               96-bit Transaction ID (12 random bytes)         |
 *     ++-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 *   Message type 0x0001 = Binding Request; 0x0101 = Binding Success
 *   Response. The top two bits of EVERY STUN message are 0 — that's how a
 *   multiplexed port tells STUN apart from RTP (top two bits of an RTP
 *   header are 1,0). Remember that when bite 74 multiplexes them.
 *
 *   Magic cookie: fixed value. It exists because STUN grew up twice: in
 *   classic RFC 3489 the 32 bits here were random; RFC 5389 fixed them to
 *   0x2112A442 ("STUN" is not a coincidence: 0x2112 = 'S''T' bits...) and
 *   reuses these 4 bytes as XOR key material for obfuscated fields.
 *
 *   Transaction ID: 12 random bytes WE choose. The response must echo it.
 *   It is how we match answers to questions on a connectionless transport
 *   — and the anti-spoofing check: an attacker who can see our request but
 *   can't predict the ID can't feed us a fake mapped address... unless we
 *   SKIP the check (see BREAK IT ON PURPOSE).
 *
 * Attributes follow the header, each:
 *
 *     [type:2][length:2][value ...padded to a 4-byte boundary]
 *
 * We parse XOR-MAPPED-ADDRESS (0x0020). Its value:
 *
 *     [0x00][family=0x01 (IPv4)][port XOR 0x2112][ip[4] XOR cookie]
 *
 * THE XOR DANCE: the port is XORed with the top 16 bits of the magic
 * cookie (0x2112), and each IP byte with the cookie's bytes in order
 * (21,12,A4,42). Why? NATs (and middleboxes) have a documented history of
 * mangling literal IP addresses that appear in packet PAYLOADS (trying to
 * "help" rewrite them). XORed with an apparently-random cookie they no
 * longer look like addresses, so they survive the trip intact. The server
 * picked the cookie so the client can un-XOR with the same bytes.
 *
 * Compile + run (from the repo root, in WSL):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/66-stun/stun.c \
 *       -o /tmp/b66 -lssl -lcrypto
 *   /tmp/b66                                  # default server, 4 attempts
 *   /tmp/b66 stun.l.google.com 19302
 *   /tmp/b66 --no-txid-check                  # the break-it exercise
 *
 * Verify independently (optional):
 *   sudo apt install stun-client && stunclient stun.l.google.com --port 19302
 *   (or open any "what is my IP" page: the MAPPED IP must agree; the port
 *   may differ if you test from a different socket)
 *
 * BREAK IT ON PURPOSE (from phases/phase-6-rt-media.md):
 *   Mismatch the transaction id on purpose — run with --no-txid-check, or
 *   better: sabotage the code by deleting the memcmp(txid, res_txid, 12)
 *   and rebuild. The program now accepts ANY Binding Response that arrives.
 *   Now imagine an off-path attacker who guesses (or races) a response
 *   claiming your mapped address is HIS machine: we'd happily aim our
 *   media at the attacker. The txid check IS the defense — and it costs
 *   one memcmp. Real ICE stacks additionally require the source IP:port of
 *   the response to match the request's destination.
 *
 * Done when: your C program prints your public IP:port from a public STUN
 * server, verified against an independent tool.
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <netdb.h>
#include <openssl/rand.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define STUN_HEADER_LEN 20
#define MAGIC_COOKIE 0x2112A442u
#define STUN_BINDING_REQUEST 0x0001
#define STUN_BINDING_RESPONSE 0x0101
#define ATTR_XOR_MAPPED_ADDRESS 0x0020
#define ATTR_MAPPED_ADDRESS 0x0001 /* plaintext fallback, for comparison */
#define RESPONSE_TIMEOUT_MS 1000
#define MAX_ATTEMPTS 4

static void die(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

static void hex_print(const char *label, const uint8_t *buf, size_t len)
{
    printf("%-18s", label);
    for (size_t i = 0; i < len; i++)
        printf("%02x", buf[i]);
    printf("\n");
}

int main(int argc, char **argv)
{
    const char *server = "stun.l.google.com";
    const char *port = "19302";
    int check_txid = 1;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--no-txid-check"))
            check_txid = 0;
        else if (!strcmp(argv[i], "--help")) {
            printf("usage: %s [--no-txid-check] [host] [port]\n", argv[0]);
            return 0;
        } else if (i == 1 && strcmp(argv[i], "--no-txid-check") != 0 &&
                   argv[1][0] != '-') {
            server = argv[i];
        } else if (i == 2) {
            port = argv[i];
        }
    }

    /* --- resolve the server (UDP, so this is just getaddrinfo work) --- */
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;       /* IPv4 keeps the XOR example simple */
    hints.ai_socktype = SOCK_DGRAM;  /* STUN rides on UDP */
    struct addrinfo *res = NULL;
    if (getaddrinfo(server, port, &hints, &res) != 0 || !res) {
        fprintf(stderr, "cannot resolve %s:%s\n", server, port);
        return 1;
    }
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0)
        die("socket");
    struct timeval tv = { .tv_sec = RESPONSE_TIMEOUT_MS / 1000,
                          .tv_usec = (RESPONSE_TIMEOUT_MS % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    /* --- build the Binding Request --- */
    uint8_t txid[12];
    if (RAND_bytes(txid, sizeof txid) != 1)
        die("RAND_bytes"); /* unpredictable txid = the anti-spoof anchor */

    uint8_t req[STUN_HEADER_LEN];
    req[0] = 0x00; req[1] = (uint8_t)(STUN_BINDING_REQUEST & 0xff); /* type */
    req[2] = 0x00; req[3] = 0x00;             /* length: no attributes */
    req[4] = (uint8_t)(MAGIC_COOKIE >> 24);   /* 0x21 */
    req[5] = (uint8_t)(MAGIC_COOKIE >> 16);   /* 0x12 */
    req[6] = (uint8_t)(MAGIC_COOKIE >> 8);    /* 0xA4 */
    req[7] = (uint8_t)(MAGIC_COOKIE);         /* 0x42 */
    memcpy(req + 8, txid, 12);

    hex_print("request header:", req, sizeof req);
    printf("type=0x%04x len=%u cookie=0x%08x txid=12 random bytes\n\n",
           STUN_BINDING_REQUEST, 0u, MAGIC_COOKIE);

    uint8_t resp[1024];
    int got = 0;
    for (int attempt = 1; attempt <= MAX_ATTEMPTS && !got; attempt++) {
        /* New txid per attempt: every question needs its own unpredictable
         * id — reusing one lets a replayed old response look fresh. */
        if (attempt > 1) {
            if (RAND_bytes(txid, sizeof txid) != 1)
                die("RAND_bytes");
            memcpy(req + 8, txid, 12);
        }
        if (sendto(fd, req, sizeof req, 0, res->ai_addr, res->ai_addrlen) < 0) {
            perror("sendto");
            freeaddrinfo(res);
            close(fd);
            return 1;
        }
        printf("attempt %d: sent %zu-byte Binding Request to %s:%s\n",
               attempt, sizeof req, server, port);

        ssize_t n = recvfrom(fd, resp, sizeof resp, 0, NULL, NULL);
        if (n < 0) {
            printf("          ...timeout, retrying (UDP: no reply is not an "
                   "error, it's silence)\n");
            continue;
        }
        got = 1;
        printf("          received %zd bytes\n", n);
    }
    freeaddrinfo(res);
    if (!got)
        die("no response from STUN server (offline? blocked UDP? try another "
            "server, e.g. stun.cloudflare.com 3478)");

    /* --- parse and validate the response -------------------------------- */
    if (resp[0] >> 6)
        die("top two bits are not 00: not a STUN message");
    uint16_t rtype = (uint16_t)((resp[0] << 8) | resp[1]);
    uint16_t rlen = (uint16_t)((resp[2] << 8) | resp[3]);
    uint32_t cookie = ((uint32_t)resp[4] << 24) | ((uint32_t)resp[5] << 16) |
                      ((uint32_t)resp[6] << 8) | (uint32_t)resp[7];
    const uint8_t *rtxid = resp + 8;

    printf("\nresponse: type=0x%04x len=%u cookie=0x%08x\n", rtype, rlen,
           cookie);
    if (rtype != STUN_BINDING_RESPONSE)
        die("not a Binding Success Response (error response? 0x0111)");
    if (cookie != MAGIC_COOKIE)
        die("magic cookie mismatch: not talking to who we think we are");
    if ((size_t)STUN_HEADER_LEN + rlen > sizeof resp || (rlen % 4) != 0)
        die("bad attribute length");

    if (check_txid) {
        if (memcmp(txid, rtxid, 12) != 0)
            die("TRANSACTION ID MISMATCH: forged/spoofed/stale response — "
                "dropping it. (This one memcmp defeats response spoofing.)");
        printf("txid matches our request: this answer is OURS\n");
    } else {
        printf("!! --no-txid-check: accepting ANY response — the spoofing\n"
               "   hole the BREAK IT section warns about. An off-path\n"
               "   attacker who races a forged response now owns our\n"
               "   believed address.\n");
    }

    /* --- walk attributes, find XOR-MAPPED-ADDRESS ------------------------ */
    const uint8_t *p = resp + STUN_HEADER_LEN;
    const uint8_t *end = resp + STUN_HEADER_LEN + rlen;
    int found = 0;

    while (p + 4 <= end) {
        uint16_t atype = (uint16_t)((p[0] << 8) | p[1]);
        uint16_t alen = (uint16_t)((p[2] << 8) | p[3]);
        const uint8_t *val = p + 4;
        if (val + alen > end)
            die("attribute overruns message — malformed response");
        p = val + alen + (uint16_t)((4 - (alen % 4)) % 4); /* skip padding */

        if (atype != ATTR_XOR_MAPPED_ADDRESS || alen < 8)
            continue;

        /* value: [rsv=0][family][x-port:2][x-ip:4] */
        uint8_t family = val[1];
        if (family != 0x01)
            die("expected IPv4 (family 0x01) from this server");

        /* THE XOR DANCE, un-done: */
        uint16_t xport = (uint16_t)((val[2] << 8) | val[3]);
        uint16_t port = (uint16_t)(xport ^ (uint16_t)(MAGIC_COOKIE >> 16));
        uint8_t ip[4];
        ip[0] = val[4] ^ (uint8_t)(MAGIC_COOKIE >> 24);
        ip[1] = val[5] ^ (uint8_t)(MAGIC_COOKIE >> 16);
        ip[2] = val[6] ^ (uint8_t)(MAGIC_COOKIE >> 8);
        ip[3] = val[7] ^ (uint8_t)(MAGIC_COOKIE);

        char ipstr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, ip, ipstr, sizeof ipstr);
        printf("\nXOR-MAPPED-ADDRESS: %s:%u\n", ipstr, port);
        printf("-> that is how the public internet sees THIS socket. In\n"
           "   WebRTC this (IP, port) becomes a server-reflexive (srflx)\n"
           "   ICE candidate, gathered right here.\n");
        found = 1;
        break;
    }
    if (!found)
        die("no XOR-MAPPED-ADDRESS attribute in response");
    close(fd);
    return 0;
}
