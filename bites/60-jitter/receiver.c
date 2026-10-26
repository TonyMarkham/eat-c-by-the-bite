/*
 * Bite 60 — The network in practice: loss, jitter, reordering (RECEIVER)
 *
 * The mirror half of bites/60-jitter/sender.c. It logs every datagram's
 * arrival order and — using the monotonic send stamp the sender put in the
 * packet — its travel time. At the end it prints an impairment report:
 * loss %, min/avg/max delay (latency + jitter), and the reorder count.
 *
 * This log IS the "tiny analyzer" from the lesson: the same report you will
 * demand from bites 61–68. Loss is inferred from sequence-number gaps
 * (highest seq seen + 1 minus arrivals) — which is exactly how an RTP
 * receiver must reason, since nobody ever announces "packet 37 died".
 *
 * Compile + run (from the repo root, in WSL — two terminals):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/60-jitter/receiver.c -o /tmp/b60-recv
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/60-jitter/sender.c   -o /tmp/b60-send
 *   /tmp/b60-recv 9990                       # terminal 1
 *   /tmp/b60-send 127.0.0.1 9990 --jitter 20 --shuffle 6 --loss 8
 *
 * BREAK IT ON PURPOSE (from phases/phase-6-rt-media.md):
 *   Run the sender with `--loss 100` against a fresh receiver: after the
 *   idle timeout the receiver reports 0 received / 100% loss. UDP will not
 *   tell you packets vanished — absence is only visible at the receiver,
 *   and only because we numbered the packets. Then, if you ever ran
 *   `sudo tc qdisc add dev lo root netem ...`, run
 *   `sudo tc qdisc del dev lo root` — forgetting it wrecks tomorrow's tests.
 *
 * Done when: the report shows min/avg/max delay, loss % and a nonzero
 * reorder count under `--jitter --shuffle --loss`, and all-in-order output
 * under a clean run.
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h> /* struct timeval for SO_RCVTIMEO */
#include <time.h>
#include <unistd.h>

#define PKT_LEN (2 + 8 + 64) /* seq:2 + sent_us:8 + 64-byte payload */
#define DEFAULT_IDLE_MS 2000

static void get_u16be(const uint8_t *p, uint16_t *v)
{
    *v = (uint16_t)((p[0] << 8) | p[1]);
}

static void get_u64be(const uint8_t *p, uint64_t *v)
{
    uint64_t acc = 0;
    for (int i = 0; i < 8; i++)
        acc = (acc << 8) | p[i];
    *v = acc;
}

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
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
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY); /* any local interface */
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        return 1;
    }
    printf("receiver: listening on UDP %d, idle timeout %d ms\n", port, idle_ms);
    printf("%-6s %-11s %-11s %-9s %s\n", "seq", "sent_at(ms)", "arrived(ms)",
           "delay(ms)", "note");

    /* One-shot receive timeout so we can detect silence and finish */
    struct timeval tv = { .tv_sec = idle_ms / 1000, .tv_usec = (idle_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    uint8_t buf[2048];
    long received = 0, reordered = 0;
    uint16_t max_seq = 0;
    int have_max = 0;
    uint64_t t0 = now_us();
    double sum_delay = 0.0, min_delay = 1e12, max_delay = 0.0;
    uint16_t prev_seq = 0;

    for (;;) {
        ssize_t n = recvfrom(fd, buf, sizeof buf, 0, NULL, NULL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break; /* idle timeout: no packets for idle_ms -> report */
            perror("recvfrom");
            break;
        }
        if (n < 10) {
            printf("runt datagram of %zd bytes — dropped (real receivers "
                   "validate headers before trusting anything)\n",
                   n);
            continue;
        }

        uint16_t seq;
        uint64_t sent_us;
        get_u16be(buf, &seq);
        get_u64be(buf + 2, &sent_us);

        double sent_at = (double)(int64_t)(sent_us - t0) / 1000.0;
        double arrived_at = (double)(int64_t)(now_us() - t0) / 1000.0;
        double delay = arrived_at - sent_at;

        const char *note = "";
        if (have_max && seq < prev_seq) {
            note = "OUT-OF-ORDER";
            reordered++;
        }
        prev_seq = seq;
        if (!have_max || seq > max_seq) {
            max_seq = seq;
            have_max = 1;
        }

        received++;
        sum_delay += delay;
        if (delay < min_delay) min_delay = delay;
        if (delay > max_delay) max_delay = delay;

        printf("%-6u %-11.3f %-11.3f %-9.3f %s\n", seq, sent_at, arrived_at,
               delay, note);
    }
    close(fd);

    /* ---- the report (this is your impairment analyzer) ---- */
    printf("\n==== impairment report ====\n");
    printf("received:            %ld datagrams\n", received);
    if (received > 0) {
        long expected = (long)max_seq + 1;
        long lost = expected - received;
        printf("highest seq:         %u -> expected %ld\n", max_seq, expected);
        printf("loss:                %ld (%.1f%%)  [inferred from seq gaps]\n",
               lost, 100.0 * (double)lost / (double)expected);
        printf("reordered:           %ld\n", reordered);
        printf("delay min/avg/max:   %.3f / %.3f / %.3f ms\n", min_delay,
               sum_delay / (double)received, max_delay);
        printf("jitter (avg|Δdelay|): see bite 61/68 analyzer — here the\n"
               "                     spread between min and max is the story.\n");
    } else {
        printf("NOTHING ARRIVED (loss 100%% case, or sender never ran).\n");
    }
    return 0;
}
