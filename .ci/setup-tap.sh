#!/usr/bin/env bash
# CI-only TAP setup. Brings up a host<->guest point-to-point link plus a
# tiny DNS forwarder at 192.168.100.3 so the kernel's hard-coded resolver
# (user/shell.c -- shell_dns_resolve) has somewhere to send queries when
# QEMU's SLIRP built-in DNS isn't in the picture.
#
# Unlike scripts/setup_vm_network.sh (the developer workflow with bridge,
# MASQUERADE, and PREROUTING DNAT for outbound internet and external port
# forwarding), the CI runner only needs:
#   - vm0 carrying 192.168.100.1/24 -- host<->guest L2 path
#   - 192.168.100.3 alias -- where mazu's shell sends DNS
#   - dnsmasq on .3 forwarding to the runner's resolver
# Everything else is dead weight on a sandboxed runner and breaks if the
# runner has no $IF.
#
# Idempotent: re-running while vm0 / dnsmasq already exist is a no-op.

set -euo pipefail

TAP_DEV=${TAP_DEV:-vm0}
HOST_CIDR=${HOST_CIDR:-192.168.100.1/24}
DNS_HOST_IP=${DNS_HOST_IP:-192.168.100.3}
DNS_HOST_CIDR=${DNS_HOST_CIDR:-${DNS_HOST_IP}/32}
DNSMASQ_PID_FILE=${DNSMASQ_PID_FILE:-/run/mazu-ci-dnsmasq.pid}
TAP_USER=${TAP_USER:-${SUDO_USER:-$(id -un)}}

if [ "$(id -u)" -ne 0 ]; then
    echo "$0 must be run with sudo" >&2
    exit 1
fi

# Verify /dev/net/tun is present; otherwise QEMU will fail later with a
# less-obvious error. GitHub-hosted Linux runners always ship the tun
# module loaded.
if [ ! -c /dev/net/tun ]; then
    echo "$0: /dev/net/tun missing; runner does not support TAP" >&2
    exit 1
fi

if ! command -v dnsmasq >/dev/null 2>&1; then
    echo "$0: dnsmasq not installed; install it before invoking this script" >&2
    exit 1
fi

if ! ip link show "$TAP_DEV" >/dev/null 2>&1; then
    ip tuntap add "$TAP_DEV" mode tap user "$TAP_USER"
fi
ip addr replace "$HOST_CIDR" dev "$TAP_DEV"
ip addr replace "$DNS_HOST_CIDR" dev "$TAP_DEV"
ip link set "$TAP_DEV" up

# Pick the runner's first nameserver to forward DNS to. Fall back to a
# public resolver only if /etc/resolv.conf has nothing usable; a public
# fallback keeps the test running on hosts where systemd-resolved isn't
# advertising a usable upstream.
upstream=$(awk '/^nameserver[[:space:]]+[0-9]/ {print $2; exit}' /etc/resolv.conf 2>/dev/null || true)
if [ -z "$upstream" ] || [ "$upstream" = "127.0.0.53" ]; then
    upstream=1.1.1.1
fi

# Cleanly relaunch dnsmasq so a stale pid file from a prior run doesn't
# wedge us. dnsmasq exits silently on bind failure, hence the explicit
# pgrep below.
if [ -f "$DNSMASQ_PID_FILE" ]; then
    pid=$(cat "$DNSMASQ_PID_FILE" 2>/dev/null || true)
    if [ -n "${pid:-}" ] && kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
    fi
    rm -f "$DNSMASQ_PID_FILE"
fi

dnsmasq \
    --listen-address="$DNS_HOST_IP" \
    --bind-interfaces \
    --no-resolv \
    --no-hosts \
    --server="$upstream" \
    --pid-file="$DNSMASQ_PID_FILE" \
    --port=53

# dnsmasq daemonises; verify it actually bound before declaring success.
sleep 0.2
if ! [ -s "$DNSMASQ_PID_FILE" ] \
    || ! kill -0 "$(cat "$DNSMASQ_PID_FILE")" 2>/dev/null; then
    echo "$0: dnsmasq failed to start on $DNS_HOST_IP:53 (forward=$upstream)" >&2
    exit 1
fi

echo "TAP $TAP_DEV up; host=$HOST_CIDR; dns=$DNS_HOST_IP -> $upstream; owner=$TAP_USER"
