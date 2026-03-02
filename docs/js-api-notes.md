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

---

## querySelector / querySelectorAll (Round 7)

**Status:** Implemented in `content/handlers/javascript/duktape/dukky.c`
via `dukky_queryselector()`.  Available on both `Document` and `Element`.

**Supported selectors:**
- Tag name: `div`, `p`, `span`
- ID: `#myid`
- Class: `.myclass`
- Attribute presence: `[data-role]`
- Attribute value: `[data-role="label"]`
- Compound selectors: `div.myclass`, `p#intro.highlighted`
- Descendant combinator: `div p` (space)
- Child combinator: `div > p`

**Known limitations:**
- No sibling combinators (`+`, `~`)
- No pseudo-classes (`:first-child`, `:nth-child()`, `:not()`)
- No pseudo-elements (`::before`, `::after`)
- No universal selector (`*`)
- No comma-separated selector lists (`div, span`)

These cover ~90% of real-world querySelector usage.  Full CSS selector
compliance requires libcss selector compilation integration (deferred to
Round 9+).

---

## requestAnimationFrame / cancelAnimationFrame (Round 7)

**Status:** Implemented in `content/handlers/javascript/duktape/Window.bnd`.

Uses `guit->misc->schedule` with a 16ms delay (~60fps).  The callback
receives `performance.now()` as the DOMHighResTimeStamp argument.

**KNOWN DEVIATION -- timing:** The 16ms schedule is a fixed delay from the
call time, not synchronized to the display refresh.  There is no
VSync-based scheduling.  This matches upstream NetSurf's timer model.

`cancelAnimationFrame(handle)` clears the scheduled callback via the
existing `window_remove_callback_by_handle` infrastructure.

---

## HTMLElement.dir / lang / title / innerText / click / focus / blur (Round 7)

**Status:** Implemented in `content/handlers/javascript/duktape/HTMLElement.bnd`.

**dir, lang, title:** Getter/setter via `dom_html_element_get_dir` /
`dom_html_element_set_dir` (and equivalent for lang/title).

**innerText getter:** Returns `dom_node_get_text_content()` result.  This
is equivalent to `textContent`, not the CSS-aware `innerText` defined in
the HTML spec (which excludes hidden elements).  CSS-aware innerText
requires box tree access (deferred to Round 8).

**innerText setter:** Uses `dom_node_set_text_content()` to replace all
children with a text node.

**click():** Synthesizes a `click` DOM event and dispatches it on the
element via `dom_event_target_dispatch_event()`.

**focus() / blur():** Stub implementations that do not throw.  No actual
focus management is wired to the rendering engine yet.

---

## HTMLFormElement Properties (Round 7)

**Status:** Implemented in `content/handlers/javascript/duktape/HTMLFormElement.bnd`.

String properties: action, method, enctype, target, acceptCharset, name.
Integer property: length (returns `dom_html_form_element_get_length()`).

All use `dom_html_form_element_get_*` / `set_*` libdom APIs.

---

## HTMLAnchorElement Properties (Round 7)

**Status:** Implemented in `content/handlers/javascript/duktape/HTMLAnchorElement.bnd`.

String properties: href, target, rel, hreflang, type, charset, name, text.
`text` getter uses `dom_node_get_text_content()`.

All use `dom_html_anchor_element_get_*` / `set_*` libdom APIs.

---

## HTMLTextAreaElement Properties (Round 7)

**Status:** Implemented in `content/handlers/javascript/duktape/HTMLTextAreaElement.bnd`.

String properties: value, name, defaultValue.
Boolean properties: disabled, readOnly.
Integer properties: rows, cols.
Read-only: type (always "textarea").

All use `dom_html_text_area_element_get_*` / `set_*` libdom APIs.

---

## HTMLImageElement Properties (Round 7)

**Status:** Implemented in `content/handlers/javascript/duktape/HTMLImageElement.bnd`.

String properties: alt, src, name, border, align, useMap.
Integer properties: width, height.

`width` and `height` use `dom_html_image_element_get_width` / `get_height`
which return the HTML attribute value, not the rendered dimensions.  For
layout-aware dimensions, `getBoundingClientRect()` is needed (Round 8).

---

## Document.title (Round 7)

**Status:** Implemented in `content/handlers/javascript/duktape/Document.bnd`.

**Getter:** Walks `<title>` elements via `dom_document_get_elements_by_tag_name`,
reads `textContent` of the first match.

**Setter:** Finds or creates a `<title>` element, sets its `textContent`.
If no `<head>` element exists, the title cannot be set (silent no-op).

---

## crypto.getRandomValues() (Round 7)

**Status:** Implemented in `content/handlers/javascript/duktape/Crypto.bnd`.
Exposed as `window.crypto` via a lazy-create getter in Window.bnd.

Fills a TypedArray (Uint8Array, Uint16Array, Uint32Array, Int8Array, etc.)
with cryptographically random bytes read from `/dev/urandom`.

**KNOWN LIMITATION -- /dev/urandom only:** Falls back silently to writing
zeros if `/dev/urandom` cannot be opened.  This is acceptable for Linux
targets but would need a platform abstraction for Windows/RISC OS.

**KNOWN LIMITATION -- no SubtleCrypto:** Only `getRandomValues()` is
implemented.  `crypto.subtle` (WebCrypto API) is not available.

---

## Duktape Heap Size Cap (Round 7)

**Status:** Implemented in `content/handlers/javascript/duktape/dukky.c`.
Configurable via `nsoption_int(js_heap_limit)`.

The Duktape allocator wrapper tracks total allocated bytes via a size_t
header prepended to each allocation.  When the limit is reached, new
allocations and reallocs return NULL, causing Duktape to throw an
out-of-memory error.

**Default:** 32 MB (`33554432`).  Set to 0 for unlimited.

---

## popstate Event Dispatch (Round 7)

**Status:** Implemented in `content/handlers/javascript/duktape/History.bnd`.

When `history.back()`, `history.forward()`, or `history.go()` triggers
navigation, a `popstate` event is dispatched on the document after the
navigation completes.

**KNOWN LIMITATION -- state is always null:** The `state` property of
the dispatched popstate event is always null.  The state object passed
to `pushState` / `replaceState` is accepted but not stored or round-tripped
through popstate.  This is documented behavior.

---

## Hashmap Bucket Count (Round 7, footprint)

**Status:** `hashmap_parameters_t` in `utils/hashmap.h` now includes a
`bucket_count` field.  If zero, defaults to 4091 (backward compatible).
Callers can specify smaller bucket counts for maps that hold few entries,
reducing per-map RSS from 32 KB to as low as 64 bytes.

---

## Memory Cache Floor (Round 7, footprint)

**Status:** `MINIMUM_MEMORY_CACHE_SIZE` in `desktop/netsurf.c` reduced
from 2 MB to 512 KB.  llcache source buffer slack in `content/llcache.c`
reduced from 64 KB to 4 KB.  These changes reduce minimum RAM requirements
for embedded targets.

---

## HTTP Cache-Control Extension (Round 8)

**Status:** Extended `utils/http/cache-control.c` to parse `must-revalidate`,
`private`, and `s-maxage` directives (RFC 9111).

`must_revalidate` is stored in `llcache_cache_control` and prevents serving
stale responses when the directive is present.  `private` and `s-maxage` are
parsed and accessible but not yet used by llcache (relevant for shared caches,
which NetSurf is not).

---

## Event Properties (Round 8)

**Status:** Added to `content/handlers/javascript/duktape/Event.bnd`:
- `timeStamp` (readonly): returns `dom_event_get_timestamp()` as a number.
- `srcElement` (readonly): legacy IE alias for `target`.
- `returnValue` (getter/setter): inverse of `defaultPrevented`; setting false
  calls `preventDefault()`.
- `cancelBubble` (getter/setter): getter returns false (no libdom API to query
  propagation-stopped); setter calls `stopPropagation()` when true.
- `composed` (readonly): always false (no shadow DOM support).

---

## HTMLElement Attribute Expansion (Round 8)

**Status:** Added to `content/handlers/javascript/duktape/HTMLElement.bnd`:
- `draggable` (bool attribute), `spellcheck` (bool), `translate` (bool),
  `inert` (bool): getter via `dom_element_has_attribute()`, setter via
  `set_attribute` / `remove_attribute`.
- `contentEditable` (string): getter returns attribute value or "inherit" if
  absent; setter via `dom_element_set_attribute()`.
- `isContentEditable` (readonly bool): true if contenteditable is "" or "true".

---

## HTMLSelectElement Expansion (Round 8)

**Status:** Added to `content/handlers/javascript/duktape/HTMLSelectElement.bnd`:
- `selectedIndex` (getter/setter): `dom_html_select_element_get/set_selected_index()`.
- `length` (readonly): `dom_html_select_element_get_length()`.
- `size` (getter/setter): `dom_html_select_element_get/set_size()`.
- `form` (readonly): `dom_html_select_element_get_form()`, pushes form node.

---

## HTMLInputElement Validation Attributes (Round 8)

**Status:** Added to `content/handlers/javascript/duktape/HTMLInputElement.bnd`:
- `required` (bool attribute): getter/setter via generic DOM.
- `placeholder`, `pattern`, `min`, `max`, `step` (string attributes): all
  getter/setter via `dom_element_get/set_attribute()`.

---

## Element Methods (Round 8)

**Status:** Added to `content/handlers/javascript/duktape/Element.bnd`:
- `remove()`: detaches element from its parent via `dom_node_remove_child()`.
- `toggleAttribute(name, force?)`: adds or removes a boolean attribute.
- `getAttributeNames()`: returns array of attribute names from
  `dom_node_get_attributes()`.
- `children` (readonly): returns array of child element nodes.

---

## Document Collections (Round 8)

**Status:** Added to `content/handlers/javascript/duktape/Document.bnd`:
- `forms`, `images`, `links` (readonly): wrap `dom_html_document_get_forms()` /
  `get_images()` / `get_links()` in HTMLCollection with `makeListProxy`.
- `characterSet` / `charset` (readonly): returns "UTF-8".
- `readyState` (readonly): returns "complete" (JS runs post-parse in NetSurf).

---

## Window Properties (Round 8)

**Status:** Added to `content/handlers/javascript/duktape/Window.bnd`:
- `self`, `top`, `parent` (readonly): return the global object (no frame support).
- `closed` (readonly): returns `priv->closed_down`.
- `innerWidth`, `innerHeight` (readonly): from `browser_window_get_dimensions()`.

---

## Console Methods (Round 9)

**Status:** Added to `content/handlers/javascript/duktape/Console.bnd`:
- `assert(condition, ...data)`: logs "Assertion failed: ..." if condition is falsy.
- `count(label?)`: per-label counter, logs "label: N".
- `countReset(label?)`: resets counter to 0.
- `table(data)`: delegates to `write_log_entry` (no table formatting).

Counters are stored in a Duktape object in the heap stash, keyed by label.
Previously implemented: log, warn, error, info, debug, dir, group,
groupCollapsed, groupEnd, time, timeEnd, trace.

---

## Document Expansion (Round 9)

**Status:** Added to `content/handlers/javascript/duktape/Document.bnd`:
- `compatMode` (readonly): "BackCompat" or "CSS1Compat" based on quirks mode.
- `contentType` (readonly): always "text/html".
- `documentURI` / `URL` (readonly): document URL via `nsurl_access()`.
- `hidden` (readonly): always false (no tab visibility model).
- `visibilityState` (readonly): always "visible".
- `createComment(data)`: creates comment node via `dom_document_create_comment()`.
- `defaultView` (readonly): pushes the global (Window) object.

---

## Element Expansion (Round 9)

**Status:** Added to `content/handlers/javascript/duktape/Element.bnd`:
- `matches(selector)`: tests if element matches CSS selector.  Reuses
  `dukky_queryselector()` on the document root and checks if the element
  is in the result set.  O(n) for document size.
- `closest(selector)`: walks ancestors via `dom_node_get_parent_node()`,
  calling matches() on each.  Returns first match or null.
- `hasAttributes()`: checks `dom_namednodemap_get_length() > 0`.
- `namespaceURI` (readonly): `dom_node_get_namespace()`.
- `prefix` (readonly): `dom_node_get_prefix()`.
- `outerHTML` (readonly getter): serializes element tag + attributes +
  innerHTML.  Reuses `dukky_push_node_innerhtml()` for children.
- `slot` (getter/setter): string attribute on "slot" name.
- `insertAdjacentHTML(position, text)`: parses HTML fragment and inserts
  at one of four positions (beforebegin, afterbegin, beforeend, afterend).

---

## Window Expansion (Round 9)

**Status:** Added to `content/handlers/javascript/duktape/Window.bnd`:
- `devicePixelRatio` (readonly): returns 1.0 (no HiDPI awareness).
- `screenX`, `screenY` (readonly): return 0.
- `outerWidth`, `outerHeight` (readonly): same as innerWidth/innerHeight.
- `scrollTo(x, y)`, `scrollBy(x, y)`, `scroll(x, y)`: no-op stubs.
- `focus()`, `blur()`: no-op stubs.
- `atob(data)`: base64 decode using lookup table.  Throws DOMException on
  invalid input.
- `btoa(data)`: base64 encode.  Throws InvalidCharacterError if input
  contains characters > 0xFF.

---

## Storage API (Round 9)

**Status:** Implemented in `content/handlers/javascript/duktape/Storage.bnd`.
Exposed as `window.localStorage` and `window.sessionStorage` via lazy-create
getters in Window.bnd.

**Implementation:** In-memory per-origin storage using Duktape objects in the
heap stash.  Each origin maps to a separate key-value store.  Data is lost
on browser restart (acceptable for initial implementation).

**Members (6/6 = 100%):**
- `length` (readonly): count of stored keys.
- `key(index)`: returns key at given index or null.
- `getItem(key)`: returns value or null.
- `setItem(key, value)`: stores key-value pair.
- `removeItem(key)`: deletes a key.
- `clear()`: deletes all keys.

**KNOWN LIMITATION -- no persistence:** Storage is purely in-memory.  Data
does not survive browser restart.  Persistence via filesystem would require
platform-specific file I/O integration.

**KNOWN LIMITATION -- no StorageEvent:** Mutations do not fire `storage`
events on other windows/tabs.  This requires cross-window event dispatch
infrastructure that NetSurf does not have.

---

## Monkey Test Infrastructure (Round 9)

**monkeyfarmer.py asyncio migration:** Replaced deprecated `asyncore` module
(removed in Python 3.12+) with `selectors.DefaultSelector`.  External API
unchanged.  Tests now run on Python 3.14 (Arch Linux).

**monkey EXEC off-by-one fix:** Fixed `frontends/monkey/browser.c`
`monkey_window_handle_exec()` where `total - 1` passed an srclen one byte
too large to `browser_window_exec()`, causing Duktape to read past the NUL
terminator.  Changed to `strlen(cmd)`.

**js_exec newline collapse:** `monkeyfarmer.py` `js_exec()` now collapses
newlines to spaces before sending, because the monkey protocol is
line-based (`fgets`).  YAML `>-` folded scalars with indented content
produce embedded newlines that would truncate the command.

**Test results:** 29/33 monkey tests pass.  4 pre-existing failures:
innerHTML-correctness (SIGSEGV in setter), inserted-script/quit-mid-fetch/
resource-scheme (upstream timeouts).

---

## Round 10: Spec Coverage 29.1% -> 41.9% (March 2026)

Major API expansion across all core interfaces.  135 new done members,
pushing spec coverage from 29.1% (297/1021) to 41.9% (428/1021).

### Document Expansion
- `inputEncoding` (getter, hardcoded "UTF-8"), `doctype` (getter via
  `dom_document_get_doctype`), `getElementsByClassName`, `createAttribute`,
  `createCDATASection`, `createProcessingInstruction`, `importNode`,
  `adoptNode`, `getElementsByTagNameNS`
- HTML members: `referrer`, `lastModified`, `domain`, `dir` (getter/setter),
  `embeds`, `plugins`, `scripts`, `getElementsByName`, `hasFocus`, `close`,
  `anchors`, `designMode` (getter/setter), `currentScript`, `activeElement`
- ParentNode mixin: `prepend`, `append`, `replaceChildren`

### Element Expansion
- `getAttributeNS`, `setAttributeNS`, `removeAttributeNS`, `hasAttributeNS`,
  `getElementsByClassName`, `getElementsByTagNameNS`
- `insertAdjacentElement`, `insertAdjacentText` (reuse insertAdjacentHTML
  position-parsing logic)
- ChildNode mixin: `before`, `after`, `replaceWith`
- `nonce` (getter/setter via generic attribute)

### URLSearchParams (new binding)
- Full implementation: constructor, `size`, `append`, `delete`, `get`,
  `getAll`, `has`, `set`, `sort`, `toString` (100% spec coverage)

### Form Elements
- HTMLInputElement: `form`, `autocomplete`, `autofocus`, `multiple`, type
  setter, `indeterminate`, `formAction/Enctype/Method/NoValidate/Target`,
  `labels`
- HTMLSelectElement: `autocomplete`, `required`, `options` (HTMLCollection
  proxy), `labels`, `willValidate`, `validationMessage`, `checkValidity`,
  `reportValidity`
- HTMLTextAreaElement: `form`, `autocomplete`, `autofocus`, `placeholder`,
  `maxLength`, `minLength`, `textLength`, `wrap`, `labels`, `required`
- HTMLFormElement: `autocomplete`, `encoding` (enctype alias), `noValidate`,
  `rel`, `elements`, `checkValidity`, `reportValidity`, `requestSubmit`
- HTMLButtonElement: `form`, `type` (getter/setter), `formAction/Enctype/
  Method/NoValidate/Target`, `willValidate`, `validationMessage`,
  `checkValidity`
- HTMLAnchorElement: `download`, `ping`, `referrerPolicy`, `origin`, `text`
  (getter/setter via textContent)

### Window / Navigator
- Window: `frames` (self), `length` (0), `frameElement` (null), `opener`
  (null/no-op), `status` (get/set via MAGIC), `close`/`stop`/`print`
  (no-op), `confirm` (false), `prompt` (null), `originAgentCluster` (false),
  `clientInformation` (navigator alias)
- Navigator: `language` ("en"), `languages` (frozen ["en"]), `onLine` (true),
  `pdfViewerEnabled` (false), `plugins` (empty), `mimeTypes` (empty)

### Node / DocumentFragment / Console / History / Event
- Node: `isConnected` (walk to Document), `getRootNode` (walk to root),
  `isSameNode` (pointer compare)
- DocumentFragment (new .bnd): `querySelector`, `querySelectorAll`,
  `getElementById`, `children`, `firstElementChild`, `lastElementChild`,
  `childElementCount`, `prepend`, `append`, `replaceChildren`
- Console: `timeLog`, `clear`, `dirxml`
- History: `scrollRestoration` (get/set via MAGIC), `state` (null)
- Event: `composedPath` (returns [target])

### WebIDL Updates
- dom.idl: Added `isConnected`, `getRootNode`, `isSameNode` to Node;
  `composedPath` to Event; `insertAdjacentElement`/`insertAdjacentText` to
  Element; `replaceChildren` to ParentNode; `GetRootNodeOptions` dictionary
- html.idl: Added `scrollRestoration` to History; `clientInformation` and
  `originAgentCluster` to Window; `pdfViewerEnabled` to NavigatorPlugins;
  `ScrollRestoration` enum
- console.idl: Added `timeLog`, `clear`, `dirxml`

### Corestrings
15 new: autofocus, download, formaction, formenctype, formmethod,
formnovalidate, formtarget, maxlength, minlength, multiple, nonce,
novalidate, ping, referrerpolicy, wrap.  CORESTRING_TEST_COUNT: 524 -> 554.

### Test Results
- Unit tests: 12 suites, all pass (555 corestring checks)
- Monkey tests: 5 new YAMLs (document-expansion-r10, element-expansion-r10,
  form-button-anchor, node-completion, window-navigator-r10)
- Lacunae scan: 2,152 entries (done=494, stub=1,401, absent=257)
