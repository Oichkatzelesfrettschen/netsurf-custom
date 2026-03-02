# Speedometer 3.1 Feasibility Assessment

## Summary

Speedometer 3.1 is not feasible for NetSurf in its current state. The
benchmark requires extensive Web Platform features that are far beyond
NetSurf's scope as a lightweight browser.

## Required Features Not Present in NetSurf

### Critical (blocking)

1. **Shadow DOM** (v1): Used by all modern frameworks (React, Vue, Svelte,
   Angular). Requires `Element.attachShadow()`, shadow root encapsulation,
   slotting, and style scoping.

2. **Custom Elements** (v1): `customElements.define()`, lifecycle callbacks
   (`connectedCallback`, `disconnectedCallback`, `attributeChangedCallback`).
   Required by Lit, Angular, and Web Components benchmarks.

3. **MutationObserver**: Required by virtually all frameworks for DOM
   diffing and reactive updates. Not available in NetSurf.

4. **Full CSS Object Model**: `getComputedStyle()` with complete property
   resolution, CSS Custom Properties (`--var`), `CSSStyleSheet` constructor,
   adopted style sheets. NetSurf has basic getComputedStyle but lacks
   custom properties and dynamic sheet manipulation.

5. **Template Literals and Tagged Templates**: NetSurf's standard engine
   (Duktape) only supports ES5.1. The enhanced engine (QuickJS-NG)
   supports ES2023 but the compat shim adds overhead.

6. **ES Modules**: `import`/`export` syntax. Required by modern framework
   builds. Not supported by either engine in NetSurf.

### Major (required for most subtests)

7. **Proxy/Reflect**: Used by Vue 3's reactivity system. Duktape lacks
   Proxy; QuickJS-NG supports it.

8. **async/await + microtask integration**: QuickJS-NG supports this
   natively; Duktape does not. The enhanced engine now flushes microtasks
   (Phase 6.2) but framework-level async patterns remain untested.

9. **requestAnimationFrame scheduling**: NetSurf has a basic rAF shim
   but lacks proper frame scheduling tied to rendering.

10. **IntersectionObserver / ResizeObserver**: Used by framework benchmarks
    for visibility and layout measurement.

11. **Canvas 2D API**: Used by some Speedometer subtests for rendering.

12. **Fetch API**: `fetch()` with Promises, Headers, Response, Request.
    NetSurf uses XMLHttpRequest internally but does not expose `fetch()`.

### Minor (needed for full compatibility)

13. **Web Animations API**: `element.animate()`.
14. **Clipboard API**: `navigator.clipboard`.
15. **Structured Clone**: `structuredClone()`.
16. **AbortController integration**: With fetch, not just standalone.

## Effort Estimate

Implementing the critical features (Shadow DOM, Custom Elements,
MutationObserver, CSS OM) would require 6-12 months of focused work
on libdom and the rendering engine, far exceeding the scope of the
enhanced JS engine project.

## Recommendation

Focus on achieving and maintaining 100% on the web-standards-benchmark
(currently 421/421). Expand the benchmark with new test categories as
APIs are added:

1. **Proxy/Reflect** tests (enhanced engine already supports these)
2. **async/await** tests (microtask flush now works)
3. **ES Modules** (if added to enhanced engine)
4. **Fetch API** (when implemented)

This provides measurable, incremental progress without requiring the
massive infrastructure changes that Speedometer 3.1 demands.
