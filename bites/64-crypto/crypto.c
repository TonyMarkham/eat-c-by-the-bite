/*
 * Bite 64 — Crypto primitives with OpenSSL: SHA-256, HMAC, AES-256-CTR
 *
 * The vocabulary SRTP assumes, exercised by hand via the modern EVP API
 * (context -> init -> update -> final; verbose but uniform across every
 * primitive — once you can drive EVP for one algorithm you can drive it
 * for all of them).
 *
 *   SHA-256   — integrity / fingerprint. THIS is the hash behind SDP's
 *               `a=fingerprint:` line (bite 73): the self-signed
 *               certificate's digest, compared out-of-band to defeat MITM.
 *   HMAC      — integrity WITH a secret key. What SRTP's auth tag is made
 *               of: proves the packet came from someone holding the key.
 *   AES-CTR   — confidentiality: AES in counter mode turns a block cipher
 *               into a stream cipher (key + 16-byte IV/counter block);
 *               what SRTP's cipher is. Same key encrypts and decrypts.
 *
 * The punchline (play with mode `aes` until it clicks): crypto primitives
 * are never used alone. SRTP = AES-CTR + HMAC, exactly the combo you run
 * below. Encryption without authentication fails SILENTLY.
 *
 * Compile + run (from the repo root, in WSL):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/64-crypto/crypto.c \
 *       -o /tmp/b64 -lssl -lcrypto
 *   /tmp/b64 sha256 /etc/hostname        # then: sha256sum /etc/hostname
 *   /tmp/b64 sha256 /tmp/test.bin        # (any file; compare with sha256sum)
 *   /tmp/b64 hmac
 *   /tmp/b64 aes
 *
 * BREAK IT ON PURPOSE (from phases/phase-6-rt-media.md):
 *   Decrypt with the WRONG key — the `aes` mode does this after the
 *   round-trip: garbage comes out and OpenSSL reports SUCCESS. No error,
 *   no exception, no warning. Crypto fails silently when keys mismatch;
 *   authentication (an HMAC over the ciphertext) is how you DETECT it.
 *   Write that sentence in your notes — it is why SRTP appends an auth tag
 *   to every packet and why DTLS hands SRTP keys at all.
 *   (The `hmac` mode shows the other half: flip ONE byte of a message and
 *   the HMAC changes completely — integrity you can check.)
 *
 * Done when: your digest matches `sha256sum`, and you can explain
 * fingerprint (SDP, identity) vs HMAC (SRTP, per-packet integrity) in your
 * own words.
 */

#define _POSIX_C_SOURCE 200809L

#include <openssl/evp.h>
#include <openssl/params.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void hex_print(const char *label, const uint8_t *buf, size_t len)
{
    printf("%-22s", label);
    for (size_t i = 0; i < len; i++)
        printf("%02x", buf[i]);
    printf("\n");
}

/* ---- 1. SHA-256 of a file (streaming: EVP context -> update -> final) --- */
static int sha256_file(const char *path, uint8_t out[32])
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return -1;
    }

    /* The EVP shape: create a context, initialize with an algorithm,
     * feed data in chunks (update), collect the digest (final). */
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
        fprintf(stderr, "digest init failed\n");
        fclose(f);
        return -1;
    }

    uint8_t buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        if (EVP_DigestUpdate(ctx, buf, n) != 1) {
            fprintf(stderr, "digest update failed\n");
            fclose(f);
            EVP_MD_CTX_free(ctx);
            return -1;
        }

    unsigned int mdlen = 0;
    int ok = EVP_DigestFinal_ex(ctx, out, &mdlen); /* mdlen will be 32 */
    EVP_MD_CTX_free(ctx);
    fclose(f);
    return ok == 1 && mdlen == 32 ? 0 : -1;
}

static void mode_sha256(const char *path)
{
    uint8_t digest[32];
    if (sha256_file(path, digest) != 0)
        exit(1);
    hex_print("sha256:", digest, 32);
    printf("compare with: sha256sum %s\n", path);
    printf("-> in WebRTC this digest (of a CERTIFICATE) appears as\n"
           "   `a=fingerprint:sha-256 <hex>` in the SDP, checked out-of-band.\n\n");
}

/* ---- 2. HMAC-SHA256: integrity bound to a secret ------------------------ */
static int hmac_sha256(const uint8_t *key, size_t keylen,
                       const uint8_t *msg, size_t msglen,
                       uint8_t out[32], unsigned int *outlen)
{
    /* OpenSSL 3's MAC API: fetch an implementation by NAME ("HMAC"),
     * parameterize it (digest=SHA256), then the usual init/update/final. */
    EVP_MAC *mac = EVP_MAC_fetch(NULL, "HMAC", NULL);
    if (!mac)
        return -1;
    EVP_MAC_CTX *ctx = EVP_MAC_CTX_new(mac);
    if (!ctx) {
        EVP_MAC_free(mac);
        return -1;
    }
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_utf8_string("digest", (char *)"SHA256", 0),
        OSSL_PARAM_construct_end(),
    };
    int ok = EVP_MAC_init(ctx, key, keylen, params) == 1 &&
             EVP_MAC_update(ctx, msg, msglen) == 1 &&
             EVP_MAC_final(ctx, out, (size_t *)outlen, 32) == 1;
    EVP_MAC_CTX_free(ctx);
    EVP_MAC_free(mac);
    return ok ? 0 : -1;
}

static void mode_hmac(void)
{
    /* A demo shared secret. Real systems derive keys (bite 65), never
     * hardcode them; and 16 bytes here would be "key material" in SRTP. */
    static const uint8_t key[16] = "shared-secret16"; /* 15 chars + NUL pad */
    const char *msg = "attack at dawn";
    uint8_t tag1[32], tag2[32];
    unsigned int l1, l2;

    if (hmac_sha256(key, sizeof key, (const uint8_t *)msg, strlen(msg),
                    tag1, &l1) != 0)
        exit(1);
    hex_print("hmac(\"attack...\"):", tag1, 32);

    /* Flip ONE bit of the message — 'd'->'e' in "dawn". */
    const char *tampered = "attack at eawn";
    if (hmac_sha256(key, sizeof key, (const uint8_t *)tampered,
                    strlen(tampered), tag2, &l2) != 0)
        exit(1);
    hex_print("hmac(one bit flipped):", tag2, 32);

    printf("-> tags differ completely (avalanche): an attacker cannot alter\n"
           "   a single payload byte without the check failing. THIS is what\n"
           "   an SRTP auth tag is: HMAC over the packet, 80 bits of it.\n\n");
}

/* ---- 3. AES-256-CTR: encrypt / decrypt / tamper / wrong key -------------- */
/* CTR mode: AES encrypts a COUNTER block -> keystream -> XOR with data.
 * Same operation both directions. The IV here is really the initial
 * counter block; SRTP derives it per-packet from the RTP header. */
static int aes256_ctr(const uint8_t key[32], const uint8_t iv[16],
                      const uint8_t *in, size_t len, uint8_t *outbuf,
                      int encrypt)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return -1;
    int ok = (encrypt ? EVP_EncryptInit_ex(ctx, EVP_aes_256_ctr(), NULL, key, iv)
                      : EVP_DecryptInit_ex(ctx, EVP_aes_256_ctr(), NULL, key, iv)) == 1;
    int outl = 0;
    if (ok)
        ok = EVP_CipherUpdate(ctx, outbuf, &outl, in, (int)len) == 1;
    int tail = 0;
    if (ok)
        ok = (encrypt ? EVP_EncryptFinal_ex(ctx, outbuf + outl, &tail)
                      : EVP_DecryptFinal_ex(ctx, outbuf + outl, &tail)) == 1;
    EVP_CIPHER_CTX_free(ctx);
    return ok ? outl + tail : -1; /* CTR is a stream: outl == len, tail == 0 */
}

static void mode_aes(void)
{
    /* 32-byte key (AES-256) + 16-byte counter block, all zeros for clarity */
    uint8_t key[32] = {0};
    uint8_t iv[16] = {0};
    for (int i = 0; i < 32; i++)
        key[i] = (uint8_t)i;

    const char *plaintext = "SRTP protects RTP with AES-CTR + HMAC-SHA1.";
    size_t len = strlen(plaintext);
    uint8_t ct[256], rt[256], tampered[256], wrongkey_out[256];

    int ctl = aes256_ctr(key, iv, (const uint8_t *)plaintext, len, ct, 1);
    if (ctl < 0)
        exit(1);
    hex_print("ciphertext:", ct, (size_t)ctl);

    int rtl = aes256_ctr(key, iv, ct, (size_t)ctl, rt, 0);
    printf("round-trip: %.*s  [%s]\n", rtl, (char *)rt,
           rtl == (int)len && memcmp(rt, plaintext, len) == 0 ? "OK"
                                                              : "FAILED");

    /* Tamper ONE ciphertext byte and decrypt with the right key: CTR mangles
     * exactly ONE plaintext byte (the one XORed by the damaged keystream
     * byte) — bit-flipping is cheap without authentication, which is the
     * whole reason malleable ciphers need a MAC. */
    memcpy(tampered, ct, (size_t)ctl);
    tampered[3] ^= 0x01;
    aes256_ctr(key, iv, tampered, (size_t)ctl, rt, 0);
    printf("tampered:   %.*s  <- only byte 3 changed, but notice you COULD\n"
           "            change it to anything: that's why SRTP adds HMAC.\n",
           rtl, (char *)rt);

    /* THE BREAK-IT DEMO: decrypt with a wrong key. */
    uint8_t badkey[32];
    memcpy(badkey, key, sizeof badkey);
    badkey[0] ^= 0x01; /* one bit! */
    int wl = aes256_ctr(badkey, iv, ct, (size_t)ctl, wrongkey_out, 0);
    printf("wrong key:  %.*s  <- OpenSSL returned SUCCESS (%d bytes).\n"
           "            No error is raised: wrong key == silent garbage.\n"
           "            Detection needs authentication (HMAC), not decryption.\n",
           wl, (char *)wrongkey_out, wl);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "all";

    if (!strcmp(mode, "sha256") && argc > 2) {
        mode_sha256(argv[2]);
        return 0;
    }
    if (!strcmp(mode, "hmac")) {
        mode_hmac();
        return 0;
    }
    if (!strcmp(mode, "aes")) {
        mode_aes();
        return 0;
    }
    if (!strcmp(mode, "all")) {
        mode_sha256("/etc/hostname");
        mode_hmac();
        mode_aes();
        return 0;
    }
    fprintf(stderr, "usage: %s [sha256 <file>|hmac|aes|all]\n", argv[0]);
    return 1;
}
