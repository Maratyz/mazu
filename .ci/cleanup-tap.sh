#!/usr/bin/env bash
# Tear down whatever .ci/setup-tap.sh created. Best-effort: never fails
# the job, since CI cleanup must not mask a real test failure.

set -u
set +e

TAP_DEV=${TAP_DEV:-vm0}
DNSMASQ_PID_FILE=${DNSMASQ_PID_FILE:-/run/mazu-ci-dnsmasq.pid}

# Cleanup must never fail the job: a non-fatal exit here lets the actual
# test result remain visible.  If the script was invoked without sudo
# (CI misconfiguration), warn and bail with success rather than masking
# the test outcome.
if [ "$(id -u)" -ne 0 ]; then
    echo "$0: skipped, requires sudo" >&2
    exit 0
fi

if [ -f "$DNSMASQ_PID_FILE" ]; then
    pid=$(cat "$DNSMASQ_PID_FILE" 2>/dev/null || true)
    if [ -n "${pid:-}" ] && kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null
        echo "dnsmasq pid $pid killed"
    fi
    rm -f "$DNSMASQ_PID_FILE"
fi

if ip link show "$TAP_DEV" >/dev/null 2>&1; then
    ip link set "$TAP_DEV" down
    ip link delete "$TAP_DEV"
    echo "TAP $TAP_DEV removed"
else
    echo "TAP $TAP_DEV not present; nothing to clean up"
fi

exit 0
