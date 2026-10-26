# Bite 69 — WebRTC Anatomy Day

> Goal: own the complete connection flow on paper before writing a line of code.
> This file *is* the paper. Every arrow is labeled with (a) the protocol,
> (b) who sends it — browser or C server, and (c) which earlier bite taught
> the skill that piece is built from.

---

## The full connection flow

```
 BROWSER (peer A, "full" ICE agent)                    C SERVER (ICE-lite media endpoint)
 --------------------------------------------------------------------------------------------

 1. Page loads, getUserMedia(), new RTCPeerConnection()
    |
    |  pc.createOffer()  -- browser builds SDP locally (media codecs, DTLS cert fingerprint,
    |                        ICE ufrag/pwd). SERVER does NOT generate the offer.
    v
 2. Browser --OFFER (SDP)-->  [SIGNaling CHANNEL: WebSocket over TCP]
    |                          server: bites 55 (HTTP), 51 (event loop), 71 (WS framing),
    |                          72 (JSON relay), 73 (SDP parse)
    v
 3. Server relays offer to peer B  (or -- in our backend -- the server itself is the
    remote media endpoint and answers directly; bite 80's test double plays peer B)
    |
    v
 4. Peer B --ANSWER (SDP)--> ...back through signaling --> browser
    |                          (same bites as step 2, direction reversed)
    v
 5. BOTH sides now know:  a=ice-ufrag / a=ice-pwd  (STUN short-term credentials, bite 74)
                          a=fingerprint:sha-256 ...  (cert hash to verify in DTLS, bites 73/75)
                          a=candidate:...            (addresses to try, this step)
    |
    v
 6. ICE: both sides send STUN Binding REQUESTS to each other's candidates over UDP
    BROWSER --STUN Binding req (USERNAME "srvUfrag:brwUfrag", MESSAGE-INTEGRITY=HMAC-SHA1
              keyed with the SERVER's ice-pwd -- RFC 8445 s7.2.2)-->  SERVER
    SERVER  --STUN Binding resp (XOR-MAPPED-ADDRESS + MESSAGE-INTEGRITY)--> BROWSER
              * ICE-lite server: it only ANSWERS; it never originates checks. (bites 66, 64)
              * Demux on the media port: same UDP socket carries STUN, DTLS, SRTP
                (classify by first byte -- bites 54/42 framing discipline, bites 74/75)
    v
 7. ICE state = "connected". Now the channel is *authenticated*: DTLS handshake on the
    same UDP port, same 5-tuple:
    BROWSER --DTLS ClientHello--> SERVER          (bites 67 theory, 75 implementation)
             <-- ServerHello, Certificate, ... --  server sends ITS cert
    BROWSER --Certificate, Finished-->            browser sends ITS cert
    SERVER: SSL_get_peer_certificate -> SHA-256 fingerprint -> strcmp against the
            a=fingerprint: parsed in bite 73.  MISMATCH = abort. This check IS the
            security model (no check = trivial MITM, which "break it on purpose" shows).
    v
 8. SRTP key derivation, still inside DTLS:
    SSL_export_keying_material("EXTRACTOR-dtls_srtp") on both sides -> identical
    client_write_key/server_write_key/salts (RFC 5764 s4.2). Bites 64 (crypto), 76.
    v
 9. Media flows as SRTP = RTP packets encrypted with the derived keys:
    BROWSER --SRTP (RTP: seq, ts, PCMU payload, encrypted+authenticated)--> SERVER
    SERVER  --SRTP--> BROWSER          (bites 42 endianness, 59 pacing, 61 sequencing, 77 echo)
```

## Who does what (annotated)

| Step | Browser (full agent) | C backend (what we build in bites 71-80) |
|------|----------------------|------------------------------------------|
| Media capture, codecs | YES (getUserMedia, Opus/PCMU encode) | no (bite 77 pass-through only) |
| Signaling | `RTCPeerConnection` callbacks | WS server + JSON relay (71, 72) |
| SDP | generates offer/answer | parses it (73) |
| ICE | full agent: gathers, checks, nominates | **ICE-lite**: only answers checks (74) |
| STUN/TURN client | yes | no (78: provisions coturn + credentials) |
| DTLS | client role usually | server role, verify fingerprint (75) |
| SRTP | encrypt/decrypt | same, via libsrtp (76, 77) |

**ICE-lite** = a server that responds to connectivity checks but never sends its
own. It is legal because a server is presumed publicly reachable — there is
nothing for it to discover about its own topology.

## Attribution table (arrow -> bite)

| Piece of the flow | Built by bite |
|-------------------|---------------|
| TCP sockets, bind/listen/accept | 47 |
| Byte-stream framing / message boundaries | 48, 54 |
| poll() event loop | 51 |
| UDP sendto/recvfrom | 52 |
| Line protocol design + strict parsing | 53 |
| Big-endian serialization | 42 |
| HTTP parsing | 55, 56 |
| SHA-1/SHA-256, HMAC, AES | 64 |
| Cryptographic RNG / key material | 65 |
| Raw STUN Binding request/response | 66 |
| DTLS concepts (ClientHello, use_srtp) | 67 |
| Fixed-rate 20 ms pacing | 59 |
| Sequencing/reordering/dup detection | 61, 68 |

Gaps in your labels = bites to revisit. (This table was filled in *after* the
flow above was drawn from memory — that is the "done when" test.)

## README skeleton for the final project (written NOW, bite 80 fills it in)

```
# webrtc-backend (bites 71-80)
Architecture:
  Browser tab A <--WS:8081--> [C backend] <--UDP:50000--> Browser tab B media plane
Layers:
  1. Signaling: libwebsockets WS server, JSON relay (71,72)
  2. SDP: line parser -> struct (73)
  3. ICE-lite: STUN binding responder w/ short-term creds (74)
  4. DTLS 1.2 + fingerprint verification (75)
  5. SRTP: exporter keys -> libsrtp (76)
  6. Media echo: RTP re-header + protect (77)
  7. TURN: coturn sidecar (78)
  8. Hardening: timeouts, cleanup, stats, graceful shutdown (79)
```

## Break it on purpose

Explain the flow out loud, without the paper. The steps people usually stumble
on, and what the stumble means:

- "then they connect" (vague) -> you skipped who sends the *first* STUN packet
  and why ICE-lite answers but never asks. Re-read bite 74's section.
- "the server gives the browser a key" -> key material is never sent on the
  wire; it is *derived identically on both sides* from the DTLS exporter. Bites 67/76.
- "DTLS runs on port 443" -> it does not here: DTLS shares the media UDP port
  with STUN and SRTP. Demux by first byte. Bites 74/75.
- "fingerprint is for encryption" -> it is for *authentication* (identifying
  which key holder you are talking to), not secrecy. Bite 75.

## Done when

- [x] Diagram exists in the repo, every arrow labeled with protocol + sender
- [x] Attribution table maps every piece to a bite
- [x] Can narrate it cold (checked via the "break it" list above)
