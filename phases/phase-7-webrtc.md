# Phase 7 — The Whole Elephant: WebRTC Backend (Bites 69–80)

*Goal: understand WebRTC from the server's seat, then build one.*

**The map, one more time.** WebRTC = **signaling** (your HTTP/WebSocket server,
relays offers/answers) + **ICE** (find a network path; STUN discovers your
address, TURN relays when nothing else works) + **DTLS** (authenticate + agree
keys) + **SRTP** (encrypted media on UDP). Browsers do the codec heavy lifting;
your C backend shuttles signaling, answers ICE checks, secures the channel,
and moves media.

Reality check: bites 71–77 use real libraries (libwebsockets, OpenSSL,
libsrtp) — the bites are about *wiring them correctly*, which is genuine
backend work. Every connection step has been pre-built by an earlier bite:
sockets (5), framing (54), time (59), STUN (66), DTLS theory (67).

---

## Bite 69 — WebRTC Anatomy Day

> **Goal:** Own the complete connection flow on paper before writing a line of code.
>
> **Learn:**
> - The sequence: page loads → JS gets offer from `RTCPeerConnection` → offer sent over *your* signaling channel → your server relays to the other peer (or itself answers) → answer returns → both exchange ICE candidates → connectivity checks (STUN binding requests!) → DTLS handshake → SRTP flows.
> - Who does what: browser = media capture, codecs, ICE full agent, DTLS, SRTP. Server (yours) = signaling relay, optionally ICE-lite endpoint / TURN / media processing.
> - "ICE-lite" = a server that *responds* to connectivity checks but doesn't initiate them — exactly what your backend implements in bite 74.
>
> **Do:**
> 1. Draw the full flow on paper (or in the repo as a diagram): every arrow labeled with the protocol and which side sends it.
> 2. Annotate each arrow with which earlier bite taught you that piece (STUN → 66, framing → 54, event loop → 51…). Gaps in your labels = bites to revisit.
> 3. Write the README skeleton for your final project now — the architecture won't change; only the fill-in details will.
>
> **Break it on purpose:** Explain the flow out loud without the paper. Where you stumble is where bite 69 wasn't done yet.
>
> **Done when:** The diagram exists in the repo, every arrow is labeled and attributed to a bite, and you can narrate it cold.

## Bite 70 — Observe Before Building

> **Goal:** Watch a complete real WebRTC session so every later byte has a face.
>
> **Learn:**
> - Running reference implementations (Pion's examples in Go, or Python's `aiortc` examples) shows the *behavior* you're replicating: signaling messages, STUN checks, SRTP flows.
> - Wireshark + `SSLKEYLOGFILE` (for DTLS key export in supporting stacks) lets you see inside; even without decryption, STUN packets and DTLS handshakes are plainly visible.
> - Your capture becomes the ground truth you debug against in bites 74–77.
>
> **Do:**
> 1. Install and run a Pion or aiortc echo example between two browser tabs. It works — good, that's the elephant already eaten by someone else; you're studying the teeth marks.
> 2. Capture the whole session: `tcpdump -i any -w webrtc.pcap port 3478 or port 8080 or udp` (adjust to the demo's ports). Open in Wireshark.
> 3. Identify in the capture: the signaling messages (WebSocket frames), STUN Binding requests/responses, the DTLS handshake, and the SRTP/RTP flow. Screenshot each with annotations into the repo.
>
> **Break it on purpose:** Block UDP between the peers (firewall rule) and rerun — watch ICE fail or fall back to TURN. Now "why TURN exists" is not trivia; it's something you watched happen.
>
> **Done when:** Four annotated screenshots in the repo (signaling, STUN, DTLS, media) — each with a one-line caption in your own words.

## Bite 71 — WebSocket Signaling Channel in C

> **Goal:** A WebSocket server in C — your signaling transport.
>
> **Learn:**
> - WebSocket = HTTP upgrade (`Upgrade: websocket`, `Sec-WebSocket-Key` → SHA-1 magic GUID response) then a small framing protocol: `[fin/opcode:1][mask/len:1][ext len][mask key:4][payload]`. You have every skill for this: HTTP (55), SHA-1 via OpenSSL (64), framing (54).
> - Use **libwebsockets** (`apt install libwebsockets-dev`, `pkg-config --cflags --libs libwebsockets`) for the sane path — hand-rolling WS is a rewarding detour, not a requirement.
> - Integration shape: WS server alongside your httpd's event loop (bite 51's pattern, one more fd type).
>
> **Do:**
> 1. libwebsockets echo server on port 8081. Test with a browser one-liner (`new WebSocket("ws://localhost:8081")` in the devtools console) — send, receive back.
> 2. Two browser tabs connect; server relays any message from tab A to tab B and vice versa. **You now have a signaling channel.**
> 3. Log every frame's opcode and size. Handle: fragmented messages, ping/pong (keepalive), abrupt disconnects.
>
> **Break it on purpose:** Close a tab mid-session; the server must clean up the peer (and never relay to a dead peer). Send a 1 MB message; the server must not choke (fragmentation handling).
>
> **Done when:** Two tabs echo through your C server, disconnects clean up, valgrind-clean.

## Bite 72 — The Signaling Protocol

> **Goal:** Define and relay real WebRTC signaling messages.
>
> **Learn:**
> - WebRTC signaling messages in the wild: `{"type":"offer"|"answer"|"candidate", ...}` — you define your own envelope; WebRTC doesn't mandate the wire format, only that the peers understand each other.
> - Parsing JSON in C: **cJSON** (`apt install libcjson-dev`) — small, clean API. Extract `type`, `sdp`, `candidate` fields; strict on missing fields.
> - The server's role: a *relay*, not a participant. It never generates SDP; it routes it. (Media servers go further — that's bite 77.)
>
> **Do:**
> 1. Spec your signaling envelope in a comment block (your bite-53 discipline, applied). Unknown type → error message back to sender.
> 2. Server keeps a peer registry: connection → peer id. `{"type":"join","id":"a"}` registers; messages addressed `{"to":"b",...}` get routed; unknown `to` → error.
> 3. Two tabs: A joins as "a", B as "b", A sends a message to "b" — B receives it. This loop is your signaling plane, complete.
>
> **Break it on purpose:** Send malformed JSON, missing `type`, unknown `to`, a 10 KB message, JSON with duplicate keys. Every case → clean error or clean handling, never a crash, never a relay of garbage.
>
> **Done when:** Routing works between two tabs under abuse; cJSON errors are handled; valgrind-clean.

## Bite 73 — SDP Literacy

> **Goal:** Read Session Description Protocol as structured text — because you'll need its fields for DTLS and ICE.
>
> **Learn:**
> - SDP = line-based (`key=value`): `v=`, `o=`, `m=audio 9 UDP/TLS/RTP/SAVPF 0`, `a=` attributes. The ones your backend cares about: `m=` (media + port), `a=fingerprint:` (certificate hash!), `a=candidate:` (ICE candidates), `a=ice-ufrag`/`a=ice-pwd` (credentials for bite 74).
> - Offer/answer semantics: one side proposes, the other selects. Your backend mostly *observes* this exchange.
> - The fingerprint you parse here is *the* input to bite 75's certificate verification. Nothing in WebRTC is unconnected.
>
> **Do:**
> 1. From a browser tab, `pc.createOffer()` and send it through your server; log the full SDP server-side. Read every line.
> 2. Write `sdp_extract(const char *sdp, ...)` — parse out: media type/port, fingerprint (algo + hex), ice-ufrag, ice-pwd, candidate lines. Store in a struct.
> 3. Verify: feed a hand-crafted minimal SDP and a real browser offer; both parse; garbage SDP → clean error.
>
> **Break it on purpose:** Corrupt the fingerprint (flip a hex char) and *keep that value* — bite 75 must then reject the DTLS cert. You're planting the tripwire now, on purpose.
>
> **Done when:** Real browser offers parse into your struct, and you can name what each extracted field is used for in the remaining bites.

## Bite 74 — ICE-lite Responder

> **Goal:** Answer connectivity checks on your media UDP port — your server becomes a real ICE endpoint.
>
> **Learn:**
> - ICE pairs candidates and runs STUN binding requests *with credentials* (short-term: `USERNAME = "remoteufrag:localufrag"`, `MESSAGE-INTEGRITY` = HMAC-SHA1 over the message with `ice-pwd`). Bite 66's STUN + bite 64's HMAC = this bite.
> - ICE-lite: you respond to checks; you never send your own. Sufficient for a server that's always reachable.
> - The media socket now has two jobs: STUN checks and (soon) media — demux by checking the first byte's STUN magic (`0x00`–`0x03` type range + magic cookie at offset 4).
>
> **Do:**
> 1. Open the media UDP port (e.g., 50000). Parse STUN Binding Requests (extend bite 66's parser — same wire format).
> 2. Verify: request method, magic cookie, `USERNAME` contains your ufrag from the SDP, `MESSAGE-INTEGRITY` HMAC validates with the peer's `ice-pwd`. Failures → STUN error response (400/401), never silence (silence breaks ICE timers).
> 3. Respond: Binding Response with XOR-MAPPED-ADDRESS + MESSAGE-INTEGRITY. Then watch the browser's ICE state turn "connected" in `chrome://webrtc-internals`.
>
> **Break it on purpose:** Respond with a wrong MESSAGE-INTEGRITY → browser retries/fails the check. Then fix the HMAC input (it's the STUN message *up to* the integrity attribute — a classic parsing footgun) → success. The integrity computation is this bite's real exam.
>
> **Done when:** A browser peer reaches "connected" ICE state through *your* C responder. **Your server is now speaking WebRTC's transport layer.**

## Bite 75 — DTLS Handshake

> **Goal:** Complete the DTLS handshake with a browser and verify its certificate against the SDP fingerprint. The hardest bite in the plan — take it in two sittings, unashamed.
>
> **Learn:**
> - OpenSSL DTLS: `DTLS_method()` → `SSL_CTX` → `DTLSv1_listen` (or cookie callbacks) on your UDP socket → per-peer `SSL` with `SSL_set_fd`-equivalent for UDP (`BIO_dgram`) — DTLS-over-UDP plumbing is fiddlier than TLS-over-TCP; expect friction.
> - Same demux port as bite 74: incoming packets on the media socket are STUN *or* DTLS — distinguish by first byte (STUN = 0x00–0x03 with cookie at offset 4; DTLS = 0x16–0x15 range for handshake/alerts).
> - Verification: after handshake, `SSL_get_peer_certificate` → compute cert fingerprint (SHA-256, bite 64) → `strcmp` against the SDP fingerprint (bite 73). Mismatch = abort. This check is the whole security model.
>
> **Do (sitting 1):** OpenSSL DTLS server accepting a handshake from the browser; log every handshake step; handle retransmissions/timeouts (DTLS needs timer callbacks — `DTLSv1_set_timer_cb`).
> **Do (sitting 2):** Fingerprint verification wired to bite 73's parsed SDP; plant tripwire from bite 73 — corrupt fingerprint → handshake *must* abort with a clear log. Then restore → success.
>
> **Break it on purpose:** Accept a handshake without fingerprint verification against a *self-crafted* SDP — the browser happily completes. That's the MITM you exist to prevent. Then enforce the check. Never again optional.
>
> **Done when:** Browser and your C server complete DTLS; fingerprint verified (and verified *negatively* with the planted tripwire); both sitting-1 friction points documented in the README. Valgrind: every SSL object freed.

## Bite 76 — SRTP: Keying and the First Media Packet

> **Goal:** Derive SRTP keys from DTLS, initialize libsrtp, and push a real RTP packet to the browser.
>
> **Learn:**
> - Key export: `SSL_export_keying_material` with the `EXTRACTOR-dtls_srtp` label and `use_srtp` extension values → client/server write keys + salts (RFC 5764 §4.2 — the exact recipe, follow it literally).
> - **libsrtp** (`apt install libsrtp2-dev`): `srtp_init` → `srtp_create` + policy (profile `SRTP_AES128_CM_SHA1_80`, keys from above, direction) → `srtp_protect`/`srtp_unprotect`.
> - RTP header (your bite-42 skills, final form): `[V=2,P,X,CC][M][PT:7][seq:16][timestamp:32][SSRC:32]` — 12 bytes, all big-endian. 20 ms of PCMU audio = 160-byte payload after the header.
>
> **Do:**
> 1. Export keys, build the srtp policy, init a session. Print derived key lengths — sanity-check against RFC 5764's numbers.
> 2. Craft an RTP packet: version 2, payload type 0 (PCMU), seq 1, timestamp 0, your SSRC, 160 bytes of silence (zeros). `srtp_protect` it. Send on the media socket.
> 3. Verify externally: Wireshark decodes it as RTP/SRTP, or `chrome://webrtc-internals` shows packets received on the inbound stream. First byte of your server's media has reached a browser, encrypted.
>
> **Break it on purpose:** Use the client key where the server key belongs (key-direction mixup — the #1 SRTP setup bug) — browser silently drops the packet. Swap back — arrives. Direction errors are silent; only external verification catches them.
>
> **Done when:** A browser-visible SRTP packet originates from your C server, and you can explain where each key byte came from.

## Bite 77 — Media Echo

> **Goal:** Full-duplex: receive the browser's audio and send it back — the loopback that proves your media plane.
>
> **Learn:**
> - Inbound: browser's SRTP → `srtp_unprotect` → RTP header parse → payload (PCMU if you negotiated it — no codec library needed; G.711 is just 8-bit companded samples).
> - Outbound: rewrite the RTP header (your SSRC, your seq/timestamp counters — bite 59's clocks ticking at 20 ms) around the received payload → `srtp_protect` → send back.
> - Pacing discipline: echo per received packet (passive) vs paced retransmission (active). Start passive; understand why real servers re-pace (jitter buffers).
>
> **Do:**
> 1. Full receive path: demux → DTLS-managed → `srtp_unprotect` → RTP parse → log seq/ts/payload size. Verify sequence numbers arrive (mostly) in order — netem can teach the receiver humility here too.
> 2. Echo path: re-header + `srtp_protect` + send. Browser test: a `<audio autoplay>` element fed by your returned stream — **you hear yourself through your C server.** The plan's emotional peak; schedule something nice after.
> 3. Robustness: unknown SSRC, wrong payload type, malformed RTP, duplicate seq → drop + log, never crash.
>
> **Break it on purpose:** Echo with the *browser's* SSRC instead of your own — WebRTC requires distinct SSRC per direction; the browser rejects/confuses. Then fix. Header-field discipline, learned audibly.
>
> **Done when:** You hear yourself loop back through your own C WebRTC backend, under mild netem impairment, valgrind-clean.

## Bite 78 — TURN: When P2P Fails

> **Goal:** Understand and provision the relay path for peers that can't connect directly.
>
> **Learn:**
> - Symmetric NATs break direct P2P: each peer's STUN-learned address only works from *that* peer's direction. TURN = a server with a public address that both peers can reach, relaying media.
> - Production answer: run **coturn** (`apt install coturn`) as a sidecar; your backend hands out TURN credentials (time-limited, HMAC-signed — short-term credential mechanism) inside the ICE candidates it relays.
> - Learning answer (optional, hard mode): implement the TURN Allocate + Permission + Send/Data subset on your UDP server. Only if you want depth; coturn integration is the professional choice.
>
> **Do:**
> 1. Install and configure coturn with a static auth secret; enable `lt-cred-mech`. Verify with `turnutils_uclient` against yourself.
> 2. Your signaling: embed the TURN server + time-limited credentials in the ICE configuration your frontend receives. Generate credentials server-side (HMAC of username:expiry with the shared secret — bite 64's HMAC, doing its real job).
> 3. Prove relay works: block direct UDP between two peers (firewall/netem blackhole), rerun the echo — media now flows *through* coturn. Capture it in Wireshark: two legs, one relay.
>
> **Break it on purpose:** Hand out expired credentials → TURN allocation fails → ICE fails → you've now seen the exact failure mode your credential-expiry logic prevents. Fix the expiry window.
>
> **Done when:** A connection succeeds *through* TURN where direct UDP is blocked, and your server generates valid time-limited credentials.

## Bite 79 — Hardening & Operations

> **Goal:** Make it survive reality: bad networks, bad peers, long uptimes, shutdowns.
>
> **Learn:**
> - Resource lifecycle: every connection owns (socket, SSL*, srtp session, buffers, timers). One owner each (bite 26) — teardown on disconnect frees all five, in order, exactly once.
> - DoS surface: unauthenticated packets on the media port, garbage WebSocket frames, signaling floods. Rate-limit and drop; never allocate unbounded memory on unauthenticated input.
> - Observability: structured logs (connect, ICE state, DTLS state, media stats — packets/loss/jitter from bite 61's counters). A server you can't see into is a server you can't debug.
>
> **Do:**
> 1. Disconnect/reconnect matrix: kill a tab mid-handshake, mid-media, mid-DTLS; reconnect both peers repeatedly. Every path cleans up; counters stay correct; no fd/slot leaks (`/proc/<pid>/fd` count is stable across churn).
> 2. Valgrind the *server* (not the browser) across a full session with connect/disconnect churn. Zero leaks, zero errors.
> 3. Graceful shutdown: SIGTERM → stop accepting → close peers in order → final stats log → exit 0. And a stats endpoint or periodic log: uptime, peers served, packets relayed, loss seen.
>
> **Break it on purpose:** 500 garbage UDP packets/second at the media port from a loop. CPU stays sane, memory stays flat, legitimate peers unaffected. If not — you've found your rate-limiter homework; write it.
>
> **Done when:** Churn-clean, flood-resistant, valgrind-clean, graceful shutdown, stats visible. Committed as `pre-final: hardening`.

## Bite 80 — ✅ Final Checkpoint: The WebRTC Backend, Shipped

> **Goal:** The elephant, eaten. A real C WebRTC backend in your portfolio.
>
> **Do:**
> - **The system:** WebSocket signaling (two browser tabs), SDP parsing, ICE-lite STUN responder, DTLS with fingerprint verification, SRTP audio loopback, coturn TURN integration for hostile networks, structured logging, graceful shutdown.
> - **The quality bar:** valgrind-clean full session; test script (curl + Python WS client + netem impairment scenarios) with a green run; `-Werror` build; README with the architecture diagram from bite 69, updated with reality.
> - **The human bar:** README's "What I learned" section in your own words — which bite was hardest, which lesson you'd give your past self.
> - **The ship bar:** push to a public repo with a demo video/gif of the loopback and a Wireshark screenshot of your SRTP packets.
>
> **Break it on purpose:** The final gauntlet: run every abuse script from every phase against the final system in one session. It should bore you — that's the goal. Boring means done.
>
> **Done when:** Committed as `checkpoint 7: webrtc backend`. The elephant is eaten — and you're the kind of engineer who can now read the *next* elephant's RFCs on sight.
