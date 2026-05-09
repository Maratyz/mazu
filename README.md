<h1 align="center">The Mazu Operating System</h1>

Mazu is a bare-metal RISC-V 64-bit hard RTOS that combines Linux kernel
discipline with Plan 9 philosophy in a system small enough to read end to end.
SMP correctness, hard real-time scheduling, and kernel-integrated networking
are not bolted on after the fact -- they shape every data structure and code
path from the start.

Unlike RTOSes that treat networking as an optional middleware layer and SMP
as a bolt-on configuration flag, Mazu takes the opposite position: a
connected embedded system needs bounded-latency scheduling, per-CPU execution
paths, and a TCP/IP stack that respects both, all in the same address space,
all under the same lock discipline. The kernel serves REST APIs and runs a
web-based shell as ordinary preemptible tasks alongside deadline-scheduled
control work.

Two design lineages run through the codebase:

- From Linux: subsystem modularity (initcall registration, irqchip vtables,
  IRQ descriptor tables, waitqueues, lockdep), synchronization primitives
  (priority-inheritance mutexes, futexes with PI and requeue, counting
  semaphores with direct handover), buddy allocator, per-CPU data via the
  `gp` register, and the convention that every subsystem is SMP-safe or
  explicitly documented otherwise.
- From Plan 9: the "everything is a file" control plane. Synthetic
  filesystems (`/dev`, `/proc`, `/net`) expose hardware, process state, and
  network tables as readable files -- no `ioctl`, no sysfs, no procfs
  special-case parsers. System observability comes from `cat /net/tcp/stats`,
  not a dedicated monitoring daemon.

What Mazu does not import from either lineage is equally deliberate: no
loadable modules, no virtual memory isolation between tasks, no VFS page
cache, no socket API. The kernel runs all tasks in a single shared page
table (identity-mapped kernel space, shared user mappings at fixed VAs) with
VMA-based access control, and the networking API is a direct function
interface rather than a Berkeley sockets layer. Disk-backed SFS has its own
block buffer cache (`kernel/fs/bcache.c`); the synthetic and RAM filesystems
are uncached because their data is either memory-resident or generated on
demand. These choices keep the system small and auditable. PSE51 syscall coverage is a work-in-progress
target; the kernel-level primitives that back those syscalls
(PI mutexes, condvars, semaphores, futexes) are already in place.

Core profile:

- Hard-RT scheduling: mandatory kernel preemption, SMP per-CPU run queues, EEVDF fairness, EDF deadline scheduling with admission control, mixed-criticality domains, load balancing, and scheduling domains with budget enforcement
- SMP by design: per-hart state via `gp` register, per-CPU run queues and merged deadline management, lockdep lock-ordering enforcement, cache-line-aligned per-CPU structures to eliminate false sharing
- Kernel-integrated networking: IPv4, TCP (Reno CC, SACK, RTT estimation, connection pooling, per-IP flood limits), optional UDP/DHCP/mDNS, outbound client connections, HTTP/1.1 server with REST endpoints, WebSocket, and SSE -- all running as preemptible scheduler tasks
- Plan 9-style VFS: synthetic `/dev` (null, zero, console, time), `/proc` (meminfo, uptime, cpuinfo), `/net` (arp, iface, tcp/stats) alongside a RAM filesystem with optional writable and virtio-blk paths
- Linux-grade synchronization: PI mutexes with direct handover, condition variables, counting semaphores, futexes (WAIT/WAKE/CMP_REQUEUE/LOCK_PI/UNLOCK_PI)
- Type-driven safety: length-prefixed fat strings (never null-terminated), macro-generated result types (errors never share the return-value space), read-only/read-write/appendable buffer types encoding mutability in the type system
- Memory: buddy allocator for pages, pool allocators for fixed-size objects (no external fragmentation), arena allocators for request-scoped temporaries (no per-object allocation headers), pluggable allocator vtable
- Kernel-user isolation: W^X, VMA-based user-pointer containment validation, per-process syscall allow-list, kernel-stack guard pages, stack-protector canaries
- Debug and verification: lockdep, scheduler invariant checks on every context switch, callout lateness histograms, self-test framework, UBSan trap mode, static analysis via clang
- QEMU `virt` machine: virtio-mmio devices, PLIC, OpenSBI, Sv39 paging (identity-mapped, 2 MiB superpages with on-demand shattering)

Primary development target is QEMU on Linux with standard tooling (`make`,
`python3`, RISC-V cross toolchain, QEMU).

A planned use case is running an embedded AI assistant (similar to
[MimiClaw](https://github.com/memovai/mimiclaw)) directly on the kernel. This
requires outbound TCP client connections, TLS or a proxy relay strategy, JSON
request/response parsing, streaming responses (SSE or WebSocket), and persistent
state management.

- [Using Mazu](#using-mazu)
- [Build configuration](#build-configuration)
- [Security](#security)
- [Debugging](#debugging)
- [Design philosophy](#design-philosophy)
- [Internals](#internals)
- [Further documentation](#further-documentation)

# Using Mazu

Quick start (SLIRP networking, local host access):

```bash
make defconfig          # generate .config from configs/defconfig
DEBUG=2 make run        # build and launch in QEMU (SLIRP, http://localhost:8080)
```

TAP networking (guest at `192.168.100.2`, Linux + iptables required):

```bash
export IF=eth0          # outward-facing host interface
./scripts/setup_vm_network.sh
TAP=1 make run
```

Runtime content model:

- `rootfs/` is packed into the kernel image at build time by `scripts/archive.py`.
- `rootfs/config.txt` provides runtime network settings (used when DHCP is disabled/fails).
- `rootfs/web/` is the HTTP document root (static assets and web UI).
- `rootfs/hello.txt` is printed during boot.

The web server (`user/net/web.c`) provides both static file serving and dynamic REST API
endpoints. Current API surface:

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/stats` | GET | Kernel stats (tasks, IRQs, memory, scheduler, callout, security) as JSON |
| `/api/tcp` | GET | TCP connection table (including cwnd/ssthresh per connection) as JSON |
| `/api/arp` | GET | ARP table as JSON |
| `/api/klog` | GET | Kernel log ring buffer as JSON |
| `/api/fs?path=X` | GET | Directory listing as JSON |
| `/api/fs/read?path=X` | GET | File content as text/plain |
| `/api/shell/in` | GET/POST | Web terminal: create session / submit command |
| `/api/shell/out` | GET | Web terminal: read output (polling) |
| `/api/sse/test` | GET | SSE test endpoint (chunked transfer encoding) |

WebSocket upgrade is supported for real-time communication (e.g., terminal streaming).
Additional MIME types and API endpoints can be added in `user/net/web.c`.

Build/runtime knobs:

- `DEBUG` controls kernel log verbosity:
  - `0`: warnings and errors
  - `1`: info
  - `2`: debug
  - `3`: verbose
- `RELEASE=1` enables optimized builds.

A practical default for development is `DEBUG=2` with `RELEASE` unset.

## Build configuration

Mazu uses a [Kconfiglib](https://github.com/ulfalizer/Kconfiglib)-based
configuration system (the same Kconfig language used by the Linux kernel).
The configuration schema lives in `configs/Kconfig`.

```bash
make config             # interactive menuconfig TUI
make defconfig          # apply configs/defconfig (default config)
make defconfig DEFCONFIG=configs/rt_defconfig       # apply a named defconfig
make savedefconfig      # save current .config back to configs/defconfig
make oldconfig          # update .config for new/changed Kconfig symbols
```

Kconfiglib is auto-cloned into `tools/kconfig/` on first use.  The generated
`.config` file is included by Make, and `build/config.h` is generated for C
code.  Disabled features contribute zero text and zero BSS to the kernel image.

Predefined configurations:

| Defconfig | Description |
|-----------|-------------|
| `configs/defconfig` | Default hard-RT profile: SMP, latency tracing, TCP/UDP, DHCP, mDNS, SACK, virtio-blk |
| `configs/rt_defconfig` | Leaner hard-RT validation profile with SMP, EEVDF, and network options enabled |

Key feature flags (all configurable via `make config`):

| Symbol | Default | Description |
|--------|---------|-------------|
| `CONFIG_NET_TCP` | y | TCP/IP stack with connection pool, Reno CC, RTT retransmission, sliding window |
| `CONFIG_WEBSOCKET` | y | WebSocket upgrade path (SHA-1, Base64, frame codec, PING/PONG/CLOSE) |
| `CONFIG_SCHED_PREEMPTIVE` | y | Mandatory timer-driven kernel preemption |
| `CONFIG_SCHED_EEVDF` | y | EEVDF fair scheduling within priority levels |
| `CONFIG_SMP` | y | Symmetric multiprocessing (per-CPU run queues, load balancing) |
| `CONFIG_NET_UDP` | y | UDP transport (required by DHCP and mDNS) |
| `CONFIG_VIRTIO_BLK` | y | VirtIO block device driver |
| `CONFIG_RAMFS_WRITABLE` | y | Writable RAM filesystem |
| `CONFIG_SEMIHOSTING` | y | RISC-V semihosting for host communication and self-tests |
| `CONFIG_DHCP` | n | DHCPv4 boot-time client |
| `CONFIG_NET_MDNS` | n | mDNS responder for `mazu.local` (RFC 6762) |
| `CONFIG_DEBUG_ENDPOINT` | n | `/debug` HTTP endpoint for runtime inspection |

A legacy configuration path via `config-riscv64.mk` is still supported for
backward compatibility when no `.config` file exists.

Validation shortcuts:

```bash
make check              # HTTP integration tests (SLIRP networking)
make check-selftest     # semihosting self-tests (requires CONFIG_SEMIHOSTING=y)
make check-smp          # SMP-focused checks (requires CONFIG_SMP=y)
./scripts/check.sh      # matrix-style checks across selected profiles
./scripts/check.sh --profile-matrix  # build + selftest all config profiles
```

## Security

Mazu transmits all traffic — including web-terminal keystrokes and output
— over **plaintext HTTP with no transport security**.  It is not safe to
expose the kernel directly on an untrusted LAN or the public internet.

The recommended approach is to place Mazu behind a TLS-terminating reverse
proxy on the same host or inside the same trusted network segment.  Popular
options include Nginx (`proxy_pass http://192.168.100.2`) and Caddy.  A
WireGuard tunnel is also appropriate when the host itself is remote.

Deployment constraints to enforce:

1. **TLS**: Use a reverse proxy (Nginx, Caddy) or a WireGuard tunnel to
   encrypt traffic in transit.  TLS alone does not authenticate users.

2. **Authentication**: Add HTTP basic auth at the proxy layer (or equivalent
   network ACLs) to prevent unauthorized access to the web terminal.
   The kernel itself has no user authentication beyond session tokens for
   the terminal API.

3. **Network isolation**: The kernel listens on every address it has an IP
   for.  Restrict access at the firewall or via the TAP interface to limit
   exposure.

Note: Adding a TLS library (e.g., BearSSL, ~45 KB) directly to the kernel
is an option if standalone deployment is required, but conflicts with the
compact-kernel goal. A host-side TLS relay proxy (`tools/tls_relay.py`) is
provided for development: it accepts plaintext HTTP from the guest and
forwards to upstream HTTPS APIs.

The kernel enforces a syscall security policy at the kernel-user boundary.
Each syscall passes through a 3-gate authorization check: valid syscall
number (ENOSYS), process context requirement (EPERM), and per-process
`syscall_allow` bitmask whitelist (EACCES). Denied syscalls are logged to
the kernel log ring buffer. Security counters are exposed via `/api/stats`.

Kernel memory safety hardenings:

- W^X enforcement: user-space pages cannot be simultaneously writable and
  executable. Code pages are loaded as R+W, then transitioned to R+X after
  the binary data is copied. ELF binaries requesting W+X segments are
  rejected at load time.
- User-pointer sanitization: `copy_from_user`/`copy_to_user` validate
  addresses at three layers: range check (within user address space),
  overflow check (no wrap-around), and per-page PTE_U verification via
  page-table walk. When the process has registered VMAs (`n_vmas > 0`),
  a fourth layer checks VMA containment (address must fall within a
  registered virtual memory area). Note: the current VMA check
  (`proc_vma_contains`) verifies spatial containment only -- VMA
  permission bits (READ/WRITE/EXEC) are not consulted; permission
  enforcement relies on PTE bits and hardware faults. Additionally,
  `copy_to_user` calls `user_addr_valid` rather than the stricter
  `user_addr_writable`, so the software-side write check is deferred to
  the hardware SUM fault path.
- VMA tracking: per-process virtual memory area list records code, stack,
  and data regions with permissions. Accesses outside any registered VMA
  return EFAULT. Processes with no registered VMAs (`n_vmas == 0`, e.g.,
  kernel tasks) fall back to range + PTE_U validation only. VMA permission
  enforcement is a known gap.
- Stack-protector: GCC `-fstack-protector-strong` (when
  `CONFIG_STACK_PROTECTOR=y`) places canary values between local variables
  and return addresses. The canary is initialized early in boot from
  entropy-mixed `rdtime()` samples. Corruption triggers a hard halt.
- Guard pages: the bottom 4 KiB of each kernel task stack is unmapped.
  Stack overflow causes a page fault instead of silent memory corruption.
  User-space stacks do not currently have an explicit guard page.
- Magic number validation (debug builds): `struct proc` and `struct
  sched_task` carry magic numbers validated on access. Freed process and
  task objects are poisoned with `0xDEADDEAD` for use-after-free detection.
  This coverage is scoped to proc/task structures, not all kernel objects.

The C programming style provides additional defense through
length-prefixed strings (`struct str`) that eliminate null-termination
bugs, and separate types for read-only, read-write, and appendable
buffers that enforce mutability constraints at the type level. No uses of
`strlen`, `strcpy`, `sprintf`, or other unbounded C string functions
exist in the kernel or user code.

## Debugging

Use QEMU's GDB stub:

```bash
GDB=1 make run
riscv-none-elf-gdb build/kernel.elf
# inside gdb:
target remote localhost:1234
```

QEMU will pause at startup until GDB connects.

# Design philosophy

## Lineage: what Mazu takes from Linux and Plan 9

Mazu sits at an intersection that few systems occupy. Linux is the gold
standard for SMP correctness and subsystem modularity but carries decades of
generality that a hard-RT embedded kernel cannot afford. Plan 9 solved the
observability problem elegantly -- expose system state as files, not ioctls --
but never targeted real-time or bare-metal embedded systems. Mazu takes the
structural discipline of one and the operational philosophy of the other, and
leaves behind the parts that conflict with bounded latency and small code size.

From Linux, Mazu borrows the patterns that keep a concurrent kernel honest:
level-ordered init hooks (`DEFINE_INIT_HOOK` with `INIT_LEVEL_CORE` /
`INIT_LEVEL_SUBSYS` levels, plus a DAG-based initgraph for dependency
ordering), irqchip vtables that decouple trap dispatch from
PLIC-specific MMIO, IRQ descriptor tables with `request_irq()` / `free_irq()`
registration, lockdep-style lock-ordering enforcement via per-CPU `held_locks`
bitmasks, waitqueue-based blocking with timeout callouts, and per-CPU state
accessed through the `gp` register so that hot-path scheduler and timer code
never touches a global lock. The synchronization primitives -- PI mutexes with
direct handover, condition variables, semaphores with FIFO direct-handover,
futexes with CMP_REQUEUE and PI -- are not simplified versions of their Linux
counterparts; they enforce the same invariants, just without the backward-
compatibility layers.

From Plan 9, Mazu borrows the idea that system state belongs in the filesystem
namespace. Three synthetic filesystems -- `/dev` (null, zero, console, time,
sysname), `/proc` (meminfo, uptime, cpuinfo), `/net` (arp, iface, tcp/stats)
-- generate their content on each read with no pre-computed state and no heap
allocation. The VFS mount table uses longest-prefix matching to dispatch reads
to the correct filesystem vtable. The result: `cat /proc/meminfo` or
`cat /net/tcp/stats` works the same way whether called from a shell task or
the REST API, with no special-purpose monitoring code.

What Mazu explicitly does not take: Linux's loadable modules, VFS page cache,
socket layer, and process isolation model; Plan 9's network stack and
user-space server model. The kernel runs all tasks in a single shared page
table (kernel regions identity-mapped, user pages at fixed VAs within the
same table). Disk-backed SFS uses a block buffer cache; synthetic and RAM
filesystems are uncached. Networking is a direct function interface, not a
socket API. These omissions are permanent design choices, not items on a
backlog.

## SMP as a structural property

When SMP support is added as a configuration option on top of a single-core
design, concurrency bugs hide until someone enables the second core. Mazu
inverts this: SMP shapes the data structures, and single-core is the
`NR_CPUS=1` special case.

Every hart owns its own run queue, sorted callout list, timer deadline, and
interrupt counters. The `struct pcpu` is cache-line aligned (64 bytes) and accessed via
the `gp` register in a single instruction -- no hash table, no array index.
Merged deadline management (`min(timer, preempt, watchdog)`) reduces hardware
timer reprogramming to the cases where the earliest deadline actually changes.
Lock ordering is enforced at compile time through level constants
(IRQ < PROC < FD < SIG < WAITQ < TCP < SCHED < CALLOUT < ALLOC) and at
runtime through lockdep assertions on every acquire and release.

The scheduler is unconditionally preemptive -- `CONFIG_SCHED_PREEMPTIVE` is
mandatory, and the build fails if someone tries to disable it. Every trap exit
drains `need_resched` via an atomic exchange. EEVDF provides fairness within
priority levels. EDF deadline scheduling with admission control and budget
enforcement targets hard-deadline workloads. Mixed-criticality scheduling
domains partition CPU time between high-criticality (control) and
low-criticality (web/telemetry) task groups with automatic escalation and
recovery.

## Networking and real-time in the same kernel

A common RTOS approach to networking is a separately-maintained IP stack
(lwIP, or a BSD-derived layer) integrated with the scheduler through an
adapter that bridges two different threading and memory models. That
integration boundary can become a source of priority inversions, lock
contention, and latency surprises.

Mazu puts TCP/IP inside the kernel under the same lock discipline as everything
else. The receive path is a preemptible scheduler task that drains the
virtio-net ring buffer, demultiplexes through ARP/ICMP/TCP, and hands data to
connection-specific circular buffers -- all under the same lockdep enforcement
that governs the scheduler. TCP connections live in a pool allocator (no
external fragmentation, O(1) alloc/free). Per-IP connection limits (`TCP_MAX_CONNS_PER_IP`,
`TCP_MAX_SYN_RCVD_PER_IP`) bound resource consumption under SYN floods without
a separate firewall. The HTTP server runs as a normal preemptible task that
yields its quantum like any other -- a deadline-scheduled task on the same
hart preempts it on the next trap exit.

The design goal is that bounded latency and network correctness share the
same scheduler, the same lock hierarchy, and the same per-CPU state rather
than living in separate subsystems that must be reconciled at runtime.

## Programming style

The C programming style emphasizes correctness and readability through
abstractions that differ from the C standard library, heavily inspired by
Chris Wellons' writing (see [nullprogram.com](https://nullprogram.com/)).
Core elements:

- [Length-prefixed strings](https://nullprogram.com/blog/2023/10/08/)
  (`struct str { char *dat; sz len; }`) instead of null-terminated strings
- Structured return values: either a success with a value or an error with
  a code -- error codes never share the return value space
- Separate types for read-only (`byte_view`, `str`), read-write
  (`byte_array`), and appendable (`byte_buf`, `str_buf`) memory regions
- Arena allocators for short-lived storage (e.g., per-request HTTP parsing)
- Pool allocators for fixed-size objects (TCP connections, send buffers)

These abstractions carry semantic information in the type system. The
mutability hierarchy eliminates entire classes of buffer-overflow and
use-after-free bugs while keeping the code readable enough for low-level
maintenance. Representative examples of this style include the ramfs
(`kernel/fs/ramfs.c`) and IP layer (`kernel/net/ip.c`).

# Internals

This section documents how the major subsystems fit together -- context that
cannot be found in the code alone.

- [Boot procedure](#boot-procedure)
- [Tasks](#tasks)
- [Networking](#networking)
- [TCP implementation](#tcp-implementation)
- [RAM fs](#ram-fs)
- [Verification and debugging](#verification-and-debugging)

## Boot procedure

OpenSBI firmware runs in M-mode and hands control to the kernel entry point
at `arch/riscv64/entry.c`.  The entry code saves the FDT pointer from `a1`,
sets up the initial stack, zeros BSS, and jumps to `kernel_init()` in
`kernel/init/main.c`.

Early boot parses the Flattened Device Tree (FDT) to discover hardware:
PLIC base, UART base, VirtIO-mmio slots, timebase frequency, and DRAM
layout.  All MMIO addresses are resolved from the FDT with fallbacks to the
QEMU `virt` machine defaults.

The `kernel_init` function calls subsystem initialization in dependency
order: memory (`mem_init` configures the dynamic region), paging
(`paging_init` builds Sv39 three-level identity-mapped page tables using
2&#8239;MiB superpages, activates `satp`, and flushes the TLB), heap
allocators (`kvalloc_init`), then `initgraph_run(INIT_FLAG_PRIMARY)` which
executes a DAG-based dependency graph of init tasks using Kahn's topological
sort (scheduler -> watchdog/loadbal/tcp -> mdns).
Architecture init (`arch_init`) brings up the UART, installs the PLIC-backed
trap vector via the `irqchip` vtable, and hardens CSR state (clears
`sstatus.SUM` and `sstatus.MXR`).

After init, the kernel creates core service tasks (packet receive, TCP
maintenance, web serving, optional probes) and enters the scheduler loop.

## Tasks

Mazu ships only hard-RT scheduling profiles:

- Default SMP hard-RT profile (`configs/defconfig`)
- RT validation profile (`configs/rt_defconfig`)

Timer-driven quanta force reschedule points at every priority level. When
`CONFIG_SMP` is enabled, each hart has its own run queue with a per-CPU lock,
an idle-steal path for pull migration, and a periodic load balancer using
exponential-decay estimation. Optional EEVDF fair scheduling
(`CONFIG_SCHED_EEVDF`) bounds wake-to-run latency within each priority level.
Deadline scheduling and mixed-criticality extensions build on the same SMP-first
model. Scheduling domains (`struct sched_domain`) enforce per-group CPU budgets
with automatic refill.

Kernel services are driven by scheduler tasks created during boot. Typical long-lived tasks include:

- Packet receive path (`netdev` -> protocol dispatch)
- TCP retransmission/callout maintenance
- Activity watchdog (detects hung tasks after 5s inactivity)
- SMP load balancer (periodic rebalancing across harts)
- HTTP/WebSocket request handling

## Networking

Networking is a core subsystem in Mazu because the kernel is intended for
connected embedded systems rather than isolated firmware. On QEMU `virt`,
networking is provided by the virtio-mmio net driver
(`drivers/net/virtio_mmio.c`) through the `netdev` abstraction layer. The RX
interrupt path pushes frames into the `netdev` input queue; scheduler tasks
drain that queue and pass packets up the protocol stack.

The receive task checks the queue, validates packet structure at each layer, and demultiplexes to
ARP/ICMP/TCP (plus optional UDP-based protocols). Replies are usually emitted during packet handling;
TCP also stages sent data in retransmission queues.

The routing table is initialized from `rootfs/config.txt` in `kernel_init` (or DHCP-derived values
when enabled). At transmit time, route lookup chooses interface/gateway; unresolved L2 next-hop
addresses trigger ARP resolution before payload transmission.

## TCP implementation

The TCP subsystem is the most complex part of Mazu and is documented in the most detail here.

* [RFC 1323: TCP Extensions for High Performance](https://www.rfc-editor.org/rfc/rfc1323)
* [RFC 6298: Computing TCP's Retransmission Timer](https://www.rfc-editor.org/rfc/rfc6298)
* [RFC 9293: Transmission Control Protocol (TCP)](https://www.rfc-editor.org/rfc/rfc9293)

### Public API

The essential functions exposed by the TCP subsystem:

```c
/* Server-side (listen/accept) */
struct tcp_conn *tcp_conn_listen(struct ipv4_addr addr, u16 port, struct arena tmp);
struct tcp_conn *tcp_conn_accept(struct tcp_conn *listen_conn);

/* Client-side (active open) */
struct tcp_conn *tcp_conn_connect(struct ipv4_addr remote_addr, u16 remote_port, struct arena tmp);
bool tcp_conn_is_connected(struct tcp_conn *conn);
bool tcp_conn_is_reset(struct tcp_conn *conn);

/* Data transfer and teardown */
struct result_sz tcp_conn_send(struct tcp_conn *conn, struct byte_view payload, bool *peer_closed_conn,
                               struct arena tmp);
struct result_sz tcp_conn_recv(struct tcp_conn *conn, struct byte_buf *buf, bool *peer_closed_conn);
struct result tcp_conn_close(struct tcp_conn **conn, struct arena tmp);
```

The primary consumer of the TCP interface is the web server. For comparison,
the equivalent Berkeley Sockets setup looks like:

```c
sfd = socket(AF_INET, SOCK_STREAM, 0);
bind(sfd, (struct sockaddr *)&addr, sizeof(addr));
listen(sfd, BACKLOG_SIZE);
```

The effect of all three calls is implemented by `tcp_conn_listen` in the Mazu TCP interface,
which takes as arguments an IP address and a port number. `tcp_conn_listen` returns a connection
structure that serves as a handle for a `LISTEN`-state connection ("listen connection" for short)
that the function creates. This connection structure essentially serves the purpose of the
`sfd` file descriptor in the example above.

Connections can be accepted with `tcp_conn_accept`. The only argument to `tcp_conn_accept` is
a listen connection. If a peer has tried to establish a connection with the right IP address
and port before `tcp_conn_accept` is called, a `struct tcp_conn` handle for this connection is
returned by `tcp_conn_accept`. Here, the "right" IP address and port number are, of course, the
IP address and port number that were passed to `tcp_conn_listen`. `tcp_conn_accept` can be polled
to await a connection.

Note that `tcp_conn_listen` and `tcp_conn_accept` both create a new connection. I.e., after
calling each function once and getting a non-`NULL` return value both times, there exist two
connections: one in the `LISTEN` state and one representing an active connection to a peer. This,
in turn, means a listen connection can be reused indefinitely to accept further connections. The
listen connection is deleted only after calling `tcp_conn_close` on it.

Three operations can be performed on open connections returned by `tcp_conn_accept`: sending data
to the peer, receiving data from the peer, and closing the connection. Connections are closed and
deleted by `tcp_conn_close`.

The send and receive functions can be called arbitrarily often.

`tcp_conn_send` takes a connection, a payload of bytes, and a pointer to a
boolean flag indicating whether the peer has closed the connection. If the peer
has closed the connection, it will not acknowledge new data; callers should
check this flag periodically and close the connection when it is set. The return
value indicates the number of bytes transmitted. TCP uses a sliding-window
approach to traffic control. If the caller sends data faster than the peer can
acknowledge, the window fills up, the implementation stops transmitting, and the
return value of `tcp_conn_send` is smaller than the payload length.

`tcp_conn_send` internally splits the payload into fragments small enough to fit
one Ethernet frame. Larger TCP segments could rely on IP-layer fragmentation,
but that has a downside: TCP retransmits at the segment level, so losing a
single IP fragment forces retransmission of the entire segment. Fragmenting at
the TCP level avoids this overhead. After splitting the payload, each fragment
is transmitted immediately and also added to the send buffer queue (SBQ) of the
connection for retransmission (see below).

`tcp_conn_recv` takes a connection, a destination buffer, and the peer-closed
flag. The TCP implementation buffers all received data internally. On each call,
available data is copied from the internal circular buffer into the destination
buffer. The amount copied is limited by whichever is smaller: available data or
destination capacity. The return value is the byte count copied; `0` means no
data is available.

The TCP subsystem also supports outbound (active open) connections via `tcp_conn_connect`.
This allocates an ephemeral port (49152-65535), sends a SYN, and returns a handle in SYN_SENT
state. The caller polls `tcp_conn_is_connected` until the three-way handshake completes (or
`tcp_conn_is_reset` to detect failure). Once connected, `tcp_conn_send` and `tcp_conn_recv`
work identically to server-side connections. This enables the kernel to make outbound HTTP
requests, which is required for the AI assistant use case.

A note on the peer-closed flag: the Berkeley Sockets API returns `-EOF` from
`read(2)` when a connection closes, packing error codes into the negative range
of non-negative return values. Mazu rejects this practice. A separate boolean
flag is a better fit because the condition ("has the peer closed?") does not
need to be checked on every call -- only eventually, to avoid infinite loops.

### The TCP state machine

The TCP protocol is based on a per-connection state machine. RFC 9293 contains an ASCII
diagram of the different states:

```plain
                            ┌─────────┐ ────────────\      active OPEN
                            │  CLOSED │              \    ───────────
                            └─────────┘◀─────────\    \   create TCB
                              │     ▲             \    \  snd SYN
                 passive OPEN │     │   CLOSE      \    \
                 ──────────── │     │ ──────────     \    \
                  create TCB  │     │ delete TCB      \    \
                              ▼     │                   \    \
          rcv RST (note 1)  ┌─────────┐            CLOSE │    \
       ────────────────────▶│  LISTEN │          ────────│     │
      /                     └─────────┘          delete TCB    │
     /           rcv SYN      │     │     SEND           │     │
    /           ───────────   │     │    ───────         │     ▼
┌────────┐      snd SYN,ACK  /       \   snd SYN         ┌────────┐
│        │◀─────────────────           ────────────────▶ │        │
│  SYN   │                    rcv SYN                    │  SYN   │
│  RCVD  │◀──────────────────────────────────────────────│  SENT  │
│        │                  snd SYN,ACK                  │        │
│        │──────────────────           ──────────────────│        │
└────────┘   rcv ACK of SYN  \       /  rcv SYN,ACK      └────────┘
   │         ──────────────   │     │   ───────────
   │                ×         │     │     snd ACK
   │                          ▼     ▼
   │  CLOSE                 ┌─────────┐
   │ ───────                │  ESTAB  │
   │ snd FIN                └─────────┘
   │                 CLOSE    │     │    rcv FIN
   ▼                ───────   │     │    ───────
┌─────────┐         snd FIN  /       \   snd ACK          ┌─────────┐
│  FIN    │◀────────────────           ──────────────────▶│  CLOSE  │
│ WAIT-1  │──────────────────                             │   WAIT  │
└─────────┘          rcv FIN  \                           └─────────┘
  │ rcv ACK of FIN   ───────   │                           CLOSE  │
  │ ──────────────   snd ACK   │                          ─────── │
  ▼        ×                   ▼                          snd FIN ▼
┌─────────┐               ┌─────────┐                    ┌─────────┐
│FINWAIT-2│               │ CLOSING │                    │ LAST-ACK│
└─────────┘               └─────────┘                    └─────────┘
  │              rcv ACK of FIN │                 rcv ACK of FIN │
  │  rcv FIN     ────────────── │    Timeout=2MSL ────────────── │
  │  ───────            ×       ▼    ────────────        ×       ▼
   \ snd ACK              ┌─────────┐delete TCB           ┌─────────┐
     ────────────────────▶ │TIME-WAIT│───────────────────▶│ CLOSED  │
                           └─────────┘                    └─────────┘
```

The Mazu TCP implementation adheres to these states closely and is modeled according to them.
When a segment is received from the IP layer, the corresponding connection is looked up, and a
call into a handler based on the current state of the connection is made. These handlers
manage the connection: they handle  transitions between states, allocate and free connections
and receive buffers, and update the different variables in the connection structures. Their
function names start with `tcp_handle_receive_`.

Handling each state separately is verbose, and coalescing similar behavior into
a generic handler that treats per-state differences as special cases would reduce
code size. The trade-off is deliberate: per-state handlers mirror the RFC
specification directly, making the code straightforward to verify and debug.
Where obvious, common behavior is factored into the `tcp_conn_update_*`
functions.

### Reception and circular receive buffers

A TCP connection receives data in the `ESTABLISHED` state. A dedicated task
polls the network device; when IP data arrives, the IP layer extracts the
TCP/IP pseudo header and passes it to `tcp_handle_packet`, which invokes the
per-state handlers described above.

When data is received by an `ESTABLISHED` TCP connection, it is appended to a circular buffer.
The buffer is allocated right before transitioning the connection to the `ESTABLISHED` state,
and it has a fixed size. The TCP implementation advertises the amount of available space to
the peer with the window size field of the TCP header. The advertised window size decreases
while the circular buffer fills up, which discourages the peer from sending more data. The
window size increases again after the caller has copied received data out of
the circular buffer via `tcp_conn_recv`.

### Send buffer queues (SBQs) and retransmissions

A key function of TCP, besides traffic control, is ensuring reliable delivery.

`tcp_conn_send` calls the internal `tcp_send_segment` function, which allocates a
send buffer (SB) -- a data structure designed to make prepending protocol headers
easy as the packet moves down the network stack. The payload is copied into the
send buffer, and the buffer is appended to the connection's send buffer queue
(SBQ). The SBQ is a linked list where each node carries timestamps for
retransmission timing and the ACK number that must arrive before the segment can
be freed.

`tcp_send_segment` calls into the IP layer to transmit the segment immediately
after adding the payload to the retransmission queue. A dedicated scheduler task
periodically calls `tcp_poll_retransmit`, which iterates
over all active connections and their SBQs. Each node in the SBQ is processed as follows (where
one node represents one segment waiting for retransmission or acknowledgment):

1. If the ACK for the segment has arrived since the last poll, the segment data is freed
   and the node is removed from the queue.
2. If a maximum number of retransmission attempts has been reached, the segment data is freed
   and the node is removed from the queue.
3. If neither of the two above conditions holds, and the retransmission timeout of the segment has
   expired, the segment is retransmitted. The timeout doubles on each
   retransmission (exponential backoff).

The TCP timestamps option (TSopt) measures the round-trip time (RTT) of each
connection. The base retransmission timeout (RTO) is computed dynamically from
RTT measurements using the algorithm in RFC 6298. If TSopt is absent, a default
RTO of 1 second is used (unlikely in practice -- TSopt has been standard since
1992).

### Congestion control

TCP congestion control is factored into a pluggable vtable (`struct tcp_cc_ops`) with three
callbacks: `on_ack`, `on_dup_ack`, and `on_timeout`. The default implementation is Reno
(RFC 5681): slow start, congestion avoidance, fast recovery on 3 duplicate ACKs, and
exponential backoff on RTO. The initial congestion window is 10 segments (RFC 6928). The
send path uses `min(rwnd, cwnd)` as the effective window. Per-connection `cwnd` and `ssthresh`
are exposed in the `/api/tcp` JSON endpoint. The vtable design allows future replacement with
Cubic or BBR without touching the TCP state machine.

### Memory management and allocators

TCP frequently needs new connection structures and data buffers, most of which
are short-lived. The implementation uses two allocation strategies.

Connection structures live in a global array. Each entry has an in-use flag;
allocation scans for an unused slot. This is simple, fast, and provides good
data locality. The array also supports the frequent full-table scans required
by retransmission polling and connection cleanup -- something a pool allocator
is not designed for.

A connection structure is allocated when the TCP handshake begins, but no data
buffers are allocated at that point. Receive buffers are allocated from a
fixed-size pool after the handshake completes. The pool allocator has low
overhead and strong locality.

Send buffers in the retransmission queue (SBQ) are allocated from their own
pool allocator. Any number of send buffers can be allocated for a single
connection, depending on how much data the caller is transmitting and how much
remains unacknowledged. Send buffers are freed based on the rules above.

The pool allocators, in turn, are backed by big contiguous allocations from `kvalloc`. All of them
are allocated at boot. This strategy leads to low fragmentation and speedy allocations.

## RAM fs

The RAM file system (ramfs) stores content served by the web server. The core
data structure is `struct ram_fs_node`:
```c
struct ram_fs_node {
    // First node in the directory if this node is of type RAM_FS_TYPE_DIR.
    struct ram_fs_node *first;
    // Next node in the same directory as this node. A linked list.
    struct ram_fs_node *next;
    enum ram_fs_node_type type;
    struct str name;
    // Data of the file if this node is of type RAM_FS_TYPE_FILE.
    struct byte_buf data;
    // Pointer back to parent FS.
    struct ram_fs *fs;
};
```

An instance of a ramfs is defined by the `struct ram_fs`:

```c
struct ram_fs {
    struct alloc data_alloc;
    struct pool node_alloc;
    struct arena scratch;
    struct ram_fs_node *root;
};
```

The `data_alloc` is an abstract allocator (could be any) that's used to allocate buffers for file
data and the names of nodes. The `node_alloc` is a pool allocator that hands out fixed-size chunks
of memory for `struct ram_fs_node` allocations. A ramfs is created by calling `ram_fs_new`. This
function takes the `data_alloc` as its only argument. The `node_alloc` is then allocated from the
`data_alloc`.

A separate allocator for node names would make sense because the allocation
patterns of names and data buffers differ. However, names vary in size, so a
pool allocator would waste memory (each slot must be the maximum size). Instead,
`data_alloc` serves all variable-length allocations.

The `first` and `next` fields of `struct ram_fs_node` form a tree. `next`
links all nodes in the same directory as a linked list. `first` is set only on
directory nodes and points to the first child. Subsequent children are reached
by following `next` pointers.

```
┌─ram_fs_node───────────┐
│                       │
│ /web                  │ NULL
│                       │
└───┬───────────────────┘
    │
    │ first
    ▼
┌─ram_fs_node───────────┐                   ┌─ram_fs_node───────────┐
│                       │ next        next  │                       │
│ /web/index.html       ├──────▶ ... ──────▶│ /web/public           │ NULL
│                       │                   │                       │
└───────────────────────┘                   └───┬───────────────────┘
   NULL                                         │
                                                │ first
                                                ▼
                                            ┌─ram_fs_node───────────┐
                                            │                       │
                                            │ /web/public/style.css │ NULL
                                            │                       │
                                            └───────────────────────┘
                                               NULL
```

Internally, paths are represented by the `struct path_name` structure. It looks like this:

```c
struct path_name {
    struct str src;
    // The path '/' is represented by a `struct path_name` where `n_components` is 0, the empty path.
    sz n_components;
    struct str *components;
    bool is_absolute;
};
```

`path_name_parse` takes a path string and an arena allocator, and returns a
`struct path_name`. The `src` field holds a full, unmodified copy of the path
string (allocated from the arena). The `components` array contains string slices
pointing into `src`, one per path component, with slashes stripped.

This structure makes path lookup trivial and keeps parsing cleanly separated
from tree traversal -- two concerns that are easier to verify independently.

## Verification and debugging

Mazu includes several runtime verification mechanisms, all gated on debug builds
(`__DEBUG__ > 0`) and compiled out entirely in release builds.

Lock ordering enforcement (`include/mazu/lockdep.h`): a per-CPU `held_locks`
bitmask tracks which lock levels are currently held. `lockdep_acquire()` asserts
that no same-or-higher-level lock is held before acquiring; `lockdep_release()`
asserts the lock was actually held (catches double-release). The lock hierarchy
is: IRQ(0) < PROC(1) < FD(2) < SIG(3) < WAITQ(4) < TCP(5) < SCHED(6) <
CALLOUT(7) < ALLOC(8). Violations
trigger `DEBUG_ASSERT` in debug builds.

Scheduler invariant checking (`kernel/sched/core.c`): `sched_check_invariants()`
runs at the end of every context switch and verifies three properties:
MutualExclusion (the selected task is in RUNNING state), SingleExecution (no
other hart is running the same task), and QueueConsistency (run queue bitmap
matches actual queue occupancy, all queued tasks are in READY state). Uses
`spin_trylock` to avoid deadlock during the check.

Callout telemetry (`kernel/timer/callout.c`): per-CPU lateness histogram bins
track how late each callout fires relative to its deadline. Six bins from 0-10us
to >100ms. Aggregated via `callout_get_stats()` and exposed in `/api/stats`
JSON. Callbacks more than 100us late are counted as missed.

## License

`mazu` is available under a permissive
[MIT](https://opensource.org/license/mit)-style license.
Use of this source code is governed by a MIT license that can be found
in the [LICENSE](LICENSE) file.
