#!/usr/bin/env bash
# CI integration test over TAP networking.
#
# Boots build/kernel.elf with QEMU's -netdev tap pointed at vm0 (created
# beforehand by .ci/setup-tap.sh), waits for mazu's HTTP server to
# answer at http://192.168.100.2, then runs scripts/check.sh against
# that URL. Mirrors the boot+poll dance in the Makefile's check: target
# but skips the semihosting selftest preamble (which the SLIRP make-check
# step already covered) and binds to TAP instead of SLIRP.
#
# Designed to run unprivileged: vm0 was created with `user=` ownership
# by setup-tap.sh, so QEMU can attach to it without sudo.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

KERNEL_ELF=${KERNEL_ELF:-build/kernel.elf}
DISK_IMG=${DISK_IMG:-build/disk.img}
TAP_DEV=${TAP_DEV:-vm0}
VM_URL=${VM_URL:-http://192.168.100.2}
BOOT_TIMEOUT=${BOOT_TIMEOUT:-60}
SERIAL_LOG=${SERIAL_LOG:-build/check_tap_serial.log}
QEMU_PID_FILE=${QEMU_PID_FILE:-build/check_tap_qemu.pid}

if [ ! -f "$KERNEL_ELF" ]; then
    echo "$KERNEL_ELF not found; run 'make' first" >&2
    exit 1
fi
if ! ip link show "$TAP_DEV" >/dev/null 2>&1; then
    echo "$TAP_DEV not up; run 'sudo .ci/setup-tap.sh' first" >&2
    exit 1
fi

# Match the Makefile's QEMU invocation but with TAP networking.
QEMU_ARGS=(
    -machine virt -cpu rv64 -m 1G
    -device virtio-net-device,netdev=net0
    -netdev "tap,id=net0,ifname=$TAP_DEV,script=no,downscript=no"
    -serial "file:$SERIAL_LOG"
    -monitor none
    -display none
    -no-reboot
    -kernel "$KERNEL_ELF"
    -pidfile "$QEMU_PID_FILE"
)

# Mirror the Makefile's config-driven flags so a UP-built kernel sees
# exactly one hart and an SMP build sees CONFIG_CPU_MAX. Mismatching
# -smp with CONFIG_SMP=n leaves spurious secondary harts in the FDT
# which the kernel ignores but which can confuse virtio-blk handshake
# under Ubuntu's stock QEMU 8.2.
if grep -q '^CONFIG_SMP=y' .config 2>/dev/null; then
    cpu_max=$(awk -F= '$1=="CONFIG_CPU_MAX"{gsub(/"/,"",$2); print $2}' .config)
    QEMU_ARGS+=(-smp "${cpu_max:-4}")
fi
if grep -q '^CONFIG_SEMIHOSTING=y' .config 2>/dev/null; then
    QEMU_ARGS+=(-semihosting-config enable=on,target=native)
fi
if [ -f "$DISK_IMG" ] && grep -q '^CONFIG_VIRTIO_BLK=y' .config 2>/dev/null; then
    # Reset the disk image so the second QEMU launch (after make check's
    # SLIRP boot) starts from a known-zero blob; otherwise the SFS
    # superblock written by the SLIRP boot can race virtio-blk handshake
    # under QEMU 8.2 and lock the boot in I/O timeout.
    dd if=/dev/zero of="$DISK_IMG" bs=1M count=4 status=none
    QEMU_ARGS+=(
        -drive "file=$DISK_IMG,format=raw,if=none,id=blk0"
        -device virtio-blk-device,drive=blk0
    )
fi

rm -f "$SERIAL_LOG" "$QEMU_PID_FILE"
qemu-system-riscv64 "${QEMU_ARGS[@]}" 2>/dev/null &
QEMU_PID=$!

cleanup() {
    if kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        sleep 0.5
        kill -0 "$QEMU_PID" 2>/dev/null && kill -9 "$QEMU_PID" 2>/dev/null
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    rm -f "$QEMU_PID_FILE"
}
trap cleanup EXIT INT TERM

echo "QEMU started (pid $QEMU_PID); polling $VM_URL for up to ${BOOT_TIMEOUT}s..."
ready=0
for i in $(seq 1 "$BOOT_TIMEOUT"); do
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        echo "QEMU exited before mazu became ready (iteration $i)" >&2
        echo "Last 60 lines of $SERIAL_LOG:" >&2
        tail -n 60 "$SERIAL_LOG" 2>/dev/null || true
        exit 1
    fi
    if curl -s --max-time 1 -o /dev/null "$VM_URL/"; then
        ready=1
        echo "  mazu reachable at $VM_URL after ${i}s"
        break
    fi
    sleep 1
done

if [ "$ready" != "1" ]; then
    echo "Timeout: $VM_URL did not respond within ${BOOT_TIMEOUT}s" >&2
    echo "Last 60 lines of $SERIAL_LOG:" >&2
    tail -n 60 "$SERIAL_LOG" 2>/dev/null || true
    exit 1
fi

bash ./scripts/check.sh "$VM_URL"
