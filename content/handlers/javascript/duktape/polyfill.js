/* Polyfiller for Duktape for NetSurf
 *
 * This JavaScript will be loaded into heaps before the generics
 *
 * We only care for the side-effects of this, be careful.
 */

// Production steps of ECMA-262, Edition 6, 22.1.2.1
if (!Array.from) {
  Array.from = (function () {
    var toStr = Object.prototype.toString;
    var isCallable = function (fn) {
      return typeof fn === 'function' || toStr.call(fn) === '[object Function]';
    };
    var toInteger = function (value) {
      var number = Number(value);
      if (isNaN(number)) { return 0; }
      if (number === 0 || !isFinite(number)) { return number; }
      return (number > 0 ? 1 : -1) * Math.floor(Math.abs(number));
    };
    var maxSafeInteger = Math.pow(2, 53) - 1;
    var toLength = function (value) {
      var len = toInteger(value);
      return Math.min(Math.max(len, 0), maxSafeInteger);
    };

    // The length property of the from method is 1.
    return function from(arrayLike/*, mapFn, thisArg */) {
      // 1. Let C be the this value.
      var C = this;

      // 2. Let items be ToObject(arrayLike).
      var items = Object(arrayLike);

      // 3. ReturnIfAbrupt(items).
      if (arrayLike == null) {
        throw new TypeError('Array.from requires an array-like object - not null or undefined');
      }

      // 4. If mapfn is undefined, then let mapping be false.
      var mapFn = arguments.length > 1 ? arguments[1] : void undefined;
      var T;
      if (typeof mapFn !== 'undefined') {
        // 5. else
        // 5. a If IsCallable(mapfn) is false, throw a TypeError exception.
        if (!isCallable(mapFn)) {
          throw new TypeError('Array.from: when provided, the second argument must be a function');
        }

        // 5. b. If thisArg was supplied, let T be thisArg; else let T be undefined.
        if (arguments.length > 2) {
          T = arguments[2];
        }
      }

      // 10. Let lenValue be Get(items, "length").
      // 11. Let len be ToLength(lenValue).
      var len = toLength(items.length);

      // 13. If IsConstructor(C) is true, then
      // 13. a. Let A be the result of calling the [[Construct]] internal method 
      // of C with an argument list containing the single item len.
      // 14. a. Else, Let A be ArrayCreate(len).
      var A = isCallable(C) ? Object(new C(len)) : new Array(len);

      // 16. Let k be 0.
      var k = 0;
      // 17. Repeat, while k < len… (also steps a - h)
      var kValue;
      while (k < len) {
        kValue = items[k];
        if (mapFn) {
          A[k] = typeof T === 'undefined' ? mapFn(kValue, k) : mapFn.call(T, kValue, k);
        } else {
          A[k] = kValue;
        }
        k += 1;
      }
      // 18. Let putStatus be Put(A, "length", len, true).
      A.length = len;
      // 20. Return A.
      return A;
    };
  }());
}

// DOMTokenList formatter, in theory we can remove this if we do the stringifier IDL support

DOMTokenList.prototype.toString = function () {
  if (this.length == 0) {
    return "";
  }

  var ret = this.item(0);
  for (var index = 1; index < this.length; index++) {
    ret = ret + " " + this.item(index);
  }

  return ret;
}

// Inherit the same toString for settable lists
DOMSettableTokenList.prototype.toString = DOMTokenList.prototype.toString;

// ---- CustomEvent polyfill --------------------------------------------------
// WHY: new CustomEvent(type, {detail: x}) is used by sites and frameworks
//      for intra-component messaging. The native CustomEvent stub in
//      netsurf.bnd stores only the C-level dom_event; it does not expose the
//      JS `detail` property. This polyfill shadows the native binding.
// WHAT: Wraps the native Event (or creates a plain object) and adds `detail`.
// HOW: If typeof CustomEvent === 'undefined' or it lacks `detail`, replace it.
//      We always define it to ensure `detail` is accessible.

(function () {
  var _NativeCustomEvent = (typeof CustomEvent === 'function') ? CustomEvent : null;

  function CustomEvent(type, params) {
    params = params || {};
    var detail = (params.detail !== undefined) ? params.detail : null;

    // Attempt to create the native event for proper type/bubbles/cancelable
    var evt;
    if (_NativeCustomEvent) {
      try {
        evt = new _NativeCustomEvent(type, params);
      } catch (e) {
        evt = null;
      }
    }

    if (!evt) {
      // Fallback: plain object that looks like an event
      evt = {};
      evt.type = type;
      evt.bubbles = !!(params.bubbles);
      evt.cancelable = !!(params.cancelable);
    }

    evt.detail = detail;
    return evt;
  }

  // Expose to global scope
  if (typeof this !== 'undefined') {
    this.CustomEvent = CustomEvent;
  }
}.call(typeof globalThis !== 'undefined' ? globalThis : this));

// ---- AbortController / AbortSignal polyfills --------------------------------
// WHY: AbortController is a prerequisite for any usable Fetch API. Sites use
//      it to cancel in-flight network requests. No native binding exists.
// WHAT: Pure JS implementation. AbortController owns an AbortSignal. Calling
//      controller.abort() sets signal.aborted = true and fires abort listeners.
// HOW: Listener callbacks stored in a plain array on the signal object.

if (typeof AbortController === 'undefined') {
  (function () {
    function AbortSignal() {
      this.aborted = false;
      this._listeners = [];
    }

    AbortSignal.prototype.addEventListener = function (type, fn) {
      if (type === 'abort' && typeof fn === 'function') {
        this._listeners.push(fn);
      }
    };

    AbortSignal.prototype.removeEventListener = function (type, fn) {
      if (type !== 'abort') return;
      var idx = this._listeners.indexOf(fn);
      if (idx >= 0) this._listeners.splice(idx, 1);
    };

    AbortSignal.prototype._abort = function () {
      if (this.aborted) return;
      this.aborted = true;
      var evt = { type: 'abort', target: this };
      for (var i = 0; i < this._listeners.length; i++) {
        try { this._listeners[i].call(this, evt); } catch (e) { /* swallow */ }
      }
    };

    function AbortController() {
      this.signal = new AbortSignal();
    }

    AbortController.prototype.abort = function () {
      this.signal._abort();
    };

    this.AbortSignal = AbortSignal;
    this.AbortController = AbortController;
  }.call(typeof globalThis !== 'undefined' ? globalThis : this));
}

// ---- Map polyfill ----------------------------------------------------------
// WHY: Duktape 2.7 does not expose Map. Many modern sites use Map for
//      ordered key-value storage with arbitrary keys (including objects).
// WHAT: ES6-compatible Map backed by a pair of arrays (keys[], values[]).
//       Supports get, set, has, delete, clear, forEach, size, keys, values,
//       entries, and Symbol.iterator via a synthetic @@iterator.
//       Internal storage uses Symbol keys when available (Duktape 2.7 has
//       DUK_USE_SYMBOL_BUILTIN enabled) so that Object.keys(new Map()) returns
//       [] instead of exposing '_keys' / '_vals'.
// HOW: Uses strict equality (===) for key lookup, matching the ES6 spec.
//      For string and finite-number keys, a hash object provides O(1) average
//      lookup. Object/null/boolean/NaN keys fall back to O(n) linear scan.
//      The hash is kept in sync with the arrays by set(), delete(), and clear().

// WHY: Shared helper for Map and Set hash-based O(1) lookup.
// Must be in outer scope so both Map and Set IIFEs can access it.
function _hashKey(key) {
  var t = typeof key;
  if (t === 'string') return 's:' + key;
  if (t === 'number' && key === key && isFinite(key)) return 'n:' + key;
  return null;
}

if (typeof Map === 'undefined') {
  (function () {
    // Use Symbol-based private slots when available, string fallbacks otherwise.
    // WHY: Symbol-keyed properties are non-enumerable and not returned by
    //      Object.keys(), preventing user code from accidentally seeing or
    //      mutating internal state. String keys ('_keys'/'_vals') are still
    //      correct but visible to enumeration.
    var MAP_KEYS = (typeof Symbol === 'function') ? Symbol('Map.keys') : '_keys';
    var MAP_VALS = (typeof Symbol === 'function') ? Symbol('Map.vals') : '_vals';
    // MAP_HASH: plain object used as a string->index lookup table for
    // primitive (string, finite number) keys. Keys are prefixed to avoid
    // collisions between string 'x' and number x.
    var MAP_HASH = (typeof Symbol === 'function') ? Symbol('Map.hash') : '_hash';

    function Map(iterable) {
      this[MAP_KEYS] = [];
      this[MAP_VALS] = [];
      this[MAP_HASH] = Object.create(null); /* null prototype avoids __proto__ conflicts */
      if (iterable) {
        var arr = Array.isArray(iterable) ? iterable : null;
        if (arr) {
          for (var i = 0; i < arr.length; i++) {
            this.set(arr[i][0], arr[i][1]);
          }
        }
      }
    }

    Map.prototype._indexOf = function (key) {
      // Fast path: O(1) average for string/finite-number keys via hash.
      var hk = _hashKey(key);
      if (hk !== null) {
        var idx = this[MAP_HASH][hk];
        return (idx !== undefined) ? idx : -1;
      }
      // Slow path: linear scan for object keys and NaN.
      var ks = this[MAP_KEYS];
      for (var i = 0; i < ks.length; i++) {
        if (ks[i] === key || (key !== key && ks[i] !== ks[i])) {
          return i;
        }
      }
      return -1;
    };

    Object.defineProperty(Map.prototype, 'size', {
      get: function () { return this[MAP_KEYS].length; }
    });

    Map.prototype.set = function (key, value) {
      var hk = _hashKey(key);
      var idx = this._indexOf(key);
      if (idx >= 0) {
        this[MAP_VALS][idx] = value;
      } else {
        idx = this[MAP_KEYS].length;
        this[MAP_KEYS].push(key);
        this[MAP_VALS].push(value);
        if (hk !== null) {
          this[MAP_HASH][hk] = idx;
        }
      }
      return this;
    };

    Map.prototype.get = function (key) {
      var idx = this._indexOf(key);
      return idx >= 0 ? this[MAP_VALS][idx] : undefined;
    };

    Map.prototype.has = function (key) {
      return this._indexOf(key) >= 0;
    };

    Map.prototype['delete'] = function (key) {
      var idx = this._indexOf(key);
      if (idx < 0) return false;
      var hk = _hashKey(key);
      this[MAP_KEYS].splice(idx, 1);
      this[MAP_VALS].splice(idx, 1);
      // Rebuild hash: indices of all entries at or after idx have shifted.
      // WHY full rebuild vs patching: splice shifts every subsequent index by
      // -1, so partial patch is as expensive as rebuild for the common case.
      if (hk !== null) {
        var hash = Object.create(null);
        var ks = this[MAP_KEYS];
        for (var i = 0; i < ks.length; i++) {
          var k = _hashKey(ks[i]);
          if (k !== null) hash[k] = i;
        }
        this[MAP_HASH] = hash;
      }
      return true;
    };

    Map.prototype.clear = function () {
      this[MAP_KEYS] = [];
      this[MAP_VALS] = [];
      this[MAP_HASH] = Object.create(null);
    };

    Map.prototype.forEach = function (cb, thisArg) {
      var ks = this[MAP_KEYS], vs = this[MAP_VALS];
      for (var i = 0; i < ks.length; i++) {
        cb.call(thisArg, vs[i], ks[i], this);
      }
    };

    Map.prototype.keys = function () {
      return makeIterator(this[MAP_KEYS].slice());
    };

    Map.prototype.values = function () {
      return makeIterator(this[MAP_VALS].slice());
    };

    Map.prototype.entries = function () {
      var ks = this[MAP_KEYS], vs = this[MAP_VALS];
      var pairs = [];
      for (var i = 0; i < ks.length; i++) {
        pairs.push([ks[i], vs[i]]);
      }
      return makeIterator(pairs);
    };

    if (typeof Symbol !== 'undefined' && Symbol.iterator) {
      Map.prototype[Symbol.iterator] = Map.prototype.entries;
    }

    this.Map = Map;
  }.call(typeof globalThis !== 'undefined' ? globalThis : this));
}

// ---- Set polyfill ----------------------------------------------------------
// WHY: Duktape 2.7 does not expose Set. Sites use Set for unique-value
//      collections.
// WHAT: ES6-compatible Set backed by an array. Supports add, has, delete,
//       clear, forEach, size, values, keys, entries.
//       Internal storage uses Symbol keys when available (same rationale as Map).
//       For string and finite-number values, SET_HASH provides O(1) average
//       lookup (same hash-object strategy as Map). Object/NaN values fall back
//       to O(n) linear scan.
// HOW: SET_HASH is kept in sync with SET_VALS by add(), delete(), and clear().

if (typeof Set === 'undefined') {
  (function () {
    var SET_VALS = (typeof Symbol === 'function') ? Symbol('Set.vals') : '_svals';
    // SET_HASH: plain null-prototype object mapping _hashKey(val) -> index in
    // SET_VALS array.  Same prefix scheme as MAP_HASH ('s:' / 'n:').
    var SET_HASH = (typeof Symbol === 'function') ? Symbol('Set.hash') : '_shash';

    function Set(iterable) {
      this[SET_VALS] = [];
      this[SET_HASH] = Object.create(null);
      if (iterable) {
        var arr = Array.isArray(iterable) ? iterable : null;
        if (arr) {
          for (var i = 0; i < arr.length; i++) {
            this.add(arr[i]);
          }
        }
      }
    }

    Set.prototype._indexOf = function (val) {
      // Fast path: O(1) average for string/finite-number values.
      var hk = _hashKey(val);
      if (hk !== null) {
        var idx = this[SET_HASH][hk];
        return (idx !== undefined) ? idx : -1;
      }
      // Slow path: linear scan for objects and NaN.
      var vs = this[SET_VALS];
      for (var i = 0; i < vs.length; i++) {
        if (vs[i] === val || (val !== val && vs[i] !== vs[i])) {
          return i;
        }
      }
      return -1;
    };

    Object.defineProperty(Set.prototype, 'size', {
      get: function () { return this[SET_VALS].length; }
    });

    Set.prototype.add = function (val) {
      if (this._indexOf(val) < 0) {
        var idx = this[SET_VALS].length;
        this[SET_VALS].push(val);
        var hk = _hashKey(val);
        if (hk !== null) {
          this[SET_HASH][hk] = idx;
        }
      }
      return this;
    };

    Set.prototype.has = function (val) {
      return this._indexOf(val) >= 0;
    };

    Set.prototype['delete'] = function (val) {
      var idx = this._indexOf(val);
      if (idx < 0) return false;
      var hk = _hashKey(val);
      this[SET_VALS].splice(idx, 1);
      // Rebuild hash: splice shifts every subsequent index by -1.
      // WHY full rebuild: same reasoning as Map.delete -- partial patch is
      // as expensive as rebuild for the common case.
      if (hk !== null) {
        var hash = Object.create(null);
        var vs = this[SET_VALS];
        for (var i = 0; i < vs.length; i++) {
          var k = _hashKey(vs[i]);
          if (k !== null) hash[k] = i;
        }
        this[SET_HASH] = hash;
      }
      return true;
    };

    Set.prototype.clear = function () {
      this[SET_VALS] = [];
      this[SET_HASH] = Object.create(null);
    };

    Set.prototype.forEach = function (cb, thisArg) {
      var vs = this[SET_VALS];
      for (var i = 0; i < vs.length; i++) {
        cb.call(thisArg, vs[i], vs[i], this);
      }
    };

    Set.prototype.values = function () {
      return makeIterator(this[SET_VALS].slice());
    };

    // Per spec, keys() is an alias for values() on Set
    Set.prototype.keys = Set.prototype.values;

    Set.prototype.entries = function () {
      var vs = this[SET_VALS];
      var pairs = [];
      for (var i = 0; i < vs.length; i++) {
        pairs.push([vs[i], vs[i]]);
      }
      return makeIterator(pairs);
    };

    if (typeof Symbol !== 'undefined' && Symbol.iterator) {
      Set.prototype[Symbol.iterator] = Set.prototype.values;
    }

    this.Set = Set;
  }.call(typeof globalThis !== 'undefined' ? globalThis : this));
}

// ---- WeakMap polyfill -------------------------------------------------------
// WHY: WeakMap is needed by many frameworks even if GC semantics are not
//      exactly replicated. This polyfill provides the interface; it holds
//      strong references (no GC weakening), which is correct for correctness
//      even if suboptimal for memory in long-lived pages.
// WHAT: ES6-compatible WeakMap backed by a unique per-key tag property.

if (typeof WeakMap === 'undefined') {
  (function () {
    var counter = 0;

    function WeakMap() {
      this._id = '__wm_' + (++counter) + '_';
    }

    WeakMap.prototype._key = function (obj) {
      return this._id;
    };

    WeakMap.prototype.set = function (key, value) {
      if (typeof key !== 'object' || key === null) {
        throw new TypeError('WeakMap key must be an object');
      }
      Object.defineProperty(key, this._id, {
        value: value,
        configurable: true,
        writable: true
      });
      return this;
    };

    WeakMap.prototype.get = function (key) {
      return key[this._id];
    };

    WeakMap.prototype.has = function (key) {
      return typeof key === 'object' && key !== null &&
             Object.prototype.hasOwnProperty.call(key, this._id);
    };

    WeakMap.prototype['delete'] = function (key) {
      if (!this.has(key)) return false;
      delete key[this._id];
      return true;
    };

    this.WeakMap = WeakMap;
  }.call(typeof globalThis !== 'undefined' ? globalThis : this));
}

// ---- WeakSet polyfill -------------------------------------------------------
// WHY: WeakSet is used by some frameworks as a way to tag objects.
// WHAT: Backed on the same tag-property approach as WeakMap.

if (typeof WeakSet === 'undefined') {
  (function () {
    var counter = 0;

    function WeakSet() {
      this._id = '__ws_' + (++counter) + '_';
    }

    WeakSet.prototype.add = function (obj) {
      if (typeof obj !== 'object' || obj === null) {
        throw new TypeError('WeakSet value must be an object');
      }
      Object.defineProperty(obj, this._id, {
        value: true,
        configurable: true,
        writable: true
      });
      return this;
    };

    WeakSet.prototype.has = function (obj) {
      return typeof obj === 'object' && obj !== null &&
             Object.prototype.hasOwnProperty.call(obj, this._id);
    };

    WeakSet.prototype['delete'] = function (obj) {
      if (!this.has(obj)) return false;
      delete obj[this._id];
      return true;
    };

    this.WeakSet = WeakSet;
  }.call(typeof globalThis !== 'undefined' ? globalThis : this));
}

// ---- Iterator helper --------------------------------------------------------
// Shared by Map and Set polyfills for .keys()/.values()/.entries()

function makeIterator(arr) {
  var i = 0;
  return {
    next: function () {
      return i < arr.length
        ? { value: arr[i++], done: false }
        : { value: undefined, done: true };
    }
  };
}

// ---- Promise polyfill -------------------------------------------------------
// WHY: Promise (commented out in Window.bnd) is required by virtually all
//      modern JS. Without it, fetch(), dynamic imports, and async patterns
//      fail entirely. Duktape 2.7 has no native Promise.
// WHAT: A microtask-queue polyfill that schedules callbacks via setTimeout(0)
//       as the closest equivalent to a real microtask queue. Supports:
//       - new Promise(executor)
//       - Promise.resolve / Promise.reject / Promise.all / Promise.race /
//         Promise.allSettled / Promise.any
//       - .then() / .catch() / .finally()
// HOW: State machine with PENDING/FULFILLED/REJECTED states. Callbacks are
//      deferred via an internal micro-queue flushed by setTimeout(0).
//
// KNOWN DEVIATION: This polyfill schedules .then() callbacks via setTimeout(0)
// (macrotask queue) rather than the spec-mandated microtask queue. Duktape 2.7
// does not expose a native microtask queue hook. As a result, the execution
// order of `Promise.resolve().then(cb)` vs synchronous code that follows it is
// INCORRECT per ECMA-262 section 8.6 (the callback fires after the next event
// loop turn, not immediately after the current task). Code that relies on
// microtask ordering (e.g. `let x=0; Promise.resolve().then(()=>x=1); x===0`)
// may behave differently under this polyfill than in a compliant engine.

if (typeof Promise === 'undefined') {
  (function () {
    var PENDING = 0, FULFILLED = 1, REJECTED = 2;

    // Micro-queue: we batch callbacks and flush them after current execution
    // via setTimeout(0). This is not a true microtask queue but is the best
    // approximation available under Duktape 2.7 without native engine support.
    var queue = [];
    var flushing = false;

    function enqueue(fn) {
      queue.push(fn);
      if (!flushing) {
        flushing = true;
        setTimeout(function () {
          flushing = false;
          var q = queue.splice(0, queue.length);
          for (var i = 0; i < q.length; i++) {
            try { q[i](); } catch (e) { /* swallow: per spec */ }
          }
        }, 0);
      }
    }

    function resolve(promise, x) {
      if (promise._state !== PENDING) return;
      if (x === promise) {
        reject(promise, new TypeError('Promise resolved with itself'));
        return;
      }
      if (x && (typeof x === 'object' || typeof x === 'function')) {
        var then;
        try { then = x.then; } catch (e) { reject(promise, e); return; }
        if (typeof then === 'function') {
          var settled = false;
          try {
            then.call(x,
              function (y) { if (!settled) { settled = true; resolve(promise, y); } },
              function (r) { if (!settled) { settled = true; reject(promise, r); } }
            );
          } catch (e) {
            if (!settled) reject(promise, e);
          }
          return;
        }
      }
      promise._state = FULFILLED;
      promise._value = x;
      flush(promise);
    }

    function reject(promise, reason) {
      if (promise._state !== PENDING) return;
      promise._state = REJECTED;
      promise._value = reason;
      flush(promise);
    }

    function flush(promise) {
      var handlers = promise._handlers;
      promise._handlers = null;
      if (handlers) {
        for (var i = 0; i < handlers.length; i++) {
          handle(promise, handlers[i]);
        }
      }
    }

    function handle(promise, handler) {
      if (promise._state === PENDING) {
        promise._handlers = promise._handlers || [];
        promise._handlers.push(handler);
        return;
      }
      enqueue(function () {
        var cb = promise._state === FULFILLED ? handler.onFulfilled : handler.onRejected;
        if (typeof cb !== 'function') {
          if (promise._state === FULFILLED) {
            resolve(handler.promise, promise._value);
          } else {
            reject(handler.promise, promise._value);
          }
          return;
        }
        try {
          resolve(handler.promise, cb(promise._value));
        } catch (e) {
          reject(handler.promise, e);
        }
      });
    }

    function Promise(executor) {
      if (typeof executor !== 'function') {
        throw new TypeError('Promise executor must be a function');
      }
      this._state = PENDING;
      this._value = undefined;
      this._handlers = [];
      var self = this;
      try {
        executor(
          function (v) { resolve(self, v); },
          function (r) { reject(self, r); }
        );
      } catch (e) {
        reject(this, e);
      }
    }

    Promise.prototype.then = function (onFulfilled, onRejected) {
      var p = new Promise(function () {});
      handle(this, { onFulfilled: onFulfilled, onRejected: onRejected, promise: p });
      return p;
    };

    Promise.prototype['catch'] = function (onRejected) {
      return this.then(undefined, onRejected);
    };

    Promise.prototype['finally'] = function (fn) {
      return this.then(
        function (v) { return Promise.resolve(fn()).then(function () { return v; }); },
        function (r) { return Promise.resolve(fn()).then(function () { throw r; }); }
      );
    };

    Promise.resolve = function (v) {
      if (v && v.constructor === Promise) return v;
      return new Promise(function (res) { res(v); });
    };

    Promise.reject = function (r) {
      return new Promise(function (res, rej) { rej(r); });
    };

    Promise.all = function (promises) {
      return new Promise(function (res, rej) {
        var count = promises.length;
        var results = new Array(count);
        if (count === 0) { res(results); return; }
        for (var i = 0; i < count; i++) {
          (function (idx) {
            Promise.resolve(promises[idx]).then(
              function (v) { results[idx] = v; if (--count === 0) res(results); },
              rej
            );
          }(i));
        }
      });
    };

    Promise.race = function (promises) {
      return new Promise(function (res, rej) {
        for (var i = 0; i < promises.length; i++) {
          Promise.resolve(promises[i]).then(res, rej);
        }
      });
    };

    Promise.allSettled = function (promises) {
      return Promise.all(promises.map(function (p) {
        return Promise.resolve(p).then(
          function (v) { return { status: 'fulfilled', value: v }; },
          function (r) { return { status: 'rejected', reason: r }; }
        );
      }));
    };

    Promise.any = function (promises) {
      return new Promise(function (res, rej) {
        var count = promises.length;
        var errors = new Array(count);
        if (count === 0) { rej(new Error('All promises were rejected')); return; }
        for (var i = 0; i < count; i++) {
          (function (idx) {
            Promise.resolve(promises[idx]).then(res, function (r) {
              errors[idx] = r;
              if (--count === 0) rej(new Error('All promises were rejected'));
            });
          }(i));
        }
      });
    };

    this.Promise = Promise;
  }.call(typeof globalThis !== 'undefined' ? globalThis : this));
}