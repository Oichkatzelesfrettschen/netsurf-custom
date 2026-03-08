NetSurf Performance Baselines
=============================

This directory contains tracked baseline policies for repo-root
benchmarking and profiling targets.

- `*-baseline.json` files describe the accepted floor or ceiling values
  for a scenario.
- Actual run outputs are written to `build/profiles/` and compared
  against these baselines by `test/perf-baseline.py`.
- `heaptrack` baselines are informational only for now. They document
  reference metrics but do not fail the build.

Update workflow
---------------

1. Run the relevant repo-root target, for example
   `make benchmark-monkey`, `make benchmark-monkey-enhanced`, or
   `make profile-valgrind-monkey`.
2. Inspect the generated summary JSON in `build/profiles/`.
3. If the change is intentional, copy the metric thresholds or reference
   values into the matching `*-baseline.json` file.

Profiler notes
--------------

- `heaptrack` must stay terminal-only. Use `--record-only` together with
  `heaptrack_print`; do not use `heaptrack -a` or `heaptrack_gui`.
- `perf` is opportunistic. Unsupported hosts should report a skip status
  instead of failing the workflow.
