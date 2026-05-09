#!/usr/bin/env python3
"""
KTRACE replay verification checker for the Mazu RISC-V kernel.

Parses KTRACE structured event lines from a QEMU serial log and replays
them against a shadow state model.  Flags any transition that violates
the state machine rules defined by the event schema notes.

Exit codes:
    0  -- no violations (or no KTRACE lines found)
    1  -- at least one violation detected

Usage:
    scripts/check_eventv1.py [OPTIONS] [LOG_FILE]

If LOG_FILE is omitted or "-", reads from stdin.

Options:
    -h, --help       Show this help message and exit.
    -v, --verbose    Print each parsed event (not just violations).
    -q, --quiet      Suppress the summary line on success.
    --strict         Treat unknown event names as violations.

Requires Python 3.6+.  No third-party dependencies.
"""

from __future__ import print_function

import json
import sys
import os
import re

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

# Valid task states (match enum td_state in include/mazu/sched.h)
TASK_STATES = frozenset(
    [
        "READY",
        "RUNNING",
        "SLEEPING",
        "SEM_WAIT",
        "YIELDING",
        "TERMINATING",
        "BLOCKED",
    ]
)

# Valid TCP states (match kernel/net/tcp.c)
TCP_STATES = frozenset(
    [
        "CLOSED",
        "LISTEN",
        "SYN_SENT",
        "SYN_RCVD",
        "ESTABLISHED",
        "FIN_WAIT_1",
        "FIN_WAIT_2",
        "CLOSE_WAIT",
        "CLOSING",
        "LAST_ACK",
        "TIME_WAIT",
        "RESET",
    ]
)

# RFC 793 + RESET shortcut: source -> set of valid destinations
TCP_TRANSITIONS = {
    "CLOSED": frozenset(["LISTEN", "SYN_SENT"]),
    "LISTEN": frozenset(["SYN_RCVD", "CLOSED"]),
    "SYN_SENT": frozenset(["ESTABLISHED", "RESET", "CLOSED"]),
    "SYN_RCVD": frozenset(["ESTABLISHED", "FIN_WAIT_1", "RESET", "CLOSED"]),
    "ESTABLISHED": frozenset(["FIN_WAIT_1", "CLOSE_WAIT", "RESET"]),
    "CLOSE_WAIT": frozenset(["LAST_ACK", "RESET"]),
    "FIN_WAIT_1": frozenset(["FIN_WAIT_2", "CLOSING", "TIME_WAIT", "RESET"]),
    "FIN_WAIT_2": frozenset(["TIME_WAIT", "RESET"]),
    "CLOSING": frozenset(["TIME_WAIT", "RESET"]),
    "LAST_ACK": frozenset(["CLOSED", "RESET"]),
    "TIME_WAIT": frozenset(["CLOSED", "RESET"]),
    "RESET": frozenset(["CLOSED"]),
}

# Known event names (for --strict mode)
KNOWN_EVENTS = frozenset(
    [
        "task_state_changed",
        "task_switched",
        "running_committed",
        "ready_queued",
        "ready_dequeued",
        "tcp_state_changed",
        "syscall_entry",
        "syscall_exit",
    ]
)

# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

# Matches "KTRACE " anywhere in the line (strips log-level prefixes)
_KTRACE_RE = re.compile(r"KTRACE\s+(.*)")

# Matches key=value tokens (value has no whitespace)
_KV_RE = re.compile(r"([a-zA-Z_][a-zA-Z0-9_]*)=(\S+)")


def parse_eventv1(line):
    """Parse an KTRACE line into (event_name, {key: value_str}) or None."""
    m = _KTRACE_RE.search(line)
    if m is None:
        return None
    payload = m.group(1)
    fields = dict(_KV_RE.findall(payload))
    event_name = fields.pop("event", None)
    if event_name is None:
        return None
    return event_name, fields


def field_int(fields, key):
    """Extract an integer field, or None if missing / not numeric."""
    v = fields.get(key)
    if v is None:
        return None
    try:
        return int(v)
    except ValueError:
        return None


def field_str(fields, key):
    """Extract a string field, or None if missing."""
    return fields.get(key)


def replay_events(model, parsed_events):
    """Replay parsed events in timestamp order when available."""
    decorated = []
    for line_no, event_name, fields in parsed_events:
        ts = field_int(fields, "t")
        decorated.append(
            (ts is None, ts if ts is not None else line_no, line_no, event_name, fields)
        )

    decorated.sort()

    for _, _, line_no, event_name, fields in decorated:
        model.dispatch(line_no, event_name, fields)


# ---------------------------------------------------------------------------
# Shadow State Model
# ---------------------------------------------------------------------------


class ShadowModel:
    """
    Maintains shadow state for scheduler and TCP, replays events, and
    accumulates violations.
    """

    def __init__(self, verbose=False, strict=False):
        self.verbose = verbose
        self.strict = strict

        # Per-task state: tid -> state string
        self.task_state = {}

        # Per-hart current task: cpu -> tid (or None)
        self.current_task = {}

        # Run queue membership: set of tids currently in a run queue
        self.in_runq = set()

        # Per-TCP-connection state: conn -> state string
        self.tcp_state = {}

        # Pending syscalls: (cpu, tid) -> nr
        self.pending_syscall = {}

        # Counters
        self.events_parsed = 0
        self.violations = []

    def violation(self, line_no, msg):
        """Record a state machine violation."""
        entry = "line {}: {}".format(line_no, msg)
        self.violations.append(entry)
        print("VIOLATION: {}".format(entry), file=sys.stderr)

    # -- Scheduler events --------------------------------------------------

    def handle_task_state_changed(self, line_no, fields):
        tid = field_int(fields, "tid")
        old = field_str(fields, "old")
        new = field_str(fields, "new")
        cpu = field_int(fields, "cpu")

        if tid is None or old is None or new is None or cpu is None:
            self.violation(
                line_no,
                "task_state_changed: missing required field " "(tid, old, new, cpu)",
            )
            return

        if old not in TASK_STATES:
            self.violation(
                line_no, "task_state_changed: unknown old state '{}'".format(old)
            )
        if new not in TASK_STATES:
            self.violation(
                line_no, "task_state_changed: unknown new state '{}'".format(new)
            )

        # Check consistency: if we have prior knowledge of the task's state,
        # it should match the declared 'old'.
        known = self.task_state.get(tid)
        if known is not None and known != old:
            self.violation(
                line_no,
                "task_state_changed: tid={} shadow state is '{}' "
                "but event claims old='{}'".format(tid, known, old),
            )

        self.task_state[tid] = new

    def handle_task_switched(self, line_no, fields):
        cpu = field_int(fields, "cpu")
        prev = field_int(fields, "prev")
        nxt = field_int(fields, "next")

        if cpu is None or prev is None or nxt is None:
            self.violation(
                line_no, "task_switched: missing required field " "(cpu, prev, next)"
            )
            return

        # MutualExclusion: if running_committed already updated
        # current_task to nxt, accept that as consistent.  Otherwise
        # the hart should have been running prev.
        cur = self.current_task.get(cpu)
        if cur is not None and cur != prev and cur != nxt:
            self.violation(
                line_no,
                "task_switched: cpu={} was running tid={} "
                "but event says prev={}".format(cpu, cur, prev),
            )

        self.current_task[cpu] = nxt

    def handle_running_committed(self, line_no, fields):
        cpu = field_int(fields, "cpu")
        tid = field_int(fields, "tid")

        if cpu is None or tid is None:
            self.violation(
                line_no, "running_committed: missing required field " "(cpu, tid)"
            )
            return

        # SingleExecution: check no other hart claims this task.
        for other_cpu, other_tid in self.current_task.items():
            if other_tid == tid and other_cpu != cpu:
                self.violation(
                    line_no,
                    "running_committed: tid={} already running "
                    "on cpu={}, cannot commit on cpu={}".format(tid, other_cpu, cpu),
                )

        self.current_task[cpu] = tid
        self.task_state[tid] = "RUNNING"

    def handle_ready_queued(self, line_no, fields):
        cpu = field_int(fields, "cpu")
        tid = field_int(fields, "tid")
        prio = field_int(fields, "prio")

        if cpu is None or tid is None or prio is None:
            self.violation(
                line_no, "ready_queued: missing required field " "(cpu, tid, prio)"
            )
            return

        # QueueConsistency: task being enqueued should be READY (or
        # transitioning to READY -- the task_state_changed event may
        # have already updated the shadow).
        state = self.task_state.get(tid)
        if state is not None and state not in ("READY",):
            self.violation(
                line_no,
                "ready_queued: tid={} has state '{}', "
                "expected READY".format(tid, state),
            )

        if tid in self.in_runq:
            self.violation(
                line_no, "ready_queued: tid={} already in run queue".format(tid)
            )

        self.in_runq.add(tid)

    def handle_ready_dequeued(self, line_no, fields):
        cpu = field_int(fields, "cpu")
        tid = field_int(fields, "tid")

        if cpu is None or tid is None:
            self.violation(
                line_no, "ready_dequeued: missing required field " "(cpu, tid)"
            )
            return

        if tid not in self.in_runq:
            self.violation(
                line_no, "ready_dequeued: tid={} not in run queue".format(tid)
            )
        else:
            self.in_runq.discard(tid)

    # -- TCP events ---------------------------------------------------------

    def handle_tcp_state_changed(self, line_no, fields):
        conn = field_int(fields, "conn")
        old = field_str(fields, "old")
        new = field_str(fields, "new")

        if conn is None or old is None or new is None:
            self.violation(
                line_no, "tcp_state_changed: missing required field " "(conn, old, new)"
            )
            return

        if old not in TCP_STATES:
            self.violation(
                line_no, "tcp_state_changed: unknown old state '{}'".format(old)
            )
            return
        if new not in TCP_STATES:
            self.violation(
                line_no, "tcp_state_changed: unknown new state '{}'".format(new)
            )
            return

        # Check shadow consistency.
        known = self.tcp_state.get(conn)
        if known is not None and known != old:
            self.violation(
                line_no,
                "tcp_state_changed: conn={} shadow state is '{}' "
                "but event claims old='{}'".format(conn, known, old),
            )

        # Validate transition.
        allowed = TCP_TRANSITIONS.get(old, frozenset())
        if new not in allowed:
            self.violation(
                line_no,
                "tcp_state_changed: conn={} invalid transition "
                "{} -> {}".format(conn, old, new),
            )

        # TCP connection IDs are pool slots, not globally unique connection
        # identities. Once a slot reaches CLOSED it may be reused for a new
        # connection whose first visible state is not derived from the old one.
        if new == "CLOSED":
            self.tcp_state.pop(conn, None)
        else:
            self.tcp_state[conn] = new

    # -- Syscall events -----------------------------------------------------

    def handle_syscall_entry(self, line_no, fields):
        cpu = field_int(fields, "cpu")
        tid = field_int(fields, "tid")
        nr = field_int(fields, "nr")

        if cpu is None or tid is None or nr is None:
            self.violation(
                line_no, "syscall_entry: missing required field " "(cpu, tid, nr)"
            )
            return

        key = (cpu, tid)
        if key in self.pending_syscall:
            self.violation(
                line_no,
                "syscall_entry: cpu={} tid={} already has "
                "pending syscall nr={}".format(cpu, tid, self.pending_syscall[key]),
            )

        self.pending_syscall[key] = nr

    def handle_syscall_exit(self, line_no, fields):
        cpu = field_int(fields, "cpu")
        tid = field_int(fields, "tid")
        nr = field_int(fields, "nr")
        ret = field_int(fields, "ret")

        if cpu is None or tid is None or nr is None or ret is None:
            self.violation(
                line_no,
                "syscall_exit: missing required field " "(cpu, tid, nr, ret)",
            )
            return

        key = (cpu, tid)
        pending = self.pending_syscall.get(key)

        if pending is None:
            self.violation(
                line_no,
                "syscall_exit: cpu={} tid={} nr={} "
                "has no matching syscall_entry".format(cpu, tid, nr),
            )
        elif pending != nr:
            self.violation(
                line_no,
                "syscall_exit: cpu={} tid={} nr={} "
                "does not match pending entry nr={}".format(cpu, tid, nr, pending),
            )
        else:
            del self.pending_syscall[key]

    # -- Dispatch -----------------------------------------------------------

    # Map event name -> handler method
    _HANDLERS = {
        "task_state_changed": "handle_task_state_changed",
        "task_switched": "handle_task_switched",
        "running_committed": "handle_running_committed",
        "ready_queued": "handle_ready_queued",
        "ready_dequeued": "handle_ready_dequeued",
        "tcp_state_changed": "handle_tcp_state_changed",
        "syscall_entry": "handle_syscall_entry",
        "syscall_exit": "handle_syscall_exit",
    }

    def dispatch(self, line_no, event_name, fields):
        """Dispatch a parsed event to the appropriate handler."""
        self.events_parsed += 1

        if self.verbose:
            print(
                "  line {}: event={} {}".format(
                    line_no,
                    event_name,
                    " ".join("{}={}".format(k, v) for k, v in fields.items()),
                )
            )

        handler_name = self._HANDLERS.get(event_name)
        if handler_name is not None:
            getattr(self, handler_name)(line_no, fields)
        elif self.strict:
            self.violation(line_no, "unknown event name '{}'".format(event_name))


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def usage():
    print(__doc__.strip())


def main():
    verbose = False
    quiet = False
    strict = False
    log_path = None

    args = sys.argv[1:]
    i = 0
    while i < len(args):
        arg = args[i]
        if arg in ("-h", "--help"):
            usage()
            sys.exit(0)
        elif arg in ("-v", "--verbose"):
            verbose = True
        elif arg in ("-q", "--quiet"):
            quiet = True
        elif arg == "--strict":
            strict = True
        elif arg == "-":
            log_path = None  # explicit stdin
        elif arg.startswith("-"):
            print("check_eventv1: unknown option: {}".format(arg), file=sys.stderr)
            usage()
            sys.exit(2)
        else:
            if log_path is not None:
                print(
                    "check_eventv1: multiple log files not supported", file=sys.stderr
                )
                sys.exit(2)
            log_path = arg
        i += 1

    # Open input.
    if log_path is not None:
        if not os.path.isfile(log_path):
            print("check_eventv1: file not found: {}".format(log_path), file=sys.stderr)
            sys.exit(2)
        fh = open(log_path, "r", errors="replace")
    else:
        fh = sys.stdin

    model = ShadowModel(verbose=verbose, strict=strict)

    try:
        parsed_events = []

        # Auto-detect JSON input from /api/klog: {"dropped":N,"log":"..."}
        # KTRACE events live in the klog ring buffer, served as JSON by
        # /api/klog.  When the input starts with '{', extract the "log"
        # field and split into lines for replay.
        first_line = None
        lines_iter = enumerate(fh, start=1)
        for line_no, raw_line in lines_iter:
            first_line = raw_line.rstrip("\n\r")
            break

        if first_line is not None and first_line.startswith("{"):
            # JSON mode: read all remaining input, concatenate, parse.
            all_text = first_line + "\n"
            for _, raw_line in lines_iter:
                all_text += raw_line
            try:
                obj = json.loads(all_text)
                dropped = obj.get("dropped", 0)
                if isinstance(dropped, int) and dropped > 0:
                    if not quiet:
                        print(
                            "check_eventv1: skipped replay because /api/klog "
                            "reported dropped={} bytes".format(dropped),
                            file=sys.stderr,
                        )
                    return 0
                log_text = obj.get("log", "")
            except (json.JSONDecodeError, ValueError):
                log_text = all_text  # fall back to raw
            for line_no, line in enumerate(log_text.split("\n"), start=1):
                parsed = parse_eventv1(line)
                if parsed is None:
                    continue
                event_name, fields = parsed
                parsed_events.append((line_no, event_name, fields))
        else:
            # Plain text mode: process line by line.
            if first_line is not None:
                parsed = parse_eventv1(first_line)
                if parsed is not None:
                    parsed_events.append((1, parsed[0], parsed[1]))
            for line_no, raw_line in lines_iter:
                line = raw_line.rstrip("\n\r")
                parsed = parse_eventv1(line)
                if parsed is None:
                    continue
                event_name, fields = parsed
                parsed_events.append((line_no, event_name, fields))

        replay_events(model, parsed_events)
    finally:
        if fh is not sys.stdin:
            fh.close()

    # Final consistency checks.
    # Unpaired syscall entries are suspicious but not fatal (the log
    # may have been truncated mid-run).  Report them as warnings.
    for (cpu, tid), nr in model.pending_syscall.items():
        print(
            "WARNING: unterminated syscall_entry: cpu={} tid={} nr={}".format(
                cpu, tid, nr
            ),
            file=sys.stderr,
        )

    # Report results.
    n_violations = len(model.violations)
    n_events = model.events_parsed

    if n_events == 0:
        if not quiet:
            print("check_eventv1: no KTRACE lines found (OK)", file=sys.stderr)
        sys.exit(0)

    if n_violations > 0:
        print(
            "check_eventv1: {} violation(s) in {} event(s)".format(
                n_violations, n_events
            ),
            file=sys.stderr,
        )
        sys.exit(1)

    if not quiet:
        print(
            "check_eventv1: {} event(s) replayed, 0 violations".format(n_events),
            file=sys.stderr,
        )
    sys.exit(0)


if __name__ == "__main__":
    main()
