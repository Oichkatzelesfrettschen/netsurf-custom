# Performance Baselines

The repo-root orchestration layer writes actual benchmark and profiler
results to `build/profiles/` and checks them against the tracked
baseline policies in `test/perf-baselines/`.

## Native commands

```sh
make benchmark-monkey
make benchmark-monkey-enhanced
make benchmark-profile PROFILE=core4m
make benchmark-profile PROFILE=script16m
make profile-profile PROFILE=core4m
make profile-profile PROFILE=script16m
make measure-monkey-engines
make measure-bootstrap-costs
make measure-i386-denominator
make rebuild-i386-libs
make build-i386-denominator
make verify-i386-denominator
make profile-valgrind-monkey
make profile-heaptrack-monkey
make profile-perf-monkey
```

## Baseline policy

- `benchmark-monkey` must keep the web-standards benchmark at the
  committed score floor with zero fails and zero skips.
- `benchmark-monkey-enhanced` must keep the enhanced-engine benchmark at
  the committed correctness floor and within its tracked elapsed-time
  ceiling.
- `measure-monkey-engines` writes a repeated-run comparison artifact for
  standard and enhanced monkey builds.
- `measure-bootstrap-costs` writes incremental timing data for the
  repo-root bootstrap commands on an existing workspace.
- `measure-i386-denominator` writes a local readiness artifact that
  proves whether this host can compile and execute a lowest-common-i386
  probe and whether the current workspace libraries match that ABI.
- `rebuild-i386-libs` rebuilds the low-spec workspace dependency set
  into `inst-i386-pc-linux-gnu` and regenerates a slim `libnsfb.pc`
  without desktop-surface requirements.
- `build-i386-denominator` attempts a `PROFILE=core4m` build using
  lowest-common-i386 `-m32 -march=i386` flags and records the failure
  or success summary. On this host the final soft-float browser link
  uses `clang` plus compiler-rt because the local GCC multilib does not
  ship the required soft-float helper symbols.
- `benchmark-profile` checks committed low-spec suite baselines for
  `core4m` and `script16m`.
- `profile-profile` records host-side elapsed-time and RSS guardrails
  for the low-spec suite runs.
- `profile-valgrind-monkey` must keep the start-stop smoke test free of
  reported memcheck errors.
- `profile-heaptrack-monkey` currently records informational reference
  metrics only.
- `profile-perf-monkey` is opportunistic. Unsupported hosts should
  produce a skip artifact rather than fail.

## Artifacts

- Raw or tool-native artifacts go to `build/profiles/`.
- Summaries go to `build/profiles/*.json`.
- Tracked baseline policies live in `test/perf-baselines/`.
- Informational measurement artifacts currently include
  `monkey-engine-compare.json`, `bootstrap-costs.json`,
  `i386-denominator.json`, `i386-workspace-rebuild.json`, and
  `i386-build-attempt.json`.
- `script16m` keeps JavaScript disabled in its shipping profile
  defaults; the scripted acceptance suite uses
  `profiles/Choices.script16m.suite` as an explicit opt-in overlay.

## Headless heaptrack policy

`heaptrack` must remain terminal-only in this repository.

- Use `heaptrack --record-only`.
- Summarize with `heaptrack_print`.
- Do not use `heaptrack -a`.
- Do not launch `heaptrack_gui`.
