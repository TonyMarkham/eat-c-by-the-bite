/*
 * Bite 60 — The network in practice: loss, jitter, reordering (SENDER)
 *
 * UDP gives you NO ordering, NO delivery guarantee, NO pacing: it is fire-
 * and-forget datagrams. This sender makes that visible. It fires 100
 * numbered datagrams and stamps each with its send time so the receiver
 * (bites/60-jitter/receiver.c) can measure travel time, loss and reordering.
 *
 * Packet layout (big-endian, per bite 42's serialization discipline):
 *
 *     +--------+----------------+---------------------------+
 *     | seq:2  | sent_us:8      | payload (64 filler bytes) |
 *     +--------+----------------+---------------------------+
 *
 * `sent_us` is a CLOCK_MONOTONIC microsecond stamp. On one machine that
 * clock is system-wide, so sender and receiver can diff it directly — the
 * same trick real RTCP reports use (offsets cancel, deltas don't lie).
 *
 * Compile + run (from the repo root, in WSL — two terminals):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/60-jitter/sender.c   -o /tmp/b60-send
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/60-jitter/receiver.c -o /tmp/b60-recv
 *   /tmp/b60-recv 9990                      # terminal 1
 *   /tmp/b60-send 127.0.0.1 9990            # terminal 2
 *
 * Impair it yourself (userland, no root needed — netem does this "for real"):
 *   /tmp/b60-send 127.0.0.1 9990 --jitter 30 --shuffle 6 --loss 8
 *     --jitter m : sleep a random 0..m ms before each send (latency variance)
 *     --shuffle n: send each window of n packets in random order (reordering
 *                  stand-in for `tc netem reorder`; see below)
 *     --loss p   : silently skip p% of packets (stand-in for netem loss)
 *
 * The real-world harness is Linux traffic control (needs root):
 *   sudo tc qdisc add dev lo root netem delay 40ms 20ms loss 5% reorder 5%
 *   ... run the pair ...
 *   sudo tc qdisc del dev lo root      # NEVER forget this — see below!
 *
 * BREAK IT ON PURPOSE (from phases/phase-6-rt-media.md):
 *   Set loss to 100% — the receiver hears nothing and exits after its idle
 *   timeout with a 100% loss report; the sender neither hangs nor crashes,
 *   because UDP sendto() SUCCEEDS even into the void. That silence is the
 *   whole problem of real-time media: nothing tells you a packet died.
 *     /tmp/b60-send 127.0.0.1 9991 --loss 100
 *   Then, if you ran netem, REMOVE IT — forgetting `tc qdisc del dev lo root`
 *   ruins every network test tomorrow (all your loopback traffic keeps
 *   crawling at 40 ms and you will waste an hour rediscovering why).
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

#define PAYLOAD_LEN 64
#define DEFAULT_COUNT 100
#define DEFAULT_INTERVAL_MS 10

/* --- big-endian serialization (bite 42 in its natural habitat) ----------- */
/* The wire has no native endianness; network byte order is big-endian.
 * Doing it by hand once makes htons/htonl feel less like magic. */
static void put_u16be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_u64be(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * (7 - i)));
}

/* Monotonic microseconds — system-wide on Linux, comparable across processes */
static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* Sleep ms milliseconds, best effort (we don't pace precisely in this bite;
 * bite 59's deadline pacer is the precise version). */
static void sleep_ms(double ms)
{
    struct timespec req = {
        .tv_sec = (time_t)(ms / 1000.0),
        .tv_nsec = (long)((ms - (double)req.tv_sec * 1000.0) * 1000000.0),
    };
    while (nanosleep(&req, &req) == -1 && errno == EINTR)
        ;
}

/* xorshift PRNG — enough randomness for a demo (NOT crypto: see bite 65) */
static uint32_t rng_state = 0x12345678u;
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
                "                          [--jitter ms] [--shuffle n] [--loss pct]\n",
                argv[0]);
        return 1;
    }
    const char *dest_ip = argv[1];
    int dest_port = atoi(argv[2]);

    int count = DEFAULT_COUNT;
    double interval_ms = DEFAULT_INTERVAL_MS;
    double jitter_ms = 0.0, loss_pct = 0.0;
    int shuffle = 1; /* 1 = no reordering (window of one) */

    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--count") && i + 1 < argc)
            count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--interval") && i + 1 < argc)
            interval_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--jitter") && i + 1 < argc)
            jitter_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--shuffle") && i + 1 < argc)
            shuffle = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--loss") && i + 1 < argc)
            loss_pct = atof(argv[++i]);
        else {
            fprintf(stderr, "bad/ incomplete option: %s\n", argv[i]);
            return 1;
        }
    }

    /* UDP socket: SOCK_DGRAM + no connect() — every packet names its target */
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)dest_port);
    if (inet_pton(AF_INET, dest_ip, &dst.sin_addr) != 1) {
        fprintf(stderr, "bad IPv4 address: %s\n", dest_ip);
        return 1;
    }

    printf("sender: %d packets -> %s:%d  interval=%.1fms jitter=%.1fms "
           "shuffle=%d loss=%.0f%%\n",
           count, dest_ip, dest_port, interval_ms, jitter_ms, shuffle, loss_pct);

    uint8_t buf[2 + 8 + PAYLOAD_LEN];
    int sent = 0, dropped = 0;

    /* --shuffle: hold a window of packets, emit them in random order.
     * On a real network reordering happens because packets take different
     * paths/queues; here we synthesize the same arrival pattern without
     * needing root for netem. */
    int win = shuffle > 1 ? shuffle : 1;
    uint8_t (*winbuf)[sizeof buf] = malloc((size_t)win * sizeof *winbuf);
    if (!winbuf) {
        perror("malloc");
        return 1;
    }

    for (int base = 0; base < count; base += win) {
        int nwin = (count - base < win) ? count - base : win;

        for (int j = 0; j < nwin; j++) {
            uint16_t seq = (uint16_t)(base + j);
            uint8_t *p = winbuf[j];

            put_u16be(p, seq);
            put_u64be(p + 2, now_us());
            memset(p + 10, (int)(seq & 0xff), PAYLOAD_LEN);
        }

        /* Fisher–Yates shuffle of the window */
        for (int j = nwin - 1; j > 0; j--) {
            int k = (int)(rng() % (uint32_t)(j + 1));
            uint8_t tmp[sizeof buf];
            memcpy(tmp, winbuf[j], sizeof tmp);
            memcpy(winbuf[j], winbuf[k], sizeof tmp);
            memcpy(winbuf[k], tmp, sizeof tmp);
        }

        for (int j = 0; j < nwin; j++) {
            if (jitter_ms > 0.0)
                sleep_ms((double)(rng() % (uint32_t)(jitter_ms + 1.0)));

            if (loss_pct > 0.0 && rand_pct() < loss_pct) {
                dropped++; /* vanish silently — exactly like a lost datagram */
                continue;
            }

            /* sendto(): no connection, no ACK, no error even if nobody
             * listens — UDP's success only means "left the machine". */
            ssize_t r = sendto(fd, winbuf[j], sizeof buf, 0,
                               (struct sockaddr *)&dst, sizeof dst);
            if (r < 0) {
                perror("sendto");
                break;
            }
            sent++;

            if (interval_ms > 0.0)
                sleep_ms(interval_ms);
        }
    }
    free(winbuf);
    close(fd);

    printf("sender: sent %d, silently dropped %d (simulated loss), done.\n",
           sent, dropped);
    return 0;
}
