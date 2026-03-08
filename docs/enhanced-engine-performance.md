# Enhanced JS Engine Performance

Repo-root repeated measurements comparing the standard (Duktape 2.7.0,
ES5.1) engine with the enhanced (QuickJS-NG, ES2023) engine.

## Environment

- CPU: AMD Ryzen (Arch Linux, x86_64-pc-linux-gnu)
- Date: 2026-03-07
- Command: `make measure-monkey-engines`
- Harness: 1 warmup run plus 5 measured runs per engine
- Benchmark: `web-standards-benchmark.yaml` (421 tests, 25 categories)
- Artifact: `build/profiles/monkey-engine-compare.json`
- Both engines: 421/421 PASS (100%)

## Benchmark Execution Time

| Engine    | Warmup | Run 1 | Run 2 | Run 3 | Run 4 | Run 5 | Mean |
|-----------|--------|-------|-------|-------|-------|-------|------|
| Standard  | 78ms   | 74ms  | 75ms  | 74ms  | 79ms  | 79ms  | 76ms |
| Enhanced  | 88ms   | 88ms  | 92ms  | 94ms  | 92ms  | 93ms  | 92ms |

Enhanced is about 1.20x slower than standard on the warmed repo-root
harness, with a mean delta of about 16ms.

## Interpretation

The current repo-root repeated-run harness shows a much smaller gap than
the older March 2026 note did. The earlier 2.7x slowdown is not
reproduced by the current centralized measurement path.

The remaining enhanced-engine overhead is still consistent with the
expected compat-layer costs:

1. Virtual stack bookkeeping in the `duk_compat` shim.
2. Trampoline dispatch for DOM getters, setters, and methods.
3. Extra `JS_DupValue` and `JS_FreeValue` churn across the compatibility
   boundary.

## Next Optimization Targets

- Hot-path optimization in the compat shim.
- Direct QuickJS bindings for frequently-called DOM operations.
- Fewer temporary value duplications in the trampoline path.
