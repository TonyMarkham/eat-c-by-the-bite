/*
 * Bite 68 — CHECKPOINT: the reorder engine (ANALYZER)
 *
 * Reads the receiver's packets.log — one line per ARRIVAL, in arrival
 * order:
 *
 *     <seq> <rtp_ts> <sent_us> <recv_us> <status>
 *
 *   status D = delivered (immediately, in a gap-walk salvage, or drained
 *   from the reorder buffer); B = buffered on arrival; X = dropped
 *   (duplicate or too-late). The receiver logs D lines a second time when
 *   a buffered frame is finally placed, so "delivered" = number of D lines.
 *
 * and produces the impairment report the lesson asked for: delivery
 * delay (min/avg/max), jitter (mean absolute change of consecutive
 * delays — the RFC 3550-ish definition), reorder count (arrivals whose
 * seq is behind the highest seen so far, IN ARRIVAL ORDER), and loss %.
 *
 * This is the same report shape RTCP Receiver Reports give you in real
 * systems, in miniature.
 *
 * Compile + run (from the repo root, in WSL):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/68-checkpoint-reorder/analyzer.c -o /tmp/b68-an
 *   /tmp/b68-an /tmp/packets.log
 *
 * Notes on honesty:
 *   - delay/jitter come from sent_us stamps, valid on one machine
 *     (CLOCK_MONOTONIC is system-wide); across machines you'd need NTP/RTCP.
 *   - loss % is (consumed - delivered) / consumed where consumed = highest
 *     delivered seq + 1 counted modularly from the first delivered seq.
 *   - sent_us is 32 bits: it wraps every ~71.6 minutes; the wrap is handled
 *     with the same modular-distance trick as seq (int32 distance).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_LINES 100000

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <packets.log>\n", argv[0]);
        return 1;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        perror(argv[1]);
        return 1;
    }

    static uint32_t seqs[MAX_LINES];
    static int64_t delays_us[MAX_LINES];
    static char status[MAX_LINES];
    long n = 0;
    unsigned seq, ts;
    unsigned long sent, recv;
    char st;
    while (n < MAX_LINES &&
           fscanf(f, "%u %u %lu %lu %c", &seq, &ts, &sent, &recv, &st) == 5) {
        seqs[n] = seq;
        /* 32-bit modular distance handles the wrap */
        delays_us[n] = (int64_t)(int32_t)((uint32_t)recv - (uint32_t)sent);
        status[n] = st;
        n++;
    }
    fclose(f);

    long delivered = 0, xcount = 0;
    for (long i = 0; i < n; i++) {
        if (status[i] == 'D')
            delivered++;
        else if (status[i] == 'X')
            xcount++;
    }

    if (delivered == 0) {
        printf("no delivered frames — nothing to analyze\n");
        return 0;
    }

    long min_d = 0, max_d = 0, sum_d = 0;
    long reordered = 0, jitter_sum = 0, jitter_n = 0;
    long prev_delay = 0, stats_n = 0;
    unsigned max_seq = 0;
    static uint8_t seen[65536]; /* first occurrence of each seq */
    for (long i = 0; i < n; i++) {
        /* A buffered frame logs B on arrival and D when finally placed:
         * only the FIRST line for a seq contributes to arrival stats. */
        if (seen[seqs[i]])
            continue;
        seen[seqs[i]] = 1;

        if (stats_n == 0) {
            min_d = max_d = prev_delay = delays_us[i];
            max_seq = seqs[i];
        }
        if (delays_us[i] < min_d) min_d = delays_us[i];
        if (delays_us[i] > max_d) max_d = delays_us[i];
        sum_d += delays_us[i];
        if (stats_n > 0) {
            long diff = delays_us[i] - prev_delay;
            jitter_sum += diff < 0 ? -diff : diff;
            jitter_n++;
            prev_delay = delays_us[i];
        }
        /* network reorder: this ARRIVAL carries a seq older than one we
         * have already seen arrive (arrival order, not delivery order —
         * delivery is sorted by construction) */
        if ((int16_t)(uint16_t)(seqs[i] - max_seq) < 0)
            reordered++;
        if ((int16_t)(uint16_t)(seqs[i] - max_seq) > 0)
            max_seq = seqs[i];
        stats_n++;
    }

    /* span + loss from DELIVERED seqs (D lines) */
    long d_first = -1, d_last = -1;
    for (long i = 0; i < n; i++) {
        if (status[i] != 'D')
            continue;
        if (d_first < 0)
            d_first = seqs[i];
        d_last = seqs[i];
    }
    long consumed = (long)(int16_t)(uint16_t)((unsigned)d_last - (unsigned)d_first) + 1;
    long lost = consumed - delivered;

    printf("==== analyzer report (%s) ====\n", argv[1]);
    printf("arrivals logged:     %ld  (D=%ld delivered, B=%ld buffered on "
           "arrival, X=%ld dropped)\n",
           n, delivered, n - delivered - xcount, xcount);
    printf("delay min/avg/max:   %.3f / %.3f / %.3f ms\n",
           (double)min_d / 1000.0, (double)sum_d / (double)stats_n / 1000.0,
           (double)max_d / 1000.0);
    printf("jitter (mean |dd|):  %.3f ms\n",
           jitter_n ? (double)jitter_sum / (double)jitter_n / 1000.0 : 0.0);
    printf("reordered arrivals:  %ld\n", reordered);
    printf("consumed seq span:   %ld, lost %ld -> loss %.1f%%\n", consumed,
           lost, consumed ? 100.0 * (double)lost / (double)consumed : 0.0);
    printf("(loss here counts what never arrived; frames dropped as\n"
           " duplicates/late are arrivals, not loss.)\n");
    return 0;
}
