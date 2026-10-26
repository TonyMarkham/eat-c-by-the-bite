# Phase 6 — The Neck: Real-Time Media Concepts (Bites 59–68)

*Goal: the theory WebRTC is built on, learned by hand-rolling the primitives.*

WebRTC media is: UDP + sequence numbers + timestamps + crypto + real-time
discipline. This phase builds each primitive by hand so that Phase 7's
libraries are things you *understand* rather than things you *invoke*.

---

## Bite 59 — Time in C

> **Goal:** Measure and pace time correctly — real-time media runs on a clock.
>
> **Learn:**
> - `clock_gettime(CLOCK_MONOTONIC, &ts)` — the only clock for measuring durations (never wall-clock: it jumps with NTP). Convert: `ms = ts.tv_sec*1000 + ts.tv_nsec/1000000`.
> - Fixed-rate pacing: sleep-until-deadline (`nanosleep` with computed remainder), not sleep-fixed-amount. Drift is the enemy: sleep(fixed) accumulates error; sleep(until next tick) doesn't.
> - RTP timestamps are in media units (90 kHz for video, sample count for audio), not milliseconds — you'll compute both.
>
> **Do:**
> 1. Measure: spin a busy loop, time it with monotonic clock, print duration in ms with µs precision.
> 2. Fixed-rate loop: tick every 20 ms, print actual tick interval — run 100 ticks. First version: `usleep(20000)` per iteration (drifts). Second: absolute deadlines (doesn't). Compare the two outputs.
> 3. Compute a 48 kHz timestamp for the 100th 20-ms tick: `100 * 0.020 * 48000 = 96000`. Note how RTP timestamps work.
>
> **Break it on purpose:** Make the tick body take 25 ms (> the 20 ms budget). Watch the deadline version *skip* ticks rather than fall behind — that's back-pressure behavior you want to see, name it in your notes.
>
> **Done when:** The deadline-based pacer holds 20 ms ± small jitter over 100 ticks, and you can explain monotonic vs wall clock.

## Bite 60 — The Network in Practice: Loss, Jitter, Reordering

> **Goal:** See network impairment with your own eyes before writing code to fight it.
>
> **Learn:**
> - **Latency** = travel time; **jitter** = variance of latency; **loss** = datagrams that never arrive; **reordering** = arrival ≠ send order. UDP gives you all four, guaranteed.
> - Real-time media can't retransmit (too late!) — it must *mask* impairment: jitter buffers, sequence numbers, redundancy, concealment.
> - Tooling: `tc netem` (add delay/loss/reorder to a Linux interface) is the standard impairment harness.
>
> **Do:**
> 1. UDP sender: 100 numbered datagrams, 10 ms apart. Receiver: logs arrival order and timestamps. Run clean — all there, in order.
> 2. Add impairment: `sudo tc qdisc add dev lo root netem delay 40ms 20ms loss 5% reorder 5%` on the loopback interface. Rerun — now your log shows the real world.
> 3. Write a tiny analyzer (C or Python): computes min/avg/max delay, loss %, reorder count. This analyzer becomes your test harness for bites 61–68.
>
> **Break it on purpose:** Set `loss 100%` — receiver gets nothing; confirm your sender doesn't hang or crash waiting. Remove the qdisc afterward (`tc qdisc del dev lo root`) — forgetting this ruins tomorrow.
>
> **Done when:** Your analyzer produces a clean impairment report, and you never forget `tc qdisc del` again.

## Bite 61 — Sequencing: Your First RTP Receiver

> **Goal:** Reorder, detect loss, and dedupe — the core of RTP receiver logic, hand-rolled.
>
> **Learn:**
> - Add a header to your datagrams: `[seq:2 BE][timestamp:4 BE][payload]` — a miniature RTP header (real RTP: RFC 3550, same ideas, more fields).
> - Receiver state: highest seq seen + a small reorder window (e.g., 8). Late packets inside the window get placed; later-than-window = too old, drop.
> - Gap detection: expected seq vs received seq → loss count. Duplicate seq → discard.
>
> **Do:**
> 1. Sender from bite 60 now prefixes its header (put_u16be/put_u32be — bite 42 in its natural habitat).
> 2. Receiver: maintain window, reorder within it, report: delivered in-order count, reordered count, lost (gap) count, duplicate count.
> 3. Run under netem impairment (bite 60) — watch the report change with the network. This loop (impair → measure → improve) is real-time engineering in miniature.
>
> **Break it on purpose:** Send seq numbers with a jump of 5 (simulating a burst loss). Receiver must report loss, not crash or stall waiting for the missing packets. Then send seq wraparound (65535 → 0) — the classic bug; handle it with modular comparison.
>
> **Done when:** Under 5% loss + reorder, the receiver delivers the maximum possible in-order stream and its loss/reorder numbers match the analyzer's.

## Bite 62 — Packetization

> **Goal:** Split large payloads into MTU-sized datagrams and reassemble them — with loss in the middle.
>
> **Learn:**
> - Networks carry ≤ ~1400 bytes comfortably (MTU 1500 minus headers). Anything bigger must fragment — and *you* fragment, not the network (IP fragmentation exists but is unreliable and evil).
> - Fragment header: `[seq][frag_index][frag_count][payload]`. Last-fragment flag (or count) tells the receiver when reassembly completes.
> - Reassembly needs buffering per sequence — incomplete frames get dropped after a timeout. Losing *one* fragment loses the *whole* frame: this is why video keyframes are big and important.
>
> **Do:**
> 1. Sender: take a file (or generated bytes), slice into 1200-byte fragments, send with the fragment header at 5 ms intervals.
> 2. Receiver: reassemble by seq, deliver complete frames to stdout/a file, drop incomplete after 200 ms timeout with a report.
> 3. Run under 10% loss: some frames survive, some drop — report which and why. Verify delivered bytes are byte-identical to the original (`cmp`).
>
> **Break it on purpose:** Deliver a frame missing one fragment (disable the completeness check) — compare with `cmp`, see the corruption. Then re-enable the check. Partial data delivered as if whole = the exact bug class real-time systems must refuse.
>
> **Done when:** With 10% loss, complete frames are byte-perfect and incomplete frames are dropped with a report, never delivered corrupt.

## Bite 63 — Audio, Hands Dirty

> **Goal:** Touch real audio data so "media" stops being an abstraction.
>
> **Learn:**
> - PCM = raw samples. 16-bit signed, 48 kHz, mono = 48000 × 2 bytes/second ≈ 96 KB/s — that's what the wire carries.
> - A "frame" in audio ≈ 20 ms = 960 samples = 1920 bytes — exactly one RTP payload's worth. Now the 20-ms pacer from bite 59 has a job.
> - Codecs (Opus, PCMU) transform PCM ↔ compressed; WebRTC always negotiates which codec over SDP (bite 73).
>
> **Do:**
> 1. Record 3 s: `arecord -f S16_LE -r 48000 -c 1 test.raw` (or `ffmpeg -f alsa ...`). Play it back: `aplay` — confirm it's your voice.
> 2. C program: read the raw file, compute RMS amplitude per 20-ms chunk, print a crude waveform (ASCII bars). You are now "reading" audio.
> 3. Halve the amplitude (multiply samples by 0.5 with saturation), write out, play. Then halve *without* saturation, clip a loud recording, listen to the crackle. Saturation: heard.
>
> **Break it on purpose:** Interpret the PCM as unsigned instead of signed (just change the cast) — playback becomes DC-shifted noise. One wrong type interpretation = broken media. This is why header/type discipline in bites 41–42 mattered.
>
> **Done when:** You've recorded, analyzed (RMS bars), scaled, and played back audio — and can state the byte rate of 48kHz/16-bit/mono from memory.

## Bite 64 — Crypto Primitives with OpenSSL

> **Goal:** Use hashes, HMAC, and symmetric encryption fluently — the vocabulary SRTP assumes.
>
> **Learn:**
> - SHA-256: integrity/fingerprint (this is what appears in SDP `a=fingerprint:` lines!). HMAC: integrity *with* a secret key. AES-CTR: confidentiality with a key + counter/IV.
> - The OpenSSL EVP API is the modern interface: context → init → update → final. Verbose but uniform.
> - Key lesson: crypto primitives are never used alone — real systems combine them (SRTP = AES-CTR + HMAC, exactly this combo).
>
> **Do:**
> 1. `sha256_file`: hash a file, print the digest hex. Verify against `sha256sum`.
> 2. HMAC a message with a shared secret; flip one byte of the message; HMAC differs. Integrity demonstrated.
> 3. AES-256-CTR encrypt then decrypt a buffer; verify round-trip; tamper one ciphertext byte, decrypt — observe avalanche corruption (CTR mangles only the affected block-region — see it).
>
> **Break it on purpose:** Decrypt with the wrong key — garbage out, no error. Crypto fails *silently* when keys mismatch; authentication (HMAC/tag) is how you *detect* it. Write that sentence in your notes; it's why SRTP has auth tags.
>
> **Done when:** Your digest matches `sha256sum`, and you can explain fingerprint (SDP) vs HMAC (SRTP auth) in your own words.

## Bite 65 — Randomness & Key Agreement

> **Goal:** Generate secrets properly and watch two parties derive the same key from nothing shared but public numbers.
>
> **Learn:**
> - `RAND_bytes` (OpenSSL) for anything cryptographic; `rand()` is for games, never keys.
> - Diffie–Hellman: both sides exchange public values, both compute the same secret. Eavesdroppers see only the public values. This is (conceptually) how DTLS keys are agreed.
> - Use the high-level `EVP_PKEY` API (`EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, ...)`) — DH params by hand is 1998.
>
> **Do:**
> 1. Generate 32 random bytes with `RAND_bytes`; hex-print. Rerun — never the same twice.
> 2. In *one* program, simulate two parties (two keypairs), do the X25519 derive on both sides, print both shared secrets — identical. This is the "aha" bite; don't rush it.
> 3. Extend to two *processes*: parent/child (bite 45 fork!), exchange pubkeys over a pipe, derive, print hashes of the shared secrets — equal. A real (toy) key exchange.
>
> **Break it on purpose:** Give party B a *different* private key pair on one side — derived secrets differ; HMAC between them now fails. Key mismatch = silent garbage (bite 64's lesson) — detectable only via authentication.
>
> **Done when:** Two-process key exchange produces matching secrets, and you can explain in 3 sentences what problem DH solves.

## Bite 66 — STUN by Hand

> **Goal:** Speak STUN to a real server with raw bytes — your first genuine WebRTC wire protocol.
>
> **Learn:**
> - STUN (RFC 5389) message: `[type:2][length:2][magic cookie 0x2112A442:4][transaction id:12]` + attributes (`[type:2][length:2][value]`, padded to 4 bytes).
> - Binding Request = type 0x0001; the response's XOR-MAPPED-ADDRESS attribute (0x0020) reveals your public IP/port — the mechanism by which WebRTC peers discover where they appear on the internet.
> - XOR obfuscation exists because NATs are known to mangle raw IP addresses in payloads; XOR defeats that.
>
> **Do:**
> 1. Build the request: type 0x0001, magic cookie, 12 random bytes (RAND_bytes) as transaction id. No attributes needed. Send over UDP to `stun.l.google.com:19302`.
> 2. Parse the response: verify type (0x0101), magic cookie, and **matching transaction id** (anti-spoofing — check it!). Walk attributes.
> 3. Decode XOR-MAPPED-ADDRESS: `port ^ 0x2112` (top 16 bits of cookie), IP bytes XOR cookie byte-by-byte. Print your public IP:port.
> 4. Verify against `stunclient` (from `stun-client` package) or a browser-based STUN test — same answer?
>
> **Break it on purpose:** Mismatch the transaction id on purpose (accept any response) — then imagine an attacker injecting fake responses. The id check *is* the defense; now it's yours too.
>
> **Done when:** Your C program prints your public IP:port from a public STUN server, verified against an independent tool. **This is the first true WebRTC bite — notice the moment.**

## Bite 67 — DTLS, Conceptually

> **Goal:** Understand the DTLS handshake well enough to drive one (bite 75) without drowning.
>
> **Learn:**
> - TLS's handshake over UDP = DTLS: same messages (ClientHello, ServerHello, Certificate, Finished), plus retransmission and sequence handling because UDP loses packets.
> - In WebRTC: the *SDP fingerprint* (bite 73) pins which certificate is legitimate — you compare the cert you receive against the fingerprint learned via signaling. Skip this check = MITM. Never skip.
> - `use_srtp`: the TLS extension by which the DTLS session exports the keys SRTP will use (bite 76). Read RFC 5764 §4–5.
>
> **Do:**
> 1. Read RFC 5764 §4–5 and the DTLS overview section of RFC 6347 (intro only). Take notes: who sends what, in order.
> 2. Observe one: run the Pion/aiortc demo from bite 70 with `SSLKEYLOGFILE` set, open in Wireshark, follow the DTLS handshake — put real packet names to the words.
> 3. Write (in your notes) the exact sequence your server will perform in bite 75: listen on media port → DTLS accept → verify fingerprint → export SRTP keys.
>
> **Break it on purpose:** Conceptual bite — break your *understanding*: explain the handshake out loud from memory. Anything you can't explain is what you go back and re-read. (Writing it as a diagram in the repo earns bonus points.)
>
> **Done when:** You can draw the DTLS handshake message flow and state where the SRTP keys come from.

## Bite 68 — ✅ Checkpoint: Reorder Engine

> **Goal:** Assemble Phase 6's primitives into one working real-time pipeline.
>
> **Do:**
> - Three programs: `sender` (reads PCM file, chops into 20-ms frames, adds seq+timestamp header, sends UDP at a paced rate — optionally through netem impairment), `receiver` (reorders/dedupes/drops-late, writes in-order PCM), `analyzer` (loss/reorder/delay report).
> - The receiver plays the reassembled audio with `aplay` reading from a FIFO, or writes a playable file — *if the output sounds like the input, the pipeline works end to end.*
> - Sequence numbers, big-endian headers, monotonic pacing, graceful handling of 5–10% loss.
> - README: header format diagram, test matrix (clean vs impaired runs + their reports).
>
> **Break it on purpose:** Run the gauntlet: 20% loss, 50 ms jitter, heavy reorder. The pipeline degrades (drops, reports gaps) but never crashes, never hangs, never plays corrupted audio as if it were fine.
>
> **Done when:** Impaired audio arrives audibly intact where possible, reports are honest about what was lost, valgrind-clean. Committed as `checkpoint 6: reorder engine`. **You now understand what WebRTC media actually is. Phase 7 assembles it.**
