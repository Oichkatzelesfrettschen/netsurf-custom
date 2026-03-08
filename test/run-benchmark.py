#!/usr/bin/env python3
"""
Web Standards Compliance Benchmark Runner for NetSurf.

Runs the web-standards-benchmark.yaml monkey test and parses
the output into a scored report with per-category breakdown.

Usage:
    python3 test/run-benchmark.py [-m ./nsmonkey]
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time


def find_monkey():
    """Find the nsmonkey binary."""
    candidates = [
        "./nsmonkey",
        "build/Linux-monkey/nsmonkey",
        os.path.expanduser("~/dev-netsurf/workspace/bin/nsmonkey"),
    ]
    for c in candidates:
        if os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return None


def run_benchmark(monkey_path, test_path):
    """Run the benchmark via monkey_driver.py and capture output."""
    driver = os.path.join(os.path.dirname(__file__), "monkey_driver.py")
    cmd = [sys.executable, driver, "-m", monkey_path, "-t", test_path]
    start = time.monotonic()
    result = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        timeout=120,
    )
    elapsed = time.monotonic() - start
    return result.stdout + result.stderr, elapsed


def parse_results(output):
    """Parse PASS/FAIL/SKIP lines and summary from benchmark output."""
    categories = {}
    results = []
    total_pass = 0
    total_fail = 0
    total_skip = 0
    summary_line = None

    for line in output.splitlines():
        # Match PASS/FAIL/SKIP lines with category (only in LOG: lines)
        m = re.search(r"LOG: (PASS|FAIL|SKIP) \[(\w+)\] (.+?)(?:\s*$)", line)
        if m:
            status, cat, name = m.group(1), m.group(2), m.group(3)
            results.append({"status": status, "category": cat, "name": name})
            if cat not in categories:
                categories[cat] = {"pass": 0, "fail": 0, "skip": 0}
            if status == "PASS":
                categories[cat]["pass"] += 1
                total_pass += 1
            elif status == "FAIL":
                categories[cat]["fail"] += 1
                total_fail += 1
            else:
                categories[cat]["skip"] += 1
                total_skip += 1

        # Match summary line (in LOG: output)
        m2 = re.search(
            r"LOG: TOTAL: (\d+) PASS: (\d+) FAIL: (\d+) SKIP: (\d+) SCORE: ([\d.]+)%",
            line,
        )
        if m2:
            summary_line = {
                "total": int(m2.group(1)),
                "pass": int(m2.group(2)),
                "fail": int(m2.group(3)),
                "skip": int(m2.group(4)),
                "score": float(m2.group(5)),
            }

    return {
        "results": results,
        "categories": categories,
        "summary": summary_line,
        "totals": {
            "pass": total_pass,
            "fail": total_fail,
            "skip": total_skip,
            "total": total_pass + total_fail + total_skip,
        },
    }


def print_report(parsed, elapsed):
    """Print a formatted benchmark report."""
    totals = parsed["totals"]
    cats = parsed["categories"]

    total = totals["total"]
    if total == 0:
        print("ERROR: No test results parsed from benchmark output.")
        return False

    score = round(totals["pass"] * 1000 / total) / 10 if total > 0 else 0

    bar_width = 40

    print()
    print("=" * 72)
    print("  NetSurf Web Standards Compliance Benchmark")
    print("=" * 72)
    print()

    # Overall score
    filled = int(bar_width * score / 100)
    bar = "#" * filled + "-" * (bar_width - filled)
    print(f"  Overall Score: {score}%  [{bar}]")
    print(f"  Passed: {totals['pass']}  Failed: {totals['fail']}  "
          f"Skipped: {totals['skip']}  Total: {total}")
    print(f"  Elapsed: {elapsed:.1f}s")
    print()

    # Per-category breakdown
    print("-" * 72)
    print(f"  {'Category':<24} {'Pass':>5} {'Fail':>5} {'Skip':>5} "
          f"{'Total':>5}  {'Score':>7}  Bar")
    print("-" * 72)

    cat_names = sorted(cats.keys())
    for cat in cat_names:
        c = cats[cat]
        ct = c["pass"] + c["fail"] + c["skip"]
        cpct = round(c["pass"] * 1000 / ct) / 10 if ct > 0 else 0
        cbar_filled = int(20 * cpct / 100)
        cbar = "#" * cbar_filled + "-" * (20 - cbar_filled)
        status = "PERFECT" if cpct == 100 else ""
        print(f"  {cat:<24} {c['pass']:>5} {c['fail']:>5} {c['skip']:>5} "
              f"{ct:>5}  {cpct:>6.1f}%  [{cbar}] {status}")

    print("-" * 72)
    print()

    # Failures detail
    failures = [r for r in parsed["results"] if r["status"] == "FAIL"]
    if failures:
        print("  FAILURES:")
        for f in failures:
            print(f"    [{f['category']}] {f['name']}")
        print()

    # Skips detail
    skips = [r for r in parsed["results"] if r["status"] == "SKIP"]
    if skips:
        print("  SKIPPED:")
        for s in skips:
            print(f"    [{s['category']}] {s['name']}")
        print()

    # Grade
    if score >= 95:
        grade = "A+"
    elif score >= 90:
        grade = "A"
    elif score >= 85:
        grade = "B+"
    elif score >= 80:
        grade = "B"
    elif score >= 70:
        grade = "C"
    elif score >= 60:
        grade = "D"
    else:
        grade = "F"

    print(f"  Grade: {grade}")
    print()
    print("=" * 72)

    return totals["fail"] == 0


def main():
    parser = argparse.ArgumentParser(
        description="NetSurf Web Standards Compliance Benchmark"
    )
    parser.add_argument(
        "-m", "--monkey",
        help="Path to nsmonkey binary",
        default=None,
    )
    parser.add_argument(
        "-t", "--test",
        help="Path to benchmark YAML",
        default=os.path.join(
            os.path.dirname(__file__),
            "monkey-tests",
            "web-standards-benchmark.yaml",
        ),
    )
    parser.add_argument(
        "--json",
        help="Output results as JSON to file",
        default=None,
    )
    args = parser.parse_args()

    monkey = args.monkey or find_monkey()
    if not monkey:
        print("ERROR: Cannot find nsmonkey binary. Use -m to specify path.")
        sys.exit(1)

    if not os.path.isfile(args.test):
        print(f"ERROR: Benchmark file not found: {args.test}")
        sys.exit(1)

    print(f"Running benchmark with {monkey}...")
    try:
        output, elapsed = run_benchmark(monkey, args.test)
    except subprocess.TimeoutExpired:
        print("ERROR: Benchmark timed out after 120s")
        sys.exit(1)
    except Exception as e:
        print(f"ERROR: {e}")
        sys.exit(1)

    parsed = parse_results(output)
    all_pass = print_report(parsed, elapsed)

    if args.json:
        with open(args.json, "w") as f:
            json.dump(
                {
                    "elapsed_s": round(elapsed, 6),
                    "score": parsed["summary"]["score"]
                    if parsed["summary"]
                    else round(
                        parsed["totals"]["pass"]
                        * 1000
                        / max(1, parsed["totals"]["total"])
                    )
                    / 10,
                    "totals": parsed["totals"],
                    "categories": parsed["categories"],
                    "failures": [
                        r for r in parsed["results"] if r["status"] == "FAIL"
                    ],
                    "skipped": [
                        r for r in parsed["results"] if r["status"] == "SKIP"
                    ],
                },
                f,
                indent=2,
            )
        print(f"JSON results written to {args.json}")

    sys.exit(0 if all_pass else 1)


if __name__ == "__main__":
    main()
