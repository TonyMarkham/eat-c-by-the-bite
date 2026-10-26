/*
 * Bite 68 — CHECKPOINT: the reorder engine (RECEIVER)
 *
 * The consumer end of the pipeline: UDP datagrams arrive late, early-ish,
 * duplicated, or never. This receiver assembles an IN-ORDER byte stream
 * from the chaos — the same job libsrtp-feeding RTP receivers do — and
 * reports honestly about everything it could not fix.
 *
 * Input frame (big-endian): [seq:2][rtp_ts:4][sent_us:4][1920 B PCM]
 *
 * The machinery, all from bites 61–62:
 *   - reorder window (default 16): a packet arriving up to W positions
 *     ahead is BUFFERED until the hole before it fills, then placed.
 *   - dedupe ring (256 entries): a seq we already delivered is dropped as
 *     a duplicate; the ring makes wraparound comparison cheap.
 *   - modular seq arithmetic (int16 distance): 65535 -> 0 is "newer".
 *   - late/gap handling: beyond the window the delivery point JUMPS,
 *     declaring the hole lost — the stream must flow even under 20% loss.
 *   - every delivered frame's bytes go to out.raw IN ORDER; corrupt or
 *     partial data is never written (there are no partial frames here:
 *     one frame = one datagram; bite 62 covered the multi-fragment case).
 *
 * Output: out.raw (the reordered PCM), packets.log (one line per DELIVERED
 * frame: `seq rtp_ts sent_us recv_us`, for bites/68-checkpoint-reorder/
 * analyzer.c), and a final report on stdout.
 *
 * Compile + run (from the repo root, in WSL):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/68-checkpoint-reorder/receiver.c -o /tmp/b68-recv
 *   /tmp/b68-recv 7100 /tmp/out.raw /tmp/packets.log &   # then:
 *   /tmp/b68-send /tmp/tone.raw 127.0.0.1 7100
 *
 * Options: --window N (reorder depth), --idle ms (silence before shutdown)
 *
 * BREAK IT ON PURPOSE (from phases/phase-6-rt-media.md):
 *   The 20%-loss gauntlet (see sender.c header). Watch three properties:
 *   1. It never stalls: gaps force the delivery point forward, buffered
 *      frames keep flowing.
 *   2. It never fabricates: bytes missing from the input are missing from
 *      out.raw — cmp shows a shorter file, never a shifted/corrupt one.
 *   3. The report tells the truth: delivered + lost == seq numbers
 *      consumed, and loss % matches the analyzer's within rounding.
 *   Sabotage checks to try: set the window to 1 (--window 1) and watch
 *   reorder tolerance collapse; or make deliver() write even when the
 *   cell seq doesn't match next (simulate a memset of the ring) and watch
 *   cmp light up — that's the "corrupt output as if fine" cardinal sin.
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define FRAME_BYTES 1920
#define HDR_LEN (2 + 4 + 4)
#define RING 256           /* dedupe/history ring (power of 2) */
#define DEFAULT_WINDOW 16
#define DEFAULT_IDLE_MS 2000

struct cell {
    int present;
    uint16_t seq;
    uint32_t ts;
    uint32_t sent_us;
    uint64_t arrive_us; /* when the datagram actually arrived (the drain
                         * delivery later logs THIS, not the drain time,
                         * so the analyzer measures transport delay) */
    uint8_t *data;
};

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* signed modular distance: b ahead of a, through the 65535->0 wrap */
static int seq_dist(uint16_t a, uint16_t b)
{
    return (int)(int16_t)(uint16_t)(b - a);
}

struct recv {
    uint16_t next;
    int started;
    struct cell ring[RING];
    uint8_t delivered[RING];
    long delivered_n, reordered, lost, dups, late;
    long max_reorder; /* furthest a buffered packet was ahead of next */
    FILE *out;
    FILE *log;
};

/* write one frame's payload in order + an analyzer log line (status D:
 * delivered — either in-order, salvaged in a gap walk, or drained).
 * recv_us = arrival time of the datagram (now, or the buffered arrival). */
static void deliver(struct recv *r, uint16_t seq, uint32_t ts,
                    uint32_t sent_us, const uint8_t *data, uint64_t recv_us)
{
    if (fwrite(data, 1, FRAME_BYTES, r->out) != FRAME_BYTES) {
        perror("fwrite");
        exit(1);
    }
    fprintf(r->log, "%u %u %u %llu D\n", seq, ts, sent_us,
            (unsigned long long)recv_us);
    r->delivered[seq % RING] = 1;
    r->delivered_n++;
}

/* log an arrival whose payload was buffered (B) or dropped (X: dup/late) */
static void log_arrival(struct recv *r, uint16_t seq, uint32_t ts,
                        uint32_t sent_us, const char *status)
{
    fprintf(r->log, "%u %u %u %llu %s\n", seq, ts, sent_us,
            (unsigned long long)now_us(), status);
}

/* deliver consecutive buffered frames starting at r->next */
static void drain(struct recv *r)
{
    for (;;) {
        struct cell *c = &r->ring[r->next % RING];
        if (!c->present || c->seq != r->next)
            break;
        c->present = 0;
        deliver(r, c->seq, c->ts, c->sent_us, c->data, c->arrive_us);
        free(c->data);
        c->data = NULL;
        r->next = (uint16_t)(r->next + 1);
    }
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <port> <out.raw> <packets.log> "
                        "[--window n] [--idle ms]\n",
                argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);
    const char *out_path = argv[2];
    const char *log_path = argv[3];
    int window = DEFAULT_WINDOW, idle_ms = DEFAULT_IDLE_MS;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--window") && i + 1 < argc)
            window = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--idle") && i + 1 < argc)
            idle_ms = atoi(argv[++i]);
        else {
            fprintf(stderr, "bad option: %s\n", argv[i]);
            return 1;
        }
    }

    FILE *out = fopen(out_path, "wb");
    FILE *log = fopen(log_path, "w");
    if (!out || !log) {
        perror(out_path);
        return 1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        return 1;
    }
    /* wake up often enough to sweep, exit on true idleness (bite 62 lesson) */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 50 * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    printf("receiver: UDP %d -> %s (window %d, idle %d ms)\n", port, out_path,
           window, idle_ms);

    struct recv r;
    memset(&r, 0, sizeof r);
    r.out = out;
    r.log = log;

    uint64_t last_packet = now_us();
    long arrivals = 0;
    uint8_t buf[2048];

    for (;;) {
        ssize_t n = recvfrom(fd, buf, sizeof buf, 0, NULL, NULL);
        uint64_t now = now_us();

        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("recvfrom");
                break;
            }
            if (r.started && now - last_packet > (uint64_t)idle_ms * 1000ULL) {
                printf("idle %d ms — stream over, finalizing\n", idle_ms);
                break;
            }
            continue;
        }
        if (n != HDR_LEN + FRAME_BYTES) {
            printf("bad datagram %zd bytes (expect %d) — dropped\n", n,
                   HDR_LEN + FRAME_BYTES);
            continue;
        }

        uint16_t s = (uint16_t)((buf[0] << 8) | buf[1]);
        uint32_t ts = ((uint32_t)buf[2] << 24) | ((uint32_t)buf[3] << 16) |
                      ((uint32_t)buf[4] << 8) | (uint32_t)buf[5];
        uint32_t sent_us = ((uint32_t)buf[6] << 24) | ((uint32_t)buf[7] << 16) |
                           ((uint32_t)buf[8] << 8) | (uint32_t)buf[9];
        const uint8_t *payload = buf + HDR_LEN;

        arrivals++;
        last_packet = now;

        /* Ramp-up sync like bite 61: BUFFER the first arrival instead of
         * delivering it — with reordering, the first datagram to arrive
         * is not necessarily the stream's first frame. Delivery starts
         * once the (possibly rewound) start matches. */
        if (!r.started) {
            r.started = 1;
            r.next = s;
            struct cell *c0 = &r.ring[s % RING];
            c0->present = 1;
            c0->seq = s;
            c0->ts = ts;
            c0->sent_us = sent_us;
            c0->arrive_us = now;
            c0->data = malloc(FRAME_BYTES);
            if (!c0->data) {
                perror("malloc");
                return 1;
            }
            memcpy(c0->data, payload, FRAME_BYTES);
            log_arrival(&r, s, ts, sent_us, "B");
            continue;
        }
        if (r.delivered_n == 0 && seq_dist(r.next, s) < 0) {
            r.next = s; /* still syncing: rewind to the earlier arrival */
        }

        int d = seq_dist(r.next, s);

        if (d < 0) {
            if (r.delivered[s % RING] && -d < RING) {
                r.dups++;
                log_arrival(&r, s, ts, sent_us, "X");
            } else {
                r.late++;
                log_arrival(&r, s, ts, sent_us, "X");
            }
            continue;
        }

        if (d >= window) {
            /* Jump the delivery point: salvage what the ring holds,
             * declare the rest of the gap lost, keep the stream flowing. */
            while (r.next != s) {
                struct cell *c = &r.ring[r.next % RING];
                if (c->present && c->seq == r.next) {
                    c->present = 0;
                    deliver(&r, c->seq, c->ts, c->sent_us, c->data,
                            c->arrive_us);
                    free(c->data);
                    c->data = NULL;
                } else {
                    r.lost++;
                }
                r.next = (uint16_t)(r.next + 1);
            }
            deliver(&r, s, ts, sent_us, payload, now);
            r.next = (uint16_t)(s + 1);
            drain(&r);
            continue;
        }

        if (d == 0) {
            deliver(&r, s, ts, sent_us, payload, now);
            r.next = (uint16_t)(s + 1);
            drain(&r);
            continue;
        }

        /* 0 < d < window: a future frame while a hole is open — buffer it */
        struct cell *c = &r.ring[s % RING];
        if (c->present && c->seq == s) {
            r.dups++;
            log_arrival(&r, s, ts, sent_us, "X");
            continue;
        }
        if (!c->present) {
            c->data = malloc(FRAME_BYTES);
            if (!c->data) {
                perror("malloc");
                return 1;
            }
        }
        c->present = 1;
        c->seq = s;
        c->ts = ts;
        c->sent_us = sent_us;
        c->arrive_us = now;
        memcpy(c->data, payload, FRAME_BYTES);
        /* count as "reordered" only once real delivery has started — the
         * startup ramp (sync frame waiting for its successor) is not the
         * network's fault */
        if (r.delivered_n > 0)
            r.reordered++;
        log_arrival(&r, s, ts, sent_us, "B");
        if (d > r.max_reorder)
            r.max_reorder = d;
    }
    close(fd);

    /* Frames still buffered: not lost, but undelivered at cutoff — the
     * stream ended (or went idle) with holes still open. Honest report:
     * they are neither in out.raw nor counted as lost. */
    long buffered = 0;
    for (int i = 0; i < RING; i++)
        if (r.ring[i].present) {
            buffered++;
            free(r.ring[i].data);
            r.ring[i].data = NULL;
        }

    long consumed = r.delivered_n + r.lost + buffered;
    printf("\n==== receiver report ====\n");
    printf("arrivals (datagrams):   %ld\n", arrivals);
    printf("delivered in order:     %ld frames (%ld bytes)\n", r.delivered_n,
           r.delivered_n * FRAME_BYTES);
    printf("reordered (buffered then placed): %ld (max %ld ahead)\n",
           r.reordered, r.max_reorder);
    printf("lost (declared gaps):   %ld\n", r.lost);
    printf("duplicates dropped:     %ld\n", r.dups);
    printf("late dropped:           %ld\n", r.late);
    printf("buffered at cutoff:     %ld\n", buffered);
    if (consumed > 0)
        printf("loss %%:                 %.1f%%  (%ld lost of %ld consumed)\n",
               100.0 * (double)r.lost / (double)consumed, r.lost, consumed);
    printf("play with: aplay -f S16_LE -r 48000 -c 1 %s\n", out_path);
    printf("analyze with: analyzer %s\n", log_path);

    fclose(out);
    fclose(log);
    return 0;
}
