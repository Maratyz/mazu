#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Offline WCRT (Worst-Case Response Time) schedulability analysis.

Validates that a task set meets deadlines before deployment.
Supports fixed-priority (Rate Monotonic / Deadline Monotonic) and EDF
(implicit-deadline only, utilization bound).

Usage:
    python3 scripts/wcrt_check.py tasks.json
    python3 scripts/wcrt_check.py --policy edf tasks.json

Input JSON format:
    [
      {"name": "task1", "period": 100, "runtime": 20, "deadline": 100, "priority": 3},
      {"name": "task2", "period": 200, "runtime": 50, "deadline": 200, "priority": 2},
      ...
    ]
    - period, runtime, deadline in microseconds
    - priority: higher = more important (for RM/DM; ignored for EDF)

Exit code: 0 if all tasks meet deadlines, 1 otherwise.
"""

import json
import sys
import argparse


def wcrt_fixed_priority(tasks):
    """Compute WCRT for fixed-priority scheduling (RM/DM).

    Uses the iterative recurrence:
        W(n+1) = C_i + sum(ceil(W(n) / T_j) * C_j) for all j with higher priority

    A task is schedulable iff WCRT <= deadline.
    """
    # Sort by priority descending (highest priority first)
    sorted_tasks = sorted(tasks, key=lambda t: t["priority"], reverse=True)

    results = []
    for i, task in enumerate(sorted_tasks):
        hp_tasks = sorted_tasks[:i]  # higher-priority tasks
        C = task["runtime"]
        T = task["period"]
        D = task["deadline"]

        # Iterative WCRT computation
        W = C
        for _ in range(1000):  # convergence bound
            W_next = C
            for hp in hp_tasks:
                # ceil(W / T_j) * C_j
                interfere = ((W + hp["period"] - 1) // hp["period"]) * hp["runtime"]
                W_next += interfere

            if W_next == W:
                break
            if W_next > D:
                W = W_next
                break
            W = W_next

        schedulable = W <= D
        util = task["runtime"] / task["period"]
        results.append(
            {
                "name": task["name"],
                "wcrt": W,
                "deadline": D,
                "period": T,
                "runtime": C,
                "utilization": util,
                "schedulable": schedulable,
            }
        )

    return results


def check_edf_utilization(tasks):
    """EDF schedulability check (implicit-deadline: D == T).

    For implicit-deadline task sets, EDF is schedulable iff
    sum(C_i / T_i) <= 1.0.

    Rejects task sets where deadline != period (explicit-deadline
    EDF requires demand-bound analysis, which is not implemented).
    """
    for task in tasks:
        if task["deadline"] != task["period"]:
            print(
                f"Error: EDF mode requires implicit deadlines (D == T), "
                f"but task '{task['name']}' has D={task['deadline']} != "
                f"T={task['period']}",
                file=sys.stderr,
            )
            sys.exit(2)

    results = []
    total_util = 0.0

    for task in tasks:
        util = task["runtime"] / task["period"]
        total_util += util
        results.append(
            {
                "name": task["name"],
                "deadline": task["deadline"],
                "period": task["period"],
                "runtime": task["runtime"],
                "utilization": util,
                "schedulable": None,  # set below
            }
        )

    all_schedulable = total_util <= 1.0
    for r in results:
        r["schedulable"] = all_schedulable
        r["total_utilization"] = total_util

    return results


def print_results(results, policy):
    """Print schedulability report."""
    all_ok = all(r["schedulable"] for r in results)

    print(f"Policy: {policy.upper()}")
    print(
        f"{'Task':<20} {'Runtime':>8} {'Deadline':>10} {'Period':>10} " f"{'Util':>6} ",
        end="",
    )
    if policy == "fp":
        print(f"{'WCRT':>10} ", end="")
    print(f"{'Result':>10}")
    print("-" * 80)

    for r in results:
        util_str = f"{r['utilization']:.3f}"
        sched_str = "PASS" if r["schedulable"] else "FAIL"
        line = (
            f"{r['name']:<20} {r['runtime']:>8} {r['deadline']:>10} "
            f"{r['period']:>10} {util_str:>6} "
        )
        if policy == "fp":
            line += f"{r['wcrt']:>10} "
        line += f"{sched_str:>10}"
        print(line)

    if policy == "edf" and results:
        print(f"\nTotal utilization: {results[0].get('total_utilization', 0):.4f}")

    print(f"\nVerdict: {'SCHEDULABLE' if all_ok else 'NOT SCHEDULABLE'}")
    return all_ok


def main():
    parser = argparse.ArgumentParser(
        description="Offline WCRT schedulability analysis for Mazu RTOS"
    )
    parser.add_argument("taskfile", help="JSON file with task parameters")
    parser.add_argument(
        "--policy",
        choices=["fp", "edf"],
        default="fp",
        help="Scheduling policy: fp (fixed-priority, "
        "caller-supplied priorities), edf (implicit-"
        "deadline utilization bound). Default: fp",
    )
    args = parser.parse_args()

    with open(args.taskfile) as f:
        tasks = json.load(f)

    # Validate input
    for t in tasks:
        for field in ("name", "period", "runtime", "deadline"):
            if field not in t:
                print(f"Error: task missing '{field}' field", file=sys.stderr)
                sys.exit(2)
        if t["runtime"] <= 0 or t["period"] <= 0 or t["deadline"] <= 0:
            print(f"Error: task '{t['name']}' has non-positive params", file=sys.stderr)
            sys.exit(2)
        if t["runtime"] > t["deadline"]:
            print(f"Error: task '{t['name']}' runtime > deadline", file=sys.stderr)
            sys.exit(2)
        if args.policy == "fp" and "priority" not in t:
            print(
                f"Error: task '{t['name']}' missing 'priority' for {args.policy}",
                file=sys.stderr,
            )
            sys.exit(2)

    if args.policy == "fp":
        results = wcrt_fixed_priority(tasks)
    else:
        results = check_edf_utilization(tasks)

    ok = print_results(results, args.policy)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
