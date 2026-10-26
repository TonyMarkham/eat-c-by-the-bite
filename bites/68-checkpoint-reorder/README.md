# Bite 68 — Checkpoint: the Reorder Engine

Phase 6's primitives assembled into one working real-time pipeline:
paced send → jittered/reordered/lossy UDP → reorder buffer → in-order PCM out.

## The wire format

Every datagram carries one 20 ms frame of raw PCM (48 kHz, 16-bit, mono):

```
 0               1               2               3
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |            seq (2 B, BE)      |        rtp_ts (4 B, BE)       |
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |        sent_us (4 B, BE)      |                               |
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+                               +
 |                  PCM payload: 1920 bytes (960 samples)        |
 |                        (20 ms at 48 kHz)                      |
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

- **seq** — increments per frame, wraps 65535→0 (handled with modular
  `(int16_t)` distance — bite 61's lesson).
- **rtp_ts** — media timestamp: +960 per frame (20 ms × 48 kHz = 960
  samples — bite 59's "media units" lesson; RTP-style random start offset).
- **sent_us** — low 32 bits of the send-time monotonic clock, for the
  analyzer's delay/jitter numbers (loopback only: one machine shares one
  CLOCK_MONOTONIC).

1920-byte payloads exceed the ~1200-byte comfortable MTU from bite 62 —
fine on loopback; real deployments send compressed audio. (Noted honestly
in sender.c.)

## The three programs

| program     | role                                                              |
| ----------- | ----------------------------------------------------------------- |
| `sender`    | reads a PCM file, chops 20 ms frames, headers them, sends paced UDP with optional jitter / reorder / loss |
| `receiver`  | reorder buffer + dedupe + late/gap handling; writes in-order PCM and a per-frame log; prints an honest report |
| `analyzer`  | reads the log: delay min/avg/max, jitter, reorder count, loss %    |

## Build

```sh
gcc -Wall -Wextra -Werror -std=c17 -g bites/68-checkpoint-reorder/sender.c   -o /tmp/b68-send
gcc -Wall -Wextra -Werror -std=c17 -g bites/68-checkpoint-reorder/receiver.c -o /tmp/b68-recv
gcc -Wall -Wextra -Werror -std=c17 -g bites/68-checkpoint-reorder/analyzer.c -o /tmp/b68-an
```

## Run

Need a PCM input? Bite 63 synthesizes one: `/tmp/b63 gen /tmp/tone.raw 3`.

```sh
# terminal 1
/tmp/b68-recv 7100 /tmp/out.raw /tmp/packets.log
# terminal 2
/tmp/b68-send /tmp/tone.raw 127.0.0.1 7100
# anywhere, after the receiver reports
/tmp/b68-an /tmp/packets.log
cmp /tmp/tone.raw /tmp/out.raw          # clean run: IDENTICAL
aplay -f S16_LE -r 48000 -c 1 /tmp/out.raw
```

## Test matrix (actual runs, WSL2 loopback, 3 s / 150-frame tone)

| run | sender flags | receiver flags | result |
| --- | ------------ | -------------- | ------ |
| clean | (none) | default | 150/150 delivered, 0 loss, `cmp` byte-IDENTICAL, sender wall time 3.000 s for 3.000 s of audio (0 pace skips) |
| jittered | `--pace 2 --jitter 30 --shuffle 32` | `--window 32` | 150/150 delivered despite 114 buffered reorderings (max 31 ahead), `cmp` IDENTICAL |
| moderate | `--pace 10 --jitter 10 --shuffle 8 --loss 10` | `--window 32` | ~102 delivered, ~23 lost, ~10 late — degraded, honest report |
| gauntlet | `--pace 10 --jitter 50 --shuffle 24 --loss 20` | `--window 48` | ~65 delivered, ~53 declared lost, ~28 late — degrades (drops + gaps), never crashes, never hangs, never writes corrupt audio |

Exact numbers vary run to run — the *report* is the contract, not the
constants. Note: under loss+jitter the receiver's declared-lost count can
double-report frames that later arrive too late (they were declared lost in
a gap walk, then arrive and are dropped as late). That accounting quirk is
pointed out in receiver.c; real receivers have the same ambiguity.

## Impairment options

- `--jitter ms` — random 0..ms delay before each send (latency variance).
- `--shuffle n` — each window of n frames is emitted in random order
  (arrival reordering without root).
- `--loss pct` — silently skip pct of frames (pure loss).
- Real-network equivalent (root): `sudo tc qdisc add dev lo root netem
  delay 40ms 20ms loss 5% reorder 5%` — and ALWAYS
  `sudo tc qdisc del dev lo root` afterwards.

## Design notes / lessons crystallized

- **Deadline pacing, not fixed sleeps** (bite 59): the sender's timeline is
  absolute; overruns skip, never drift.
- **Sequence numbers make loss *visible*** (bite 60): absence is only
  detectable because frames are numbered.
- **A reorder window trades delay for completeness** (bite 61): frames up
  to `window` positions ahead are held; deeper gaps jump the delivery
  point and are declared lost.
- **Delivered output is never corrupt** (bite 62's cardinal rule): what
  didn't arrive is simply absent from `out.raw` — `cmp` proves it.
- **Reports must be honest** (bite 68's point): delivered + lost ==
  consumed; duplicates and late arrivals are counted separately; buffered
  frames at cutoff are reported as "buffered", not disguised as delivered.
