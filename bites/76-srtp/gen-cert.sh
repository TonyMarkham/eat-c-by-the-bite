#!/usr/bin/env bash
# bite 76 — gen-cert.sh: self-signed EC certs for the local DTLS loopback pair
# (same as bite 75's, copied so this bite is self-contained; run from anywhere).
#
# WHY SELF-SIGNED: WebRTC does not use a PKI. Each side generates a
# certificate at runtime and publishes its SHA-256 fingerprint in the SDP
# (a=fingerprint:...). Trust comes from matching that fingerprint, NOT from a
# CA. So for our loopback test the honest simulation is two self-signed certs
# and a fingerprint check — exactly what bites 75/76/80 do.
#
# RUN:  bash bites/75-dtls/gen-cert.sh            (from the repo root)
#       bash bites/75-dtls/gen-cert.sh <outdir>   (or with an explicit dir;
#            defaults to the bite's own folder so cert paths in the C files
#            match)
#
# OUTPUTS (in this folder):
#   server.key / server.crt   — the C backend's DTLS identity ("our" side)
#   client.key / client.crt   — the test double's identity ("the browser")
#   client-fingerprint.txt    — SHA-256 of client.crt, hex, no colons.
#                               THIS IS THE SDP a=fingerprint VALUE for the
#                               test double; the DTLS server loads it to
#                               verify the peer certificate (bite 75's exam).

set -euo pipefail
cd "$(dirname "$0")"

echo "generating EC P-256 key + self-signed cert for the SERVER (your C backend)..."
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 \
        -keyout server.key -out server.crt -days 3650 -nodes \
        -subj "/CN=c-prep-dtls-server" -addext "subjectAltName=IP:127.0.0.1"

echo "generating EC P-256 key + self-signed cert for the CLIENT (test double 'browser')..."
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 \
        -keyout client.key -out client.crt -days 3650 -nodes \
        -subj "/CN=c-prep-dtls-client"

# Fingerprint of the client cert in the SDP format, then as bare hex.
echo "client certificate SHA-256 fingerprint (SDP style):"
openssl x509 -in client.crt -noout -fingerprint -sha256
openssl x509 -in client.crt -noout -fingerprint -sha256 \
    | sed 's/^.*=//; s/://g' > client-fingerprint.txt
echo "wrote client-fingerprint.txt: $(cat client-fingerprint.txt)"
echo "done. Run the server first, then the client."
