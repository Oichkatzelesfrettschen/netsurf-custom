#!/usr/bin/env python3
"""Summarize and check NetSurf benchmark and profiler baselines."""

from __future__ import annotations

import argparse
import json
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def summarize_benchmark(args: argparse.Namespace) -> int:
    data = json.loads(Path(args.input).read_text(encoding="utf-8"))
    payload = {
        "schema": 1,
        "scenario": args.scenario,
        "tool": "benchmark",
        "target": args.target,
        "command": args.command,
        "environment": args.environment,
        "metrics": {
            "elapsed_s": data.get("elapsed_s", 0.0),
            "score": data.get("score", 0.0),
            "pass": data.get("totals", {}).get("pass", 0),
            "fail": data.get("totals", {}).get("fail", 0),
            "skip": data.get("totals", {}).get("skip", 0),
            "total": data.get("totals", {}).get("total", 0),
        },
    }
    write_json(Path(args.output), payload)
    return 0


def summarize_suite(args: argparse.Namespace) -> int:
    data = json.loads(Path(args.input).read_text(encoding="utf-8"))
    totals = data.get("totals", {})
    payload = {
        "schema": 1,
        "scenario": args.scenario,
        "tool": "profile-suite",
        "target": args.target,
        "command": args.command,
        "environment": args.environment,
        "metrics": {
            "elapsed_s": data.get("elapsed_s", 0.0),
            "pass": totals.get("pass", 0),
            "fail": totals.get("fail", 0),
            "total": totals.get("total", 0),
        },
    }
    write_json(Path(args.output), payload)
    return 0


def summarize_valgrind(args: argparse.Namespace) -> int:
    root = ET.fromstring(Path(args.input).read_text(encoding="utf-8"))
    errors = root.findall(".//error")
    leak_kinds: dict[str, int] = {}
    for error in errors:
        kind = error.findtext("kind", default="UNKNOWN")
        leak_kinds[kind] = leak_kinds.get(kind, 0) + 1
    payload = {
        "schema": 1,
        "scenario": args.scenario,
        "tool": "valgrind",
        "target": args.target,
        "command": args.command,
        "environment": args.environment,
        "metrics": {
            "error_count": len(errors),
            "leak_kinds": leak_kinds,
        },
    }
    write_json(Path(args.output), payload)
    return 0


def summarize_heaptrack(args: argparse.Namespace) -> int:
    text = Path(args.input).read_text(encoding="utf-8")
    metrics = {}
    patterns = {
        "allocations": r"allocations:\s+(\d+)",
        "leaked_allocations": r"leaked allocations:\s+(\d+)",
        "temporary_allocations": r"temporary allocations:\s+(\d+)",
    }
    for key, pattern in patterns.items():
        match = re.search(pattern, text)
        metrics[key] = int(match.group(1)) if match else 0
    payload = {
        "schema": 1,
        "scenario": args.scenario,
        "tool": "heaptrack",
        "target": args.target,
        "command": args.command,
        "environment": args.environment,
        "profile_file": args.profile_file,
        "metrics": metrics,
    }
    write_json(Path(args.output), payload)
    return 0


def summarize_time(args: argparse.Namespace) -> int:
    text = Path(args.input).read_text(encoding="utf-8")
    metrics: dict[str, float | int] = {}
    patterns = {
        "user_s": r"User time \(seconds\):\s+([0-9.]+)",
        "system_s": r"System time \(seconds\):\s+([0-9.]+)",
        "cpu_percent": r"Percent of CPU this job got:\s+([0-9]+)%",
        "elapsed_clock": r"Elapsed \(wall clock\) time .*:\s+([0-9:.\-]+)",
        "max_resident_kib": r"Maximum resident set size \(kbytes\):\s+([0-9]+)",
    }
    for key, pattern in patterns.items():
        match = re.search(pattern, text)
        if match is None:
            continue
        if key in {"cpu_percent", "max_resident_kib"}:
            metrics[key] = int(match.group(1))
        elif key == "elapsed_clock":
            raw = match.group(1)
            parts = raw.split(":")
            if len(parts) == 3:
                hours = float(parts[0])
                minutes = float(parts[1])
                seconds = float(parts[2])
                metrics["elapsed_s"] = hours * 3600.0 + minutes * 60.0 + seconds
            elif len(parts) == 2:
                minutes = float(parts[0])
                seconds = float(parts[1])
                metrics["elapsed_s"] = minutes * 60.0 + seconds
            else:
                metrics["elapsed_s"] = float(parts[0])
        else:
            metrics[key] = float(match.group(1))

    payload = {
        "schema": 1,
        "scenario": args.scenario,
        "tool": "time",
        "target": args.target,
        "command": args.command,
        "environment": args.environment,
        "metrics": metrics,
    }
    write_json(Path(args.output), payload)
    return 0


def check(args: argparse.Namespace) -> int:
    actual = json.loads(Path(args.actual).read_text(encoding="utf-8"))
    baseline = json.loads(Path(args.baseline).read_text(encoding="utf-8"))
    thresholds = baseline.get("thresholds", {})
    metrics = actual.get("metrics", {})
    failures: list[str] = []
    for key, value in thresholds.items():
        if key.endswith("_min"):
            metric = key[:-4]
            actual_value = metrics.get(metric)
            if actual_value is None or actual_value < value:
                failures.append(f"{metric} regression: {actual_value} < baseline {value}")
        elif key.endswith("_max"):
            metric = key[:-4]
            actual_value = metrics.get(metric)
            if actual_value is None or actual_value > value:
                failures.append(f"{metric} regression: {actual_value} > baseline {value}")
    if failures:
        print(f"PERF REGRESSION: {baseline.get('scenario', 'unknown scenario')}")
        for failure in failures:
            print(f"  {failure}")
        return 1
    print(f"OK: {baseline.get('scenario', 'unknown scenario')} meets baseline")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Perf baseline helpers")
    sub = parser.add_subparsers(dest="cmd", required=True)

    bench = sub.add_parser("summarize-benchmark")
    bench.add_argument("--input", required=True)
    bench.add_argument("--output", required=True)
    bench.add_argument("--scenario", required=True)
    bench.add_argument("--target", required=True)
    bench.add_argument("--command", required=True)
    bench.add_argument("--environment", default="")
    bench.set_defaults(func=summarize_benchmark)

    suite = sub.add_parser("summarize-suite")
    suite.add_argument("--input", required=True)
    suite.add_argument("--output", required=True)
    suite.add_argument("--scenario", required=True)
    suite.add_argument("--target", required=True)
    suite.add_argument("--command", required=True)
    suite.add_argument("--environment", default="")
    suite.set_defaults(func=summarize_suite)

    valgrind = sub.add_parser("summarize-valgrind")
    valgrind.add_argument("--input", required=True)
    valgrind.add_argument("--output", required=True)
    valgrind.add_argument("--scenario", required=True)
    valgrind.add_argument("--target", required=True)
    valgrind.add_argument("--command", required=True)
    valgrind.add_argument("--environment", default="")
    valgrind.set_defaults(func=summarize_valgrind)

    heaptrack = sub.add_parser("summarize-heaptrack")
    heaptrack.add_argument("--input", required=True)
    heaptrack.add_argument("--output", required=True)
    heaptrack.add_argument("--scenario", required=True)
    heaptrack.add_argument("--target", required=True)
    heaptrack.add_argument("--command", required=True)
    heaptrack.add_argument("--environment", default="")
    heaptrack.add_argument("--profile-file", required=True)
    heaptrack.set_defaults(func=summarize_heaptrack)

    time_summary = sub.add_parser("summarize-time")
    time_summary.add_argument("--input", required=True)
    time_summary.add_argument("--output", required=True)
    time_summary.add_argument("--scenario", required=True)
    time_summary.add_argument("--target", required=True)
    time_summary.add_argument("--command", required=True)
    time_summary.add_argument("--environment", default="")
    time_summary.set_defaults(func=summarize_time)

    checker = sub.add_parser("check")
    checker.add_argument("--actual", required=True)
    checker.add_argument("--baseline", required=True)
    checker.set_defaults(func=check)

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
