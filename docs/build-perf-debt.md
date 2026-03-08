# Build And Performance Debt

This document tracks the repo-level debt addressed by the centralized
repo-root build, verification, and profiling targets.

## Status board

### Completed

- [x] Added repo-root `make doctor` so developers can check missing
  tools, workspace state, and profiler support without mutating the
  checkout.
- [x] Added repo-root `make bootstrap` so workspace creation, tool
  installation, and library installation follow the same path locally
  and in CI.
- [x] Standardized repo-root native build entrypoints around `make
  build-gtk`, `make build-monkey`, `make build-framebuffer`, and `make
  verify-native`.
- [x] Added tracked monkey benchmark and Valgrind baseline summaries so
  runtime changes can be compared against committed expectations.
- [x] Added terminal-only heap profiling through `heaptrack
  --record-only` plus `heaptrack_print`.
- [x] Reduced the standing `cppcheck` backlog enough for
  `make static-analysis` to return to the default native verification
  gate.
- [x] Updated the Arch Linux build guide so it leads with the repo-root
  workflow instead of the old implicit workspace path.
- [x] Routed the native Linux GitHub Actions job through
  `make verify-native` while keeping the dedicated Valgrind artifact
  capture step.
- [x] Added JS-engine-aware repo-root monkey builds so switching between
  standard and enhanced JavaScript modes forces a clean rebuild instead
  of silently reusing stale objects.
- [x] Added repo-root `make benchmark-monkey-enhanced` so standard and
  enhanced monkey benchmarks can be compared with tracked correctness
  and elapsed-time summaries.
- [x] Added repeated-run measurement targets for warmed monkey engine
  comparison and incremental bootstrap timing.
- [x] Added tracked `core4m` and `script16m` monkey suite baselines plus
  host-side telemetry guardrails for the new low-spec profiles.
- [x] Added local lowest-common-i386 readiness and build-attempt targets
  plus build-flavour isolation so 32-bit experiments do not contaminate
  subsequent native builds.
- [x] Rebuilt the low-spec workspace dependency set as
  `inst-i386-pc-linux-gnu` and added a reproducible repo-root target for
  that rebuild.
- [x] Slimmed the low-spec `libnsfb.pc` so the shipping `core4m`
  profile no longer pulls XCB, VNC, Wayland, or SDL helper surfaces.
- [x] Fixed content-factory duplicate handler teardown and null
  fragment-ID unref paths that the low-spec monkey suites exposed
  during profile bring-up.
- [x] Kept `script16m` shipping defaults conservative while adding a
  suite-only choices overlay for scripted acceptance coverage.

### In progress

- [ ] Reduce the remaining enhanced-engine overhead now that the
  repeated repo-root harness measures enhanced at about 1.20x the
  standard mean instead of the older 2.7x note.
- [ ] Trim the incremental `bootstrap-libs` no-op cost, which is now the
  slowest measured repo-root bootstrap step on an existing workspace.
- [ ] Move from local lowest-common-i386 build evidence to full target
  image acceptance checks for the `386SX/4MB` and `16MB` profile
  budgets.
- [ ] Split the low-spec transport stack away from the current
  curl/OpenSSL path so the framebuffer shipping profile is not blocked
  on host-sized network dependencies.

### Next optimization loop

- [ ] Profile fetch, cache, and layout paths only after monkey and
  bootstrap measurements identify them as meaningful hotspots.

## Debt register

### Bootstrap and dependency debt

- A plain repo-root `make` used to fail immediately when
  `Makefile.config` enabled workspace libraries such as `libnspsl` but
  the NetSurf workspace had not been created yet.
- The old workflow depended on manually sourcing `docs/env.sh` and
  implicitly using `~/dev-netsurf/workspace`, which made local and CI
  behavior easy to drift apart.
- Multiple workflows duplicated `ns-clone`, `ns-make-tools`, and
  `ns-make-libs` bootstrapping logic instead of sharing a single
  repo-root interface.

### Profiling workflow debt

- `monkey` already gives a good benchmark harness, but profiling
  invocation details were tribal knowledge rather than stable commands.
- `perf` is not reliable on all hosts. Unsupported environments should
  emit a skip artifact, not a false failure.
- `heaptrack` can auto-open a GUI if invoked incorrectly. Repo targets
  must remain terminal-only by using `--record-only` plus
  `heaptrack_print`.
- The low-spec profiles now have host-side monkey acceptance baselines
  plus a local lowest-common-i386 readiness lane, but they still need
  emulator-backed budget checks before they can claim the real target
  envelopes.

### Low-spec product debt

- `core4m` and `script16m` now have tracked build and monkey-suite
  identities, but the transport path is still the host-oriented
  curl/OpenSSL backend rather than a dedicated embedded stack.
- `script16m` intentionally keeps JavaScript disabled in its shipping
  defaults. The committed scripted suite uses an explicit choices
  overlay so compliance coverage does not silently raise the runtime
  floor.
- The current monkey telemetry is a host-side regression guardrail, not
  proof that the profiles fit the final `386SX/4MB` and `16MB` target
  machines.
- The new `measure-i386-denominator` and `build-i386-denominator`
  targets make ABI drift visible on developer machines. The repo-root
  `rebuild-i386-libs` target now installs the low-spec dependency set
  into `inst-i386-pc-linux-gnu`, and the local denominator build is
  green on this host.
- The current host GCC multilib still lacks the soft-float helper
  symbols needed for a full no-FPU browser link, so the repo-root
  denominator build uses `clang` plus compiler-rt for the final browser
  link on this machine while keeping the rebuilt workspace libraries in
  the `ELF32/i386` prefix.

### Measurement and regression debt

- The repo had WPT baseline tracking, but not an equivalent tracked
  baseline for the monkey standards benchmark or memcheck smoke runs.
- Runtime optimization work was easy to start without a committed
  baseline, which makes regressions harder to prove.

## Prioritization

1. Keep repo-root bootstrap deterministic.
2. Keep native build, test, benchmark, and analysis entrypoints
   reproducible.
3. Capture stable baseline summaries before changing runtime behavior.
4. Use measured hotspots, not TODO counts, to choose optimization work.

## Current measured candidates

- Incremental repo-root bootstrap timing on an existing workspace:
  `bootstrap-tools` mean 0.193s, `bootstrap-libs` mean 2.453s, and
  `bootstrap` mean 1.145s.
- Warmed repeated monkey benchmark timing:
  standard mean 0.076s, enhanced mean 0.092s, enhanced/standard ratio
  1.204x.
- `core4m` monkey profile suite:
  3/3 PASS in 0.156s, host-side peak RSS 16504 KiB.
- `script16m` monkey profile suite:
  5/5 PASS in 0.356s, host-side peak RSS 16504 KiB using the scripted
  suite choices overlay.
- Cache, fetch, and layout paths only after profiling shows them as
  meaningful contributors.
