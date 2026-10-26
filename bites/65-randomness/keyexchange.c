/*
 * Bite 65 — Randomness & key agreement: RAND_bytes + X25519 via EVP
 *
 * Two skills:
 *
 *   1. Secrets come from RAND_bytes (a CSPRNG seeded by the kernel pool).
 *      rand()/random() are for games and test fixtures, NEVER keys — their
 *      output is predictable, and predictability of a key is fatal.
 *
 *   2. Diffie–Hellman key agreement. Two parties who have never shared a
 *      secret each pick a random private value, exchange only their PUBLIC
 *      values, and both independently compute the same shared secret. An
 *      eavesdropper who sees both public values still cannot compute it
 *      (that's the discrete-log/CDH hardness assumption). This is,
 *      conceptually, how DTLS agrees the keys SRTP will use.
 *
 * We use X25519 (Curve25519 ECDH) through the high-level EVP_PKEY API —
 * keygen + derive, no DH parameter files, no 1998-era code:
 *
 *   EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, NULL) -> keygen -> two EVP_PKEYs
 *   EVP_PKEY_get_raw_public_key / EVP_PKEY_new_raw_public_key  (transport)
 *   derive_init -> derive_set_peer(peer_pub) -> derive -> 32 shared bytes
 *
 * Modes:
 *   rand   — 32 random bytes, hex; rerun to see it never repeats
 *   local  — two parties in ONE process: derive both secrets, compare
 *   fork   — two real PROCESSES (fork, bite 45!) exchanging public keys
 *            over a pipe; each prints the SHA-256 of its derived secret;
 *            the parent compares them
 *   mismatch — the break-it demo: the child corrupts the peer public key
 *            it received, so the two sides derive DIFFERENT secrets; an
 *            HMAC built from them fails on one side. Key mismatch is
 *            silent garbage — only authentication detects it.
 *
 * Compile + run (from the repo root, in WSL):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/65-randomness/keyexchange.c \
 *       -o /tmp/b65 -lssl -lcrypto
 *   /tmp/b65 rand
 *   /tmp/b65 rand      # different every time
 *   /tmp/b65 local
 *   /tmp/b65 fork
 *   /tmp/b65 mismatch
 *
 * BREAK IT ON PURPOSE (from phases/phase-6-rt-media.md):
 *   Give party B a different keypair on one side — `mismatch` does this by
 *   flipping a byte of the public key B derives against. Derived secrets
 *   differ; an HMAC keyed with each side's secret over the same message no
 *   longer matches. Nothing ERRORS: X25519 derives happily from wrong
 *   input. That silence is why real protocols immediately run a finished/
 *   MAC check over the handshake — bite 67's DTLS Finished message — and
 *   why key agreement without authentication is MITM-able.
 *
 * Done when: two-process key exchange produces matching secrets, and you
 * can explain in 3 sentences what problem DH solves (answer sketch:
 * "both sides compute the same secret from public values alone; the
 * eavesdropper sees the public values but can't invert the one-way
 * function; authentication is still needed so a MITM can't substitute
 * his OWN keys").
 */

#define _POSIX_C_SOURCE 200809L

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define KEY_LEN 32
#define RAW_PUB_LEN 32 /* X25519 public keys are 32 raw bytes */

static void die(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

static void hex_print(const char *label, const uint8_t *buf, size_t len)
{
    printf("%-28s", label);
    for (size_t i = 0; i < len; i++)
        printf("%02x", buf[i]);
    printf("\n");
}

/* ---- X25519 helpers: the whole point is how SMALL this is now ---------- */

static EVP_PKEY *x25519_generate(void)
{
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, NULL);
    if (!ctx)
        die("ctx_new_id failed");
    EVP_PKEY *pkey = NULL;
    if (EVP_PKEY_keygen_init(ctx) != 1 || EVP_PKEY_keygen(ctx, &pkey) != 1)
        die("keygen failed");
    EVP_PKEY_CTX_free(ctx);
    return pkey; /* caller owns; free with EVP_PKEY_free */
}

static void x25519_pubkey(const EVP_PKEY *pkey, uint8_t out[RAW_PUB_LEN])
{
    size_t len = RAW_PUB_LEN;
    if (EVP_PKEY_get_raw_public_key(pkey, out, &len) != 1 || len != RAW_PUB_LEN)
        die("get_raw_public_key failed");
}

static EVP_PKEY *x25519_pubkey_from_raw(const uint8_t raw[RAW_PUB_LEN])
{
    EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, raw,
                                                 RAW_PUB_LEN);
    if (!pkey)
        die("new_raw_public_key failed");
    return pkey;
}

/* Derive the 32-byte shared secret from our private key + their public key.
 * The peer EVP_PKEY we build here is OURS to free — done by the caller via
 * the returned-pointer convention below (we free it internally). */
static void x25519_derive(const EVP_PKEY *own, const uint8_t peer_raw[RAW_PUB_LEN],
                          uint8_t secret[KEY_LEN])
{
    EVP_PKEY *peer = x25519_pubkey_from_raw(peer_raw);
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new((EVP_PKEY *)own, NULL);
    if (!ctx)
        die("derive ctx failed");
    size_t len = KEY_LEN;
    if (EVP_PKEY_derive_init(ctx) != 1 ||
        EVP_PKEY_derive_set_peer(ctx, peer) != 1 ||
        EVP_PKEY_derive(ctx, secret, &len) != 1 || len != KEY_LEN)
        die("derive failed");
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(peer); /* the raw->PKEY wrapper is ours: release it */
}

/* ---- mode: rand ---------------------------------------------------------- */
static void mode_rand(void)
{
    uint8_t buf[KEY_LEN];
    if (RAND_bytes(buf, sizeof buf) != 1)
        die("RAND_bytes failed"); /* only fails if the CSPRNG is broken */
    hex_print("32 bytes from RAND_bytes:", buf, sizeof buf);
    printf("-> rerun me: never the same twice. rand()/random() CANNOT\n"
           "   replace this: their sequence is guessable, and a guessable\n"
           "   key is no key. (These 32 bytes are an X25519 private key,\n"
           "   an AES-256 key, or an SRTP master key — all 32 bytes.)\n");
}

/* ---- mode: local — two parties in one process ---------------------------- */
static void mode_local(void)
{
    printf("party A: generate\nparty B: generate\n");
    EVP_PKEY *a = x25519_generate();
    EVP_PKEY *b = x25519_generate();

    uint8_t a_pub[RAW_PUB_LEN], b_pub[RAW_PUB_LEN];
    x25519_pubkey(a, a_pub);
    x25519_pubkey(b, b_pub);
    hex_print("A sends her PUBLIC key:", a_pub, sizeof a_pub);
    hex_print("B sends his PUBLIC key:", b_pub, sizeof b_pub);

    /* Each side uses ONLY its own private key + the peer's public key.
     * The private keys never travel; the public values could be shouted
     * from a rooftop. */
    uint8_t s_a[KEY_LEN], s_b[KEY_LEN];
    x25519_derive(a, b_pub, s_a);
    x25519_derive(b, a_pub, s_b);

    hex_print("A's derived secret:", s_a, KEY_LEN);
    hex_print("B's derived secret:", s_b, KEY_LEN);
    printf("secrets identical: %s  <- the DH 'aha'\n",
           memcmp(s_a, s_b, KEY_LEN) == 0 ? "YES" : "NO");

    EVP_PKEY_free(a);
    EVP_PKEY_free(b);
}

/* Shared logic for fork/mismatch: one side of the exchange over two pipes.
 * `corrupt_peer` flips a bit in the peer pubkey we receive (the bug). */
static void exchange_side(int read_fd, int write_fd, const char *name,
                          int corrupt_peer)
{
    EVP_PKEY *own = x25519_generate();
    uint8_t my_pub[RAW_PUB_LEN], peer_pub[RAW_PUB_LEN];
    x25519_pubkey(own, my_pub);

    if (write(write_fd, my_pub, sizeof my_pub) != sizeof my_pub)
        die("write pubkey");
    /* NOTE the ordering: we read the peer AFTER publishing our own key —
     * with pipes each side writes first, then reads, so nobody deadlocks. */
    if (read(read_fd, peer_pub, sizeof peer_pub) != sizeof peer_pub)
        die("read pubkey");

    if (corrupt_peer)
        peer_pub[0] ^= 0x01; /* THE BUG: wrong peer key material */

    uint8_t secret[KEY_LEN];
    x25519_derive(own, peer_pub, secret);

    /* Print the SHA-256 of the secret (never print raw key material in
     * logs — this habit matters as soon as anything is real). */
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    uint8_t hash[32];
    unsigned int hlen = 0;
    EVP_DigestInit_ex(md, EVP_sha256(), NULL);
    EVP_DigestUpdate(md, secret, KEY_LEN);
    EVP_DigestFinal_ex(md, hash, &hlen);
    EVP_MD_CTX_free(md);

    printf("%s derived secret sha256: ", name);
    for (unsigned int i = 0; i < hlen; i++)
        printf("%02x", hash[i]);
    printf("\n");

    /* Also prove the failure mode with an HMAC both sides compute over the
     * same message: equal secrets -> equal HMAC. */
    const char *msg = "handshake-complete";
    EVP_MAC *mac = EVP_MAC_fetch(NULL, "HMAC", NULL);
    EVP_MAC_CTX *mc = EVP_MAC_CTX_new(mac);
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_utf8_string("digest", (char *)"SHA256", 0),
        OSSL_PARAM_construct_end(),
    };
    uint8_t tag[32];
    size_t tlen = 0;
    EVP_MAC_init(mc, secret, KEY_LEN, params);
    EVP_MAC_update(mc, (const uint8_t *)msg, strlen(msg));
    EVP_MAC_final(mc, tag, &tlen, sizeof tag);
    printf("%s hmac(secret, \"%s\"): ", name, msg);
    for (size_t i = 0; i < tlen; i++)
        printf("%02x", tag[i]);
    printf("\n");

    EVP_MAC_CTX_free(mc);
    EVP_MAC_free(mac);
    EVP_PKEY_free(own);
}

/* ---- mode: fork — the same handshake between two real processes ---------- */
static void mode_fork(int corrupt_child)
{
    /* Two pipes: parent->child and child->parent (bite 45's fork + pipes) */
    int p2c[2], c2p[2];
    if (pipe(p2c) < 0 || pipe(c2p) < 0)
        die("pipe");

    pid_t pid = fork();
    if (pid < 0)
        die("fork");

    if (pid == 0) {
        /* child: read parent's key from p2c[0], write own to c2p[1] */
        close(p2c[1]);
        close(c2p[0]);
        exchange_side(p2c[0], c2p[1], "child ", corrupt_child);
        close(p2c[0]);
        close(c2p[1]);
        /* _exit skips stdio teardown: without this flush the child's
         * buffered printf output would be LOST (a classic fork bug —
         * observe it by deleting this line). */
        fflush(stdout);
        _exit(0);
    }

    /* parent: write own key to p2c[1], read child's from c2p[0] */
    close(p2c[0]);
    close(c2p[1]);
    exchange_side(c2p[0], p2c[1], "parent", 0);
    close(p2c[1]);
    close(c2p[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    printf("(child exit status %d)\n", WEXITSTATUS(status));
    printf("-> %s\n",
           corrupt_child
               ? "hashes AND hmacs differ: garbage on the wire means garbage\n"
                 "   in the key. Only the MAC comparison would catch it —\n"
                 "   that check is DTLS's Finished message."
               : "identical hashes: two processes that never shared a secret\n"
                 "  now hold the same 32 bytes. That's the keying material\n"
                 "  DTLS exports to SRTP (bite 67/76).");
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "all";

    if (!strcmp(mode, "rand")) {
        mode_rand();
        return 0;
    }
    if (!strcmp(mode, "local")) {
        mode_local();
        return 0;
    }
    if (!strcmp(mode, "fork")) {
        mode_fork(0);
        return 0;
    }
    if (!strcmp(mode, "mismatch")) {
        mode_fork(1);
        return 0;
    }
    if (!strcmp(mode, "all")) {
        mode_rand();
        printf("\n");
        mode_local();
        printf("\n");
        mode_fork(0);
        return 0;
    }
    fprintf(stderr, "usage: %s [rand|local|fork|mismatch|all]\n", argv[0]);
    return 1;
}
