#!/usr/bin/env python3
"""Run a deterministic list of monkey tests and emit a compact JSON summary."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path


def load_suite(path: Path) -> list[str]:
    tests: list[str] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        tests.append(stripped)
    return tests


def run_test(monkey: str, test_path: str) -> tuple[bool, float, str]:
    cmd = [sys.executable, "test/monkey_driver.py", "-m", monkey, "-t", test_path]
    start = time.monotonic()
    proc = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        timeout=180,
    )
    elapsed = time.monotonic() - start
    output = proc.stdout + proc.stderr
    return proc.returncode == 0, elapsed, output


def main() -> int:
    parser = argparse.ArgumentParser(description="Run a profile monkey suite")
    parser.add_argument("--monkey", required=True)
    parser.add_argument("--suite", required=True)
    parser.add_argument("--json", required=True)
    args = parser.parse_args()

    suite_path = Path(args.suite)
    tests = load_suite(suite_path)
    results = []
    total_start = time.monotonic()

    for test_path in tests:
        ok, elapsed, output = run_test(args.monkey, test_path)
        status = "PASS" if ok else "FAIL"
        print(f"{status}: {test_path} ({elapsed:.3f}s)")
        if not ok and output:
            print(output, file=sys.stderr, end="")
        results.append(
            {
                "test": test_path,
                "status": status,
                "elapsed_s": round(elapsed, 6),
            }
        )

    total_elapsed = time.monotonic() - total_start
    pass_count = sum(1 for result in results if result["status"] == "PASS")
    fail_count = len(results) - pass_count
    payload = {
        "suite": str(suite_path),
        "elapsed_s": round(total_elapsed, 6),
        "totals": {
            "pass": pass_count,
            "fail": fail_count,
            "total": len(results),
        },
        "results": results,
    }
    Path(args.json).write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    return 0 if fail_count == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
