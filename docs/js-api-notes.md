# JS API Implementation Notes

Per-feature notes on known limitations, deviations from the spec, and
implementation status for JavaScript APIs added in this fork.

---

## innerHTML (Element)

**Getter:** Fully implemented in `content/handlers/javascript/duktape/dukky.c`
via `dukky_push_node_innerhtml()`.  Handles element/text/comment nodes, void
elements (no closing tag), and escapes attribute values containing `"`, `<`, `>`,
and `&`.

**Setter:** Fully implemented in `content/handlers/javascript/duktape/Element.bnd`
using libhubbub's fragment parser.  Setting `elem.innerHTML = '<b>text</b>'`
replaces the element's children with the parsed subtree.

**Known limitation:** The setter uses Hubbub's fragment-parser which wraps the
content in an implicit HTML/BODY context.  This is correct per the HTML5 parsing
spec but may differ from other browsers for edge cases like `<tr>` fragments set
on a `<table>` element without a `<tbody>`.

---

## Promise

**Status:** Polyfill in `content/handlers/javascript/duktape/polyfill.js`.
Supports `new Promise`, `.then()`, `.catch()`, `.finally()`,
`Promise.resolve`, `Promise.reject`, `Promise.all`, `Promise.race`,
`Promise.allSettled`, `Promise.any`.

**KNOWN DEVIATION -- microtask scheduling:** This polyfill schedules `.then()`
callbacks via `setTimeout(0)` (a macrotask) rather than the spec-mandated
microtask queue.  Duktape 2.7 does not expose a native microtask queue hook.

Consequence: code that relies on the ordering of microtasks vs synchronous code
will behave incorrectly.  For example:
```js
let x = 0;
Promise.resolve().then(() => { x = 1; });
// x is still 0 here in this polyfill (fires in next event loop turn)
// x would be 0 here in a compliant engine too (microtask deferred)
// but the relative order vs other macrotasks will differ
```

This is a fundamental constraint of Duktape 2.7 and cannot be fixed without
engine changes.

---

## History API (history.pushState / replaceState / go / back / forward / length)

**Status:** Fully implemented in `content/handlers/javascript/duktape/History.bnd`
and `desktop/browser_history.c`.

**Same-origin enforcement:** `pushState` and `replaceState` compare the new URL's
scheme, host, and port against the current page URL.  A cross-origin URL throws a
`RangeError`.  This matches the HTML5 spec security requirement.

**State argument is ignored:** The `state` object passed to `pushState`/
`replaceState` is accepted but not stored or returned by `popstate` events.
`popstate` events are not yet fired on navigation (no `popstate` event dispatch
is implemented).  The state argument will be no-op until `popstate` is wired.

**go(delta):** Implemented as a loop over `browser_window_history_back/forward`.
Large deltas (e.g. go(100) with only 2 entries) clamp silently at the list ends
without crashing or throwing.

---

## Map / Set

**Status:** Polyfill in `content/handlers/javascript/duktape/polyfill.js`.
Implements the ES6 `Map` and `Set` interfaces including `forEach`, `keys()`,
`values()`, `entries()`, and `Symbol.iterator`.

**Performance:** Both Map and Set use a Symbol-keyed hash object (`MAP_HASH` /
`SET_HASH`) for O(1) average lookup on string and finite-number keys.  Object
keys and `NaN` fall back to O(n) linear scan.  Lookup on 10,000-entry collections
with string keys completes in well under 1ms.

**Internal property visibility:** Internal slots are Symbol-keyed when Duktape's
`Symbol` builtin is available (it is, in Duktape 2.7 with `DUK_USE_SYMBOL_BUILTIN`).
Symbol properties are non-enumerable so `Object.keys(new Map())` returns `[]`.
When `Symbol` is absent (unusual), string fallback keys `_keys`/`_vals`/`_hash`
are used and are visible to enumeration.

---

## WeakMap / WeakSet

**Status:** Polyfill in `content/handlers/javascript/duktape/polyfill.js`.
Implements the ES6 `WeakMap` and `WeakSet` interface using a unique tag property
attached to each key object.

**GC semantics not replicated:** This polyfill holds strong references.  Objects
used as keys will NOT be garbage collected while the WeakMap/WeakSet exists.
This is correct for correctness guarantees but means memory will not be freed
in the same way as a native WeakMap.  Long-lived pages that use WeakMap/WeakSet
heavily may accumulate more memory than expected.

---

## Same-origin checks (nsurl_same_origin)

**Status:** Implemented in `utils/nsurl/nsurl.c` as `nsurl_same_origin()`.
Used by `History.bnd` pushState/replaceState to enforce the HTML5 security
requirement that navigation must stay same-origin.

**KNOWN LIMITATION -- opaque origins:** Per the WHATWG URL standard, `data:`
and `blob:` URLs have opaque origins; every `data:` URL is a distinct origin
from every other, regardless of content.  `nsurl_same_origin()` uses
`nsurl_compare(NSURL_SCHEME | NSURL_HOST | NSURL_PORT)` which does not
implement opaque-origin semantics.  Two identical `data:` URLs are reported as
same-origin.

This matches upstream NetSurf behaviour.  For a desktop browser without
cross-origin iframes this is acceptable.  Fixing it requires tracking an
origin tuple (scheme, host, port, opaque-flag) inside `nsurl`, which is an
upstream change beyond the scope of this fork.

---

## innerHTML Serializer Performance

**Implementation:** `dukky_push_node_innerhtml()` in
`content/handlers/javascript/duktape/dukky.c`.

**Complexity:** O(N * M) where N is the number of nodes and M is the maximum
attribute count per element.  Each element node generates 5 + 2*M Duktape
stack pushes followed by a `duk_concat` call.  All pieces are buffered on the
Duktape stack until the final concat; there is no streaming output.

**Future optimization:** The natural next step is to cache the serialized
string and invalidate it via a MutationObserver when the subtree changes.
This would make repeated `elem.innerHTML` reads free.  MutationObserver is
not yet implemented (XL effort, requires libdom event hooks).

---

## :lang() Pseudo-class Performance

**Implementation:** `node_is_lang()` in
`content/handlers/css/select.c`.

**Cost:** On every CSS selector match involving `:lang()`, the function walks
the ancestor chain from the matched element to the root looking for a `lang`
attribute.  This is O(depth) per match.  On pages with many `:lang()` rules
and deep DOM trees this can be visible.

**Memoization:** The natural fix is to cache the effective language on the
select context (`nscss_select_ctx`).  This requires storing a `lang_cache`
entry per node in the context and invalidating it between sibling traversals.
The libcss select callback API does not provide a hook for cache invalidation,
so any memoization must be done conservatively (cache per full selector pass,
not per node).  This is deferred until there is a measured performance
regression to justify the complexity.

---

## performance.now() / timeOrigin

**Status:** Implemented in `content/handlers/javascript/duktape/Performance.bnd`.
Uses `nsu_getmonotonic_ms()` from libnsutils for monotonic millisecond timing.

**KNOWN DEVIATION -- epoch:** `timeOrigin` returns the monotonic clock value at
Performance object creation time (effectively process-relative), not the
navigation-start timestamp as the High Resolution Time spec requires.  This
means `performance.timeOrigin + performance.now()` does not equal `Date.now()`.
For relative timing (the primary use case), this is correct.

---

## Element.tagName / localName

**Status:** Implemented in `content/handlers/javascript/duktape/Element.bnd`.

`tagName` returns `dom_node_get_node_name()` which is uppercase for HTML
elements (e.g. "DIV").  `localName` uses `dom_node_get_local_name()` with a
fallback to `dom_node_get_node_name()` for HTML elements that may not have
namespace-aware local names.

---

## HTMLElement.hidden / tabIndex

**Status:** Implemented in `content/handlers/javascript/duktape/HTMLElement.bnd`.

`hidden` is a boolean attribute: getter uses `dom_element_has_attribute()`,
setter uses `dom_element_set_attribute()` / `dom_element_remove_attribute()`.

`tabIndex` is an integer attribute defaulting to -1.  Getter uses
`dom_element_get_attribute()` + `strtol()`, setter uses `snprintf()` +
`dom_element_set_attribute()`.

---

## HTMLInputElement Properties

**Status:** Implemented in `content/handlers/javascript/duktape/HTMLInputElement.bnd`.

All properties use the `dom_html_input_element_get_*` / `set_*` APIs from libdom:
- String: accept, align, alt, defaultValue, name, src, useMap, value
- Boolean: checked, defaultChecked, disabled, readOnly
- Integer: maxLength, size
- Read-only: type (defaults to "text" if NULL)

**Deferred:** `valueAsNumber`, `valueHigh`, `valueLow` remain as stubs.

---

## HTMLButtonElement / HTMLSelectElement Properties

**Status:** Implemented in their respective `.bnd` files.

HTMLButtonElement: disabled (bool), name (string), value (string).
HTMLSelectElement: disabled (bool), multiple (bool), name (string),
type (read-only, defaults to "select-one"), value (string).
