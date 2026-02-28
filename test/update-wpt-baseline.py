#!/usr/bin/env python3
"""
update-wpt-baseline.py -- Update test/wpt-baseline.json from WPT results.

WHY: The baseline file must contain real pass counts so the regression
     check in CI actually catches regressions. Running this script after
     a WPT test run sets floor values from the current results.

Usage:
  python3 test/update-wpt-baseline.py [--results test/wpt-results.json]
                                       [--baseline test/wpt-baseline.json]
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Update WPT baseline from results JSON",
    )
    parser.add_argument(
        "--results", default="test/wpt-results.json",
        help="Path to WPT results JSON (default: test/wpt-results.json)",
    )
    parser.add_argument(
        "--baseline", default="test/wpt-baseline.json",
        help="Path to baseline JSON to update (default: test/wpt-baseline.json)",
    )
    args = parser.parse_args()

    results_path = Path(args.results)
    baseline_path = Path(args.baseline)

    if not results_path.exists():
        print(f"ERROR: {results_path} not found. Run WPT tests first.", file=sys.stderr)
        return 1

    results = json.loads(results_path.read_text(encoding="utf-8"))
    suites: dict = {}

    for suite_name, suite_data in results.get("suites", {}).items():
        files_pass = 0
        assert_pass = 0
        assert_fail = 0

        for test in suite_data.get("tests", []):
            status = test.get("status", "ERROR")
            if status == "PASS":
                files_pass += 1
            assert_pass += test.get("assert_pass", 0)
            assert_fail += test.get("assert_fail", 0)

        suites[suite_name] = {
            "files_pass_min": files_pass,
            "assert_pass_min": assert_pass,
            "assert_fail_max": assert_fail,
        }

    baseline = {
        "note": "WPT regression floor values. Updated by update-wpt-baseline.py.",
        "suites": suites,
    }

    baseline_path.write_text(json.dumps(baseline, indent=2) + "\n", encoding="utf-8")
    print(f"Updated {baseline_path} with {len(suites)} suites:")
    for name, vals in suites.items():
        print(f"  {name}: pass>={vals['files_pass_min']} "
              f"assert_pass>={vals['assert_pass_min']} "
              f"assert_fail<={vals['assert_fail_max']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
