# Bite 70 — Observe Before Building

> Goal: watch a complete, *real* WebRTC session so every byte you write in
> bites 71-80 already has a face. Your capture is the ground truth you debug
> against later: when your C responder misbehaves, you diff your packets
> against what a proven stack (aiortc/Pion) produced.

All commands below were run inside WSL (Ubuntu 24.04).

---

## 1. Run a reference echo demo (aiortc path — Python, no Go toolchain needed)

```bash
# inside WSL
sudo apt install python3-pip ffmpeg
python3 -m pip install --user aiortc aiohttp
# aiortc ships example apps when cloned from source:
git clone https://github.com/aiortc/aiortc.git
cd aiortc/examples/webcam   # or examples/echoclient for a pure-media echo
python3 server.py           # serves a web page on http://0.0.0.0:8080
```

Open `http://localhost:8080` in a browser, allow the mic/camera — media now
flows between the browser and the aiortc process. The Go equivalent is
`pion/webrtc` `examples/pion-to-pion` if you prefer Go; either works, you are
studying the *wire*, not the code.

Note the two kinds of traffic you are about to capture:

- **signaling**: HTTP + WebSocket upgrade on TCP :8080 (aiortc example)
- **media**: UDP — STUN checks, DTLS handshake, SRTP packets, all on ONE port
  (this is the demux fact from bite 69; the capture will prove it)

## 2. Capture the whole session (tcpdump)

```bash
# inside WSL, another terminal; find your interface:  ip -brief addr
sudo tcpdump -i any -w webrtc.pcap -s 0 'udp or (tcp port 8080)'
# ... then load the page, speak for ~20 s, Ctrl-C the capture
```

The `-s 0` (snaplen) matters: without it DTLS certificate packets get
truncated and Wireshark cannot dissect them.

Windows-side alternative: capture on the *Windows* interface if the browser
runs on Windows and the demo in WSL2 — WSL2 NAT means you may prefer running
the demo browser *inside* WSL or capturing with `-i any` which sees both sides
of the WSL2 NAT.

## 3. Identify the four species of packets in Wireshark

Open `webrtc.pcap`, then apply these display filters one at a time:

| What | Wireshark filter | What it looks like |
|------|------------------|--------------------|
| Signaling | `tcp.port == 8080` | HTTP GET `/`, `101 Switching Protocols`, then WebSocket frames (yellow "WebSocket" rows) |
| STUN | `stun` | "STUN 26 Binding Request"/"Binding success Response" pairs, ~100-150 bytes, with attributes USERNAME, MESSAGE-INTEGRITY, FINGERPRINT, XOR-MAPPED-ADDRESS |
| DTLS | `dtls` | "Client Hello", "Server Hello", "Certificate", "Client Key Exchange", "Change Cipher Spec", "Encrypted Handshake Message" — handshake messages carry version `DTLS 1.2` |
| SRTP/media | `rtp` | Wireshark labels encrypted SRTP as RTP if it looks like RTP (v=2 first nibble 0x80/0x81...); payload unreadable without keys |

Useful companion filters:

```
stun.type == 1              # Binding requests only
stun.att.type == 6          # USERNAME attributes
dtls.record.content_type == 22   # handshake records
udp.port == 50000           # your own C server's future media port
```

Annotate four screenshots and store them next to this file:
`70-observe/signaling.png`, `70-observe/stun.png`, `70-observe/dtls.png`,
`70-observe/media.png`, each with a one-line caption in your own words.
(Insert yours here when done — the captures are machine-specific.)

## 4. See *inside* the DTLS/SRTP (optional but mind-blowing)

Browsers export their DTLS keys when `SSLKEYLOGFILE` is set:

```bash
# Windows (PowerShell) — then start Chrome from that shell:
$env:SSLKEYLOGFILE="$HOME\sslkeys.log"; chrome
# In Wireshark: Preferences -> Protocols -> TLS -> (Pre)-Master-Secret log filename
#   -> point at sslkeys.log.  The "DTLSv1.2 Application Data" rows now dissect,
#   and with "RTP" decode-as you can see real PCMU payloads.
```

Even without key export, everything in this course's bites is already visible
in plaintext: STUN attributes, DTLS handshake structure, packet sizes/timing.

## 5. Break it on purpose — block UDP, watch ICE fail

```bash
# Block outbound UDP except to your own subnet (coarse, demo-grade):
sudo iptables -A OUTPUT -p udp -m iprange ! --dst-range 10.0.0.0/8 -j DROP
# ... reload the demo page: the peer connection hangs in "connecting"
#     (chrome://webrtc-internals shows ICE state stuck in "checking"/"failed")
# remove the rule:
sudo iptables -D OUTPUT -p udp -m iprange ! --dst-range 10.0.0.0/8 -j DROP
```

If the demo was configured with a TURN server, the capture now shows
**ChannelData / TURN Allocate** traffic instead of direct SRTP — you have
*watched* why TURN exists (bite 78). If it had no TURN server, you watched
the exact failure your future coturn sidecar prevents.

## 6. Cross-reference for later bites

- Count STUN retransmissions in the capture (same transaction id, several
  times) — that is the timer behavior bite 75's `DTLSv1_handle_timeout`
  discussion parallels.
- Look at the STUN USERNAME attribute value: `peerUfrag:ownUfrag` — exactly
  what bite 74's parser must verify.
- Look at DTLS "Application Data" record sizes around 130-200 bytes — that is
  a 20 ms PCMU RTP frame (160 B payload) + SRTP overhead. Your bite 76 packets
  will look identical in shape.
