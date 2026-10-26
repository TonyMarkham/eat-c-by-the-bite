/*
 * Bite 62 — Packetization: MTU-sized fragments and reassembly (SENDER)
 *
 * Networks comfortably carry ~1400-byte datagrams (Ethernet MTU 1500 minus
 * IP/UDP headers). Anything bigger must be split — and YOU split it: IP
 * fragmentation exists but is unreliable and evil (lose one IP fragment and
 * the whole datagram is gone, with no way to know which piece died).
 *
 * Frame/fragment layout (all fields big-endian, bite 42 discipline):
 *
 *     +-----------+--------------+--------------+----------------------+
 *     | frame:2   | frag_idx:2   | frag_cnt:2   | payload (<=1200)     |
 *     +-----------+--------------+--------------+----------------------+
 *
 *   frame     — sequence number of the FRAME (a "video frame" in miniature)
 *   frag_idx  — 0-based index of this fragment within the frame
 *   frag_cnt  — total fragments in the frame
 *
 * The "last-fragment flag": a receiver learns a frame's size from ANY
 * fragment (frag_cnt is in every one) and recognizes the final fragment by
 * `frag_idx == frag_cnt - 1`. Explicit flag byte or count field — both are
 * common; the count doubles as the flag here.
 *
 * Losing ONE fragment loses the WHOLE frame — that's the economics behind
 * video keyframes: big frames are exponentially more likely to die.
 *
 * Compile + run (from the repo root, in WSL — two terminals):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/62-packetization/sender.c   -o /tmp/b62-send
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/62-packetization/receiver.c -o /tmp/b62-recv
 *
 *   /tmp/b62-send --gen /tmp/test.bin          # make a 64 KiB test file
 *   /tmp/b62-recv 9996 /tmp/out.bin            # terminal 1
 *   /tmp/b62-send /tmp/test.bin 127.0.0.1 9996 # terminal 2
 *   cmp /tmp/test.bin /tmp/out.bin && echo IDENTICAL
 *
 * Impair (userland stand-ins; netem does this for real with root):
 *   /tmp/b62-send /tmp/test.bin 127.0.0.1 9996 --loss 10 --shuffle 8
 *
 * BREAK IT ON PURPOSE (from phases/phase-6-rt-media.md):
 *   Deliver a frame that is MISSING one fragment: in receiver.c, disable
 *   the completeness check (make reassembly_flush() fire on a timer instead
 *   of only when have_count == frag_count). Run with --loss 10, then
 *   `cmp /tmp/test.bin /tmp/out.bin` — the first differing byte shows the
 *   corruption: a 1200-byte hole of garbage in the middle of a "delivered"
 *   frame. Partial data delivered as if whole is THE bug class real-time
 *   systems must refuse: a decoder fed a truncated frame glitches or dies.
 *   Re-enable the check and the corrupt frames vanish from the output —
 *   silence about loss is honest; corrupt "success" is not.
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FRAG_PAYLOAD 1200
#define HDR_LEN (2 + 2 + 2)
#define FRAME_BYTES 65536 /* a 64 KiB frame -> 55 fragments */
#define DEFAULT_INTERVAL_MS 5

static void put_u16be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void sleep_ms(double ms)
{
    struct timespec req = {
        .tv_sec = (time_t)(ms / 1000.0),
        .tv_nsec = (long)((ms - (double)req.tv_sec * 1000.0) * 1000000.0),
    };
    while (nanosleep(&req, &req) == -1 && errno == EINTR)
        ;
}

static uint32_t rng_state = 0xdeadbeefu;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static double rand_pct(void) { return (double)(rng() % 10000u) / 100.0; }

/* --gen: write a 64 KiB file of recognizable pseudo-random bytes */
static int gen_file(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return -1;
    }
    uint8_t chunk[4096];
    for (size_t off = 0; off < FRAME_BYTES; off += sizeof chunk) {
        for (size_t i = 0; i < sizeof chunk; i++)
            chunk[i] = (uint8_t)rng();
        if (fwrite(chunk, 1, sizeof chunk, f) != sizeof chunk) {
            perror("fwrite");
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    printf("wrote %d pseudo-random bytes to %s\n", FRAME_BYTES, path);
    return 0;
}

int main(int argc, char **argv)
{
    int gen = 0;
    const char *gen_path = NULL;
    const char *file = NULL, *dest_ip = NULL;
    int dest_port = 0;
    double interval_ms = DEFAULT_INTERVAL_MS, loss_pct = 0.0;
    int shuffle = 1;

    /* tiny arg walk: --gen PATH | FILE IP PORT [options] */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--gen") && i + 1 < argc) {
            gen = 1;
            gen_path = argv[++i];
        } else if (!strcmp(argv[i], "--interval") && i + 1 < argc) {
            interval_ms = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--loss") && i + 1 < argc) {
            loss_pct = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--shuffle") && i + 1 < argc) {
            shuffle = atoi(argv[++i]);
        } else if (!file) {
            file = argv[i];
        } else if (!dest_ip) {
            dest_ip = argv[i];
        } else if (!dest_port) {
            dest_port = atoi(argv[i]);
        }
    }
    if (gen) {
        if (!gen_path) {
            fprintf(stderr, "usage: %s --gen <path>\n", argv[0]);
            return 1;
        }
        return gen_file(gen_path) == 0 ? 0 : 1;
    }
    if (!file || !dest_ip || !dest_port) {
        fprintf(stderr,
                "usage: %s <file> <ip> <port> [--interval ms] [--loss pct]\n"
                "       %s --gen <path>  (create a 64 KiB test file)\n",
                argv[0], argv[0]);
        return 1;
    }

    /* Read the whole file into memory (small on purpose; streaming is
     * bite 35/36 territory). */
    FILE *f = fopen(file, "rb");
    if (!f) {
        perror(file);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)fsize);
    if (!data || fread(data, 1, (size_t)fsize, f) != (size_t)fsize) {
        fprintf(stderr, "read failed\n");
        return 1;
    }
    fclose(f);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)dest_port);
    if (inet_pton(AF_INET, dest_ip, &dst.sin_addr) != 1) {
        fprintf(stderr, "bad IPv4 address: %s\n", dest_ip);
        return 1;
    }

    printf("sender: %ld bytes -> frames of %d -> fragments of %d bytes, "
           "every %.0f ms%s%s\n",
           fsize, FRAME_BYTES, FRAG_PAYLOAD, interval_ms,
           loss_pct > 0 ? "  [simulated loss]" : "",
           shuffle > 1 ? "  [simulated reorder]" : "");

    uint16_t frame_seq = 0;
    int frags_sent = 0, frags_lost = 0;

    for (long off = 0; off < fsize; off += FRAME_BYTES, frame_seq++) {
        size_t frame_len = (fsize - off < FRAME_BYTES) ? (size_t)(fsize - off)
                                                       : (size_t)FRAME_BYTES;
        uint16_t frag_cnt = (uint16_t)((frame_len + FRAG_PAYLOAD - 1) / FRAG_PAYLOAD);

        /* Build the whole frame's fragments, then send (windowed shuffle
         * would be overkill across frames; we shuffle within the frame —
         * a reassembler must survive arbitrary arrival order). */
        uint8_t (*bufs)[HDR_LEN + FRAG_PAYLOAD] =
            malloc((size_t)frag_cnt * sizeof *bufs);
        uint16_t *lens = malloc((size_t)frag_cnt * sizeof *lens);
        if (!bufs || !lens) { perror("malloc"); return 1; }

        for (uint16_t i = 0; i < frag_cnt; i++) {
            size_t take = frame_len - (size_t)i * FRAG_PAYLOAD;
            if (take > FRAG_PAYLOAD)
                take = FRAG_PAYLOAD;
            put_u16be(bufs[i] + 0, frame_seq);
            put_u16be(bufs[i] + 2, i);
            put_u16be(bufs[i] + 4, frag_cnt);
            memcpy(bufs[i] + HDR_LEN, data + off + (size_t)i * FRAG_PAYLOAD, take);
            lens[i] = (uint16_t)(HDR_LEN + take);
        }

        if (shuffle > 1) { /* Fisher–Yates within the frame */
            for (uint16_t j = frag_cnt - 1; j > 0; j--) {
                uint16_t k = (uint16_t)(rng() % (uint32_t)(j + 1));
                uint8_t tmp[HDR_LEN + FRAG_PAYLOAD];
                uint16_t tl = lens[j];
                memcpy(tmp, bufs[j], sizeof tmp);
                memcpy(bufs[j], bufs[k], sizeof tmp);
                memcpy(bufs[k], tmp, sizeof tmp);
                lens[j] = lens[k];
                lens[k] = tl;
            }
        }

        for (uint16_t i = 0; i < frag_cnt; i++) {
            if (loss_pct > 0.0 && rand_pct() < loss_pct) {
                frags_lost++; /* one fragment gone = whole frame gone */
                continue;
            }
            if (sendto(fd, bufs[i], lens[i], 0,
                       (struct sockaddr *)&dst, sizeof dst) < 0) {
                perror("sendto");
                free(bufs);
                free(lens);
                return 1;
            }
            frags_sent++;
            sleep_ms(interval_ms);
        }
        free(bufs);
        free(lens);
    }
    close(fd);
    free(data);
    printf("sender: %d frames, %d fragments sent, %d fragments lost "
           "(=> some frames unrecoverable). done.\n",
           frame_seq, frags_sent, frags_lost);
    return 0;
}
