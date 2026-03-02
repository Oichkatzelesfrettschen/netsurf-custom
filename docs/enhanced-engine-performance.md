# Enhanced JS Engine Performance

Baseline measurements comparing the standard (Duktape 2.7.0, ES5.1) engine
with the enhanced (QuickJS-NG, ES2023) engine.

## Environment

- CPU: AMD Ryzen (Arch Linux 6.19.3-2-cachyos, GCC 15.2.1)
- Date: 2026-03-02
- Benchmark: web-standards-benchmark.yaml (421 tests, 25 categories)
- Both engines: 421/421 PASS (100%)

## Benchmark Execution Time (5 runs each)

| Engine    | Run 1 | Run 2 | Run 3 | Run 4 | Run 5 | Avg   |
|-----------|-------|-------|-------|-------|-------|-------|
| Standard  | 77ms  | 76ms  | 76ms  | 77ms  | 75ms  | 76ms  |
| Enhanced  | 223ms | 204ms | 194ms | 189ms | 201ms | 202ms |

Enhanced is ~2.7x slower than standard.

## Peak RSS (Maximum Resident Set Size)

| Engine   | RSS (KB) |
|----------|----------|
| Standard | 18,092   |
| Enhanced | 19,320   |

Enhanced uses ~7% more memory.

## Analysis

The enhanced engine is slower primarily due to the duk_compat shim layer:

1. **Virtual stack overhead**: Every duk_push/duk_pop translates to
   JS_DupValue/JS_FreeValue plus array management on the emulated stack.
2. **Trampoline dispatch**: Each C function call (getter, setter, method)
   goes through `duk_cfunc_trampoline` which saves/restores frame state.
3. **Double bookkeeping**: Values exist both as QuickJS JSValues and as
   entries in the emulated stack array, doubling reference counting work.

Memory overhead is minimal because both engines share the same DOM
(libdom) and the JS heap difference is small.

## Improvement opportunities

- Hot-path optimization in the compat shim (inline more functions)
- Direct QuickJS bindings for frequently-called methods (bypassing shim)
- Reducing JS_DupValue/JS_FreeValue in the trampoline (move semantics)

## Monkey Test Suite Results

| Engine   | Pass | Fail | Total |
|----------|------|------|-------|
| Standard | 38   | 3    | 41    |
| Enhanced | 40   | 1    | 41    |

Standard mode pre-existing failures: inserted-script (timeout),
quit-mid-fetch (timeout), resource-scheme (assertion).

Enhanced mode failure: diagnostic.yaml (childNodes returns undefined
in enhanced mode -- pre-existing binding limitation).

Enhanced mode passes 2 additional tests vs standard mode because
the QuickJS runtime is more tolerant of certain edge cases.
