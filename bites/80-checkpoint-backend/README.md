# Bite 80 — Final Checkpoint: The WebRTC Backend in C

One process, four planes, zero browsers required to *prove* it works —
although every plane speaks the exact protocol a browser would speak.

```
      signaling plane                       media plane (one UDP port!)
 browser tab A <--WS tcp/8081--> +---------------------+
 browser tab B <--WS tcp/8081--> |   bites/80 server   |  STUN Binding <----> peer
                                 |                     |  DTLS 1.2     <----> peer
                                 |  (C, ~1000 lines)   |  SRTP/RTP     <----> peer
                                 +---------------------+
                                     |        |     |
                            libwebsockets  OpenSSL  libsrtp2
                            (JSON relay)   (DTLS)   (AES-128-CM/HMAC-SHA1-80)
```

## The layers, in the order a connection meets them

| Layer | Bite | What this server does |
|-------|------|-----------------------|
| Signaling | 71/72 | libwebsockets WS server on 8081; JSON `join`/`offer`/`answer`/`candidate` relay with a peer registry; strict parse, verbatim relay, clean errors |
| SDP | 73 | (parsing lives in bites/73-sdp; the server consumes its *outputs*: ufrag/pwd/fingerprint) |
| ICE | 74 | ICE-lite on UDP 50000: validates STUN Binding requests (USERNAME carries our ufrag, MESSAGE-INTEGRITY = HMAC-SHA1 keyed with our ice-pwd — RFC 8445 §7.2.2), answers with XOR-MAPPED-ADDRESS + integrity |
| Security | 75 | DTLS 1.2 on the SAME UDP port; after the handshake, SHA-256 of the peer certificate MUST equal the expected fingerprint (`client-fingerprint.txt`, which stands for the SDP's `a=fingerprint:` line) or the connection dies |
| Keys | 76 | `SSL_export_keying_material("EXTRACTOR-dtls_srtp")` → 60 bytes → client/server write keys + salts (RFC 5764 §4.2) → two libsrtp2 sessions (outbound = our key, inbound = peer's key) |
| Media | 77 | SRTP echo: unprotect → parse RTP header (V=2, PT=0 PCMU) → **re-header with OUR SSRC/seq/timestamps** → protect → send back |
| Ops | 79 | Fixed per-peer buffers, flood drops, idle timeout, signalfd-driven graceful shutdown, per-plane counters merged at exit |

## Build (inside WSL, from the repo root)

```bash
# once: certificates + the expected fingerprint
bash bites/80-checkpoint-backend/gen-cert.sh

# the server
gcc -Wall -Wextra -Werror -std=c17 -g \
    bites/80-checkpoint-backend/dtls_common.c \
    bites/80-checkpoint-backend/server.c -o /tmp/b80 \
    -lwebsockets -lssl -lcrypto -lsrtp2 -lcjson -pthread

# the browser test double
gcc -Wall -Wextra -Werror -std=c17 -g \
    bites/80-checkpoint-backend/dtls_common.c \
    bites/80-checkpoint-backend/test_client.c -o /tmp/b80t \
    -lwebsockets -lssl -lcrypto -lsrtp2
```

## Run

```bash
/tmp/b80 &                     # WS :8081, media UDP :50000
timeout 40 /tmp/b80t           # full 4-phase loopback; prints PASS/FAIL per check
```

Expected tail: `test: ALL PHASES PASSED (0 failure(s))`, and in the server log:
`srtp: sessions ready — media echo is LIVE` followed by ten `media: echoed`
lines.

Abuse scenarios (each is a "break it on purpose" from the plan):

```bash
timeout 20 /tmp/b80t --flood-udp     # 2000 garbage UDP packets; a valid ICE
                                     # check right after must still succeed
timeout 40 /tmp/b80t --corrupt-fp    # completes a DTLS handshake with a cert
                                     # whose fingerprint is NOT the signaled
                                     # one; server must reject the data path
kill -TERM <pid>                     # graceful: peers closed, final stats, 0
```

Valgrind gate (the quality bar):

```bash
valgrind --leak-check=full --error-exitcode=9 /tmp/b80 &   # run a full session, then SIGTERM
# expect: ERROR SUMMARY: 0 errors, definitely lost: 0 bytes
```

## What is simplified (the honest list)

1. **No real SDP offer/answer generation.** A WebRTC server that *answers*
   must build an SDP (media sections, BUNDLE, fingerprint of its own cert,
   ICE credentials, DTLS setup attribute). Here the server's credentials and
   fingerprint are pre-generated constants (gen-cert.sh + defines), and the
   "offer" that flows through signaling is relayed verbatim between the two
   WS peers but never parsed to configure the media plane. Wiring
   bites/73-sdp's parser to *configure* the ICE/DTLS layers per session is
   the deliberate next step (it changes ~30 lines: read ufrag/pwd/fingerprint
   from the parsed struct instead of the #defines).
2. **One media session at a time.** The media plane keeps a single
   `media_t`; a second peer's ClientHello replaces the first session. A real
   backend maps (address → session) in a table. The demux and per-session
   state machine are already shaped for it.
3. **The test double is the browser.** `test_client.c` speaks WebSocket,
   STUN-with-credentials, DTLS-client-role-with-certificate, and SRTP —
   the wire behavior of a browser — but it does not run a codec pipeline or
   `chrome://webrtc-internals`. PCMU payload bytes are synthetic patterns.
4. **No DTLS HelloVerify cookie on the final server.** Bite 75's server
   implements the full `DTLSv1_listen` cookie exchange; the checkpoint's
   hand-rolled accept path (peek → pin peer → SSL_accept) skips the cookie
   state machine, which means the anti-DoS guarantee of HelloVerify is not
   present here. Restoring it means running DTLSv1_listen's loop instead of
   the manual state machine.
5. **PCMU only, no codec, no RTCP.** The echo passes G.711 payloads through
   untouched; there is no Opus, no RTCP SR/RR, no jitter buffer. Echo pacing
   is passive (one packet out per packet in); a real server re-paces from a
   jitter buffer on a 20 ms ticker (bite 59/61).
6. **Signaling has no TLS (wss://).** Real deployments terminate TLS in
   front of the WS server. Bite 57 covered TLS client-side; server-side TLS
   for lws is a config flag away but not exercised here.
7. **TURN is configured, not integrated** (bite 78): notes + coturn config
   + a compile-checked Allocate client, but the checkpoint does not hand out
   time-limited TURN credentials in its signaling relay.
8. **Threading model:** the lws service loop runs on its own thread and the
   media plane on the main thread, with *zero* shared mutable state (the
   peer registry is lws-thread-only; stats counters are per-plane and merged
   at exit). This side-steps locking entirely — a multi-peer version would
   need a mutex around the session table.
9. **Valgrind "possibly lost" blocks** (~5.5 KB from libnss3, which Ubuntu's
   libsrtp2 initializes internally, plus glibc's dlopen caches) are library
   internals; our own allocations free completely (`definitely lost: 0`,
   `ERROR SUMMARY: 0 errors`).

## What I learned (the meta-lesson)

The hard part of a WebRTC backend is not any single protocol — it is the
*shared demux point*: one UDP socket carrying STUN, DTLS and SRTP, where a
wrong first-byte classification or an over-eager `recvfrom` (eating a
ClientHello that OpenSSL's BIO still needed) costs hours. The second lesson
is that integrity checks live or die on byte-exact ranges: STUN's HMAC
covers the message *up to the attribute preceding MESSAGE-INTEGRITY*, the
FINGERPRINT CRC covers bytes *as transmitted*, and SRTP's key direction is
silent when wrong. Every one of those bit a real debugging session in this
phase — which is exactly why the bites have you break them on purpose.
