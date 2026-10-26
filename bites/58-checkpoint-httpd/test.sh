#!/usr/bin/env bash
#
# Bite 58 — test script for the checkpoint httpd.
# Drives curl through the happy paths AND the abuse gauntlet.
#
# Run:  ./test.sh          (starts its own server on ${PORT:-48058})
#       PORT=49123 ./test.sh
#
# Exits 0 only if every check passes. Always leaves NO server running.

set -u
cd "$(dirname "$0")"

PORT="${PORT:-48058}"
BASE="http://127.0.0.1:$PORT"
SRV=./server

# Build if the binary isn't there (or is stale) — the checkpoint builds
# by hand, but the test script shouldn't depend on your memory.
echo "[build] gcc -Wall -Wextra -Werror -std=c17 -g -pthread server.c -o server"
gcc -Wall -Wextra -Werror -std=c17 -g -pthread server.c -o server || {
    echo "[build] FAILED"; exit 1; }

./server "$PORT" 2>server.log &
SRV_PID=$!
sleep 0.4

# Make sure we NEVER leave the server behind, whatever happens.
trap 'kill "$SRV_PID" 2>/dev/null; wait "$SRV_PID" 2>/dev/null' EXIT

pass=0; fail=0

check_status() { # desc want_status url [curl-args...]
    local desc="$1" want="$2" url="$3"; shift 3
    local got
    got=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$@" "$url")
    if [ "$got" = "$want" ]; then
        echo "PASS  $desc ($got)"; pass=$((pass+1))
    else
        echo "FAIL  $desc (want $want, got $got)"; fail=$((fail+1))
    fi
}

check_body() { # desc grep_pattern url
    local desc="$1" pattern="$2" url="$3"
    local body
    body=$(curl -s --max-time 5 "$url")
    if printf '%s' "$body" | grep -q -- "$pattern"; then
        echo "PASS  $desc"; pass=$((pass+1))
    else
        echo "FAIL  $desc (body did not contain '$pattern')"; fail=$((fail+1))
    fi
}

echo "=== happy paths ==="
check_status "GET /"                 200 "$BASE/"
check_status "GET /index.html"       200 "$BASE/index.html"
check_status "GET /time"             200 "$BASE/time"
check_status "GET /api/time"         200 "$BASE/api/time"
check_status "GET /api/stats"        200 "$BASE/api/stats"
check_body    "/api/time is JSON"    '"unix":' "$BASE/api/time"
check_body    "/api/stats is JSON"   '"requests_served":' "$BASE/api/stats"

echo "=== errors ==="
check_status "GET /nope -> 404"          404 "$BASE/nope"
check_status "POST / -> 405"             405 "$BASE/" -X POST
check_status "POST /api/time -> 405"     405 "$BASE/api/time" -X POST
check_status "DELETE /api/stats -> 405"  405 "$BASE/api/stats" -X DELETE

echo "=== abuse gauntlet (the 'break it on purpose' items) ==="
# 1. Path traversal: --path-as-is stops curl from normalizing ../ out.
check_status "traversal /../../etc/passwd -> 400" 400 \
    "$BASE/../../etc/passwd" --path-as-is
check_status "traversal /www/../../../etc/passwd -> 400" 400 \
    "$BASE/www/../../../etc/passwd" --path-as-is

# 2. Malformed request line (no HTTP version): server must answer 400
#    or close — and still be alive afterwards.
printf 'GARBAGE\r\n\r\n' | timeout 3 nc 127.0.0.1 "$PORT" >/dev/null 2>&1
check_status "alive after malformed request line" 200 "$BASE/"

# 3. Giant header block (>16KB): 400 or close, then still alive.
{
    printf 'GET / HTTP/1.1\r\nHost: x\r\nX-Big: '
    head -c 20000 /dev/zero | tr '\0' 'a'
    printf '\r\n\r\n'
} | timeout 5 nc 127.0.0.1 "$PORT" >/dev/null 2>&1
check_status "alive after giant header" 200 "$BASE/"

# 4. Slowloris-lite: open a connection, send HALF a request, then go
#    quiet. The server must NOT respond early and must NOT crash;
#    after its timeout it closes. We just prove the server is healthy
#    right after (a full slowloris test would wait the 10s timeout).
sl_pid=""
(
    printf 'GET / HTTP/1.1\r\nHost: x\r\n' >&3
    sleep 3
    exec 3>&-
) 3> >(timeout 4 nc 127.0.0.1 "$PORT" >/dev/null 2>&1) &
sl_pid=$!
wait $sl_pid 2>/dev/null
check_status "alive after slowloris-lite" 200 "$BASE/"

# 5. 20 parallel curls — all must succeed.
echo -n "      20 parallel curls: "
cpids=""
for i in $(seq 20); do
    curl -s -o /dev/null -w '%{http_code}\n' --max-time 5 "$BASE/" &
    cpids="$cpids $!"
done > /tmp/b58_parallel.$$ 2>&1
wait $cpids
if [ "$(grep -c 200 /tmp/b58_parallel.$$)" -eq 20 ]; then
    echo "all 20 got 200"; pass=$((pass+1))
else
    echo "FAIL — some did not get 200:"; grep -v 200 /tmp/b58_parallel.$$ || true
    fail=$((fail+1))
fi
rm -f /tmp/b58_parallel.$$

# 6. Keep-alive: 50 requests over ONE connection.
if command -v python3 >/dev/null 2>&1; then
    n=$(PORT="$PORT" python3 - <<'PY'
import http.client, os
c = http.client.HTTPConnection('127.0.0.1', int(os.environ.get('PORT', '48058')))
ok = 0
for i in range(50):
    c.request('GET', '/')
    r = c.getresponse(); r.read()
    if r.status == 200:
        ok += 1
print(ok)
PY
)
    if [ "$n" = "50" ]; then
        echo "PASS  keep-alive: 50 requests on one connection"; pass=$((pass+1))
    else
        echo "FAIL  keep-alive: only $n/50 succeeded"; fail=$((fail+1))
    fi
else
    echo "SKIP  keep-alive python test (python3 unavailable)"
fi

echo "=== graceful shutdown ==="
# SIGTERM mid-traffic: exit code must be 0.
curl -s -o /dev/null "$BASE/api/stats" &   # a request in flight-ish
CURL_PID=$!
sleep 0.1
kill -TERM "$SRV_PID"
wait "$SRV_PID"
SRV_RC=$?
wait "$CURL_PID" 2>/dev/null || true
trap - EXIT   # server already stopped; don't kill twice
if [ "$SRV_RC" -eq 0 ]; then
    echo "PASS  SIGTERM -> exit code 0"; pass=$((pass+1))
else
    echo "FAIL  SIGTERM -> exit code $SRV_RC (want 0)"; fail=$((fail+1))
fi

echo
echo "log excerpt (server.log):"
tail -n 5 server.log
echo
if [ "$fail" -eq 0 ]; then
    echo "ALL $pass CHECKS PASSED"
    exit 0
fi
echo "$fail CHECK(S) FAILED ($pass passed)"
exit 1
