/*
 * Bite 61 — Sequencing: your first RTP receiver (SENDER)
 *
 * We upgrade bite 60's datagram with a miniature RTP header (real RTP is
 * RFC 3550; same ideas, more fields):
 *
 *     +--------+-------------+---------------------------+
 *     | seq:2  | ts:4        | payload (48 filler bytes) |
 *     | BE     | BE          |                           |
 *     +--------+-------------+---------------------------+
 *
 *   seq — increments by 1 per packet; the receiver uses it to reorder,
 *         detect loss and drop duplicates. Wraps at 65535 -> 0.
 *   ts  — 32-bit media timestamp in RTP style: +960 per packet (one 20 ms
 *         frame of 48 kHz audio = 960 samples; see bite 59). It advances
 *         with the MEDIA timeline, independent of seq — a lost packet's
 *         media time is missing, not replayed.
 *
 * Why big-endian: network byte order. Why hand-rolled put_u16be/put_u32be:
 * bite 42's serialization skill, in its natural habitat.
 *
 * Impairment knobs (userland stand-ins for `tc netem`; no root needed):
 *   --jitter ms    random 0..ms sleep before each send (latency variance)
 *   --shuffle n    send windows of n packets in random order (reordering)
 *   --loss pct     silently skip pct% of the packets (real loss)
 *   --jump n       every 10th packet, skip n sequence numbers at once —
 *                  simulated BURST loss (the "break it" exercise)
 *   --wrap         start the sequence at 65530 so it wraps 65535 -> 0 after
 *                  6 packets — the classic sequence-number bug (the other
 *                  "break it" exercise)
 *   --dup-every n  send every n-th packet twice (duplicate test)
 *
 * Compile + run (from the repo root, in WSL — two terminals):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/61-sequencing/sender.c   -o /tmp/b61-send
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/61-sequencing/receiver.c -o /tmp/b61-recv
 *   /tmp/b61-recv 9995                     # terminal 1
 *   /tmp/b61-send 127.0.0.1 9995           # terminal 2
 *
 * BREAK IT ON PURPOSE (from phases/phase-6-rt-media.md):
 *   1. Burst loss:  /tmp/b61-send 127.0.0.1 9995 --jump 5
 *      The receiver must report the gap as LOSS — not crash, not stall
 *      waiting for the missing packets. Watch it advance past the hole.
 *   2. Wraparound:  /tmp/b61-send 127.0.0.1 9995 --wrap
 *      If the receiver compared seqs with plain `<`, 0 would look like it
 *      came BEFORE 65535 and the stream would stall. It handles wrap with
 *      modular (int16) distance — the RFC 1982 trick. Break the receiver
 *      by replacing that distance computation with `(s < next)` and watch
 *      the wrap run fail.
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

#define PAYLOAD_LEN 48
#define HDR_LEN (2 + 4)
#define DEFAULT_COUNT 100
#define DEFAULT_INTERVAL_MS 10

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

static void sleep_ms(double ms)
{
    struct timespec req = {
        .tv_sec = (time_t)(ms / 1000.0),
        .tv_nsec = (long)((ms - (double)req.tv_sec * 1000.0) * 1000000.0),
    };
    while (nanosleep(&req, &req) == -1 && errno == EINTR)
        ;
}

static uint32_t rng_state = 0x9e3779b9u;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static double rand_pct(void) { return (double)(rng() % 10000u) / 100.0; }

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <dest_ip> <dest_port> [--count n] [--interval ms]\n"
                "    [--jitter ms] [--shuffle n] [--loss pct] [--jump n] [--wrap]\n"
                "    [--dup-every n]\n",
                argv[0]);
        return 1;
    }
    const char *dest_ip = argv[1];
    int dest_port = atoi(argv[2]);

    int count = DEFAULT_COUNT;
    double interval_ms = DEFAULT_INTERVAL_MS, jitter_ms = 0.0, loss_pct = 0.0;
    int shuffle = 1, jump = 0, dup_every = 0, start_wrap = 0;

    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--count") && i + 1 < argc) count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--interval") && i + 1 < argc) interval_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--jitter") && i + 1 < argc) jitter_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--shuffle") && i + 1 < argc) shuffle = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--loss") && i + 1 < argc) loss_pct = atof(argv[++i]);
        else if (!strcmp(argv[i], "--jump") && i + 1 < argc) jump = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dup-every") && i + 1 < argc) dup_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wrap")) start_wrap = 1;
        else { fprintf(stderr, "bad option: %s\n", argv[i]); return 1; }
    }

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

    /* RTP sequence numbers start at a RANDOM value (RFC 3550) so receivers
     * can't be spoofed by replaying an old stream; here --wrap pins it to
     * 65530 on purpose to exercise wraparound. */
    uint16_t seq = start_wrap ? 65530u : (uint16_t)(rng() & 0xffffu);
    uint16_t first_seq = seq;

    /* ---- pass 1: compute each packet slot's (seq, ts) pair ----
     * Timestamps advance by 960 per packet (20 ms of 48 kHz audio) with the
     * MEDIA timeline, independent of seq jumps — that's how real RTP works:
     * a lost packet's media time is simply missing, not replayed. */
    uint16_t *seqs = malloc(sizeof(uint16_t) * (size_t)count);
    uint32_t *tss = malloc(sizeof(uint32_t) * (size_t)count);
    if (!seqs || !tss) { perror("malloc"); return 1; }

    int skipped = 0;
    for (int i = 0; i < count; i++) {
        if (jump > 0 && i > 0 && i % 10 == 0) {
            /* Simulated burst loss: consume `jump` sequence numbers without
             * sending them — on the wire, indistinguishable from packets
             * that died in transit. */
            seq = (uint16_t)(seq + (uint16_t)jump);
            skipped += jump;
        }
        seqs[i] = seq;
        tss[i] = (uint32_t)(0x10000000u) + (uint32_t)i * 960u;
        seq = (uint16_t)(seq + 1);
    }

    printf("sender: %d packets -> %s:%d seq starts at %u%s jitter=%.0f "
           "shuffle=%d loss=%.0f%% jump=%d dup=%d\n",
           count, dest_ip, dest_port, first_seq,
           start_wrap ? " (wrap test)" : "", jitter_ms, shuffle, loss_pct,
           jump, dup_every);

    /* ---- pass 2: send, with jitter / shuffle / loss / dup ---- */
    uint8_t pkt[HDR_LEN + PAYLOAD_LEN];
    int win = shuffle > 1 ? shuffle : 1;
    uint8_t (*winbuf)[sizeof pkt] = malloc((size_t)win * sizeof *winbuf);
    uint32_t *wts_buf = malloc(sizeof(uint32_t) * (size_t)win);
    if (!winbuf || !wts_buf) { perror("malloc"); return 1; }

    int emitted = 0, dups_sent = 0;

    for (int base = 0; base < count; base += win) {
        int nwin = (count - base < win) ? count - base : win;

        for (int j = 0; j < nwin; j++) {
            uint8_t *p = winbuf[j];
            put_u16be(p, seqs[base + j]);
            put_u32be(p + 2, tss[base + j]);
            memset(p + HDR_LEN, (int)(seqs[base + j] & 0xff), PAYLOAD_LEN);
            wts_buf[j] = tss[base + j];
        }

        /* Fisher–Yates shuffle of the window: synthesizes network
         * reordering without needing root for netem. */
        for (int j = nwin - 1; j > 0; j--) {
            int k = (int)(rng() % (uint32_t)(j + 1));
            uint8_t tmp[sizeof pkt];
            memcpy(tmp, winbuf[j], sizeof tmp);
            memcpy(winbuf[j], winbuf[k], sizeof tmp);
            memcpy(winbuf[k], tmp, sizeof tmp);
            uint32_t tt = wts_buf[j]; wts_buf[j] = wts_buf[k]; wts_buf[k] = tt;
        }

        for (int j = 0; j < nwin; j++) {
            if (jitter_ms > 0.0)
                sleep_ms((double)(rng() % (uint32_t)(jitter_ms + 1.0)));
            if (loss_pct > 0.0 && rand_pct() < loss_pct) {
                skipped++;
                continue;
            }
            if (sendto(fd, winbuf[j], sizeof pkt, 0,
                       (struct sockaddr *)&dst, sizeof dst) < 0) {
                perror("sendto");
                goto out;
            }
            emitted++;

            /* Re-send the same seq: a duplicate datagram (retransmissions,
             * buggy middleboxes and our own retries all produce these) */
            if (dup_every > 0 && emitted % dup_every == 0) {
                if (sendto(fd, winbuf[j], sizeof pkt, 0,
                           (struct sockaddr *)&dst, sizeof dst) >= 0)
                    dups_sent++;
            }
            if (interval_ms > 0.0)
                sleep_ms(interval_ms);
        }
    }
out:
    uint16_t last_seq = seqs[count - 1];
    free(winbuf);
    free(wts_buf);
    free(seqs);
    free(tss);
    close(fd);
    printf("sender: emitted %d (+%d deliberate dups), %d seqs skipped/lost. "
           "final seq = %u (check the receiver survived the journey)\n",
           emitted, dups_sent, skipped, last_seq);
    return 0;
}
