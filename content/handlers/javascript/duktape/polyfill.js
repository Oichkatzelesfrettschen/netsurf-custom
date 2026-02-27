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

// ---- Map polyfill ----------------------------------------------------------
// WHY: Duktape 2.7 does not expose Map. Many modern sites use Map for
//      ordered key-value storage with arbitrary keys (including objects).
// WHAT: ES6-compatible Map backed by a pair of arrays (keys[], values[]).
//       Supports get, set, has, delete, clear, forEach, size, keys, values,
//       entries, and Symbol.iterator via a synthetic @@iterator.
// HOW: Uses strict equality (===) for key lookup, matching the ES6 spec.

if (typeof Map === 'undefined') {
  (function () {
    function Map(iterable) {
      this._keys = [];
      this._vals = [];
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
      for (var i = 0; i < this._keys.length; i++) {
        if (this._keys[i] === key || (key !== key && this._keys[i] !== this._keys[i])) {
          return i;
        }
      }
      return -1;
    };

    Object.defineProperty(Map.prototype, 'size', {
      get: function () { return this._keys.length; }
    });

    Map.prototype.set = function (key, value) {
      var idx = this._indexOf(key);
      if (idx >= 0) {
        this._vals[idx] = value;
      } else {
        this._keys.push(key);
        this._vals.push(value);
      }
      return this;
    };

    Map.prototype.get = function (key) {
      var idx = this._indexOf(key);
      return idx >= 0 ? this._vals[idx] : undefined;
    };

    Map.prototype.has = function (key) {
      return this._indexOf(key) >= 0;
    };

    Map.prototype['delete'] = function (key) {
      var idx = this._indexOf(key);
      if (idx < 0) return false;
      this._keys.splice(idx, 1);
      this._vals.splice(idx, 1);
      return true;
    };

    Map.prototype.clear = function () {
      this._keys = [];
      this._vals = [];
    };

    Map.prototype.forEach = function (cb, thisArg) {
      for (var i = 0; i < this._keys.length; i++) {
        cb.call(thisArg, this._vals[i], this._keys[i], this);
      }
    };

    Map.prototype.keys = function () {
      return makeIterator(this._keys.slice());
    };

    Map.prototype.values = function () {
      return makeIterator(this._vals.slice());
    };

    Map.prototype.entries = function () {
      var pairs = [];
      for (var i = 0; i < this._keys.length; i++) {
        pairs.push([this._keys[i], this._vals[i]]);
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

if (typeof Set === 'undefined') {
  (function () {
    function Set(iterable) {
      this._vals = [];
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
      for (var i = 0; i < this._vals.length; i++) {
        if (this._vals[i] === val || (val !== val && this._vals[i] !== this._vals[i])) {
          return i;
        }
      }
      return -1;
    };

    Object.defineProperty(Set.prototype, 'size', {
      get: function () { return this._vals.length; }
    });

    Set.prototype.add = function (val) {
      if (this._indexOf(val) < 0) {
        this._vals.push(val);
      }
      return this;
    };

    Set.prototype.has = function (val) {
      return this._indexOf(val) >= 0;
    };

    Set.prototype['delete'] = function (val) {
      var idx = this._indexOf(val);
      if (idx < 0) return false;
      this._vals.splice(idx, 1);
      return true;
    };

    Set.prototype.clear = function () {
      this._vals = [];
    };

    Set.prototype.forEach = function (cb, thisArg) {
      for (var i = 0; i < this._vals.length; i++) {
        cb.call(thisArg, this._vals[i], this._vals[i], this);
      }
    };

    Set.prototype.values = function () {
      return makeIterator(this._vals.slice());
    };

    // Per spec, keys() is an alias for values() on Set
    Set.prototype.keys = Set.prototype.values;

    Set.prototype.entries = function () {
      var pairs = [];
      for (var i = 0; i < this._vals.length; i++) {
        pairs.push([this._vals[i], this._vals[i]]);
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