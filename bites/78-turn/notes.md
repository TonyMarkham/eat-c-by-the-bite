# Bite 78 — TURN: when direct P2P fails

> Goal: understand the relay path and provision it. The code in this folder
> (`turn_allocate.c`) is a minimal TURN **Allocate client** — it is
> compile-checked and will run against a local coturn, but nothing here
> requires coturn to be installed to *study* it.

---

## 1. Why TURN exists (the 30-second version of RFC 5766)

ICE candidates come in three flavors:

| Type | Learned from | Works when |
|------|--------------|------------|
| host | the NIC | peers share a network |
| srflx | STUN Binding | NAT is *endpoint-independent mapping* (cone-ish) |
| relay | TURN Allocate | always (it is a public server holding a port open) |

**Symmetric NAT** breaks srflx: the NAT assigns a *different* mapped port per
destination. Peer A's STUN-learned address `A'` is only valid for traffic
going to the STUN server — when peer B sends to `A':port`, the NAT drops it.
No amount of retrying fixes that; the fix is a third party both peers can
reach: **TURN**. Both peers send media to the TURN server; it relays.

You watched this exact failure in bite 70 (block UDP, watch ICE hang).

## 2. Production answer: coturn as a sidecar

```bash
sudo apt install coturn
```

Minimal `/etc/turnserver.conf` for a lab behind one NAT:

```
# listen on all interfaces, standard ports
listening-port=3478
# use the long-term credential mechanism with a STATIC SECRET:
# credentials are derived per-user (REST API style, RFC ... "time-limited")
lt-cred-mech
use-auth-secret
static-auth-secret=c-prep-demo-secret
realm=c-prep.local
# no TLS in the lab (add cert paths for production)
no-tls
no-dtls
# loopback-friendly
listening-ip=0.0.0.0
external-ip=127.0.0.1
```

Start it: `sudo turnserver -c /etc/turnserver.conf --no-cli` (or the systemd
unit). Verify:

```bash
# 4-turn CheckTimeCoding etc.; simplest smoke test:
turnutils_uclient -T -u user -w pass -p 3478 127.0.0.1   # with lt-cred-mech users
# or with the REST/time-limited scheme:
#   user = base64(HMAC-SHA1(secret, "expiry:user"))  (the coturn "rest" format)
turnutils_peer &    # echo target
turnutils_uclient -u "user" -w "secret-derived-pass" 127.0.0.1
```

### Time-limited credentials your backend hands out (the "Do" item)

The web-native scheme (a.k.a. TURN REST) puts expiry in the *username* and
derives the password with HMAC — bite 64's HMAC doing its real job:

```
username = <unix-expiry-timestamp> : <arbitrary user id>     e.g. "1798000000:peerA"
password = base64( HMAC-SHA1( shared_secret, username ) )
```

```python
# server-side, ~5 lines; hand {username, password, urls:["turn:host:3478"]}
import hmac, hashlib, base64, time
secret = b"c-prep-demo-secret"
username = f"{int(time.time()) + 600}:peerA"          # 10-minute lifetime
password = base64.b64encode(hmac.new(secret, username.encode(),
                                      hashlib.sha1).digest()).decode()
```

Break it on purpose: hand out `username = f"{int(time.time()) - 5}:peerA"`
(already expired). coturn rejects the Allocate with 401 — you have now seen
the exact failure your expiry logic prevents. Fix the window (10 min is the
common choice) and watch it succeed.

### Proving media flows *through* the relay

```bash
# blackhole direct UDP between the two peers, then rerun the echo demo:
sudo iptables -A OUTPUT -p udp -d <peer-B-ip> -j DROP
# in Wireshark you now see two legs: peer->coturn and coturn->peer,
# carrying TURN ChannelData (0x4000-0x40FF) instead of raw SRTP.
# filter: turn.channel.number or udp.port == 3478
```

## 3. Learning answer: the minimal client in C

`turn_allocate.c` speaks just enough TURN to be educational:

1. **Allocate request** (STUN method 0x003) with `REQUESTED-TRANSPORT`
   (UDP=17) — sent without credentials.
2. coturn answers **401 Unauthorized** with `REALM` + `NONCE` (long-term
   credential mechanism challenge).
3. We retry the Allocate with `USERNAME`, `REALM`, `NONCE` and
   `MESSAGE-INTEGRITY` keyed with `MD5(user:realm:password)` (RFC 5389 §15.4
   long-term key — note this is MD5-based, unlike ICE's short-term password).
4. On success we parse `XOR-RELAYED-ADDRESS` (the relayed candidate your
   backend would advertise in SDP) and `LIFETIME`.

This is the same STUN/attribute machinery as bite 66/74, so the marginal
learning is: *the challenge flow*, *the long-term key derivation*, and *what
a relayed candidate is*.

```bash
gcc -Wall -Wextra -Werror -std=c17 -g bites/78-turn/turn_allocate.c \
    -o /tmp/b78 -lcrypto
# WITHOUT coturn (expected: clean error, no crash):
timeout 5 /tmp/b78 127.0.0.1 3478      # -> "connection refused / timeout"
# WITH coturn running per section 2:
/tmp/b78 127.0.0.1 3478 c-prep-user c-prep-pass
```

**Honest note:** this client is compile-checked and its protocol handling is
complete, but the loopback runs in this repo were done WITHOUT a live coturn
(installing/starting one is left as the bite's hands-on step). Without
coturn, the program reports the failure cleanly and exits — that is the
tested behavior.

## 4. When your backend needs TURN at all

- Backend with a public IP + browsers: the browser→server path rarely needs
  TURN (the server is reachable). TURN matters for *browser↔browser* legs
  (SFU-less P2P), or servers behind NAT.
- Your signaling (bite 72) embeds the TURN config in the ICE setup you hand
  the frontend — the credentials generated above go THERE, time-limited.
