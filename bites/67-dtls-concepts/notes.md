# Bite 67 — DTLS, Conceptually (reading bite — no C required)

> Goal: understand the DTLS handshake well enough to *drive* one in bite 75
> without drowning. Sources skimmed per the lesson: RFC 6347 (intro/overview
> only), RFC 5764 §4–5, plus RFC 8122 for the `a=fingerprint` SDP attribute.

## 1. What DTLS is

TLS assumes a reliable, ordered byte stream (TCP). The WebRTC media path is
UDP. **DTLS = TLS 1.2's handshake and record layer adapted to UDP**:

- the same cryptographic core: cipher suites, certificates, key derivation;
- plus the machinery TCP gives TLS for free, which DTLS must re-invent:

| TLS on TCP gets it free      | DTLS on UDP must do it itself                          |
| ---------------------------- | ------------------------------------------------------ |
| reliability (retransmit)     | handshake retransmission timers (1s, doubling)         |
| ordering                     | explicit **epoch + sequence numbers** per record       |
| no lost-record ambiguity     | receive window; a lost record is *skipped*, not fatal  |
| stream integrity             | no stream ciphers/CBC-drag problems; per-record MAC     |
| MTU (TCP segments are small) | handshake messages are **fragmented** across datagrams |
| no spoofed resets            | **HelloVerifyRequest cookie** against flooding/DoS     |

The last point is a direct echo of bites 60–62: UDP gives you loss,
reordering and duplication, so DTLS is "TLS plus a mini reorder/retransmit
engine" — the same machinery you hand-rolled in bites 61 and 62.

## 2. The handshake, message by message (WebRTC flavor)

```
 client (browser)                     server (us, bite 75)
 ----------------                     --------------------
 ClientHello  ----------------------->            (offers suites, random,
                             HelloVerifyRequest      random nonce)
                            <-----------------------  (stateless cookie
 ClientHello (cookie) ------>                      anti-DoS check)
 ServerHello  <-----------------------            (picks suite, its random)
 Certificate   <-----------------------           (server cert; DTLS-SRTP
                                                   is usually MUTUAL — the
                                                   client sends one too)
 ClientKeyExchange <------------------            (ECDHE: bite 65's math!)
 CertificateVerify  <-----------------            (client signs the handshake
                                                   = proof of key possession)
 ChangeCipherSpec -------------------->
 Finished      --------------------->            (MAC over whole handshake)
 ChangeCipherSpec <------------------
 Finished      <---------------------

 *** handshake done: application data flows, or (in WebRTC) SRTP takes over ***
```

The words the lesson demands, each in one sentence:

- **ClientHello** — "here are my versions, cipher suites, random, and (on
  the retry) the cookie you gave me." Starts the handshake; may be re-sent
  on a timer if nothing comes back (DTLS retransmission).
- **ServerHello** — "I choose this version, this suite, here is my random."
- **Certificate** — the X.509 cert binding an identity to a public key.
  In WebRTC it's usually *self-signed*; its authenticity comes not from a
  CA but from the SDP fingerprint (below). Usually sent by BOTH sides
  (mutual TLS), because either side may be the media sender.
- **ClientKeyExchange / ServerKeyExchange** — the ephemeral DH exchange
  (X25519 in practice): the bite 65 handshake, wrapped in signatures.
- **CertificateVerify** — a signature over the handshake transcript;
  proves the sender holds the private key of the cert it sent.
- **Finished** — a MAC over the ENTIRE transcript. This is the integrity
  check that would have caught bite 65's `mismatch` bug: any tampered
  handshake byte makes Finished fail. This is where a MITM dies.

## 3. Where SRTP keys come from: `use_srtp` (RFC 5764 §4–5)

- The client includes a TLS extension, **`use_srtp`**, listing SRTP
  protection profiles it can do, e.g. `SRTP_AES128_CM_SHA1_80`
  (AES-128-CTR mode + HMAC-SHA1 with an 80-bit auth tag — bite 64's exact
  combo, in SRTP costume).
- If the server agrees, the DTLS session is labeled a "DataChannel-less
  media transport": NO application data is ever sent. Instead, after the
  handshake both sides call the TLS **exporter** (`RFC 5705` keying
  material exporter) with label `"EXTRACTOR-dtls_srtp"` and length
  = 2 × (key + salt) for the profile — 60 bytes for AES128_CM_SHA1_80.
- Each side takes the bytes on *its own* half: client writes client key+salt,
  server takes server key+salt (client key decrypts what server encrypts).
- Those keys initialize libsrtp (bite 76). The keys never appear in SDP;
  they live only in the two endpoints' memories.

Why an exporter: the master secret is already mixed from both sides'
randoms + the ECDHE result; the exporter derives purpose-specific keys
without exposing the master secret itself. Key separation = one handshake
can't leak keys between uses.

## 4. The fingerprint: pinning, not PKI

The SDP offer/answer carries, per m-line:

    a=fingerprint:sha-256 82:B1:EF:7C:50:C4:... (the cert's SHA-256, bite 64!)

Signaling (our WebSocket server, bite 71–72) carries SDP, so the fingerprint
is learned **out-of-band**. During the DTLS handshake we compare the
certificate we RECEIVE against this pin. Self-signed certs + out-of-band
pinning = no CA needed, MITM defeated (the attacker can't present a cert
matching the fingerprint without also owning the private key AND the
signaling channel).

**Never skip this check.** A DTLS server that accepts any certificate turns
every client into a victim of the first MITM on the media path.

## 5. DTLS vs TLS — the checklist answers

1. Transport: UDP datagrams vs TCP stream — so DTLS adds per-record
   sequence numbers, epochs, retransmission timers, handshake fragmentation.
2. Handshake messages may arrive twice, out of order, or never: each
   handshake flight has a timer; a lost flight is retransmitted whole.
3. DoS: HelloVerifyRequest cookie makes flooding expensive for the attacker
   (they must prove receipt of a server-chosen value per connection).
4. No TLS session resumption shortcuts in WebRTC media; a fresh full
   handshake per peer (though ICE restarts may reuse DTLS via resumption).
5. After the handshake, in WebRTC the socket multiplexes STUN + RTP/SRTCP
   (RFC 5764 demux: first byte 0–3 = STUN, 128–191 = RTP/RTCP — bite 74).

## 6. The exact sequence our server will perform in bite 75

1. After ICE connectivity is confirmed on the media UDP port (bite 74's
   ICE-lite answers STUN Binding requests), the client sends DTLS
   ClientHello to the same port.
2. `SSL_accept`-style state machine (OpenSSL `DTLSv1_listen` → `SSL_accept`
   with `DTLSv1_get_timeout`/`DTLSv1_handle_timeout` driving retransmits)
   completes the handshake above.
3. When the handshake finishes, retrieve the peer certificate, compute its
   SHA-256 (bite 64!) and **compare against the SDP fingerprint**. Mismatch
   ⇒ abort the connection.
4. Check the negotiated `use_srtp` profile (RFC 5764 §4.1.1: we must have
   offered/accepted `SRTP_AES128_CM_SHA1_80` or similar).
5. Export keying material via `SSL_export_keying_material(...,
   "EXTRACTOR-dtls_srtp", 19, NULL, 0, 60)`; split per RFC 5764 §4.2 into
   client-write key(16) + client salt(14) + server-write key(16) + salt(14),
   plus 2×2 bytes of EKT... (no EKT: just key/salt; use_mki = 0).
6. Hand the right half to libsrtp (`srtp_init`, create a session per
   direction) — bite 76 sends the first real RTP packet.

## 7. Break it on purpose (conceptual)

Explain out loud, from memory, no peeking:

1. Why does DTLS need retransmission but TLS doesn't?
2. What does Finished prove, and which of my earlier bites does it replace?
3. Where do SRTP keys come from, and why not just use the TLS master secret
   directly as an SRTP key?
4. Who checks the fingerprint, against what, and what attack does it stop?
5. Why is the media path *multiplexed* on one port, and how do we tell
   STUN packets from RTP packets on it?

Anything that doesn't come out whole = the section to re-read. (Drawing
section 2's diagram from memory earns the bonus points the lesson mentions.)

## 8. Glossary cross-reference (what maps to what we built)

| WebRTC/DTLS word        | Our bite that built the concept                  |
| ----------------------- | ------------------------------------------------ |
| Jitter buffer / reorder | bite 61's reorder window + bite 68's checkpoint  |
| Fragmentation           | bite 62's 1200-byte packetization                |
| AES-CTR + HMAC          | bite 64's `aes` + `hmac` modes                   |
| ECDHE (X25519)          | bite 65's derive + mismatch                      |
| Fingerprint             | bite 64's `sha256` mode, bite 73's SDP parsing   |
| Cookie / anti-DoS       | bite 66's txid check (same instinct, bigger gun) |
| RTP sequence + ts       | bites 61/68's seq + 960-per-frame timestamp      |
