#!/usr/bin/env python3
# WHY: Runs a subset of the W3C Web Platform Tests (WPT) through nsmonkey and
#      reports PASS/FAIL per test file.  Used to establish a baseline pass rate
#      and detect regressions as new features are implemented.
#
# WHAT: Iterates over WPT HTML files in a given directory, drives each through
#       nsmonkey via a generated YAML plan, parses testharness.js console output
#       for PASS/FAIL lines, and writes results to JSON.
#
# HOW:
#   # First time (fetches WPT subsets used here):
#   make -C test wpt-fetch
#
#   # Run against a built nsmonkey:
#   python3 test/run-wpt.py --monkey ./nsmonkey --suite css-selectors
#   python3 test/run-wpt.py --monkey ./nsmonkey --suite all
#   python3 test/run-wpt.py --monkey ./nsmonkey --suite css-selectors --out test/wpt-results.json

import argparse
import glob
import json
import os
import re
import subprocess
import sys
import tempfile
import time

# Timeout per individual WPT HTML test (seconds).
TEST_TIMEOUT = 15

# testharness.js console output patterns.
# A PASS line looks like: "PASS test name"
# A FAIL line looks like: "FAIL test name reason"
# Completion: "Harness: the test ran to completion"
RE_PASS    = re.compile(r'^(?:LOG )?PASS\s', re.IGNORECASE)
RE_FAIL    = re.compile(r'^(?:LOG )?FAIL\s', re.IGNORECASE)
RE_DONE    = re.compile(r'(?:Harness: the test ran to completion|'
                        r'Harness Error\.)', re.IGNORECASE)
RE_TIMEOUT = re.compile(r'Timeout', re.IGNORECASE)


def find_wpt_html(suite_dir):
    """Return sorted list of .html test files under suite_dir."""
    files = glob.glob(os.path.join(suite_dir, '**', '*.html'), recursive=True)
    # Exclude reference files (named *-ref.html or in /reference/ dirs).
    files = [f for f in files
             if not re.search(r'[-/]ref(?:erence)?/', f)
             and not f.endswith('-ref.html')]
    return sorted(files)


def yaml_plan_for(html_path, timeout_ms):
    """Generate an inline YAML test plan string for a single WPT file."""
    file_url = 'file://' + os.path.abspath(html_path)
    # WHY: We use js-exec to read document.title after load; testharness.js
    #      writes results to the console.log which nsmonkey captures.  The
    #      wait-log step blocks until 'Harness:' appears or timeout fires.
    plan = {
        'title': 'wpt ' + os.path.basename(html_path),
        'group': 'wpt',
        'steps': [
            {'action': 'launch',
             'args': ['--enable_javascript=1']},
            {'action': 'window-new', 'tag': 'win1'},
            {'action': 'clear-log', 'window': 'win1'},
            {'action': 'navigate', 'window': 'win1', 'url': file_url},
            {'action': 'block',
             'conditions': [{'window': 'win1', 'status': 'complete'}]},
            {'action': 'wait-log',
             'window': 'win1',
             'substring': 'Harness',
             'timeout': timeout_ms},
            {'action': 'quit'},
        ],
    }
    lines = ['title: ' + plan['title'],
             'group: ' + plan['group'],
             'steps:']
    for step in plan['steps']:
        action = step['action']
        lines.append('- action: ' + action)
        if 'tag' in step:
            lines.append('  tag: ' + step['tag'])
        if 'window' in step:
            lines.append('  window: ' + step['window'])
        if 'url' in step:
            lines.append('  url: ' + step['url'])
        if 'substring' in step:
            lines.append('  substring: ' + step['substring'])
        if 'timeout' in step:
            lines.append('  timeout: ' + str(step['timeout']))
        if 'args' in step:
            lines.append('  args:')
            for arg in step['args']:
                lines.append('  - "' + arg + '"')
        if 'conditions' in step:
            lines.append('  conditions:')
            for cond in step['conditions']:
                lines.append('  - window: ' + cond['window'])
                lines.append('    status: ' + cond['status'])
    return '\n'.join(lines) + '\n'


def run_one(monkey_path, html_path, timeout):
    """Run a single WPT HTML file through nsmonkey. Returns (passes, fails, harness_ok)."""
    plan_yaml = yaml_plan_for(html_path, timeout * 1000)

    with tempfile.NamedTemporaryFile(mode='w', suffix='.yaml',
                                     delete=False) as f:
        f.write(plan_yaml)
        plan_path = f.name

    passes = 0
    fails = 0
    harness_ok = False
    timed_out = False

    try:
        result = subprocess.run(
            [sys.executable,
             os.path.join(os.path.dirname(__file__), 'monkey_driver.py'),
             monkey_path, plan_path],
            capture_output=True, text=True, timeout=timeout + 5)
        output = result.stdout + result.stderr
    except subprocess.TimeoutExpired:
        timed_out = True
        output = ''

    for line in output.splitlines():
        if RE_PASS.search(line):
            passes += 1
        elif RE_FAIL.search(line):
            fails += 1
        if RE_DONE.search(line):
            harness_ok = True

    os.unlink(plan_path)
    return passes, fails, harness_ok, timed_out


SUITES = {
    'css-selectors':  'test/wpt/css/selectors',
    'css-flexbox':    'test/wpt/css/css-flexbox',
    'html':           'test/wpt/html',
    'history':        'test/wpt/html/browsers/history',
}


def resolve_suite_dir(suite_arg, repo_root):
    """Return absolute path to WPT suite dir, or None if it does not exist."""
    if suite_arg == 'all':
        return None  # special: run all registered suites
    path = SUITES.get(suite_arg, suite_arg)
    if not os.path.isabs(path):
        path = os.path.join(repo_root, path)
    return path


def main():
    parser = argparse.ArgumentParser(
        description='Run WPT tests through nsmonkey and report results.')
    parser.add_argument('--monkey', default='./nsmonkey',
                        help='Path to nsmonkey binary (default: ./nsmonkey)')
    parser.add_argument('--suite', default='css-selectors',
                        help='Suite name or path.  Known names: ' +
                             ', '.join(list(SUITES) + ['all']))
    parser.add_argument('--out', default=None,
                        help='Write JSON results to this file')
    parser.add_argument('--timeout', type=int, default=TEST_TIMEOUT,
                        help='Per-test timeout in seconds (default: 15)')
    parser.add_argument('--max', type=int, default=0,
                        help='Stop after this many tests (0 = unlimited)')
    args = parser.parse_args()

    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    monkey = os.path.abspath(args.monkey)

    if not os.path.isfile(monkey):
        print('ERROR: nsmonkey not found at', monkey, file=sys.stderr)
        sys.exit(1)

    if args.suite == 'all':
        suite_dirs = [os.path.join(repo_root, d) for d in SUITES.values()]
    else:
        suite_dirs = [resolve_suite_dir(args.suite, repo_root)]

    html_files = []
    for d in suite_dirs:
        if d is None:
            continue
        if not os.path.isdir(d):
            print('WARNING: suite directory not found:', d, file=sys.stderr)
            continue
        html_files.extend(find_wpt_html(d))

    if not html_files:
        print('ERROR: no WPT HTML files found.  Run: make -C test wpt-fetch',
              file=sys.stderr)
        sys.exit(1)

    if args.max > 0:
        html_files = html_files[:args.max]

    print('Running', len(html_files), 'WPT test files...')

    results = []
    total_pass = 0
    total_fail = 0
    total_timeout = 0
    total_harness_err = 0

    for i, html_path in enumerate(html_files):
        rel = os.path.relpath(html_path, repo_root)
        t0 = time.monotonic()
        passes, fails, harness_ok, timed_out = run_one(
            monkey, html_path, args.timeout)
        elapsed = time.monotonic() - t0

        status = 'pass' if (passes > 0 and fails == 0 and harness_ok) else \
                 'timeout' if timed_out else \
                 'harness-error' if not harness_ok else \
                 'fail'

        total_pass     += passes
        total_fail     += fails
        total_timeout  += int(timed_out)
        total_harness_err += int(not harness_ok and not timed_out)

        results.append({
            'file':      rel,
            'status':    status,
            'passes':    passes,
            'fails':     fails,
            'harness_ok': harness_ok,
            'timed_out': timed_out,
            'elapsed_s': round(elapsed, 2),
        })

        symbol = {'pass': 'P', 'fail': 'F', 'timeout': 'T',
                  'harness-error': 'E'}.get(status, '?')
        print(f'  [{i+1:4d}/{len(html_files)}] {symbol} {rel}')

    file_pass  = sum(1 for r in results if r['status'] == 'pass')
    file_total = len(results)

    print()
    print(f'Files  : {file_pass}/{file_total} passed')
    print(f'Asserts: {total_pass} pass, {total_fail} fail')
    print(f'Timeout: {total_timeout}  Harness-err: {total_harness_err}')

    summary = {
        'timestamp':    time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
        'suite':        args.suite,
        'files_pass':   file_pass,
        'files_total':  file_total,
        'assert_pass':  total_pass,
        'assert_fail':  total_fail,
        'timeouts':     total_timeout,
        'harness_err':  total_harness_err,
        'tests':        results,
    }

    if args.out:
        out_path = args.out
    else:
        out_path = os.path.join(repo_root, 'test', 'wpt-results.json')

    with open(out_path, 'w') as f:
        json.dump(summary, f, indent=2)
    print('Results written to', out_path)

    # Exit non-zero only if there are assertion failures (not timeouts or
    # harness errors, which happen when WPT uses APIs we have not yet built).
    sys.exit(1 if total_fail > 0 else 0)


if __name__ == '__main__':
    main()
