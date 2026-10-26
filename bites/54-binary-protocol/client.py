#!/usr/bin/env python3
"""
Bite 54 — cross-language test client for the binary frame protocol.

WHY PYTHON: the spec in server.c is the contract; a client in a DIFFERENT
language proves the byte layout is real and not "whatever my C struct
happens to be". struct.pack(">BH", ...) says it plainly: big-endian
u16 after a byte — the same bytes get_u16be() parses on the server.

Run (with the server running):
    python3 bites/54-binary-protocol/client.py

Tests: ECHO round-trip, SUM round-trip, unknown type -> ERR,
oversized declared length -> ERR, and proof the connection still
works after each ERR. Exits 0 only if every test passes.
"""

import socket
import struct
import sys

HOST, PORT = "127.0.0.1", 48054
TYPE_ECHO, TYPE_SUM, TYPE_ERR = 1, 2, 255


def frame(ftype: int, payload: bytes = b"") -> bytes:
    """[type:1][length:2 BE][payload] — the whole protocol, 6 lines of it."""
    assert len(payload) <= 0xFFFF
    return struct.pack(">BH", ftype, len(payload)) + payload


def read_exact(sock: socket.socket, n: int) -> bytes:
    """Python twin of the server's read_full: TCP gives chunks, we want N."""
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("EOF mid-frame (server closed early?)")
        buf += chunk
    return buf


def read_frame(sock: socket.socket):
    hdr = read_exact(sock, 3)
    ftype, length = struct.unpack(">BH", hdr)
    return ftype, read_exact(sock, length)


def expect(name: str, ok: bool, detail: str = "") -> None:
    print(f"{name:<44} {'PASS' if ok else 'FAIL'}{('  (' + detail + ')') if detail else ''}")
    if not ok:
        globals()["_fails"] += 1


def main() -> int:
    globals()["_fails"] = 0
    s = socket.create_connection((HOST, PORT), timeout=3)

    # 1. ECHO round-trip — including a payload with a NUL and 0xFF inside,
    #    because binary protocols carry bytes, not strings.
    payload = b"hello\x00\xffbinary world"
    s.sendall(frame(TYPE_ECHO, payload))
    t, p = read_frame(s)
    expect("ECHO round-trip (incl. NUL/0xFF bytes)",
           t == TYPE_ECHO and p == payload)

    # 2. SUM round-trip: 5 + 7 = 12, four big-endian bytes back.
    s.sendall(frame(TYPE_SUM, struct.pack(">II", 5, 7)))
    t, p = read_frame(s)
    expect("SUM 5+7 -> 12 (u32 BE)",
           t == TYPE_SUM and p == struct.pack(">I", 12))

    # 2b. overflow behavior is DEFINED: sums wrap mod 2^32.
    s.sendall(frame(TYPE_SUM, struct.pack(">II", 0xFFFFFFFF, 2)))
    t, p = read_frame(s)
    expect("SUM wraps mod 2^32 (0xFFFFFFFF+2 -> 1)",
           t == TYPE_SUM and p == struct.pack(">I", 1))

    # 3. Unknown type -> ERR frame, connection stays open.
    s.sendall(frame(42, b"why"))
    t, p = read_frame(s)
    expect("unknown type 42 -> ERR, connection alive",
           t == TYPE_ERR and b"unknown" in p)

    # 4. THE DELIBERATE BUG: declare length 65535 with no payload.
    #    A server that trusts the length would block forever trying to
    #    read it. This one answers ERR immediately and closes this
    #    connection (see its spec: no resync inside garbage frames).
    s.sendall(struct.pack(">BH", TYPE_ECHO, 65535))
    t, p = read_frame(s)
    expect("declared length 65535 -> ERR (no hang)",
           t == TYPE_ERR and b"too large" in p)
    s.close()

    # 5. Still alive after all the abuse? (fresh connection)
    s = socket.create_connection((HOST, PORT), timeout=3)
    s.sendall(frame(TYPE_ECHO, b"still here"))
    t, p = read_frame(s)
    expect("server still serves after an ERR + close",
           t == TYPE_ECHO and p == b"still here")

    s.close()
    print(f"\n{'all tests passed' if globals()['_fails'] == 0 else 'FAILURES: ' + str(globals()['_fails'])}")
    return 0 if globals()["_fails"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
