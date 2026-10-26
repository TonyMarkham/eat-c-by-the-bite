/*
 * Bite 68 — CHECKPOINT: the reorder engine (SENDER)
 *
 * Phase 6's primitives assembled into one real-time pipeline:
 *
 *   [PCM file] -> chop into 20 ms frames -> seq+ts header -> UDP, paced
 *                (1920 bytes/frame)       (big-endian)     (bite 59 pacing)
 *
 * Frame wire format (big-endian, the miniature RTP header of bite 61
 * plus a send-stamp for the analyzer):
 *
 *     +--------+------------+------------+--------------------------+
 *     | seq:2  | rtp_ts:4   | sent_us:4  | PCM payload (1920 bytes) |
 *     +--------+------------+------------+--------------------------+
 *
 *   seq     — wraps 65535->0; receiver reorders/dedupes on it
 *   rtp_ts  — media timestamp: +960 per frame (20 ms of 48 kHz audio)
 *   sent_us — low 32 bits of the monotonic microsecond clock when sent;
 *             the analyzer reconstructs send->arrival delay (loopback only:
 *             both processes share the machine-wide CLOCK_MONOTONIC)
 *
 * Honest note: 1920-byte payloads exceed the ~1200-byte "comfortable MTU"
 * from bite 62. On loopback that's fine; real deployments put COMPRESSED
 * audio (Opus ~120 B/frame) or 960-sample PCMU frames in one datagram. The
 * concept stands; the numbers would be renegotiated in SDP.
 *
 * Pacing is deadline-based (bite 59): absolute next-deadline, skip on
 * overrun. --jitter adds random pre-send delay (latency variance);
 * --shuffle sends small windows out of order; --loss silently drops a
 * percentage (all userland stand-ins for `sudo tc qdisc add dev lo root
 * netem delay 40ms 20ms loss 5% reorder 5%`, which needs root — see
 * bites/60-jitter for the netem commands).
 *
 * Compile + run (from the repo root, in WSL):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/68-checkpoint-reorder/sender.c   -o /tmp/b68-send
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/68-checkpoint-reorder/receiver.c -o /tmp/b68-recv
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/68-checkpoint-reorder/analyzer.c -o /tmp/b68-an
 *
 *   make a test PCM if you don't have one (bite 63):
 *   /tmp/b63 gen /tmp/tone.raw 3
 *
 *   /tmp/b68-recv 7100 /tmp/out.raw /tmp/packets.log     # terminal 1
 *   /tmp/b68-send /tmp/tone.raw 127.0.0.1 7100           # terminal 2
 *   /tmp/b68-an /tmp/packets.log                          # the report
 *   cmp /tmp/tone.raw /tmp/out.raw                        # clean run: identical
 *
 * BREAK IT ON PURPOSE (from phases/phase-6-rt-media.md):
 *   Run the gauntlet — heavy impairment, and the pipeline must degrade
 *   gracefully: drop and REPORT, never crash, never hang, never deliver
 *   corrupt audio as if it were fine.
 *     /tmp/b68-send /tmp/tone.raw 127.0.0.1 7101 --loss 20 --jitter 50 --shuffle 24
 *   Expect: delivered < sent, honest loss %, reordered frames placed from
 *   the buffer, out.raw shorter than the input (missing frames never
 *   written), and a clean exit. Then `aplay -f S16_LE -r 48000 -c 1
 *   /tmp/out.raw` — audibly intact except for the missing holes.
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

#define FRAME_BYTES 1920 /* 20 ms of 48 kHz 16-bit mono = 960 samples */
#define HDR_LEN (2 + 4 + 4)
#define DEFAULT_PACE_MS 20
#define RTP_CLOCK 48000

static void put_u16be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_u32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* bite 59's sleep-until-deadline: the correct way to pace */
static void sleep_until_us(double deadline_us)
{
    for (;;) {
        double delta = deadline_us - (double)now_us();
        if (delta <= 0.0)
            return;
        struct timespec req = {
            .tv_sec = (time_t)(delta / 1e6),
            .tv_nsec = (long)((delta - (double)req.tv_sec * 1e6) * 1000.0),
        };
        if (nanosleep(&req, NULL) == 0)
            return;
        if (errno != EINTR)
            return;
    }
}

static uint32_t rng_state = 0xcafebabeu;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static double rand_pct(void) { return (double)(rng() % 10000u) / 100.0; }

static void sleep_ms(double ms)
{
    struct timespec req = {
        .tv_sec = (time_t)(ms / 1000.0),
        .tv_nsec = (long)((ms - (double)req.tv_sec * 1000.0) * 1000000.0),
    };
    while (nanosleep(&req, &req) == -1 && errno == EINTR)
        ;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr,
                "usage: %s <pcm_file> <ip> <port> [--pace ms] [--jitter ms]\n"
                "       [--shuffle n] [--loss pct]\n",
                argv[0]);
        return 1;
    }
    const char *file = argv[1];
    const char *ip = argv[2];
    int port = atoi(argv[3]);
    double pace_ms = DEFAULT_PACE_MS, jitter_ms = 0.0, loss_pct = 0.0;
    int shuffle = 1;

    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--pace") && i + 1 < argc) pace_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--jitter") && i + 1 < argc) jitter_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--shuffle") && i + 1 < argc) shuffle = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--loss") && i + 1 < argc) loss_pct = atof(argv[++i]);
        else { fprintf(stderr, "bad option: %s\n", argv[i]); return 1; }
    }

    FILE *f = fopen(file, "rb");
    if (!f) {
        perror(file);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *pcm = malloc((size_t)size);
    if (!pcm || fread(pcm, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "read failed\n");
        return 1;
    }
    fclose(f);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &dst.sin_addr) != 1) {
        fprintf(stderr, "bad IPv4: %s\n", ip);
        return 1;
    }

    long frames = size / FRAME_BYTES;
    printf("sender: %ld frames of %d bytes (%.3f s audio) -> %s:%d at %.0f ms/frame\n",
           frames, FRAME_BYTES, (double)frames * FRAME_BYTES / (RTP_CLOCK * 2), ip,
           port, pace_ms);

    int win = shuffle > 1 ? shuffle : 1;
    uint8_t (*pkt)[HDR_LEN + FRAME_BYTES] = malloc((size_t)win * sizeof *pkt);
    if (!pkt) { perror("malloc"); return 1; }

    /* RTP starts seq at a random value (RFC 3550); ts starts at a random
     * offset too (so timestamp arithmetic must never assume 0-based). */
    uint16_t seq = (uint16_t)(rng() & 0xffffu);
    uint32_t ts = 0x10000000u;

    uint64_t start = now_us();
    double next_deadline = (double)start; /* absolute timeline (bite 59) */
    long sent = 0, dropped = 0;    int skip_note = 0;

    for (long i = 0; i < frames; i++) {
        /* Build one window of packets */
        int nwin = (frames - i < win) ? (int)(frames - i) : win;
        for (int j = 0; j < nwin; j++) {
            uint8_t *p = pkt[j];
            put_u16be(p, seq);
            put_u32be(p + 2, ts);
            put_u32be(p + 6, (uint32_t)now_us());
            memcpy(p + HDR_LEN, pcm + (size_t)(i + j) * FRAME_BYTES, FRAME_BYTES);
            seq = (uint16_t)(seq + 1);
            ts += RTP_CLOCK / 50; /* 960 @ 48 kHz = one 20 ms frame */
        }

        /* Reordering stand-in: emit the window in random order */
        for (int j = nwin - 1; j > 0; j--) {
            int k = (int)(rng() % (uint32_t)(j + 1));
            uint8_t tmp[sizeof *pkt];
            memcpy(tmp, pkt[j], sizeof tmp);
            memcpy(pkt[j], pkt[k], sizeof tmp);
            memcpy(pkt[k], tmp, sizeof tmp);
        }

        for (int j = 0; j < nwin; j++) {
            if (jitter_ms > 0.0)
                sleep_ms((double)(rng() % (uint32_t)(jitter_ms + 1.0)));
            if (loss_pct > 0.0 && rand_pct() < loss_pct) {
                dropped++;
                continue;
            }
            if (sendto(fd, pkt[j], sizeof *pkt, 0,
                       (struct sockaddr *)&dst, sizeof dst) < 0) {
                perror("sendto");
                free(pkt);
                free(pcm);
                close(fd);
                return 1;
            }
            sent++;
        }

        /* Deadline pacing: catch up the absolute timeline. If the send
         * loop (jitter!) overran the budget, SKIP ahead rather than
         * drift — bites 59's back-pressure lesson, now with a job. */
        next_deadline += pace_ms * 1000.0;
        if (next_deadline < (double)now_us()) {
            next_deadline = (double)now_us();
            skip_note++;
        } else {
            sleep_until_us(next_deadline);
        }
        i += nwin - 1;
    }
    free(pkt);
    free(pcm);
    close(fd);
    double wall = ((double)now_us() - (double)start) / 1000.0;
    printf("sender: sent %ld, dropped %ld, wall %.3f s for %.3f s of audio "
           "(%d pace skips)\n",
           sent, dropped, wall / 1000.0, (double)frames * pace_ms / 1000.0,
           skip_note);
    return 0;
}
