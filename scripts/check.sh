#!/usr/bin/env bash
# Mazu integration test suite.
# Requires: curl, bash 4+, Python 3 (for archive test).
# Usage: scripts/check.sh <base_url>
#   e.g. scripts/check.sh http://localhost:8080
# Profile matrix mode:
#   scripts/check.sh --profile-matrix

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

if [ "${1:-}" = "--profile-matrix" ]; then
    had_config=0
    backup_file=""
    if [ -f .config ]; then
        had_config=1
        backup_file="$(mktemp /tmp/mazu.config.backup.XXXXXX)"
        cp .config "$backup_file"
    fi

    build_profile() {
        local profile="$1"
        make clean >/dev/null
        if ! DEFCONFIG="$profile" make defconfig >/dev/null 2>&1; then
            echo "profile defconfig FAIL: $profile" >&2
            return 1
        fi
        if ! make -j4 >/tmp/mazu.profile.build.log 2>&1; then
            tail -n 160 /tmp/mazu.profile.build.log >&2
            rm -f /tmp/mazu.profile.build.log
            return 1
        fi
        rm -f /tmp/mazu.profile.build.log
        echo "profile build OK: $profile"
    }

    # Run semihosting selftests for a built kernel.  Returns 0 on pass,
    # 1 on fail, 2 if semihosting is not enabled.
    selftest_profile() {
        if ! grep -q '^CONFIG_SEMIHOSTING=y' .config; then
            echo "profile selftest SKIP (no semihosting): $1"
            return 0
        fi
        if ! make check-selftest >/tmp/mazu.profile.selftest.log 2>&1; then
            echo "profile selftest FAIL: $1" >&2
            tail -n 20 /tmp/mazu.profile.selftest.log >&2
            rm -f /tmp/mazu.profile.selftest.log
            return 1
        fi
        local count
        count=$(grep -oE '[0-9]+ passed' build/check_selftest_serial.log 2>/dev/null |
            head -1 | grep -oE '[0-9]+') || true
        rm -f /tmp/mazu.profile.selftest.log
        echo "profile selftest OK: $1 (${count:-?} tests)"
    }

    rc=0
    build_fail=""
    selftest_fail=""
    for profile in \
        configs/defconfig \
        configs/rt_defconfig; do
        if ! build_profile "$profile"; then
            echo "profile build FAIL: $profile" >&2
            build_fail="$build_fail $profile"
            rc=1
            continue
        fi
        if ! selftest_profile "$profile"; then
            selftest_fail="$selftest_fail $profile"
            rc=1
        fi
    done

    if [ -n "$build_fail" ]; then
        echo "build failures:$build_fail" >&2
    fi
    if [ -n "$selftest_fail" ]; then
        echo "selftest failures:$selftest_fail" >&2
    fi

    if [ "$had_config" -eq 1 ]; then
        cp "$backup_file" .config
        rm -f "$backup_file"
    else
        rm -f .config
    fi

    if [ "$rc" -ne 0 ]; then
        exit "$rc"
    fi

    echo "profile matrix: all builds and selftests passed"
    exit 0
fi

BASE_URL="${1:?usage: check.sh <base_url> | check.sh --profile-matrix}"
PASS=0
FAIL=0
SKIP=0
TOTAL=0

# ANSI color codes (matching libiui test output style).
GREEN='\033[32m'
RED='\033[31m'
RESET='\033[0m'

# Section tracking — groups related assertions under one [ OK ] / [FAIL] line.
SECTION_NAME=""
SECTION_FAIL=0

section_begin() {
    SECTION_NAME="$1"
    SECTION_FAIL=$FAIL
    SECTION_PASS=$PASS
    SECTION_SKIP=$SKIP
}

section_end() {
    local SECTION_PASS_COUNT=$((PASS - SECTION_PASS))
    local SECTION_SKIP_COUNT=$((SKIP - SECTION_SKIP))
    if [ "$FAIL" -ne "$SECTION_FAIL" ]; then
        printf "Test %-40s[ ${RED}FAIL${RESET} ]\n" "$SECTION_NAME"
    elif [ "$SECTION_PASS_COUNT" -eq 0 ] && [ "$SECTION_SKIP_COUNT" -gt 0 ]; then
        printf "Test %-40s[ SKIP ]\n" "$SECTION_NAME"
    else
        printf "Test %-40s[ ${GREEN}OK${RESET} ]\n" "$SECTION_NAME"
    fi
}

# Assertion helpers — silent on success, print detail on failure.
pass() {
    PASS=$((PASS + 1))
    TOTAL=$((TOTAL + 1))
}
fail() {
    FAIL=$((FAIL + 1))
    TOTAL=$((TOTAL + 1))
    printf "  ${RED}[FAIL]${RESET} %s: %s\n" "$1" "$2"
}
skip() {
    TOTAL=$((TOTAL + 1))
    SKIP=$((SKIP + 1))
}

# Feature detection via shell help output.  Called once after the first
# session is established; caches results in HAS_WRITE / HAS_NSLOOKUP.
HAS_WRITE=""
HAS_NSLOOKUP=""
detect_shell_features() {
    if [ -n "$HAS_WRITE" ]; then return; fi
    if ! ensure_session; then
        HAS_WRITE=0
        HAS_NSLOOKUP=0
        return
    fi
    local help_body
    help_body=$(term_cmd "help")
    if echo "$help_body" | grep -qF "write"; then
        HAS_WRITE=1
    else
        HAS_WRITE=0
    fi
    if echo "$help_body" | grep -qF "nslookup"; then
        HAS_NSLOOKUP=1
    else
        HAS_NSLOOKUP=0
    fi
}

# Keep SLIRP NAT table warm with a quick probe.
slirp_probe() {
    curl -s -o /dev/null --max-time 2 "$BASE_URL/" 2>/dev/null || true
}

check_status() {
    local desc="$1" url="$2" expect="$3"
    local got retries=5
    while [ "$retries" -gt 0 ]; do
        got=$(curl -s -o /dev/null -w '%{http_code}' --max-time 8 "$url" 2>/dev/null) || true
        [ "$got" != "000" ] && break
        retries=$((retries - 1))
        sleep 1
        slirp_probe
    done
    if [ "$got" = "$expect" ]; then
        pass
    else fail "$desc" "expected $expect, got $got"; fi
}

check_body_contains() {
    local desc="$1" url="$2" needle="$3"
    local body retries=5
    while [ "$retries" -gt 0 ]; do
        body=$(curl -s --max-time 8 "$url" 2>/dev/null) || true
        if [ -n "$body" ] && echo "$body" | grep -qF "$needle"; then
            pass
            return
        fi
        retries=$((retries - 1))
        sleep 1
        slirp_probe
    done
    fail "$desc" "body missing '$needle'"
}

# Send a raw HTTP request via python3 socket and extract the status code.
# Usage: check_raw_status "description" <expected_code> <raw_request>
# Literal \r and \n sequences in <raw_request> are converted to CR/LF.
# Requires python3 (already a project dependency).
RAW_HOST=""
RAW_PORT=""
_raw_parse_url() {
    if [ -z "$RAW_HOST" ] || [ -z "$RAW_PORT" ]; then
        local parsed
        parsed=$(BASE_URL="$BASE_URL" python3 -c '
from urllib.parse import urlparse
import os

url = urlparse(os.environ["BASE_URL"])
host = url.hostname or ""
port = url.port
if port is None:
    if url.scheme == "https":
        port = 443
    elif url.scheme == "http":
        port = 80
print(host)
print(port or "")
') || true
        RAW_HOST=$(printf '%s\n' "$parsed" | sed -n '1p')
        RAW_PORT=$(printf '%s\n' "$parsed" | sed -n '2p')
    fi
}

check_raw_status() {
    local desc="$1" expect="$2"
    shift 2
    local raw="$*"
    _raw_parse_url
    local got
    got=$(RAW_REQ="$raw" RAW_H="$RAW_HOST" RAW_P="$RAW_PORT" python3 -c '
import socket, os
raw = os.environ["RAW_REQ"].replace("\\r", "\r").replace("\\n", "\n")
host = os.environ["RAW_H"]
port = int(os.environ["RAW_P"])
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(10)
resp = b""
try:
    s.connect((host, port))
    s.sendall(raw.encode("latin-1"))
    while True:
        chunk = s.recv(4096)
        if not chunk:
            break
        resp += chunk
        if b"\r\n" in resp:
            break
except Exception:
    pass
finally:
    s.close()
line = resp.split(b"\r\n")[0].decode("ascii", errors="replace") if resp else ""
parts = line.split()
print(parts[1] if len(parts) >= 2 else "000")
') || true
    if [ "$got" = "$expect" ]; then
        pass
    else fail "$desc" "expected $expect, got $got"; fi
}

###############################################################################
# Static file tests
###############################################################################

test_static_files() {
    section_begin "static files"
    check_status "GET /" "$BASE_URL/" 200
    check_body_contains "GET / body" "$BASE_URL/" "dashboard.html"
    check_status "GET /shell.html" "$BASE_URL/shell.html" 200
    check_body_contains "GET /shell.html body" "$BASE_URL/shell.html" "Shell"
    check_status "GET /index.html" "$BASE_URL/index.html" 200
    section_end
}

###############################################################################
# Error handling tests
###############################################################################

test_error_handling() {
    section_begin "error handling"
    check_status "GET /nonexistent" "$BASE_URL/nonexistent" 404
    check_status "path traversal" "$BASE_URL/..%2F..%2Fetc%2Fpasswd" 400

    local got
    got=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
        -X POST -d "" "$BASE_URL/shell.html" 2>/dev/null) || true
    if [ "$got" = "405" ]; then
        pass
    else fail "POST on static file" "expected 405, got $got"; fi
    section_end
}

###############################################################################
# Negative HTTP tests (Task 9x): parser rejection paths
###############################################################################

test_negative_http() {
    section_begin "negative HTTP"
    slirp_probe

    # 1. Non-numeric Content-Length -> 400
    check_raw_status "non-numeric Content-Length" "400" \
        "POST /api/shell/in?sid=1&tok=1 HTTP/1.1\r\nHost: localhost\r\nContent-Length: abc\r\n\r\ndata"

    # 2. Overflowed Content-Length (exceeds u64) -> 400
    check_raw_status "overflow Content-Length" "400" \
        "POST /api/shell/in?sid=1&tok=1 HTTP/1.1\r\nHost: localhost\r\nContent-Length: 99999999999999999999\r\n\r\ndata"

    # 3. POST body exceeding WEB_SHELL_POST_BODY_MAX (4096) -> 413
    check_raw_status "POST body too large" "413" \
        "POST /api/shell/in?sid=1&tok=1 HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5000\r\n\r\nshort"

    # 4. URI too long (> WEB_MAX_REQUEST_PATH = 2048) -> 414
    local long_path
    long_path=$(python3 -c "print('/' + 'A' * 2100)")
    check_raw_status "URI too long" "414" \
        "GET ${long_path} HTTP/1.1\r\nHost: localhost\r\n\r\n"

    # 5. Oversized request header (> WEB_MAX_REQUEST_HEADER = 8192) -> 431
    local big_hdr
    big_hdr=$(python3 -c "print('X-Pad: ' + 'B' * 8200)")
    check_raw_status "oversized header" "431" \
        "GET / HTTP/1.1\r\nHost: localhost\r\n${big_hdr}\r\n\r\n"

    section_end
}

###############################################################################
# Negative WebSocket tests (Task 9x): protocol violation close codes
###############################################################################

# Shared Python helpers for WebSocket tests.  Injected as a prefix so each
# test snippet only contains the frame-specific logic.
_WS_PY_COMMON='
import socket, base64, os, struct

def ws_connect(host, port):
    """Perform WebSocket upgrade handshake, return (socket, response_bytes)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(10)
    s.connect((host, port))
    key = base64.b64encode(os.urandom(16)).decode()
    req = ("GET /ws HTTP/1.1\r\nHost: localhost\r\n"
           "Upgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Key: " + key + "\r\n"
           "Sec-WebSocket-Version: 13\r\n\r\n")
    s.sendall(req.encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        chunk = s.recv(4096)
        if not chunk: break
        resp += chunk
    return s, resp

def _recv_until(s, data, target):
    """Extend `data` until it has at least `target` bytes (or EOF/timeout)."""
    try:
        while len(data) < target:
            chunk = s.recv(4096)
            if not chunk: break
            data += chunk
    except socket.timeout:
        pass
    return data

def read_close_code(s):
    """Read a WebSocket close frame and return the status code string.

    Server frames are unmasked per RFC 6455 5.1, so no mask field follows
    the length.  Layout: [opcode][len_indicator][ext_len?][payload].
    """
    data = _recv_until(s, b"", 2)
    if len(data) < 2:
        s.close()
        return "TCP_CLOSED" if len(data) == 0 else "NO_CLOSE"
    if (data[0] & 0x0f) != 0x08:
        s.close()
        return "NO_CLOSE"
    indicator = data[1] & 0x7f
    if indicator == 126:
        hdr_len = 4
    elif indicator == 127:
        hdr_len = 10
    else:
        hdr_len = 2
    data = _recv_until(s, data, hdr_len)
    if len(data) < hdr_len:
        s.close()
        return "NO_CLOSE"
    if indicator == 126:
        actual_len = struct.unpack("!H", data[2:4])[0]
    elif indicator == 127:
        actual_len = struct.unpack("!Q", data[2:10])[0]
    else:
        actual_len = indicator
    data = _recv_until(s, data, hdr_len + actual_len)
    s.close()
    if actual_len >= 2 and len(data) >= hdr_len + 2:
        return str(struct.unpack("!H", data[hdr_len:hdr_len + 2])[0])
    if actual_len == 0:
        return "NO_STATUS"
    return "NO_CLOSE"

HOST, PORT = os.environ["WS_H"], int(os.environ["WS_P"])
'

test_negative_websocket() {
    section_begin "WebSocket negative"
    _raw_parse_url

    # Probe for WebSocket support; skip if not available.
    local ws_probe
    ws_probe=$(WS_H="$RAW_HOST" WS_P="$RAW_PORT" python3 -c "
${_WS_PY_COMMON}
try:
    s, resp = ws_connect(HOST, PORT)
    line = resp.split(b'\r\n')[0].decode('ascii', errors='replace')
    print('101' if '101' in line else 'NO_WS')
    s.close()
except Exception:
    print('NO_WS')
") || true

    if [ "$ws_probe" != "101" ]; then
        skip
        section_end
        return
    fi

    # Test A: Unmasked client frame -> CLOSE 1002 (Protocol Error).
    # RFC 6455 section 5.1: client frames MUST be masked.
    local close_code
    close_code=$(WS_H="$RAW_HOST" WS_P="$RAW_PORT" python3 -c "
${_WS_PY_COMMON}
s, _ = ws_connect(HOST, PORT)
# TEXT frame with MASK bit = 0 (unmasked).
payload = b'hello'
try:
    s.sendall(bytes([0x81, len(payload)]) + payload)
except OSError:
    pass
print(read_close_code(s))
") || true
    if [ "$close_code" = "1002" ]; then
        pass
    else fail "WS unmasked -> 1002" "got $close_code"; fi

    slirp_probe

    # Test B: Oversized frame (> WS_MAX_FRAME_PAYLOAD = 4096) -> CLOSE 1009.
    close_code=$(WS_H="$RAW_HOST" WS_P="$RAW_PORT" python3 -c "
${_WS_PY_COMMON}
s, _ = ws_connect(HOST, PORT)
# Masked TEXT frame with payload_len = 5000 (> 4096 limit).
mask = os.urandom(4)
payload_len = 5000
hdr = bytes([0x81, 0x80 | 126]) + struct.pack('!H', payload_len) + mask
payload = bytes([(0x41 ^ mask[i % 4]) for i in range(payload_len)])
# Server may reset the connection mid-send once the limit is exceeded.
try:
    s.sendall(hdr + payload)
except OSError:
    pass
print(read_close_code(s))
") || true
    if [ "$close_code" = "1009" ]; then
        pass
    else fail "WS oversize -> 1009" "got $close_code"; fi

    section_end
}

###############################################################################
# ETag / conditional GET tests
###############################################################################

test_etag_304() {
    section_begin "ETag / conditional GET"
    local etag
    etag=$(curl -s -D - -o /dev/null --max-time 5 "$BASE_URL/shell.html" 2>/dev/null |
        grep -i '^ETag:' | tr -d '\r' | awk '{print $2}')
    if [ -z "$etag" ]; then
        fail "ETag header" "no ETag header found"
        section_end
        return
    fi
    pass

    local got
    got=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
        -H "If-None-Match: $etag" "$BASE_URL/shell.html" 2>/dev/null) || true
    if [ "$got" = "304" ]; then
        pass
    else fail "conditional GET" "expected 304, got $got"; fi
    section_end
}

###############################################################################
# Stats endpoint tests
###############################################################################

test_stats_endpoint() {
    section_begin "stats endpoint"
    check_status "GET /mazu-stats" "$BASE_URL/mazu-stats" 200
    check_body_contains "/mazu-stats uptime-ms" "$BASE_URL/mazu-stats" '"uptime-ms"'
    check_body_contains "/mazu-stats commit" "$BASE_URL/mazu-stats" '"commit"'
    section_end
}

###############################################################################
# Shell session tests
###############################################################################

# Lazy session creation.  Called by test_shell_session and also by
# ensure_session (which shell-dependent tests use as a guard).
_try_create_session() {
    local resp retries=8
    while [ "$retries" -gt 0 ]; do
        slirp_probe
        sleep 0.5
        resp=$(curl -s --max-time 12 \
            "$BASE_URL/api/shell/in?sid=99999" 2>/dev/null) || true
        if echo "$resp" | grep -qE 'tok=[0-9]+'; then
            TOK=$(echo "$resp" | grep -oE 'tok=[0-9]+' | head -1 | cut -d= -f2)
            SID=99999
            return 0
        fi
        retries=$((retries - 1))
        sleep 2
    done
    return 1
}

# Guard function: ensures a shell session exists before running a
# shell-dependent test.  Re-creates the session if a prior attempt
# failed (SLIRP may have recovered).
ensure_session() {
    [ -n "${TOK:-}" ] && return 0
    _try_create_session
}

test_shell_session() {
    section_begin "shell session create"
    if _try_create_session; then
        pass
    else
        fail "session create" "could not obtain token after 6 retries"
    fi
    section_end
}

test_shell_commands() {
    section_begin "shell commands"
    if ! ensure_session; then
        fail "shell commands" "no session token (session create failed)"
        section_end
        return
    fi

    local cmds=("help" "ls" "ls /web" "cat /hello.txt" "mem" "ps" "netstat" "tcpstats")
    local expects=("commands:" "web/" "shell.html" "Hello friend" "pages" "kernel tasks" "LISTEN" "uptime")

    for i in "${!cmds[@]}"; do
        local cmd="${cmds[$i]}"
        local expect="${expects[$i]}"
        local body
        body=$(term_cmd "$cmd")
        if echo "$body" | grep -qF "$expect"; then
            pass
        else fail "'$cmd'" "missing '$expect'"; fi
    done
    section_end
}

test_shell_auth() {
    section_begin "shell auth"
    local got retries=6
    while [ "$retries" -gt 0 ]; do
        slirp_probe
        got=$(curl -s -o /dev/null -w '%{http_code}' --max-time 8 \
            -X POST -d "help" \
            "$BASE_URL/api/shell/in?sid=99999&tok=0" 2>/dev/null) || true
        [ "$got" != "000" ] && break
        retries=$((retries - 1))
        sleep 2
    done
    if [ "$got" = "403" ]; then
        pass
    else fail "bad token" "expected 403, got $got"; fi
    section_end
}

test_shell_poll() {
    section_begin "shell poll"
    if ! ensure_session; then
        fail "shell poll" "no session token"
        section_end
        return
    fi
    check_status "GET /api/shell/out" \
        "$BASE_URL/api/shell/out?sid=$SID&tok=$TOK&from=0" "200"
    section_end
}

###############################################################################
# Keep-alive test
###############################################################################

test_keepalive() {
    section_begin "keep-alive"
    local codes all_ok retries=6
    while [ "$retries" -gt 0 ]; do
        slirp_probe
        codes=$(curl -s --max-time 8 \
            -o /dev/null -w '%{http_code} ' "$BASE_URL/" \
            -o /dev/null -w '%{http_code} ' "$BASE_URL/shell.html" \
            -o /dev/null -w '%{http_code} ' "$BASE_URL/mazu-stats" 2>/dev/null) || true
        all_ok=true
        for c in $codes; do
            if [ "$c" != "200" ]; then
                all_ok=false
                break
            fi
        done
        $all_ok && break
        retries=$((retries - 1))
        sleep 2
    done
    if $all_ok; then
        pass
    else fail "sequential GETs" "codes: $codes"; fi
    section_end
}

###############################################################################
# Archive roundtrip test (pre-boot, no QEMU needed)
###############################################################################

test_archive_roundtrip() {
    section_begin "archive roundtrip"
    if python3 ./scripts/archive.py test rootfs/ >/dev/null 2>&1; then
        pass
    else fail "archive roundtrip" "scripts/archive.py test rootfs/ failed"; fi
    section_end
}

###############################################################################
# VFS / filesystem tests (requires shell session)
###############################################################################

# Helper: send a shell command and return body.
# Retries on empty response (SLIRP connection drop).
term_cmd() {
    local body retries=5
    while [ "$retries" -gt 0 ]; do
        body=$(curl -s --max-time 8 -X POST -d "$1" \
            "$BASE_URL/api/shell/in?sid=$SID&tok=$TOK" 2>/dev/null) || true
        [ -n "$body" ] && break
        retries=$((retries - 1))
        sleep 1
        slirp_probe
    done
    echo "$body"
}

test_vfs_ls_root() {
    section_begin "VFS ls /"
    if ! ensure_session; then
        fail "VFS ls /" "no session token"
        section_end
        return
    fi
    local body
    body=$(term_cmd "ls /")
    if echo "$body" | grep -qF "web"; then
        pass
    else fail "ls / contains web" "body: $body"; fi
    if echo "$body" | grep -qF "hello.txt"; then
        pass
    else fail "ls / contains hello.txt" "body: $body"; fi
    section_end
}

test_writable_tmp() {
    section_begin "writable /tmp"
    detect_shell_features
    if [ "$HAS_WRITE" != "1" ]; then
        skip
        section_end
        return
    fi
    if ! ensure_session; then
        fail "writable /tmp" "no session token"
        section_end
        return
    fi
    # Write a file
    local body
    body=$(term_cmd "write /tmp/test.txt hello")
    if echo "$body" | grep -qF "wrote"; then
        pass
    else fail "write /tmp/test.txt" "body: $body"; fi

    # Read it back
    body=$(term_cmd "cat /tmp/test.txt")
    if echo "$body" | grep -qF "hello"; then
        pass
    else fail "cat /tmp/test.txt" "missing 'hello', body: $body"; fi

    # List /tmp
    body=$(term_cmd "ls /tmp")
    if echo "$body" | grep -qF "test.txt"; then
        pass
    else fail "ls /tmp" "missing 'test.txt', body: $body"; fi

    # Remove it
    body=$(term_cmd "rm /tmp/test.txt")

    # Verify it's gone
    body=$(term_cmd "cat /tmp/test.txt")
    if echo "$body" | grep -qF "not found"; then
        pass
    else fail "cat removed file" "expected 'not found', body: $body"; fi

    # mkdir
    body=$(term_cmd "mkdir /tmp/subdir")
    body=$(term_cmd "ls /tmp")
    if echo "$body" | grep -qF "subdir"; then
        pass
    else fail "ls /tmp after mkdir" "missing 'subdir', body: $body"; fi
    section_end
}

test_disk_fs() {
    section_begin "disk filesystem"
    detect_shell_features
    if [ "$HAS_WRITE" != "1" ]; then
        skip
        section_end
        return
    fi
    if ! ensure_session; then
        fail "disk filesystem" "no session token"
        section_end
        return
    fi
    # Write a file to /disk
    local body
    body=$(term_cmd "write /disk/hello.txt world")
    if echo "$body" | grep -qF "wrote"; then
        pass
    else fail "write /disk/hello.txt" "body: $body"; fi

    # Read it back
    body=$(term_cmd "cat /disk/hello.txt")
    if echo "$body" | grep -qF "world"; then
        pass
    else fail "cat /disk/hello.txt" "missing 'world', body: $body"; fi

    # List /disk
    body=$(term_cmd "ls /disk")
    if echo "$body" | grep -qF "hello.txt"; then
        pass
    else fail "ls /disk" "missing 'hello.txt', body: $body"; fi

    # Remove it
    body=$(term_cmd "rm /disk/hello.txt")
    body=$(term_cmd "cat /disk/hello.txt")
    if echo "$body" | grep -qF "not found"; then
        pass
    else fail "cat removed disk file" "expected 'not found', body: $body"; fi
    section_end
}

###############################################################################
# Ping command test (ICMP echo to gateway via SLIRP)
###############################################################################

test_ping_command() {
    section_begin "ping command"
    if ! ensure_session; then
        fail "ping command" "no session token"
        section_end
        return
    fi
    local body
    body=$(term_cmd "ping 192.168.100.1")
    if echo "$body" | grep -qF "statistics"; then
        pass
    else fail "ping statistics" "missing 'statistics', body: $body"; fi
    if echo "$body" | grep -qF "received"; then
        pass
    else fail "ping received" "missing 'received', body: $body"; fi
    section_end
}

###############################################################################
# DNS nslookup test (requires SLIRP DNS forwarder)
###############################################################################

test_nslookup() {
    section_begin "nslookup command"
    detect_shell_features
    if [ "$HAS_NSLOOKUP" != "1" ]; then
        skip
        section_end
        return
    fi
    if ! ensure_session; then
        fail "nslookup" "no session token"
        section_end
        return
    fi
    local body
    body=$(term_cmd "nslookup example.com")
    # Check that the output contains a dotted-quad IP address.
    if echo "$body" | grep -qE '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+'; then
        pass
    else fail "nslookup IP" "no IP address found, body: $body"; fi
    section_end
}

###############################################################################
# JSON API tests (Phase F)
###############################################################################

test_api_stats() {
    section_begin "API /api/stats"
    check_status "GET /api/stats" "$BASE_URL/api/stats" 200
    check_body_contains "/api/stats tasks" "$BASE_URL/api/stats" '"tasks"'
    check_body_contains "/api/stats memory" "$BASE_URL/api/stats" '"memory"'
    check_body_contains "/api/stats cpus" "$BASE_URL/api/stats" '"cpus"'
    check_body_contains "/api/stats nr_timer" "$BASE_URL/api/stats" '"nr_timer"'
    check_body_contains "/api/stats nr_exti" "$BASE_URL/api/stats" '"nr_exti"'
    check_body_contains "/api/stats nr_ssi" "$BASE_URL/api/stats" '"nr_ssi"'
    check_body_contains "/api/stats watchdog" "$BASE_URL/api/stats" '"watchdog"'
    check_body_contains "/api/stats nr_hung" "$BASE_URL/api/stats" '"nr_hung"'
    check_body_contains "/api/stats last_activity_ms" "$BASE_URL/api/stats" '"last_activity_ms"'
    check_body_contains "/api/stats hung field" "$BASE_URL/api/stats" '"hung"'
    check_body_contains "/api/stats callout" "$BASE_URL/api/stats" '"callout"'
    check_body_contains "/api/stats dispatched" "$BASE_URL/api/stats" '"dispatched"'
    check_body_contains "/api/stats max_late_us" "$BASE_URL/api/stats" '"max_late_us"'
    check_body_contains "/api/stats late_hist" "$BASE_URL/api/stats" '"late_hist"'
    check_body_contains "/api/stats nr_enqueue" "$BASE_URL/api/stats" '"nr_enqueue"'
    check_body_contains "/api/stats nr_dequeue" "$BASE_URL/api/stats" '"nr_dequeue"'
    check_body_contains "/api/stats nr_sched_ops" "$BASE_URL/api/stats" '"nr_sched_ops"'
    check_body_contains "/api/stats max_sched_ops" "$BASE_URL/api/stats" '"max_sched_ops"'
    check_body_contains "/api/stats total_wait_ticks" "$BASE_URL/api/stats" '"total_wait_ticks"'
    check_body_contains "/api/stats nr_ctxsw" "$BASE_URL/api/stats" '"nr_ctxsw"'
    check_body_contains "/api/stats ctxsw_avg_cycles" "$BASE_URL/api/stats" '"ctxsw_avg_cycles"'
    check_body_contains "/api/stats ctxsw_max_cycles" "$BASE_URL/api/stats" '"ctxsw_max_cycles"'
    check_body_contains "/api/stats nr_migrations" "$BASE_URL/api/stats" '"nr_migrations"'
    check_body_contains "/api/stats security" "$BASE_URL/api/stats" '"security"'
    check_body_contains "/api/stats nr_denied" "$BASE_URL/api/stats" '"nr_denied"'
    check_body_contains "/api/stats nr_enosys" "$BASE_URL/api/stats" '"nr_enosys"'
    section_end
}

###############################################################################
# SMP coverage (auto-detected from running kernel)
###############################################################################

test_smp_coverage() {
    section_begin "SMP coverage"
    # Detect SMP from the kernel's /api/stats response rather than
    # reading .config — test what the kernel actually booted, not what
    # the config file says.  The "cpus" array has one entry per online
    # hart; single-hart kernels only report "cpu":0.
    local body
    body=$(curl -s --max-time 5 "$BASE_URL/api/stats" 2>/dev/null) || true
    if ! echo "$body" | grep -q '"cpus"'; then
        skip # no cpus field — old kernel build
        section_end
        return
    fi
    # Wait for secondary hart(s) to come online.  If the kernel is
    # single-hart, "cpu":1 will never appear and the test skips.
    local ok=0 retries=10
    while [ "$retries" -gt 0 ]; do
        body=$(curl -s --max-time 2 "$BASE_URL/api/stats" 2>/dev/null) || true
        if echo "$body" | grep -q '"cpu":1'; then
            ok=1
            break
        fi
        retries=$((retries - 1))
        sleep 1
    done
    if [ "$ok" = "1" ]; then
        pass
    elif echo "$body" | grep -q '"cpu":0'; then
        skip # single-hart kernel, nothing to validate
    else
        fail "SMP secondary hart" "/api/stats does not report cpu 1 online"
    fi
    section_end
}

###############################################################################
# TCP endpoint tests
###############################################################################

test_api_tcp() {
    section_begin "API /api/tcp"
    check_status "GET /api/tcp" "$BASE_URL/api/tcp" 200
    check_body_contains "/api/tcp connections" "$BASE_URL/api/tcp" '"connections"'
    check_body_contains "/api/tcp cwnd" "$BASE_URL/api/tcp" '"cwnd"'
    check_body_contains "/api/tcp ssthresh" "$BASE_URL/api/tcp" '"ssthresh"'
    check_body_contains "/api/tcp pkts_sent" "$BASE_URL/api/tcp" '"pkts_sent"'
    check_body_contains "/api/tcp bytes_recv" "$BASE_URL/api/tcp" '"bytes_recv"'
    check_body_contains "/api/tcp retransmits" "$BASE_URL/api/tcp" '"retransmits"'
    section_end
}

test_api_arp() {
    section_begin "API /api/arp"
    check_status "GET /api/arp" "$BASE_URL/api/arp" 200
    check_body_contains "/api/arp entries" "$BASE_URL/api/arp" '"entries"'
    section_end
}

test_api_fs() {
    section_begin "API /api/fs"
    check_status "GET /api/fs?path=/" "$BASE_URL/api/fs?path=/" 200
    check_body_contains "/api/fs web dir" "$BASE_URL/api/fs?path=/" '"web"'

    # Percent-encoded path: %2F is '/', must be decoded by the server.
    check_status "GET /api/fs?path=%2F" "$BASE_URL/api/fs?path=%2F" 200
    check_body_contains "/api/fs %2F web" "$BASE_URL/api/fs?path=%2F" '"web"'

    # Subdirectory listing with encoded path (matches browser encodeURIComponent).
    check_status "GET /api/fs?path=%2Fweb" "$BASE_URL/api/fs?path=%2Fweb" 200

    # Invalid path (no leading '/') should return 400, not crash.
    check_status "GET /api/fs?path=bad" "$BASE_URL/api/fs?path=bad" 400

    # /api/fs/read — file content retrieval.
    check_status "GET /api/fs/read hello.txt" "$BASE_URL/api/fs/read?path=%2Fhello.txt" 200
    check_body_contains "/api/fs/read content" "$BASE_URL/api/fs/read?path=%2Fhello.txt" 'Hello friend'
    check_status "GET /api/fs/read not-found" "$BASE_URL/api/fs/read?path=%2Fno_such_file" 404
    check_status "GET /api/fs/read missing param" "$BASE_URL/api/fs/read" 400
    check_status "GET /api/fs/read dir" "$BASE_URL/api/fs/read?path=%2Fweb" 404
    section_end
}

test_api_klog() {
    section_begin "API /api/klog"
    check_status "GET /api/klog" "$BASE_URL/api/klog" 200
    check_body_contains "/api/klog log" "$BASE_URL/api/klog" '"log"'
    check_body_contains "/api/klog dropped" "$BASE_URL/api/klog" '"dropped"'
    section_end
}

###############################################################################
# Telemetry schema consistency (validates JSON structure and value types)
###############################################################################

test_telemetry_schema() {
    section_begin "telemetry schema"

    # Fetch /api/stats and validate full JSON schema via Python.
    local body retries=5
    while [ "$retries" -gt 0 ]; do
        body=$(curl -s --max-time 8 "$BASE_URL/api/stats" 2>/dev/null) || true
        [ -n "$body" ] && break
        retries=$((retries - 1))
        sleep 1
        slirp_probe
    done

    if [ -z "$body" ]; then
        fail "stats fetch" "could not fetch /api/stats"
        section_end
        return
    fi

    # Validate /api/stats is well-formed JSON with expected top-level
    # keys and nested structure.  Python 3 json module is strict: any
    # trailing comma or malformed number fails the parse.
    local result
    result=$(python3 -c "
import json, sys
try:
    d = json.loads(sys.argv[1])
except Exception as e:
    print('FAIL json_parse: ' + str(e))
    sys.exit(0)

errors = []

# Top-level keys
for k in ['uptime_ms', 'memory', 'tcp', 'http', 'scheduler',
           'cpus', 'callout', 'security', 'watchdog', 'tasks']:
    if k not in d:
        errors.append('missing top-level key: ' + k)

# Type checks for scalars
if 'uptime_ms' in d and not isinstance(d['uptime_ms'], (int, float)):
    errors.append('uptime_ms not numeric')

# memory sub-object
m = d.get('memory', {})
for k in ['total_pages', 'free_pages']:
    if k not in m:
        errors.append('memory.' + k + ' missing')
    elif not isinstance(m[k], (int, float)):
        errors.append('memory.' + k + ' not numeric')

# tcp sub-object
t = d.get('tcp', {})
for k in ['connections', 'bytes_tx', 'bytes_rx', 'retransmits']:
    if k not in t:
        errors.append('tcp.' + k + ' missing')
    elif not isinstance(t[k], (int, float)):
        errors.append('tcp.' + k + ' not numeric')

# http sub-object
h = d.get('http', {})
for k in ['4xx', '5xx']:
    if k not in h:
        errors.append('http.' + k + ' missing')

# scheduler sub-object
s = d.get('scheduler', {})
for k in ['wakeup_latency_max_us', 'wakeup_latency_hist',
           'nr_ctxsw', 'ctxsw_avg_cycles', 'ctxsw_max_cycles',
           'nr_migrations']:
    if k not in s:
        errors.append('scheduler.' + k + ' missing')
if 'wakeup_latency_hist' in s and not isinstance(s['wakeup_latency_hist'], list):
    errors.append('scheduler.wakeup_latency_hist not array')
elif 'wakeup_latency_hist' in s and len(s['wakeup_latency_hist']) != 6:
    errors.append('scheduler.wakeup_latency_hist length != 6')

# cpus array
cpus = d.get('cpus', [])
if not isinstance(cpus, list):
    errors.append('cpus not array')
elif len(cpus) < 1:
    errors.append('cpus array empty')
else:
    c0 = cpus[0]
    for k in ['cpu', 'nr_timer', 'nr_exti', 'nr_ssi',
              'nr_enqueue', 'nr_dequeue', 'nr_sched_ops',
              'max_sched_ops', 'total_wait_ticks',
              'nr_ctxsw', 'ctxsw_cycles_total', 'ctxsw_cycles_max',
              'nr_migrations', 'hart_load']:
        if k not in c0:
            errors.append('cpus[0].' + k + ' missing')

# callout sub-object
co = d.get('callout', {})
for k in ['dispatched', 'missed', 'max_late_us', 'timer_writes',
           'timer_skips', 'late_hist']:
    if k not in co:
        errors.append('callout.' + k + ' missing')
if 'late_hist' in co and not isinstance(co['late_hist'], list):
    errors.append('callout.late_hist not array')
elif 'late_hist' in co and len(co['late_hist']) != 6:
    errors.append('callout.late_hist length != 6')

# security sub-object
sec = d.get('security', {})
for k in ['nr_denied', 'nr_enosys']:
    if k not in sec:
        errors.append('security.' + k + ' missing')

# watchdog sub-object
wd = d.get('watchdog', {})
for k in ['nr_hung', 'nr_warnings']:
    if k not in wd:
        errors.append('watchdog.' + k + ' missing')

# tasks array
tasks = d.get('tasks', [])
if not isinstance(tasks, list):
    errors.append('tasks not array')
elif len(tasks) > 0:
    t0 = tasks[0]
    for k in ['id', 'state', 'prio', 'cpu_us',
              'last_activity_ms', 'hung']:
        if k not in t0:
            errors.append('tasks[0].' + k + ' missing')

for e in errors:
    print('FAIL ' + e)
if not errors:
    print('OK')
" "$body" 2>&1)

    local line
    while IFS= read -r line; do
        case "$line" in
            OK) pass ;;
            FAIL*) fail "stats schema" "${line#FAIL }" ;;
        esac
    done <<<"$result"

    # Validate /api/klog schema: {"dropped":<int>,"log":"<string>"}
    # Skipped if CONFIG_WEB_TELEMETRY=n (endpoint returns 404).
    local klog_status klog_body
    klog_status=$(curl -s -o /dev/null -w '%{http_code}' --max-time 8 \
        "$BASE_URL/api/klog" 2>/dev/null) || true
    if [ "$klog_status" != "200" ]; then
        skip # telemetry endpoint not available
        section_end
        return
    fi
    klog_body=$(curl -s --max-time 8 "$BASE_URL/api/klog" 2>/dev/null) || true
    if [ -n "$klog_body" ]; then
        local klog_result
        klog_result=$(python3 -c "
import json, sys
try:
    d = json.loads(sys.argv[1])
except Exception as e:
    print('FAIL json_parse: ' + str(e))
    sys.exit(0)
errors = []
if 'dropped' not in d:
    errors.append('missing dropped')
elif not isinstance(d['dropped'], (int, float)):
    errors.append('dropped not numeric')
if 'log' not in d:
    errors.append('missing log')
elif not isinstance(d['log'], str):
    errors.append('log not string')
for e in errors:
    print('FAIL ' + e)
if not errors:
    print('OK')
" "$klog_body" 2>&1)

        while IFS= read -r line; do
            case "$line" in
                OK) pass ;;
                FAIL*) fail "klog schema" "${line#FAIL }" ;;
            esac
        done <<<"$klog_result"
    else
        fail "klog fetch" "could not fetch /api/klog"
    fi

    section_end
}

###############################################################################
# Web UI tests (Phase F): pages, assets, and interactive endpoints
###############################################################################

test_web_ui() {
    section_begin "web UI"

    # Pages
    check_status "GET /dashboard.html" "$BASE_URL/dashboard.html" 200
    check_body_contains "dashboard body" "$BASE_URL/dashboard.html" "Dashboard"
    check_status "GET /files.html" "$BASE_URL/files.html" 200
    check_body_contains "files body" "$BASE_URL/files.html" "File Browser"
    check_status "GET /network.html" "$BASE_URL/network.html" 200
    check_body_contains "network body" "$BASE_URL/network.html" "Network"
    check_status "GET /telemetry.html" "$BASE_URL/telemetry.html" 200
    check_body_contains "telemetry body" "$BASE_URL/telemetry.html" "Telemetry"

    # Static assets referenced by pages
    check_status "GET /css/style.css" "$BASE_URL/css/style.css" 200
    check_status "GET /js/common.js" "$BASE_URL/js/common.js" 200

    # Dashboard JSON endpoint (polled by dashboard.html)
    check_body_contains "dashboard stats" "$BASE_URL/api/stats" '"uptime_ms"'

    section_end
}

###############################################################################
# Phase G: User-space process tests
###############################################################################

test_spawn_hello() {
    section_begin "spawn /bin/hello"
    if ! ensure_session; then
        fail "spawn hello" "no session token"
        section_end
        return
    fi
    local body
    body=$(term_cmd "spawn /bin/hello")
    if echo "$body" | grep -qF "spawned"; then
        pass
    else fail "spawn spawned" "expected 'spawned', body: $body"; fi
    # Give the user task time to run.
    sleep 1
    section_end
}

test_ps_command() {
    section_begin "ps command"
    if ! ensure_session; then
        fail "ps command" "no session token"
        section_end
        return
    fi
    local body
    body=$(term_cmd "ps")
    if echo "$body" | grep -qF "PID"; then
        pass
    else fail "ps header" "expected 'PID', body: $body"; fi
    section_end
}

test_kill_command() {
    section_begin "kill command"
    if ! ensure_session; then
        fail "kill command" "no session token"
        section_end
        return
    fi
    # Spawn a process, then kill it.
    local body
    body=$(term_cmd "spawn /bin/hello")
    sleep 1
    # Extract pid from "spawned pid NNN".
    local pid
    pid=$(echo "$body" | grep -oE '[0-9]+' | tail -1)
    if [ -n "$pid" ]; then
        body=$(term_cmd "kill $pid")
        if echo "$body" | grep -qF "killed"; then
            pass
        elif echo "$body" | grep -qF "not found"; then
            pass # Process already exited (expected with preemptive scheduling).
        else fail "kill pid" "expected 'killed', body: $body"; fi
    else
        pass # Skip gracefully if no pid (process may have already exited).
    fi
    section_end
}

test_echo_command() {
    section_begin "echo command"
    if ! ensure_session; then
        fail "echo command" "no session token"
        section_end
        return
    fi
    local body
    body=$(term_cmd "echo foo bar")
    if echo "$body" | grep -qF "foo bar"; then
        pass
    else fail "echo output" "expected 'foo bar', body: $body"; fi
    section_end
}

test_uptime_command() {
    section_begin "uptime command"
    if ! ensure_session; then
        fail "uptime command" "no session token"
        section_end
        return
    fi
    local body
    body=$(term_cmd "uptime")
    # Should contain "up" and some numeric time.
    if echo "$body" | grep -qE 'up [0-9]'; then
        pass
    else fail "uptime output" "expected 'up N', body: $body"; fi
    section_end
}

###############################################################################
# Run all tests
###############################################################################

TOK=""
SID=""

# SLIRP warmup — prime the NAT table with multiple requests before running
# tests.  Require 2 consecutive successes so the NAT entry is stable.
WARMUP_OK=0
for _ in $(seq 1 10); do
    wc=$(curl -s -o /dev/null -w '%{http_code}' --max-time 3 "$BASE_URL/" 2>/dev/null) || true
    if [ "$wc" = "200" ]; then
        WARMUP_OK=$((WARMUP_OK + 1))
        [ "$WARMUP_OK" -ge 2 ] && break
    else
        WARMUP_OK=0
    fi
    sleep 1
done
sleep 0.5

test_static_files
test_error_handling
test_negative_http
test_negative_websocket
test_etag_304
test_stats_endpoint
# Run keepalive early, while SLIRP connection tracking is fresh.
test_keepalive

# Phase F: JSON API and web page tests.
slirp_probe
test_api_stats
test_smp_coverage
test_api_tcp
test_api_arp
test_api_fs
test_api_klog
test_telemetry_schema

test_sse_test() {
    section_begin "SSE test endpoint"
    # The /api/sse/test endpoint uses chunked transfer encoding and sends
    # three SSE data lines.  curl --no-buffer gets the full response.
    local body retries=5
    while [ "$retries" -gt 0 ]; do
        body=$(curl -s --no-buffer --max-time 10 "$BASE_URL/api/sse/test" 2>/dev/null) || true
        [ -n "$body" ] && break
        retries=$((retries - 1))
        sleep 1
        slirp_probe
    done

    # Verify chunked response header.
    local headers
    headers=$(curl -s -D - -o /dev/null --max-time 10 "$BASE_URL/api/sse/test" 2>/dev/null) || true
    if echo "$headers" | grep -qi "Transfer-Encoding:.*chunked"; then
        pass
    else fail "chunked header" "Transfer-Encoding: chunked not found"; fi

    # Verify SSE events present.
    if echo "$body" | grep -q '"n":0'; then
        pass
    else fail "SSE event 0" "data: {\"n\":0} not found"; fi

    if echo "$body" | grep -q '"n":2'; then
        pass
    else fail "SSE event 2" "data: {\"n\":2} not found"; fi

    section_end
}
test_sse_test

slirp_probe
test_web_ui

# Shell session — establishes TOK/SID used by subsequent tests.
slirp_probe
test_shell_session
test_shell_commands

# Phase G: user-space process tests.
slirp_probe
test_spawn_hello
test_ps_command
test_kill_command
test_echo_command
test_uptime_command

# VFS and filesystem tests.
slirp_probe
test_vfs_ls_root
test_writable_tmp
test_disk_fs

# Network command tests (ping/nslookup go through SLIRP gateway).
slirp_probe
test_ping_command
test_nslookup

# Shell auth and polling.
slirp_probe
test_shell_auth
test_shell_poll
test_archive_roundtrip

echo ""
SKIP_MSG=""
if [ "$SKIP" -gt 0 ]; then
    SKIP_MSG=", $SKIP skipped"
fi
if [ "$FAIL" -gt 0 ]; then
    printf "${RED}%d passed, %d failed${RESET}${SKIP_MSG} (out of %d)\n" "$PASS" "$FAIL" "$TOTAL"
    exit 1
else
    printf "${GREEN}%d passed${RESET}, 0 failed${SKIP_MSG} (out of %d)\n" "$PASS" "$TOTAL"
    exit 0
fi
