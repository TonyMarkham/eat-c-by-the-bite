/*
 * Bite 61 — Sequencing: your first RTP receiver (RECEIVER)
 *
 * This is the heart of the bite: a hand-rolled miniature RTP receiver.
 * Every WebRTC media stack has some version of this logic, so understand
 * every line here and Phase 7's libraries become readable.
 *
 * Receiver state machine:
 *
 *   next — the next sequence number we are willing to DELIVER.
 *   window (W=8) — how far ahead of `next` we are willing to hold a packet
 *          while waiting for the hole to fill (a jitter buffer in
 *          miniature). A ring of slots keyed by seq % RING holds the
 *          buffered payloads; a parallel bit-ring remembers what was
 *          delivered so duplicates can be recognized.
 *
 * On each packet with sequence s (modular distance d = (int16_t)(s - next)):
 *
 *   d == 0     -> deliver NOW, then drain consecutive buffered successors
 *                 (this is where REORDERED packets finally get placed).
 *   0 < d < W  -> arrived while a gap was open: BUFFER it (a reordered
 *                 packet — it came after a later one).
 *   d >= W     -> the hole is too deep to wait for: walk the delivery
 *                 point forward to s, delivering whatever the ring holds
 *                 and declaring the rest of the gap LOST. (Burst-loss
 *                 survival: the stream never stalls, it just gets holes.)
 *   d < 0      -> we already passed this seq: if it was delivered, it's a
 *                 DUPLICATE (drop); if it was declared lost and only now
 *                 straggled in, it's TOO OLD — its window closed (drop).
 *
 * The wraparound subtlety: seq is uint16 and goes 65535 -> 0. Comparing
 * with plain `<` would call 0 "older than" 65535 and stall the stream.
 * The fix is modular arithmetic: (int16_t)(s - next) yields the SIGNED
 * distance through the wrap, valid while true distances stay within
 * +/-32767 (guaranteed for a window of 8). This is RFC 1982 serial-number
 * arithmetic — the same idea real RTP implementations use.
 *
 * Compile + run (from the repo root, in WSL — two terminals):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/61-sequencing/receiver.c -o /tmp/b61-recv
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/61-sequencing/sender.c   -o /tmp/b61-send
 *   /tmp/b61-recv 9995                          # terminal 1
 *   /tmp/b61-send 127.0.0.1 9995                # clean run
 *   /tmp/b61-send 127.0.0.1 9995 --shuffle 6 --jitter 20 --loss 5 --dup-every 7
 *
 * BREAK IT ON PURPOSE (from phases/phase-6-rt-media.md):
 *   1. Burst loss:  /tmp/b61-send 127.0.0.1 9995 --jump 5
 *      The receiver reports the gap as loss and keeps flowing.
 *   2. Wraparound:  /tmp/b61-send 127.0.0.1 9995 --wrap
 *      Then sabotage the receiver: replace the modular distance
 *        int d = seq_dist(r.next, s);
 *      with a naive
 *        int d = (s < r.next) ? -1 : (s - r.next);
 *      rebuild, rerun --wrap: every packet after the wrap is misjudged as
 *      "too old"/duplicate and the delivery count collapses. Modular
 *      comparison is not pedantry — it is the fix for a real, classic bug.
 *
 * Done when: under 5% loss + reorder the receiver delivers the maximum
 * possible in-order stream (delivered + lost == seq numbers consumed) and
 * its numbers tell a story you can cross-check against the sender's.
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
#include <unistd.h>

#define PAYLOAD_LEN 48
#define HDR_LEN (2 + 4)
#define WINDOW 8    /* reorder window: hold up to 8 future packets */
#define RING 256    /* power of 2: history/dup ring, covers seq wrap */
#define DEFAULT_IDLE_MS 2000

struct cell {       /* one slot of the reorder ring */
    int present;
    uint16_t seq;
    uint32_t ts;
    uint8_t data[PAYLOAD_LEN];
};

struct receiver {
    uint16_t next;           /* next seq we are willing to deliver */
    int started;
    struct cell ring[RING];
    uint8_t delivered[RING]; /* 1 = this seq (mod RING) was delivered */
    /* stats */
    long delivered_n, reordered, lost, duplicates, too_late;
};

/* Modular distance from a to b: how far b is ahead of a, valid while the
 * true distance fits in a signed 16-bit. The (int16_t) cast IS the trick —
 * it maps the unsigned difference through the 65535->0 wrap. */
static int seq_dist(uint16_t a, uint16_t b)
{
    return (int)(int16_t)(uint16_t)(b - a);
}

static void deliver(struct receiver *r, uint16_t seq, uint32_t ts,
                    const uint8_t *data)
{
    printf("deliver seq=%-5u ts=%-10u payload[0]=0x%02x\n", seq, ts, data[0]);
    r->delivered[seq % RING] = 1;
    r->delivered_n++;
}

/* Deliver every consecutive buffered cell starting at r->next. */
static void drain(struct receiver *r)
{
    for (;;) {
        struct cell *c = &r->ring[r->next % RING];
        if (!c->present || c->seq != r->next)
            break;
        c->present = 0;
        deliver(r, c->seq, c->ts, c->data);
        r->next = (uint16_t)(r->next + 1);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <port> [idle_timeout_ms]\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);
    int idle_ms = (argc > 2) ? atoi(argv[2]) : DEFAULT_IDLE_MS;

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

    struct timeval tv = { .tv_sec = idle_ms / 1000,
                          .tv_usec = (idle_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    printf("receiver: UDP %d, window=%d, idle timeout %d ms\n", port, WINDOW,
           idle_ms);

    struct receiver r;
    memset(&r, 0, sizeof r);

    uint8_t buf[2048];
    for (;;) {
        ssize_t n = recvfrom(fd, buf, sizeof buf, 0, NULL, NULL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break; /* idle timeout -> final report */
            perror("recvfrom");
            break;
        }
        if (n < HDR_LEN) {
            printf("runt %zd bytes — dropped\n", n);
            continue;
        }

        uint16_t s = (uint16_t)((buf[0] << 8) | buf[1]);
        uint32_t ts = ((uint32_t)buf[2] << 24) | ((uint32_t)buf[3] << 16) |
                      ((uint32_t)buf[4] << 8) | (uint32_t)buf[5];
        const uint8_t *payload = buf + HDR_LEN;

        /* Sync like a real jitter buffer's ramp-up: the first arrival is
         * BUFFERED, not delivered, because with reordering it may not be
         * the stream's first packet at all. Delivery starts only when a
         * packet arrives that matches the (possibly rewound) start. */
        if (!r.started) {
            r.started = 1;
            r.next = s;
            struct cell *c0 = &r.ring[s % RING];
            c0->present = 1;
            c0->seq = s;
            c0->ts = ts;
            memcpy(c0->data, payload, PAYLOAD_LEN);
            printf("  sync     seq=%u buffered as provisional start\n", s);
            continue;
        }
        if (r.delivered_n == 0 && seq_dist(r.next, s) < 0) {
            /* Still syncing and something arrived EARLIER than our
             * provisional start: rewind. Nothing has been delivered yet,
             * so this is safe. Once the first packet is delivered, the
             * delivery point only ever moves forward. */
            r.next = s;
        }

        int d = seq_dist(r.next, s);

        if (d < 0) {
            /* Behind the delivery point. */
            if (r.delivered[s % RING] && -d < RING) {
                r.duplicates++;
                printf("  dup      seq=%u — already delivered, dropped\n", s);
            } else {
                r.too_late++;
                printf("  too-late seq=%u — window closed, dropped\n", s);
            }
            continue;
        }

        if (d >= WINDOW) {
            /* Hole too deep to wait for: walk the delivery point up to s,
             * salvaging whatever the ring holds, declaring the rest lost. */
            printf("  gap!     jumping from seq=%u to seq=%u\n", r.next, s);
            while (r.next != s) {
                struct cell *c = &r.ring[r.next % RING];
                if (c->present && c->seq == r.next) {
                    c->present = 0;
                    deliver(&r, c->seq, c->ts, c->data);
                } else {
                    r.lost++;
                }
                r.next = (uint16_t)(r.next + 1);
            }
            deliver(&r, s, ts, payload);
            r.next = (uint16_t)(s + 1);
            drain(&r);
            continue;
        }

        if (d == 0) {
            deliver(&r, s, ts, payload);
            r.next = (uint16_t)(s + 1);
            drain(&r);
            continue;
        }

        /* 0 < d < WINDOW: a future packet while a gap is open — buffer it. */
        struct cell *c = &r.ring[s % RING];
        if (c->present && c->seq == s) {
            r.duplicates++;
            printf("  dup      seq=%u — already buffered, dropped\n", s);
            continue;
        }
        c->present = 1;
        c->seq = s;
        c->ts = ts;
        memcpy(c->data, payload, PAYLOAD_LEN);
        r.reordered++;
        printf("  buffer   seq=%u (gap of %d before it)\n", s, d);
    }
    close(fd);

    /* Packets still sitting in the reorder buffer when the sender went
     * silent: not delivered, but NOT lost either (they may yet arrive).
     * Counting them as lost would be a lie — report them separately. */
    long in_buffer = 0;
    for (int i = 0; i < RING; i++)
        if (r.ring[i].present && r.ring[i].seq >= r.next)
            in_buffer++;

    /* ---------------- final report ---------------- */
    printf("\n==== receiver report ====\n");
    printf("delivered in order: %ld\n", r.delivered_n);
    printf("reordered (buffered, then placed): %ld\n", r.reordered);
    printf("lost (seq gaps declared):          %ld\n", r.lost);
    printf("duplicates dropped:                %ld\n", r.duplicates);
    printf("too-late dropped:                  %ld\n", r.too_late);
    printf("still buffered at exit:            %ld\n", in_buffer);
    long consumed = r.delivered_n + r.lost;
    if (consumed > 0)
        printf("loss %%: %.1f%%  (%ld lost of %ld consumed)\n",
               100.0 * (double)r.lost / (double)consumed, r.lost, consumed);
    printf("(delivered + lost = consumed seq numbers; `delivered` is the\n"
           " maximum possible in-order stream given what actually arrived.)\n");
    return 0;
}
