/*
 * Copyright 2025 NetSurf Custom Contributors
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 * Duktape API compatibility shim for the enhanced (QuickJS-NG) engine.
 *
 * WHY: nsgenbind generates ~224 C files that use Duktape's stack-based
 * API (duk_push_string, duk_get_prop_string, etc.). Rather than forking
 * nsgenbind or rewriting all bindings, this shim emulates Duktape's API
 * on top of QuickJS-NG's value-based API using a JSValue array as a
 * virtual stack.
 *
 * WHAT: Provides typedefs (duk_context, duk_ret_t, etc.) and function
 * declarations matching duktape.h, so that #include "duktape.h" in
 * generated code resolves here (via the compat/ include redirect).
 *
 * HOW: Each duk_* function maps to one or more QuickJS JS_* calls.
 * The emulated stack is a growable JSValue array stored in duk_context.
 */

#ifndef NETSURF_ENHANCED_DUK_COMPAT_H
#define NETSURF_ENHANCED_DUK_COMPAT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "content/handlers/javascript/enhanced/engine.h"

/* ------------------------------------------------------------------ */
/* Duktape type aliases                                                */
/* ------------------------------------------------------------------ */

typedef int duk_ret_t;
typedef int duk_int_t;
typedef unsigned int duk_uint_t;
typedef int32_t duk_int32_t;
typedef uint32_t duk_uint32_t;
typedef int duk_bool_t;
typedef int duk_idx_t;
typedef size_t duk_size_t;
typedef uint32_t duk_uarridx_t;
typedef double duk_double_t;
typedef uint16_t duk_uint16_t;
typedef int16_t duk_int16_t;
typedef unsigned char duk_uint8_t;

/* ------------------------------------------------------------------ */
/* Duktape constants                                                   */
/* ------------------------------------------------------------------ */

#define DUK_EXEC_SUCCESS 0
#define DUK_EXEC_ERROR   1

#define DUK_ERR_ERROR      1
#define DUK_ERR_EVAL_ERROR 2
#define DUK_ERR_RANGE_ERROR 3
#define DUK_ERR_REFERENCE_ERROR 4
#define DUK_ERR_SYNTAX_ERROR 5
#define DUK_ERR_TYPE_ERROR 6
#define DUK_ERR_URI_ERROR  7

#define DUK_TYPE_NONE      0
#define DUK_TYPE_UNDEFINED 1
#define DUK_TYPE_NULL      2
#define DUK_TYPE_BOOLEAN   3
#define DUK_TYPE_NUMBER    4
#define DUK_TYPE_STRING    5
#define DUK_TYPE_OBJECT    6
#define DUK_TYPE_BUFFER    7
#define DUK_TYPE_POINTER   8
#define DUK_TYPE_LIGHTFUNC 9

#define DUK_COMPILE_EVAL    (1 << 0)
#define DUK_COMPILE_STRICT  (1 << 3)

#define DUK_VARARGS (-1)

#define DUK_GC_COMPACT (1 << 0)

/* duk_def_prop flags */
#define DUK_DEFPROP_HAVE_WRITABLE     (1 << 0)
#define DUK_DEFPROP_HAVE_ENUMERABLE   (1 << 1)
#define DUK_DEFPROP_HAVE_CONFIGURABLE (1 << 2)
#define DUK_DEFPROP_HAVE_VALUE        (1 << 3)
#define DUK_DEFPROP_HAVE_GETTER       (1 << 4)
#define DUK_DEFPROP_HAVE_SETTER       (1 << 5)
#define DUK_DEFPROP_WRITABLE          (1 << 6)
#define DUK_DEFPROP_ENUMERABLE        (1 << 7)
#define DUK_DEFPROP_CONFIGURABLE      (1 << 8)
#define DUK_DEFPROP_FORCE             (1 << 9)
#define DUK_DEFPROP_SET_WRITABLE      (DUK_DEFPROP_HAVE_WRITABLE | DUK_DEFPROP_WRITABLE)
#define DUK_DEFPROP_SET_ENUMERABLE    (DUK_DEFPROP_HAVE_ENUMERABLE | DUK_DEFPROP_ENUMERABLE)
#define DUK_DEFPROP_SET_CONFIGURABLE  (DUK_DEFPROP_HAVE_CONFIGURABLE | DUK_DEFPROP_CONFIGURABLE)

/* ------------------------------------------------------------------ */
/* Emulated Duktape context                                            */
/* ------------------------------------------------------------------ */

/* WHY: 256 initial slots avoids realloc for typical benchmark pages.
 * The previous value of 64 caused ~3-4 reallocations per page. */
#define DUK_COMPAT_INITIAL_STACK 256

/* WHY: 16-slot LRU cache avoids repeated JS_NewAtom calls for the same
 * property name strings (e.g. "prototype", "length", corestring names).
 * JS_NewAtom does a hash lookup + possible string intern on every call;
 * caching the resulting JSAtom eliminates that work for hot names. */
#define DUK_ATOM_CACHE_SIZE 16

/**
 * Emulated duk_context backed by a QuickJS JSContext.
 *
 * WHY: Duktape uses a stack-based API where values are referenced by
 * index. QuickJS uses a value-based API where values are passed directly.
 * This struct bridges the two by maintaining a JSValue array (vstack)
 * that emulates Duktape's stack semantics.
 */
typedef struct duk_context {
	JSContext *qjs;       /**< QuickJS context */
	JSRuntime *qjs_rt;    /**< QuickJS runtime */
	JSValue *vstack;      /**< emulated value stack */
	int top;              /**< absolute stack top (number of values) */
	int base;             /**< current stack frame base (for safe_call) */
	int capacity;         /**< allocated stack slots */
	JSValue this_val;     /**< current 'this' for C function dispatch */
	int argc;             /**< argument count for current C call */
	bool is_constructor_call; /**< true when invoked via `new` */
	/* Atom LRU cache: maps C string pointer to JSAtom.
	 * WHY: duk__key_to_atom is called for every non-string-literal
	 * property access (e.g. pointer-keyed NODE_MAGIC lookups).
	 * The LRU cache avoids repeated JS_NewAtom calls for the same
	 * key within a page's JS execution lifetime. */
	struct {
		const char *key; /**< C string (not owned); NULL = empty slot */
		JSAtom atom;     /**< cached atom; JS_ATOM_NULL if empty */
	} atom_cache[DUK_ATOM_CACHE_SIZE];
	int atom_cache_next; /**< next eviction index (round-robin) */
} duk_context;

/* ------------------------------------------------------------------ */
/* C function type                                                     */
/* ------------------------------------------------------------------ */

typedef duk_ret_t (*duk_c_function)(duk_context *ctx);
typedef duk_ret_t (*duk_safe_call_function)(duk_context *ctx, void *udata);

/* ------------------------------------------------------------------ */
/* Stack management                                                    */
/* ------------------------------------------------------------------ */

/**
 * Normalize a Duktape stack index.
 * Positive indices are relative to the current stack frame base.
 * Negative indices are relative to top (-1 = top of stack).
 */
static inline int duk__norm_idx(duk_context *ctx, duk_idx_t idx)
{
	if (idx >= 0)
		return ctx->base + idx;
	return ctx->top + idx;
}

/**
 * Ensure the stack has room for at least n more values.
 * WHY: Declared before the inline accessors below that use it.
 */
void duk__ensure_stack(duk_context *ctx, int extra);

/* ------------------------------------------------------------------ */
/* Inline hot property accessors (most-called paths)                  */
/* ------------------------------------------------------------------ */

/**
 * Get named property from object at obj_idx, push result on stack.
 *
 * WHY: This is the hottest non-trivial property-access path -- called
 * hundreds of times per benchmark page. Moving it inline eliminates the
 * call overhead and the JS_ToCString/FreeCString pair that the general
 * duk__key_to_atom path would require for a plain C-string literal key.
 * JS_GetPropertyStr already accepts a raw C string directly.
 */
static inline duk_bool_t duk_get_prop_string(duk_context *ctx,
					     duk_idx_t obj_idx,
					     const char *key)
{
	int norm = duk__norm_idx(ctx, obj_idx);
	JSValue result = JS_GetPropertyStr(ctx->qjs, ctx->vstack[norm], key);

	duk__ensure_stack(ctx, 1);
	ctx->vstack[ctx->top++] = result;

	return !JS_IsUndefined(result);
}

/**
 * Pop value from top of stack and set it as named property of obj_idx.
 *
 * WHY: Symmetric hot path to duk_get_prop_string. Inlining avoids the
 * duk__key_to_atom dispatch for the common case of a plain C-string key.
 * The hidden-property (\xFF\xFF prefix) branch is kept because init code
 * uses it for MAGIC/NODE_MAGIC cache keys.
 */
static inline void duk_put_prop_string(duk_context *ctx,
				       duk_idx_t obj_idx,
				       const char *key)
{
	int norm = duk__norm_idx(ctx, obj_idx);
	assert(ctx->top > 0);

	JSValue val = ctx->vstack[ctx->top - 1];
	ctx->vstack[ctx->top - 1] = JS_UNDEFINED;
	ctx->top--;

	/* WHY: Duktape treats \xFF\xFF-prefixed keys as hidden internal
	 * properties invisible to enumeration and Object.keys().  QuickJS
	 * has no equivalent, so define as non-enumerable. */
	if ((unsigned char)key[0] == 0xFF && (unsigned char)key[1] == 0xFF) {
		JSAtom atom = JS_NewAtom(ctx->qjs, key);
		JS_DefineProperty(ctx->qjs, ctx->vstack[norm], atom,
				  val, JS_UNDEFINED, JS_UNDEFINED,
				  JS_PROP_HAS_VALUE |
				  JS_PROP_HAS_WRITABLE | JS_PROP_WRITABLE |
				  JS_PROP_HAS_CONFIGURABLE | JS_PROP_CONFIGURABLE |
				  JS_PROP_HAS_ENUMERABLE /* enumerable=0 */);
		JS_FreeValue(ctx->qjs, val);
		JS_FreeAtom(ctx->qjs, atom);
	} else {
		JS_SetPropertyStr(ctx->qjs, ctx->vstack[norm], key, val);
	}
}

/**
 * Get the QuickJS JSContext from a duk_context.
 */
static inline JSContext *duk_compat_qjs(duk_context *ctx)
{
	return ctx->qjs;
}

/* ------------------------------------------------------------------ */
/* Stack introspection                                                 */
/* ------------------------------------------------------------------ */

static inline duk_idx_t duk_get_top(duk_context *ctx)
{
	return ctx->top - ctx->base;
}

static inline void duk_set_top(duk_context *ctx, duk_idx_t top)
{
	int abs_top = ctx->base + top;
	/* Free values being removed */
	while (ctx->top > abs_top) {
		ctx->top--;
		JS_FreeValue(ctx->qjs, ctx->vstack[ctx->top]);
		ctx->vstack[ctx->top] = JS_UNDEFINED;
	}
	/* Push undefined for new slots */
	while (ctx->top < abs_top) {
		duk__ensure_stack(ctx, 1);
		ctx->vstack[ctx->top++] = JS_UNDEFINED;
	}
}

static inline void duk_require_stack(duk_context *ctx, duk_idx_t extra)
{
	duk__ensure_stack(ctx, extra);
}

/* ------------------------------------------------------------------ */
/* Push operations                                                     */
/* ------------------------------------------------------------------ */

static inline void duk_push_undefined(duk_context *ctx)
{
	duk__ensure_stack(ctx, 1);
	ctx->vstack[ctx->top++] = JS_UNDEFINED;
}

static inline void duk_push_null(duk_context *ctx)
{
	duk__ensure_stack(ctx, 1);
	ctx->vstack[ctx->top++] = JS_NULL;
}

static inline void duk_push_boolean(duk_context *ctx, duk_bool_t val)
{
	duk__ensure_stack(ctx, 1);
	ctx->vstack[ctx->top++] = JS_NewBool(ctx->qjs, val);
}

static inline void duk_push_true(duk_context *ctx)
{
	duk_push_boolean(ctx, 1);
}

static inline void duk_push_false(duk_context *ctx)
{
	duk_push_boolean(ctx, 0);
}

static inline void duk_push_int(duk_context *ctx, int val)
{
	duk__ensure_stack(ctx, 1);
	ctx->vstack[ctx->top++] = JS_NewInt32(ctx->qjs, val);
}

static inline void duk_push_uint(duk_context *ctx, unsigned int val)
{
	duk__ensure_stack(ctx, 1);
	ctx->vstack[ctx->top++] = JS_NewInt32(ctx->qjs, (int32_t)val);
}

static inline void duk_push_number(duk_context *ctx, double val)
{
	duk__ensure_stack(ctx, 1);
	ctx->vstack[ctx->top++] = JS_NewFloat64(ctx->qjs, val);
}

static inline duk_idx_t duk_push_object(duk_context *ctx)
{
	duk__ensure_stack(ctx, 1);
	ctx->vstack[ctx->top] = JS_NewObject(ctx->qjs);
	return ctx->top++;
}

static inline duk_idx_t duk_push_array(duk_context *ctx)
{
	duk__ensure_stack(ctx, 1);
	ctx->vstack[ctx->top] = JS_NewArray(ctx->qjs);
	return ctx->top++;
}

static inline void duk_push_string(duk_context *ctx, const char *str)
{
	duk__ensure_stack(ctx, 1);
	if (str == NULL) {
		ctx->vstack[ctx->top++] = JS_NULL;
	} else {
		ctx->vstack[ctx->top++] = JS_NewString(ctx->qjs, str);
	}
}

static inline void duk_push_lstring(duk_context *ctx, const char *str,
				    duk_size_t len)
{
	duk__ensure_stack(ctx, 1);
	if (str == NULL) {
		/* WHY: Real Duktape pushes an empty string when ptr is NULL.
		 * Multiple .bnd getters (Location, Element, etc.) rely on
		 * this behavior -- they pass NULL when the underlying value
		 * is absent and expect "" on the JS stack. */
		ctx->vstack[ctx->top++] = JS_NewStringLen(ctx->qjs, "", 0);
	} else {
		ctx->vstack[ctx->top++] = JS_NewStringLen(ctx->qjs, str, len);
	}
}

void duk_push_pointer(duk_context *ctx, void *ptr);

void duk_push_global_object(duk_context *ctx);

void duk_push_c_function(duk_context *ctx, duk_c_function func, int nargs);

void duk_push_this(duk_context *ctx);

/* ------------------------------------------------------------------ */
/* Pop operations                                                      */
/* ------------------------------------------------------------------ */

static inline void duk_pop(duk_context *ctx)
{
	assert(ctx->top > 0);
	ctx->top--;
	JS_FreeValue(ctx->qjs, ctx->vstack[ctx->top]);
	ctx->vstack[ctx->top] = JS_UNDEFINED;
}

static inline void duk_pop_2(duk_context *ctx)
{
	duk_pop(ctx);
	duk_pop(ctx);
}

static inline void duk_pop_3(duk_context *ctx)
{
	duk_pop(ctx);
	duk_pop(ctx);
	duk_pop(ctx);
}

static inline void duk_pop_n(duk_context *ctx, int n)
{
	int i;
	for (i = 0; i < n; i++)
		duk_pop(ctx);
}

/* ------------------------------------------------------------------ */
/* Duplicate / insert / remove / replace                               */
/* ------------------------------------------------------------------ */

static inline void duk_dup(duk_context *ctx, duk_idx_t idx)
{
	int norm = duk__norm_idx(ctx, idx);
	duk__ensure_stack(ctx, 1);
	ctx->vstack[ctx->top++] = JS_DupValue(ctx->qjs,
					       ctx->vstack[norm]);
}

static inline void duk_dup_top(duk_context *ctx)
{
	duk_dup(ctx, -1);
}

void duk_insert(duk_context *ctx, duk_idx_t to_idx);
void duk_remove(duk_context *ctx, duk_idx_t idx);
void duk_replace(duk_context *ctx, duk_idx_t idx);
void duk_swap(duk_context *ctx, duk_idx_t a, duk_idx_t b);

/* ------------------------------------------------------------------ */
/* Property access                                                     */
/* ------------------------------------------------------------------ */

duk_bool_t duk_get_prop(duk_context *ctx, duk_idx_t obj_idx);
/* duk_get_prop_string: static inline above */
duk_bool_t duk_get_prop_index(duk_context *ctx, duk_idx_t obj_idx,
			      duk_uarridx_t idx);
void duk_put_prop(duk_context *ctx, duk_idx_t obj_idx);
/* duk_put_prop_string: static inline above */
void duk_put_prop_index(duk_context *ctx, duk_idx_t obj_idx,
			duk_uarridx_t idx);
duk_bool_t duk_del_prop(duk_context *ctx, duk_idx_t obj_idx);
duk_bool_t duk_del_prop_string(duk_context *ctx, duk_idx_t obj_idx,
			       const char *key);
duk_bool_t duk_del_prop_index(duk_context *ctx, duk_idx_t obj_idx,
			      duk_uarridx_t idx);
duk_bool_t duk_has_prop_string(duk_context *ctx, duk_idx_t obj_idx,
			       const char *key);

/* ------------------------------------------------------------------ */
/* Global property access                                              */
/* ------------------------------------------------------------------ */

duk_bool_t duk_get_global_string(duk_context *ctx, const char *key);
void duk_put_global_string(duk_context *ctx, const char *key);

/* ------------------------------------------------------------------ */
/* Type checks                                                         */
/* ------------------------------------------------------------------ */

static inline duk_bool_t duk_is_undefined(duk_context *ctx, duk_idx_t idx)
{
	return JS_IsUndefined(ctx->vstack[duk__norm_idx(ctx, idx)]);
}

static inline duk_bool_t duk_is_null(duk_context *ctx, duk_idx_t idx)
{
	return JS_IsNull(ctx->vstack[duk__norm_idx(ctx, idx)]);
}

static inline duk_bool_t duk_is_null_or_undefined(duk_context *ctx,
						  duk_idx_t idx)
{
	int norm = duk__norm_idx(ctx, idx);
	return JS_IsNull(ctx->vstack[norm]) ||
	       JS_IsUndefined(ctx->vstack[norm]);
}

static inline duk_bool_t duk_is_boolean(duk_context *ctx, duk_idx_t idx)
{
	return JS_IsBool(ctx->vstack[duk__norm_idx(ctx, idx)]);
}

static inline duk_bool_t duk_is_number(duk_context *ctx, duk_idx_t idx)
{
	return JS_IsNumber(ctx->vstack[duk__norm_idx(ctx, idx)]);
}

static inline duk_bool_t duk_is_string(duk_context *ctx, duk_idx_t idx)
{
	return JS_IsString(ctx->vstack[duk__norm_idx(ctx, idx)]);
}

static inline duk_bool_t duk_is_object(duk_context *ctx, duk_idx_t idx)
{
	return JS_IsObject(ctx->vstack[duk__norm_idx(ctx, idx)]);
}

static inline duk_bool_t duk_is_array(duk_context *ctx, duk_idx_t idx)
{
	(void)ctx;
	return JS_IsArray(ctx->vstack[duk__norm_idx(ctx, idx)]);
}

static inline duk_bool_t duk_is_function(duk_context *ctx, duk_idx_t idx)
{
	return JS_IsFunction(ctx->qjs, ctx->vstack[duk__norm_idx(ctx, idx)]);
}

duk_bool_t duk_is_pointer(duk_context *ctx, duk_idx_t idx);
duk_bool_t duk_is_constructor_call(duk_context *ctx);

static inline int duk_get_type(duk_context *ctx, duk_idx_t idx)
{
	int norm = duk__norm_idx(ctx, idx);
	JSValue v = ctx->vstack[norm];

	if (JS_IsUndefined(v)) return DUK_TYPE_UNDEFINED;
	if (JS_IsNull(v)) return DUK_TYPE_NULL;
	if (JS_IsBool(v)) return DUK_TYPE_BOOLEAN;
	if (JS_IsNumber(v)) return DUK_TYPE_NUMBER;
	if (JS_IsString(v)) return DUK_TYPE_STRING;
	if (JS_IsObject(v)) return DUK_TYPE_OBJECT;
	return DUK_TYPE_NONE;
}

/* ------------------------------------------------------------------ */
/* Value getters                                                       */
/* ------------------------------------------------------------------ */

static inline duk_bool_t duk_get_boolean(duk_context *ctx, duk_idx_t idx)
{
	return JS_ToBool(ctx->qjs, ctx->vstack[duk__norm_idx(ctx, idx)]);
}

static inline duk_bool_t duk_to_boolean(duk_context *ctx, duk_idx_t idx)
{
	return JS_ToBool(ctx->qjs, ctx->vstack[duk__norm_idx(ctx, idx)]);
}

static inline int duk_get_int(duk_context *ctx, duk_idx_t idx)
{
	int32_t res = 0;
	JS_ToInt32(ctx->qjs, &res, ctx->vstack[duk__norm_idx(ctx, idx)]);
	return (int)res;
}

static inline int duk_to_int(duk_context *ctx, duk_idx_t idx)
{
	return duk_get_int(ctx, idx);
}

static inline int duk_require_int(duk_context *ctx, duk_idx_t idx)
{
	return duk_get_int(ctx, idx);
}

static inline unsigned int duk_get_uint(duk_context *ctx, duk_idx_t idx)
{
	int32_t res = 0;
	JS_ToInt32(ctx->qjs, &res, ctx->vstack[duk__norm_idx(ctx, idx)]);
	return (unsigned int)res;
}

static inline double duk_get_number(duk_context *ctx, duk_idx_t idx)
{
	double res = 0;
	JS_ToFloat64(ctx->qjs, &res, ctx->vstack[duk__norm_idx(ctx, idx)]);
	return res;
}

static inline double duk_to_number(duk_context *ctx, duk_idx_t idx)
{
	return duk_get_number(ctx, idx);
}

const char *duk_get_string(duk_context *ctx, duk_idx_t idx);
const char *duk_to_string(duk_context *ctx, duk_idx_t idx);
const char *duk_safe_to_string(duk_context *ctx, duk_idx_t idx);
const char *duk_get_lstring(duk_context *ctx, duk_idx_t idx, duk_size_t *len);
const char *duk_safe_to_lstring(duk_context *ctx, duk_idx_t idx,
				duk_size_t *len);

void *duk_get_pointer(duk_context *ctx, duk_idx_t idx);
void *duk_require_pointer(duk_context *ctx, duk_idx_t idx);

/* ------------------------------------------------------------------ */
/* Prototype                                                           */
/* ------------------------------------------------------------------ */

void duk_set_prototype(duk_context *ctx, duk_idx_t obj_idx);
void duk_get_prototype(duk_context *ctx, duk_idx_t obj_idx);

/* ------------------------------------------------------------------ */
/* Object/array length                                                 */
/* ------------------------------------------------------------------ */

duk_size_t duk_get_length(duk_context *ctx, duk_idx_t idx);

/* ------------------------------------------------------------------ */
/* Error handling                                                      */
/* ------------------------------------------------------------------ */

duk_ret_t duk_error(duk_context *ctx, int code, const char *fmt, ...);
duk_ret_t duk_generic_error(duk_context *ctx, const char *fmt, ...);
duk_ret_t duk_type_error(duk_context *ctx, const char *fmt, ...);
duk_ret_t duk_range_error(duk_context *ctx, const char *fmt, ...);

/* ------------------------------------------------------------------ */
/* Safe call / pcall                                                   */
/* ------------------------------------------------------------------ */

duk_int_t duk_safe_call(duk_context *ctx, duk_safe_call_function func,
			void *udata, int nargs, int nrets);
duk_int_t duk_pcall(duk_context *ctx, int nargs);
duk_int_t duk_pcall_method(duk_context *ctx, int nargs);

void duk_call(duk_context *ctx, int nargs);

/* ------------------------------------------------------------------ */
/* Compilation                                                         */
/* ------------------------------------------------------------------ */

duk_int_t duk_pcompile_lstring_filename(duk_context *ctx, unsigned int flags,
					const char *src, duk_size_t len);

/* ------------------------------------------------------------------ */
/* Global object manipulation                                          */
/* ------------------------------------------------------------------ */

void duk_set_global_object(duk_context *ctx);

/* ------------------------------------------------------------------ */
/* String conversion / safe conversion                                 */
/* ------------------------------------------------------------------ */

const char *duk_safe_to_stacktrace(duk_context *ctx, duk_idx_t idx);

/* ------------------------------------------------------------------ */
/* Thread / context                                                    */
/* ------------------------------------------------------------------ */

void duk_push_thread(duk_context *ctx);
duk_context *duk_require_context(duk_context *ctx, duk_idx_t idx);

/* ------------------------------------------------------------------ */
/* Heap / memory                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
	void *(*alloc_func)(void *udata, size_t size);
	void *(*realloc_func)(void *udata, void *ptr, size_t size);
	void (*free_func)(void *udata, void *ptr);
	void *udata;
} duk_memory_functions;

void duk_get_memory_functions(duk_context *ctx, duk_memory_functions *funcs);
void duk_gc(duk_context *ctx, unsigned int flags);

/* ------------------------------------------------------------------ */
/* def_prop                                                            */
/* ------------------------------------------------------------------ */

void duk_def_prop(duk_context *ctx, duk_idx_t obj_idx, unsigned int flags);

/* ------------------------------------------------------------------ */
/* Stash                                                               */
/* ------------------------------------------------------------------ */

void duk_push_global_stash(duk_context *ctx);

/* ------------------------------------------------------------------ */
/* Context dump (debug)                                                */
/* ------------------------------------------------------------------ */

void duk_push_context_dump(duk_context *ctx);

/* ------------------------------------------------------------------ */
/* Misc                                                                */
/* ------------------------------------------------------------------ */

static inline duk_idx_t duk_get_top_index(duk_context *ctx)
{
	int rel_top = ctx->top - ctx->base;
	return rel_top > 0 ? rel_top - 1 : -1;
}

static inline duk_bool_t duk_check_type(duk_context *ctx, duk_idx_t idx,
					int type)
{
	return duk_get_type(ctx, idx) == type;
}

static inline duk_idx_t duk_normalize_index(duk_context *ctx, duk_idx_t idx)
{
	return duk__norm_idx(ctx, idx);
}

/* ------------------------------------------------------------------ */
/* Require (coercion with type check)                                  */
/* ------------------------------------------------------------------ */

static inline duk_bool_t duk_require_boolean(duk_context *ctx, duk_idx_t idx)
{
	return duk_get_boolean(ctx, idx);
}

static inline double duk_require_number(duk_context *ctx, duk_idx_t idx)
{
	return duk_get_number(ctx, idx);
}

const char *duk_require_string(duk_context *ctx, duk_idx_t idx);
const char *duk_require_lstring(duk_context *ctx, duk_idx_t idx,
				duk_size_t *len);

/* ------------------------------------------------------------------ */
/* To-coercion variants                                                */
/* ------------------------------------------------------------------ */

const char *duk_to_lstring(duk_context *ctx, duk_idx_t idx, duk_size_t *len);

static inline unsigned int duk_to_uint(duk_context *ctx, duk_idx_t idx)
{
	return duk_get_uint(ctx, idx);
}

static inline uint32_t duk_to_uint32(duk_context *ctx, duk_idx_t idx)
{
	int32_t res = 0;
	JS_ToInt32(ctx->qjs, &res, ctx->vstack[duk__norm_idx(ctx, idx)]);
	return (uint32_t)res;
}

/* ------------------------------------------------------------------ */
/* lstring property access (keyed by raw bytes + length)               */
/* ------------------------------------------------------------------ */

duk_bool_t duk_get_prop_lstring(duk_context *ctx, duk_idx_t obj_idx,
				const char *key, duk_size_t key_len);
void duk_put_prop_lstring(duk_context *ctx, duk_idx_t obj_idx,
			  const char *key, duk_size_t key_len);
duk_bool_t duk_del_prop_lstring(duk_context *ctx, duk_idx_t obj_idx,
				const char *key, duk_size_t key_len);
duk_bool_t duk_has_prop(duk_context *ctx, duk_idx_t obj_idx);

/* ------------------------------------------------------------------ */
/* Finalizer                                                           */
/* ------------------------------------------------------------------ */

void duk_set_finalizer(duk_context *ctx, duk_idx_t obj_idx);

/* ------------------------------------------------------------------ */
/* String operations                                                   */
/* ------------------------------------------------------------------ */

void duk_concat(duk_context *ctx, int count);
void duk_push_sprintf(duk_context *ctx, const char *fmt, ...);

/* ------------------------------------------------------------------ */
/* Comparison                                                          */
/* ------------------------------------------------------------------ */

duk_bool_t duk_strict_equals(duk_context *ctx, duk_idx_t a, duk_idx_t b);

/* ------------------------------------------------------------------ */
/* Enumeration                                                         */
/* ------------------------------------------------------------------ */

#define DUK_ENUM_OWN_PROPERTIES_ONLY (1 << 0)

void duk_enum(duk_context *ctx, duk_idx_t obj_idx, unsigned int flags);
duk_bool_t duk_next(duk_context *ctx, duk_idx_t enum_idx,
		    duk_bool_t get_value);

static inline duk_bool_t duk_is_callable(duk_context *ctx, duk_idx_t idx)
{
	return duk_is_function(ctx, idx);
}

/* ------------------------------------------------------------------ */
/* Buffer API                                                          */
/* ------------------------------------------------------------------ */

#define DUK_BUFOBJ_UINT8ARRAY        0
#define DUK_BUFOBJ_UINT8CLAMPEDARRAY 1
#define DUK_BUF_FLAG_DYNAMIC  (1 << 0)
#define DUK_BUF_FLAG_EXTERNAL (1 << 1)

void *duk_push_buffer(duk_context *ctx, duk_size_t size, duk_bool_t dynamic);
void *duk_push_fixed_buffer(duk_context *ctx, duk_size_t size);
void *duk_push_buffer_raw(duk_context *ctx, duk_size_t size,
			  unsigned int flags);
void duk_push_buffer_object(duk_context *ctx, duk_idx_t buf_idx,
			    duk_size_t offset, duk_size_t length,
			    unsigned int type);
void *duk_get_buffer_data(duk_context *ctx, duk_idx_t idx, duk_size_t *size);
duk_bool_t duk_is_buffer_data(duk_context *ctx, duk_idx_t idx);

/* ------------------------------------------------------------------ */
/* Error object creation                                               */
/* ------------------------------------------------------------------ */

#define DUK_ERR_NONE 0
#define DUK_RET_TYPE_ERROR (-DUK_ERR_TYPE_ERROR)

duk_idx_t duk_push_error_object(duk_context *ctx, int code,
				const char *fmt, ...);
duk_ret_t duk_error_raw(duk_context *ctx, int code, const char *filename,
			int line, const char *fmt, ...);

/* WHY: nsgenbind output uses duk_error_raw via DUK_ERROR_* macros,
 * not the variadic duk_error. Provide a compatible entry point. */

/* ------------------------------------------------------------------ */
/* Heap stash                                                          */
/* ------------------------------------------------------------------ */

void duk_push_heap_stash(duk_context *ctx);

/* ------------------------------------------------------------------ */
/* Boolean alias                                                       */
/* ------------------------------------------------------------------ */

static inline void duk_push_bool(duk_context *ctx, duk_bool_t val)
{
	duk_push_boolean(ctx, val);
}

/* ------------------------------------------------------------------ */
/* Compilation (additional forms)                                      */
/* ------------------------------------------------------------------ */

#define DUK_COMPILE_FUNCTION  (1 << 1)

duk_int_t duk_pcompile(duk_context *ctx, unsigned int flags);

/* ------------------------------------------------------------------ */
/* Heap lifecycle (for dukky.c compat)                                 */
/* ------------------------------------------------------------------ */

typedef void (*duk_fatal_function)(void *udata, const char *msg);

duk_context *duk_create_heap(void *alloc_func, void *realloc_func,
			     void *free_func, void *heap_udata,
			     duk_fatal_function fatal_handler);
void duk_destroy_heap(duk_context *ctx);

/* ------------------------------------------------------------------ */
/* DUK_OPT_HAVE_CUSTOM_H stub                                         */
/* ------------------------------------------------------------------ */

/* WHY: The standard (Duktape) build sets -DDUK_OPT_HAVE_CUSTOM_H which
 * makes duktape.h include duk_custom.h. In enhanced mode, we don't need
 * this. Provide an empty guard. */

/* ------------------------------------------------------------------ */
/* Persistent compat context lifecycle                                 */
/* ------------------------------------------------------------------ */

/**
 * Create a persistent compat context backed by a QuickJS context.
 *
 * WHY: nsgenbind binding code and the DOM initialization sequence in
 * js_newthread need a long-lived duk_context whose emulated stack
 * persists across calls. The trampoline creates temporary contexts
 * per call, but the init sequence and event handlers need one that
 * lives as long as the jsthread.
 */
duk_context *duk_compat_create(JSContext *qjs);

/**
 * Destroy a persistent compat context.
 */
void duk_compat_destroy(duk_context *ctx);

#endif /* NETSURF_ENHANCED_DUK_COMPAT_H */
