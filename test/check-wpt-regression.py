#!/usr/bin/env python3
# WHY: Compares current WPT results against the committed baseline to detect
#      regressions.  CI calls this after running run-wpt.py and fails the job
#      if any metric drops below the baseline floor.
#
# WHAT: Reads test/wpt-baseline.json (committed floor values) and the per-run
#       results JSON written by run-wpt.py, then exits non-zero if any suite
#       has fewer passes or more failures than the baseline allows.
#
# HOW:
#   python3 test/check-wpt-regression.py \
#       --baseline test/wpt-baseline.json \
#       --results test/wpt-results-css-selectors.json \
#       --suite css-selectors

import argparse
import json
import sys


def main():
    parser = argparse.ArgumentParser(
        description='Check WPT results for regressions against baseline.')
    parser.add_argument('--baseline', required=True,
                        help='Path to wpt-baseline.json')
    parser.add_argument('--results', required=True,
                        help='Path to wpt-results-<suite>.json from run-wpt.py')
    parser.add_argument('--suite', required=True,
                        help='Suite name (must be present in baseline)')
    args = parser.parse_args()

    with open(args.baseline) as f:
        baseline = json.load(f)

    with open(args.results) as f:
        results = json.load(f)

    suite = args.suite
    suites = baseline.get('suites', {})
    if suite not in suites:
        print(f'INFO: Suite "{suite}" not tracked in baseline, skipping check.')
        sys.exit(0)

    floor = suites[suite]
    fp_min  = floor.get('files_pass_min', 0)
    ap_min  = floor.get('assert_pass_min', 0)
    af_max  = floor.get('assert_fail_max', 0)

    fp_actual  = results.get('files_pass', 0)
    ap_actual  = results.get('assert_pass', 0)
    af_actual  = results.get('assert_fail', 0)

    regressions = []
    if fp_actual < fp_min:
        regressions.append(
            f'files_pass regression: {fp_actual} < baseline {fp_min}')
    if ap_actual < ap_min:
        regressions.append(
            f'assert_pass regression: {ap_actual} < baseline {ap_min}')
    if af_actual > af_max:
        regressions.append(
            f'assert_fail regression: {af_actual} > baseline ceiling {af_max}')

    if regressions:
        print(f'REGRESSION DETECTED in suite "{suite}":')
        for r in regressions:
            print(f'  {r}')
        sys.exit(1)
    else:
        print(f'OK: suite "{suite}" meets baseline '
              f'(files_pass={fp_actual}, assert_pass={ap_actual}, '
              f'assert_fail={af_actual})')
        sys.exit(0)


if __name__ == '__main__':
    main()
