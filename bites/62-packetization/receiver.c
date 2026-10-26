/*
 * Bite 62 — Packetization: MTU-sized fragments and reassembly (RECEIVER)
 *
 * Counterpart to bites/62-packetization/sender.c. Reassembly state per
 * frame:
 *
 *   struct frame {
 *       uint16_t seq;        — frame number (key)
 *       uint16_t frag_count; — from ANY fragment (it rides on every one)
 *       uint16_t have_count; — how many fragments arrived so far
 *       uint8_t *data;       — frame_len buffer, fragments land at
 *                              frag_idx * 1200
 *       uint8_t *have;       — per-fragment bitmap
 *       long     first_us;   — when the first fragment arrived
 *   };
 *
 * Two failure modes the lesson cares about:
 *
 *   1. INCOMPLETE frames: one fragment lost => the frame can never be
 *      complete. We keep it 200 ms (in case of reordering) then DROP it
 *      and say so. Never deliver partial data as if whole.
 *   2. REORDERED fragments: they may arrive in any order — the header
 *      carries indices precisely so arrival order doesn't matter.
 *
 * Completeness is detectable WITHOUT counting bytes: when have_count
 * reaches frag_count (and the last fragment is frag_cnt-1), the frame is
 * whole. Everything else is garbage no matter how convincing it looks.
 *
 * Compile + run (from the repo root, in WSL — two terminals):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/62-packetization/receiver.c -o /tmp/b62-recv
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/62-packetization/sender.c   -o /tmp/b62-send
 *   /tmp/b62-recv 9996 /tmp/out.bin &   # then in another terminal:
 *   /tmp/b62-send /tmp/test.bin 127.0.0.1 9996          # clean run
 *   cmp /tmp/test.bin /tmp/out.bin && echo IDENTICAL    # byte-perfect!
 *   /tmp/b62-recv 9997 /tmp/out2.bin &
 *   /tmp/b62-send /tmp/test.bin 127.0.0.1 9997 --loss 10 --shuffle 8
 *   cmp /tmp/test.bin /tmp/out2.bin || echo "differs: dropped frames are MISSING, not corrupt"
 *
 * BREAK IT ON PURPOSE (from phases/phase-6-rt-media.md):
 *   Deliver a frame missing one fragment: in reassembly_sweep(), instead of
 *   dropping incomplete frames after 200 ms, write them out anyway (remove
 *   the `complete` check). Run with --loss 10, then cmp: the first differing
 *   byte is a 1200-byte hole of stale/zero bytes inside a "delivered" frame.
 *   That is the corruption real-time systems must refuse to ship: a decoder
 *   fed a truncated frame glitches visibly/audibly. Re-enable the check and
 *   the corrupt frames vanish from the output — dropping with a report is
 *   honest; silent corruption is the cardinal sin.
 *
 * Done when: with 10% loss, complete frames are byte-perfect and incomplete
 * frames are dropped with a report, never delivered corrupt.
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

#define FRAG_PAYLOAD 1200
#define HDR_LEN (2 + 2 + 2)
#define MAX_FRAGS 64      /* 64 KiB / 1200 = 55, small headroom */
#define FRAME_TIMEOUT_MS 200
#define SWEEP_EVERY_MS 50
#define MAX_TRACKED 64    /* reassembly table slots (hash keyed) */

struct frame {
    int used;
    uint16_t seq;
    uint16_t frag_count;
    uint16_t have_count;
    uint64_t first_us;
    uint64_t last_us; /* updated on EVERY fragment: expiry measures
                       * staleness of arrival, not total send duration */
    uint8_t *data;
    uint16_t len[MAX_FRAGS]; /* actual payload length of each fragment */
    uint8_t have[MAX_FRAGS];
};

static struct frame table[MAX_TRACKED];

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static struct frame *find_slot(uint16_t seq)
{
    /* Open-addressed by seq — 64 slots is plenty for a demo stream. */
    for (int i = 0; i < MAX_TRACKED; i++) {
        int idx = (seq + (uint16_t)i) % MAX_TRACKED;
        if (!table[idx].used || table[idx].seq == seq)
            return &table[idx];
    }
    return NULL; /* table full: caller drops (real receivers evict oldest) */
}

/* Deliver a COMPLETE frame to the output file. Only ever called when
 * have_count == frag_count — the completeness check is the whole point.
 * Fragment sizes are remembered per slot, so a short last fragment
 * (frame_size % 1200) is written back at exactly its original size. */
static void reassembly_flush(struct frame *fr, FILE *out, long *delivered_bytes)
{
    size_t frame_len = 0;
    for (uint16_t i = 0; i < fr->frag_count; i++) {
        fwrite(fr->data + (size_t)i * FRAG_PAYLOAD, 1, fr->len[i], out);
        frame_len += fr->len[i];
    }
    *delivered_bytes += (long)frame_len;
    printf("frame %u COMPLETE (%u frags, %zu bytes) -> delivered\n", fr->seq,
           fr->frag_count, frame_len);
    free(fr->data);
    memset(fr, 0, sizeof *fr);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <port> <output_file> [idle_ms]\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);
    const char *out_path = argv[2];
    int idle_ms = (argc > 3) ? atoi(argv[3]) : 2000;

    FILE *out = fopen(out_path, "wb");
    if (!out) {
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

    /* Short socket timeout = sweep cadence: we wake up often enough to
     * run the reassembly sweep, and track "sender went away" separately. */
    struct timeval tv = { .tv_sec = SWEEP_EVERY_MS / 1000,
                          .tv_usec = (SWEEP_EVERY_MS % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    printf("receiver: UDP %d -> %s (frame timeout %d ms)\n", port, out_path,
           FRAME_TIMEOUT_MS);

    long delivered_bytes = 0, frames_complete = 0, frames_dropped = 0;
    long frags_received = 0;
    uint64_t last_sweep = now_us();
    uint64_t last_packet_us = last_sweep; /* for the idle exit below */

    uint8_t buf[2048];
    for (;;) {
        ssize_t n = recvfrom(fd, buf, sizeof buf, 0, NULL, NULL);
        uint64_t now = now_us();

        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("recvfrom");
                break;
            }
            now = now_us(); /* timed out: fall through to the sweep */
            if (now - last_packet_us > (uint64_t)idle_ms * 1000ULL) {
                printf("idle %d ms — sender is done, finishing up\n", idle_ms);
                break;
            }
        } else if (n < HDR_LEN) {
            printf("runt %zd bytes — dropped\n", n);
            continue;
        } else {
            uint16_t seq = (uint16_t)((buf[0] << 8) | buf[1]);
            uint16_t idx = (uint16_t)((buf[2] << 8) | buf[3]);
            uint16_t cnt = (uint16_t)((buf[4] << 8) | buf[5]);
            size_t plen = (size_t)n - HDR_LEN;

            if (cnt == 0 || cnt > MAX_FRAGS || idx >= cnt ||
                plen > FRAG_PAYLOAD) {
                printf("malformed header seq=%u idx=%u cnt=%u len=%zd — "
                       "dropped (validate EVERYTHING off the wire)\n",
                       seq, idx, cnt, plen);
                continue;
            }

            struct frame *fr = find_slot(seq);
            if (!fr) {
                printf("reassembly table full — dropping frame %u\n", seq);
                continue;
            }
            last_packet_us = now;
            if (!fr->used) {
                fr->used = 1;
                fr->seq = seq;
                fr->frag_count = cnt;
                fr->first_us = now;
                fr->last_us = now;
                fr->data = calloc((size_t)cnt * FRAG_PAYLOAD, 1);
                if (!fr->data) { perror("calloc"); break; }
            } else if (fr->frag_count != cnt) {
                /* Contradictory metadata = hostile/garbled input */
                printf("frame %u: cnt changed %u->%u — dropping frame\n",
                       seq, fr->frag_count, cnt);
                free(fr->data);
                memset(fr, 0, sizeof *fr);
                continue;
            }
            fr->last_us = now;

            if (!fr->have[idx]) {
                fr->have[idx] = 1;
                fr->have_count++;
                fr->len[idx] = (uint16_t)plen;
                memcpy(fr->data + (size_t)idx * FRAG_PAYLOAD, buf + HDR_LEN,
                       plen);
                frags_received++;
                /* Duplicate fragments overwrite cleanly: same bytes land
                 * at the same offset, so dupes are naturally idempotent. */
            }

            if (fr->have_count == fr->frag_count) {
                /* Last-fragment flag lives in the arithmetic:
                 * idx == cnt-1 marks the tail; completeness = have_count. */
                reassembly_flush(fr, out, &delivered_bytes);
                frames_complete++;
            }
        }

        /* Periodic sweep: expire incomplete frames past the timeout. */
        if (now - last_sweep >= SWEEP_EVERY_MS * 1000ULL) {
            last_sweep = now;
            for (int i = 0; i < MAX_TRACKED; i++) {
                struct frame *fr = &table[i];
                if (fr->used &&
                    now - fr->last_us > (uint64_t)FRAME_TIMEOUT_MS * 1000ULL) {
                    printf("frame %u INCOMPLETE (%u/%u frags) — no fragment "
                           "for %d ms, DROPPED (one lost fragment = whole "
                           "frame lost)\n",
                           fr->seq, fr->have_count, fr->frag_count,
                           FRAME_TIMEOUT_MS);
                    frames_dropped++;
                    free(fr->data);
                    memset(fr, 0, sizeof *fr);
                }
            }
        }
    }
    close(fd);

    /* Free any frames still in the reassembly table (valgrind discipline:
     * every malloc has exactly one free on every exit path). */
    for (int i = 0; i < MAX_TRACKED; i++) {
        if (table[i].used) {
            free(table[i].data);
            table[i].used = 0;
        }
    }
    fclose(out);

    printf("\n==== reassembly report ====\n");
    printf("fragments received:      %ld\n", frags_received);
    printf("frames delivered:        %ld (%ld bytes)\n", frames_complete,
           delivered_bytes);
    printf("frames dropped (incomplete): %ld\n", frames_dropped);
    printf("verify with: cmp <original> %s\n", out_path);
    return 0;
}
