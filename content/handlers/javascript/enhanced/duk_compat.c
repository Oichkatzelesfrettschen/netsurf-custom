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
 * Duktape API compatibility shim implementation.
 *
 * WHY: Implements the non-inline duk_* functions declared in duk_compat.h.
 * These functions emulate Duktape's stack-based API using QuickJS-NG's
 * value-based API. The emulated stack is a JSValue array in duk_context.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "content/handlers/javascript/enhanced/duk_compat.h"
#include "content/handlers/javascript/enhanced/bridge.h"

/* ------------------------------------------------------------------ */
/* QuickJS class for opaque pointer storage                            */
/* ------------------------------------------------------------------ */

/* WHY: Duktape has duk_push_pointer / duk_get_pointer for storing raw
 * void * values on the stack. QuickJS has no equivalent primitive type.
 * We create a custom class "NsPointer" to wrap a void * using
 * JS_SetOpaque / JS_GetOpaque. */

static JSClassID ns_pointer_class_id = 0;

static void ns_pointer_finalizer(JSRuntime *rt, JSValue val)
{
	/* We do NOT free the pointer; it is owned by the caller */
	(void)rt;
	(void)val;
}

static JSClassDef ns_pointer_class_def = {
	"NsPointer",
	.finalizer = ns_pointer_finalizer,
};

static void duk_compat_ensure_pointer_class(JSContext *qjs)
{
	if (ns_pointer_class_id == 0) {
		JSRuntime *rt = JS_GetRuntime(qjs);
		JS_NewClassID(rt, &ns_pointer_class_id);
		JS_NewClass(rt, ns_pointer_class_id, &ns_pointer_class_def);
	}
}

/* ------------------------------------------------------------------ */
/* Stack management                                                    */
/* ------------------------------------------------------------------ */

void duk__ensure_stack(duk_context *ctx, int extra)
{
	int needed = ctx->top + extra;

	if (needed <= ctx->capacity)
		return;

	int new_cap = ctx->capacity * 2;
	if (new_cap < needed)
		new_cap = needed;
	if (new_cap < DUK_COMPAT_INITIAL_STACK)
		new_cap = DUK_COMPAT_INITIAL_STACK;

	JSValue *new_stack = realloc(ctx->vstack,
				     (size_t)new_cap * sizeof(JSValue));
	if (new_stack == NULL) {
		/* Fatal: cannot grow stack */
		abort();
	}
	/* Initialize new slots */
	for (int i = ctx->capacity; i < new_cap; i++)
		new_stack[i] = JS_UNDEFINED;

	ctx->vstack = new_stack;
	ctx->capacity = new_cap;
}

/* ------------------------------------------------------------------ */
/* Push operations (non-inline)                                        */
/* ------------------------------------------------------------------ */

void duk_push_pointer(duk_context *ctx, void *ptr)
{
	duk_compat_ensure_pointer_class(ctx->qjs);
	duk__ensure_stack(ctx, 1);

	JSValue obj = JS_NewObjectClass(ctx->qjs, ns_pointer_class_id);
	JS_SetOpaque(obj, ptr);
	ctx->vstack[ctx->top++] = obj;
}

/* Hidden property key for the Window object on the native QuickJS global.
 * WHY: After duk_set_global_object, duk_push_global_object must return
 * the Window object (not the native QuickJS global). We store the Window
 * as a hidden property on the native global using this key. */
#define NS_WINDOW_KEY "\xFF\xFF__ns_window"

void duk_push_global_object(duk_context *ctx)
{
	duk__ensure_stack(ctx, 1);

	JSValue native_global = JS_GetGlobalObject(ctx->qjs);
	JSValue window = JS_GetPropertyStr(ctx->qjs, native_global,
					   NS_WINDOW_KEY);
	if (!JS_IsUndefined(window)) {
		/* Window has been set -- return it instead of native global */
		JS_FreeValue(ctx->qjs, native_global);
		ctx->vstack[ctx->top++] = window;
	} else {
		/* Pre-init state: no Window yet, return native global */
		JS_FreeValue(ctx->qjs, window);
		ctx->vstack[ctx->top++] = native_global;
	}
}

/* Trampoline data for wrapping duk_c_function as JSCFunction */
typedef struct {
	duk_c_function func;
	int nargs;
} duk_cfunc_wrap_t;

/* QuickJS C function that dispatches to a Duktape-style C function.
 *
 * WHY: nsgenbind C functions expect a duk_context with arguments on
 * the emulated stack. This trampoline uses the persistent compat_ctx
 * from the jsthread, saving/restoring the stack state around each call.
 * Binding code may store ctx pointers (e.g. for setTimeout callbacks),
 * so the context pointer must remain valid across calls. */
static JSValue duk_cfunc_trampoline(JSContext *qjs, JSValueConst this_val,
				    int argc, JSValueConst *argv, int magic,
				    JSValue *func_data)
{
	(void)magic;
	/* func_data[0] holds a pointer to the trampoline data */
	duk_cfunc_wrap_t *wrap = JS_GetOpaque(func_data[0],
					      ns_pointer_class_id);
	if (wrap == NULL)
		return JS_ThrowTypeError(qjs, "invalid C function wrapper");

	/* Use the persistent compat context from the jsthread.
	 * WHY: binding code stores duk_context* pointers for later use
	 * (e.g. window_schedule_t.ctx for setTimeout). A temporary
	 * context would become a dangling pointer after the trampoline
	 * returns. The persistent compat_ctx survives across calls. */
	struct jsthread *thread = enhanced_get_thread(qjs);
	if (thread == NULL || thread->compat_ctx == NULL)
		return JS_ThrowTypeError(qjs, "no compat context");

	duk_context *ctx = thread->compat_ctx;

	/* Save stack state to create a virtual frame */
	int old_top = ctx->top;
	int old_base = ctx->base;
	JSValue old_this = ctx->this_val;
	int old_argc = ctx->argc;
	bool old_is_ctor = ctx->is_constructor_call;

	ctx->base = old_top;  /* new frame starts at current top */
	ctx->this_val = JS_DupValue(qjs, this_val);
	ctx->argc = argc;

	/* WHY: When called via `new`, QuickJS passes new_target as
	 * this_val.  new_target is the constructor function (always
	 * JS_IsFunction true).  For normal calls, this_val is the
	 * receiver (Window, Element, etc.) which is an object but not
	 * a function.  Only URL/USP constructors check this flag. */
	ctx->is_constructor_call = JS_IsFunction(qjs, this_val);

	/* Push arguments onto the compat stack */
	for (int i = 0; i < argc; i++) {
		duk__ensure_stack(ctx, 1);
		ctx->vstack[ctx->top++] = JS_DupValue(qjs, argv[i]);
	}

	/* Call the Duktape-style function */
	duk_ret_t ret = wrap->func(ctx);

	JSValue result;
	if (ret == 0) {
		result = JS_UNDEFINED;
	} else if (ret == 1 && ctx->top > ctx->base) {
		/* Return the top value */
		result = JS_DupValue(qjs, ctx->vstack[ctx->top - 1]);
	} else if (ret < 0) {
		/* Error was thrown */
		result = JS_EXCEPTION;
	} else {
		result = JS_UNDEFINED;
	}

	/* Clean up: pop everything pushed during this call */
	while (ctx->top > old_top) {
		ctx->top--;
		JS_FreeValue(qjs, ctx->vstack[ctx->top]);
		ctx->vstack[ctx->top] = JS_UNDEFINED;
	}
	JS_FreeValue(qjs, ctx->this_val);

	/* Restore previous frame */
	ctx->base = old_base;
	ctx->this_val = old_this;
	ctx->argc = old_argc;
	ctx->is_constructor_call = old_is_ctor;

	return result;
}

void duk_push_c_function(duk_context *ctx, duk_c_function func, int nargs)
{
	duk_compat_ensure_pointer_class(ctx->qjs);
	duk__ensure_stack(ctx, 1);

	/* Allocate trampoline data */
	duk_cfunc_wrap_t *wrap = malloc(sizeof(duk_cfunc_wrap_t));
	if (wrap == NULL) {
		ctx->vstack[ctx->top++] = JS_UNDEFINED;
		return;
	}
	wrap->func = func;
	wrap->nargs = nargs;

	/* Store in a pointer object as func_data */
	JSValue data = JS_NewObjectClass(ctx->qjs, ns_pointer_class_id);
	JS_SetOpaque(data, wrap);

	int qjs_nargs = (nargs == DUK_VARARGS) ? 0 : nargs;

	ctx->vstack[ctx->top] = JS_NewCFunctionData(
		ctx->qjs, duk_cfunc_trampoline, qjs_nargs, 0, 1, &data);

	/* WHY: JS_NewCFunctionData does not set the constructor bit.
	 * Without it, `new Fn()` throws "not a constructor" before
	 * reaching our trampoline.  Setting the bit on all C functions
	 * is safe: only URL/USP constructors check duk_is_constructor_call. */
	JS_SetConstructorBit(ctx->qjs, ctx->vstack[ctx->top], true);
	ctx->top++;

	JS_FreeValue(ctx->qjs, data);
}

void duk_push_this(duk_context *ctx)
{
	duk__ensure_stack(ctx, 1);
	ctx->vstack[ctx->top++] = JS_DupValue(ctx->qjs, ctx->this_val);
}

/* ------------------------------------------------------------------ */
/* Insert / remove / replace / swap                                    */
/* ------------------------------------------------------------------ */

void duk_insert(duk_context *ctx, duk_idx_t to_idx)
{
	int norm = duk__norm_idx(ctx, to_idx);
	assert(norm >= 0 && norm < ctx->top);

	/* Move top element to position norm, shifting others up */
	JSValue val = ctx->vstack[ctx->top - 1];
	memmove(&ctx->vstack[norm + 1], &ctx->vstack[norm],
		(size_t)(ctx->top - 1 - norm) * sizeof(JSValue));
	ctx->vstack[norm] = val;
}

void duk_remove(duk_context *ctx, duk_idx_t idx)
{
	int norm = duk__norm_idx(ctx, idx);
	assert(norm >= 0 && norm < ctx->top);

	JS_FreeValue(ctx->qjs, ctx->vstack[norm]);
	memmove(&ctx->vstack[norm], &ctx->vstack[norm + 1],
		(size_t)(ctx->top - 1 - norm) * sizeof(JSValue));
	ctx->top--;
	ctx->vstack[ctx->top] = JS_UNDEFINED;
}

void duk_replace(duk_context *ctx, duk_idx_t idx)
{
	int norm = duk__norm_idx(ctx, idx);
	assert(norm >= 0 && norm < ctx->top);
	assert(ctx->top > 0);

	JSValue val = ctx->vstack[ctx->top - 1];
	ctx->vstack[ctx->top - 1] = JS_UNDEFINED;
	ctx->top--;

	JS_FreeValue(ctx->qjs, ctx->vstack[norm]);
	ctx->vstack[norm] = val;
}

void duk_swap(duk_context *ctx, duk_idx_t a, duk_idx_t b)
{
	int na = duk__norm_idx(ctx, a);
	int nb = duk__norm_idx(ctx, b);
	JSValue tmp = ctx->vstack[na];
	ctx->vstack[na] = ctx->vstack[nb];
	ctx->vstack[nb] = tmp;
}

/* ------------------------------------------------------------------ */
/* Property access                                                     */
/* ------------------------------------------------------------------ */

duk_bool_t duk_get_prop(duk_context *ctx, duk_idx_t obj_idx)
{
	int norm = duk__norm_idx(ctx, obj_idx);
	assert(ctx->top > 0);

	/* Key is on top of stack */
	JSValue key = ctx->vstack[ctx->top - 1];
	ctx->vstack[ctx->top - 1] = JS_UNDEFINED;
	ctx->top--;

	JSValue result;
	const char *str = JS_ToCString(ctx->qjs, key);
	if (str) {
		result = JS_GetPropertyStr(ctx->qjs, ctx->vstack[norm], str);
		JS_FreeCString(ctx->qjs, str);
	} else {
		result = JS_UNDEFINED;
	}
	JS_FreeValue(ctx->qjs, key);

	duk__ensure_stack(ctx, 1);
	ctx->vstack[ctx->top++] = result;

	return !JS_IsUndefined(result);
}

duk_bool_t duk_get_prop_string(duk_context *ctx, duk_idx_t obj_idx,
			       const char *key)
{
	int norm = duk__norm_idx(ctx, obj_idx);
	JSValue result = JS_GetPropertyStr(ctx->qjs, ctx->vstack[norm], key);

	duk__ensure_stack(ctx, 1);
	ctx->vstack[ctx->top++] = result;

	return !JS_IsUndefined(result);
}

duk_bool_t duk_get_prop_index(duk_context *ctx, duk_idx_t obj_idx,
			      duk_uarridx_t arr_idx)
{
	int norm = duk__norm_idx(ctx, obj_idx);
	JSAtom atom = JS_NewAtomUInt32(ctx->qjs, arr_idx);
	JSValue result = JS_GetProperty(ctx->qjs, ctx->vstack[norm], atom);
	JS_FreeAtom(ctx->qjs, atom);

	duk__ensure_stack(ctx, 1);
	ctx->vstack[ctx->top++] = result;

	return !JS_IsUndefined(result);
}

void duk_put_prop(duk_context *ctx, duk_idx_t obj_idx)
{
	int norm = duk__norm_idx(ctx, obj_idx);
	assert(ctx->top >= 2);

	/* Stack: ... key value */
	JSValue val = ctx->vstack[ctx->top - 1];
	JSValue key = ctx->vstack[ctx->top - 2];
	ctx->vstack[ctx->top - 1] = JS_UNDEFINED;
	ctx->vstack[ctx->top - 2] = JS_UNDEFINED;
	ctx->top -= 2;

	const char *str = JS_ToCString(ctx->qjs, key);
	if (str) {
		JS_SetPropertyStr(ctx->qjs, ctx->vstack[norm], str, val);
		JS_FreeCString(ctx->qjs, str);
	} else {
		JS_FreeValue(ctx->qjs, val);
	}
	JS_FreeValue(ctx->qjs, key);
}

void duk_put_prop_string(duk_context *ctx, duk_idx_t obj_idx,
			 const char *key)
{
	int norm = duk__norm_idx(ctx, obj_idx);
	assert(ctx->top > 0);

	JSValue val = ctx->vstack[ctx->top - 1];
	ctx->vstack[ctx->top - 1] = JS_UNDEFINED;
	ctx->top--;

	JS_SetPropertyStr(ctx->qjs, ctx->vstack[norm], key, val);
}

void duk_put_prop_index(duk_context *ctx, duk_idx_t obj_idx,
			duk_uarridx_t arr_idx)
{
	int norm = duk__norm_idx(ctx, obj_idx);
	assert(ctx->top > 0);

	JSValue val = ctx->vstack[ctx->top - 1];
	ctx->vstack[ctx->top - 1] = JS_UNDEFINED;
	ctx->top--;

	JS_SetPropertyUint32(ctx->qjs, ctx->vstack[norm], arr_idx, val);
}

duk_bool_t duk_del_prop(duk_context *ctx, duk_idx_t obj_idx)
{
	int norm = duk__norm_idx(ctx, obj_idx);
	assert(ctx->top > 0);

	JSValue key = ctx->vstack[ctx->top - 1];
	ctx->vstack[ctx->top - 1] = JS_UNDEFINED;
	ctx->top--;

	const char *str = JS_ToCString(ctx->qjs, key);
	int ret = 0;
	if (str) {
		JSAtom atom = JS_NewAtom(ctx->qjs, str);
		ret = JS_DeleteProperty(ctx->qjs, ctx->vstack[norm],
					atom, 0);
		JS_FreeAtom(ctx->qjs, atom);
		JS_FreeCString(ctx->qjs, str);
	}
	JS_FreeValue(ctx->qjs, key);

	return ret >= 0;
}

duk_bool_t duk_del_prop_string(duk_context *ctx, duk_idx_t obj_idx,
			       const char *key)
{
	int norm = duk__norm_idx(ctx, obj_idx);
	JSAtom atom = JS_NewAtom(ctx->qjs, key);
	int ret = JS_DeleteProperty(ctx->qjs, ctx->vstack[norm], atom, 0);
	JS_FreeAtom(ctx->qjs, atom);
	return ret >= 0;
}

duk_bool_t duk_del_prop_index(duk_context *ctx, duk_idx_t obj_idx,
			      duk_uarridx_t arr_idx)
{
	int norm = duk__norm_idx(ctx, obj_idx);
	JSAtom atom = JS_NewAtomUInt32(ctx->qjs, arr_idx);
	int ret = JS_DeleteProperty(ctx->qjs, ctx->vstack[norm], atom, 0);
	JS_FreeAtom(ctx->qjs, atom);
	return ret >= 0;
}

duk_bool_t duk_has_prop_string(duk_context *ctx, duk_idx_t obj_idx,
			       const char *key)
{
	int norm = duk__norm_idx(ctx, obj_idx);
	JSAtom atom = JS_NewAtom(ctx->qjs, key);
	int ret = JS_HasProperty(ctx->qjs, ctx->vstack[norm], atom);
	JS_FreeAtom(ctx->qjs, atom);
	return ret > 0;
}

/* ------------------------------------------------------------------ */
/* Global property access                                              */
/* ------------------------------------------------------------------ */

duk_bool_t duk_get_global_string(duk_context *ctx, const char *key)
{
	JSValue global = JS_GetGlobalObject(ctx->qjs);
	JSValue val = JS_GetPropertyStr(ctx->qjs, global, key);
	JS_FreeValue(ctx->qjs, global);

	duk__ensure_stack(ctx, 1);
	ctx->vstack[ctx->top++] = val;

	return !JS_IsUndefined(val);
}

void duk_put_global_string(duk_context *ctx, const char *key)
{
	assert(ctx->top > 0);

	JSValue val = ctx->vstack[ctx->top - 1];
	ctx->vstack[ctx->top - 1] = JS_UNDEFINED;
	ctx->top--;

	JSValue global = JS_GetGlobalObject(ctx->qjs);
	JS_SetPropertyStr(ctx->qjs, global, key, val);
	JS_FreeValue(ctx->qjs, global);
}

/* ------------------------------------------------------------------ */
/* Pointer access                                                      */
/* ------------------------------------------------------------------ */

duk_bool_t duk_is_pointer(duk_context *ctx, duk_idx_t idx)
{
	int norm = duk__norm_idx(ctx, idx);
	if (!JS_IsObject(ctx->vstack[norm]))
		return 0;
	void *p = JS_GetOpaque(ctx->vstack[norm], ns_pointer_class_id);
	return p != NULL || JS_IsNull(ctx->vstack[norm]);
}

void *duk_get_pointer(duk_context *ctx, duk_idx_t idx)
{
	int norm = duk__norm_idx(ctx, idx);
	if (JS_IsNull(ctx->vstack[norm]))
		return NULL;
	if (!JS_IsObject(ctx->vstack[norm]))
		return NULL;
	return JS_GetOpaque(ctx->vstack[norm], ns_pointer_class_id);
}

void *duk_require_pointer(duk_context *ctx, duk_idx_t idx)
{
	return duk_get_pointer(ctx, idx);
}

duk_bool_t duk_is_constructor_call(duk_context *ctx)
{
	return ctx->is_constructor_call ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* String getters                                                      */
/* ------------------------------------------------------------------ */

/* WHY: JS_ToCString returns a string that must be freed with
 * JS_FreeCString. But Duktape's duk_get_string returns a pointer
 * into the stack value that lives as long as the stack entry.
 * We work around this by returning the JS_ToCString result directly.
 * The caller must be aware this is NOT the same lifetime semantics. */

const char *duk_get_string(duk_context *ctx, duk_idx_t idx)
{
	int norm = duk__norm_idx(ctx, idx);
	return JS_ToCString(ctx->qjs, ctx->vstack[norm]);
}

const char *duk_to_string(duk_context *ctx, duk_idx_t idx)
{
	return duk_get_string(ctx, idx);
}

const char *duk_safe_to_string(duk_context *ctx, duk_idx_t idx)
{
	int norm = duk__norm_idx(ctx, idx);
	const char *str = JS_ToCString(ctx->qjs, ctx->vstack[norm]);
	if (str == NULL)
		return "null";
	return str;
}

const char *duk_get_lstring(duk_context *ctx, duk_idx_t idx, duk_size_t *len)
{
	int norm = duk__norm_idx(ctx, idx);
	return JS_ToCStringLen(ctx->qjs, len, ctx->vstack[norm]);
}

const char *duk_safe_to_lstring(duk_context *ctx, duk_idx_t idx,
				duk_size_t *len)
{
	int norm = duk__norm_idx(ctx, idx);
	const char *str = JS_ToCStringLen(ctx->qjs, len, ctx->vstack[norm]);
	if (str == NULL) {
		if (len) *len = 4;
		return "null";
	}
	return str;
}

/* ------------------------------------------------------------------ */
/* Prototype                                                           */
/* ------------------------------------------------------------------ */

void duk_set_prototype(duk_context *ctx, duk_idx_t obj_idx)
{
	int norm = duk__norm_idx(ctx, obj_idx);
	assert(ctx->top > 0);

	JSValue proto = ctx->vstack[ctx->top - 1];
	ctx->vstack[ctx->top - 1] = JS_UNDEFINED;
	ctx->top--;

	JS_SetPrototype(ctx->qjs, ctx->vstack[norm], proto);
	JS_FreeValue(ctx->qjs, proto);
}

void duk_get_prototype(duk_context *ctx, duk_idx_t obj_idx)
{
	int norm = duk__norm_idx(ctx, obj_idx);
	duk__ensure_stack(ctx, 1);
	ctx->vstack[ctx->top++] = JS_GetPrototype(ctx->qjs,
						   ctx->vstack[norm]);
}

/* ------------------------------------------------------------------ */
/* Length                                                               */
/* ------------------------------------------------------------------ */

duk_size_t duk_get_length(duk_context *ctx, duk_idx_t idx)
{
	int norm = duk__norm_idx(ctx, idx);
	JSValue lv = JS_GetPropertyStr(ctx->qjs, ctx->vstack[norm], "length");
	int32_t len = 0;
	JS_ToInt32(ctx->qjs, &len, lv);
	JS_FreeValue(ctx->qjs, lv);
	return (duk_size_t)(len > 0 ? len : 0);
}

/* ------------------------------------------------------------------ */
/* Error handling                                                      */
/* ------------------------------------------------------------------ */

duk_ret_t duk_error(duk_context *ctx, int code, const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	switch (code) {
	case DUK_ERR_TYPE_ERROR:
		JS_ThrowTypeError(ctx->qjs, "%s", buf);
		break;
	case DUK_ERR_RANGE_ERROR:
		JS_ThrowRangeError(ctx->qjs, "%s", buf);
		break;
	case DUK_ERR_REFERENCE_ERROR:
		JS_ThrowReferenceError(ctx->qjs, "%s", buf);
		break;
	case DUK_ERR_SYNTAX_ERROR:
		JS_ThrowSyntaxError(ctx->qjs, "%s", buf);
		break;
	default:
		JS_ThrowInternalError(ctx->qjs, "%s", buf);
		break;
	}

	return -1; /* signals error return to trampoline */
}

duk_ret_t duk_generic_error(duk_context *ctx, const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	JS_ThrowInternalError(ctx->qjs, "%s", buf);
	return -1;
}

duk_ret_t duk_type_error(duk_context *ctx, const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	JS_ThrowTypeError(ctx->qjs, "%s", buf);
	return -1;
}

duk_ret_t duk_range_error(duk_context *ctx, const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	JS_ThrowRangeError(ctx->qjs, "%s", buf);
	return -1;
}

/* ------------------------------------------------------------------ */
/* Safe call / pcall                                                   */
/* ------------------------------------------------------------------ */

duk_int_t duk_safe_call(duk_context *ctx, duk_safe_call_function func,
			void *udata, int nargs, int nrets)
{
	/* WHY: In Duktape, duk_safe_call creates a virtual stack frame.
	 * The called function sees nargs values at indices 0..nargs-1.
	 * We emulate this by temporarily adjusting ctx->base. */
	int old_base = ctx->base;
	int old_top = ctx->top;
	int new_base = old_top - nargs;

	assert(new_base >= old_base);
	ctx->base = new_base;

	duk_ret_t ret = func(ctx, udata);

	ctx->base = old_base;

	if (ret < 0) {
		/* Error: remove args, push error + padding */
		while (ctx->top > new_base) {
			ctx->top--;
			JS_FreeValue(ctx->qjs, ctx->vstack[ctx->top]);
			ctx->vstack[ctx->top] = JS_UNDEFINED;
		}
		duk_push_string(ctx, "Error in safe_call");
		while (ctx->top < new_base + nrets)
			duk_push_undefined(ctx);
		return DUK_EXEC_ERROR;
	}

	/* Success: keep top nrets values, remove everything else */
	if (nrets == 0) {
		while (ctx->top > new_base) {
			ctx->top--;
			JS_FreeValue(ctx->qjs, ctx->vstack[ctx->top]);
			ctx->vstack[ctx->top] = JS_UNDEFINED;
		}
	} else if (nrets >= 1) {
		/* Save the top nrets values */
		int result_start = ctx->top - nrets;
		if (result_start < new_base)
			result_start = new_base;
		int actual_nrets = ctx->top - result_start;

		/* Dup results so we can free the whole region */
		JSValue results[16]; /* nrets is typically 1 */
		assert(actual_nrets <= 16);
		for (int i = 0; i < actual_nrets; i++)
			results[i] = JS_DupValue(ctx->qjs,
						  ctx->vstack[result_start + i]);

		/* Free everything from new_base to top */
		while (ctx->top > new_base) {
			ctx->top--;
			JS_FreeValue(ctx->qjs, ctx->vstack[ctx->top]);
			ctx->vstack[ctx->top] = JS_UNDEFINED;
		}

		/* Push results back */
		for (int i = 0; i < actual_nrets; i++) {
			duk__ensure_stack(ctx, 1);
			ctx->vstack[ctx->top++] = results[i];
		}
		/* Pad remaining nrets with undefined */
		for (int i = actual_nrets; i < nrets; i++)
			duk_push_undefined(ctx);
	}

	return DUK_EXEC_SUCCESS;
}

duk_int_t duk_pcall(duk_context *ctx, int nargs)
{
	/* Stack: ... func arg0 arg1 ... argN
	 * func_idx is absolute (not frame-relative). */
	int func_idx = ctx->top - nargs - 1;
	if (func_idx < ctx->base)
		return DUK_EXEC_ERROR;

	/* Build QuickJS argument array */
	JSValue *args = NULL;
	if (nargs > 0) {
		args = malloc((size_t)nargs * sizeof(JSValue));
		if (args == NULL)
			return DUK_EXEC_ERROR;
		for (int i = 0; i < nargs; i++) {
			args[i] = JS_DupValue(ctx->qjs,
					      ctx->vstack[func_idx + 1 + i]);
		}
	}

	JSValue func = JS_DupValue(ctx->qjs, ctx->vstack[func_idx]);

	/* Pop function + args from compat stack (use absolute index) */
	while (ctx->top > func_idx) {
		ctx->top--;
		JS_FreeValue(ctx->qjs, ctx->vstack[ctx->top]);
		ctx->vstack[ctx->top] = JS_UNDEFINED;
	}

	/* WHY: JS_EVAL_FLAG_COMPILE_ONLY returns bytecode functions
	 * (JS_TAG_FUNCTION_BYTECODE) which must be run with
	 * JS_EvalFunction, not JS_Call. Detect and handle both. */
	JSValue result;
	if (JS_VALUE_GET_TAG(func) == JS_TAG_FUNCTION_BYTECODE) {
		/* Compiled bytecode from duk_pcompile -- evaluate it */
		result = JS_EvalFunction(ctx->qjs, func);
		/* JS_EvalFunction consumes func, do NOT free it */
	} else {
		JSValue this_obj = JS_GetGlobalObject(ctx->qjs);
		result = JS_Call(ctx->qjs, func, this_obj, nargs, args);
		JS_FreeValue(ctx->qjs, this_obj);
		JS_FreeValue(ctx->qjs, func);
	}

	if (args) {
		for (int i = 0; i < nargs; i++)
			JS_FreeValue(ctx->qjs, args[i]);
		free(args);
	}

	duk__ensure_stack(ctx, 1);
	if (JS_IsException(result)) {
		JSValue exc = JS_GetException(ctx->qjs);
		ctx->vstack[ctx->top++] = exc;
		return DUK_EXEC_ERROR;
	}

	ctx->vstack[ctx->top++] = result;
	return DUK_EXEC_SUCCESS;
}

duk_int_t duk_pcall_method(duk_context *ctx, int nargs)
{
	/* Stack: ... func this_val arg0 ... argN
	 * func_idx and this_idx are absolute (not frame-relative). */
	int func_idx = ctx->top - nargs - 2;
	int this_idx = func_idx + 1;
	if (func_idx < ctx->base)
		return DUK_EXEC_ERROR;

	JSValue *args = NULL;
	if (nargs > 0) {
		args = malloc((size_t)nargs * sizeof(JSValue));
		if (args == NULL)
			return DUK_EXEC_ERROR;
		for (int i = 0; i < nargs; i++) {
			args[i] = JS_DupValue(ctx->qjs,
					      ctx->vstack[this_idx + 1 + i]);
		}
	}

	JSValue func = JS_DupValue(ctx->qjs, ctx->vstack[func_idx]);
	JSValue this_val = JS_DupValue(ctx->qjs, ctx->vstack[this_idx]);

	/* Pop function + this + args (use absolute index) */
	while (ctx->top > func_idx) {
		ctx->top--;
		JS_FreeValue(ctx->qjs, ctx->vstack[ctx->top]);
		ctx->vstack[ctx->top] = JS_UNDEFINED;
	}

	JSValue result = JS_Call(ctx->qjs, func, this_val, nargs, args);

	JS_FreeValue(ctx->qjs, func);
	JS_FreeValue(ctx->qjs, this_val);
	if (args) {
		for (int i = 0; i < nargs; i++)
			JS_FreeValue(ctx->qjs, args[i]);
		free(args);
	}

	duk__ensure_stack(ctx, 1);
	if (JS_IsException(result)) {
		JSValue exc = JS_GetException(ctx->qjs);
		ctx->vstack[ctx->top++] = exc;
		return DUK_EXEC_ERROR;
	}

	ctx->vstack[ctx->top++] = result;
	return DUK_EXEC_SUCCESS;
}

void duk_call(duk_context *ctx, int nargs)
{
	duk_int_t rc = duk_pcall(ctx, nargs);
	if (rc != DUK_EXEC_SUCCESS) {
		/* In Duktape, duk_call throws on error. We just leave
		 * the error on the stack for now. */
	}
}

/* ------------------------------------------------------------------ */
/* Compilation                                                         */
/* ------------------------------------------------------------------ */

duk_int_t duk_pcompile_lstring_filename(duk_context *ctx, unsigned int flags,
					const char *src, duk_size_t len)
{
	/* Filename is on top of stack */
	assert(ctx->top > 0);

	const char *filename = JS_ToCString(ctx->qjs,
					    ctx->vstack[ctx->top - 1]);
	duk_pop(ctx); /* filename */

	int eval_flags = JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY;

	JSValue result = JS_Eval(ctx->qjs, src, len,
				 filename ? filename : "<input>", eval_flags);

	if (filename)
		JS_FreeCString(ctx->qjs, filename);

	if (JS_IsException(result)) {
		JSValue exc = JS_GetException(ctx->qjs);
		duk__ensure_stack(ctx, 1);
		ctx->vstack[ctx->top++] = exc;
		return DUK_EXEC_ERROR;
	}

	/* WHY: In Duktape, duk_pcompile returns a callable function object.
	 * JS_EVAL_FLAG_COMPILE_ONLY produces the same: a compiled function
	 * that the caller's duk_pcall(0) or duk_call(0) will execute. */
	duk__ensure_stack(ctx, 1);
	ctx->vstack[ctx->top++] = result;
	return DUK_EXEC_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Global object manipulation                                          */
/* ------------------------------------------------------------------ */

void duk_set_global_object(duk_context *ctx)
{
	/* WHY: In Duktape, this replaces the thread's global object so that
	 * all bare name lookups in JS code go through Window. In QuickJS
	 * we cannot replace the global, but we can:
	 * 1) Store the Window as a hidden property on the native global
	 *    so duk_push_global_object returns it.
	 * 2) Set the native global's prototype to Window so that JS code
	 *    looking up `document`, `location`, etc. traverses into Window.
	 */
	assert(ctx->top > 0);

	int norm = duk__norm_idx(ctx, -1);
	JSValue window = ctx->vstack[norm];

	/* Store on native global as hidden property */
	JSValue native_global = JS_GetGlobalObject(ctx->qjs);
	JS_SetPropertyStr(ctx->qjs, native_global, NS_WINDOW_KEY,
			  JS_DupValue(ctx->qjs, window));

	/* Set native global's prototype to Window so JS property lookups
	 * traverse into the Window object (e.g. `document`, `location`) */
	JS_SetPrototype(ctx->qjs, native_global, window);

	JS_FreeValue(ctx->qjs, native_global);

	/* Pop the Window from the compat stack (it is now ref'd by the
	 * native global's hidden property and prototype chain) */
	duk_pop(ctx);
}

/* ------------------------------------------------------------------ */
/* Stack trace                                                         */
/* ------------------------------------------------------------------ */

const char *duk_safe_to_stacktrace(duk_context *ctx, duk_idx_t idx)
{
	int norm = duk__norm_idx(ctx, idx);
	if (JS_IsObject(ctx->vstack[norm])) {
		JSValue stack = JS_GetPropertyStr(ctx->qjs,
						  ctx->vstack[norm],
						  "stack");
		if (!JS_IsUndefined(stack)) {
			/* Replace the error with stack string */
			JS_FreeValue(ctx->qjs, ctx->vstack[norm]);
			ctx->vstack[norm] = stack;
		} else {
			JS_FreeValue(ctx->qjs, stack);
		}
	}
	return duk_safe_to_string(ctx, idx);
}

/* ------------------------------------------------------------------ */
/* Thread                                                              */
/* ------------------------------------------------------------------ */

void duk_push_thread(duk_context *ctx)
{
	/* WHY: In Duktape, duk_push_thread creates a new coroutine thread
	 * sharing the same heap. In QuickJS, the equivalent is creating a
	 * new context sharing the same runtime. For now, push the current
	 * context as a placeholder. */
	duk_push_pointer(ctx, ctx);
}

duk_context *duk_require_context(duk_context *ctx, duk_idx_t idx)
{
	(void)idx;
	return ctx;
}

/* ------------------------------------------------------------------ */
/* Heap / memory                                                       */
/* ------------------------------------------------------------------ */

void duk_get_memory_functions(duk_context *ctx, duk_memory_functions *funcs)
{
	/* WHY: Some bridge code uses this to get the udata (jsheap pointer)
	 * from the context. Return the runtime opaque. */
	memset(funcs, 0, sizeof(*funcs));
	funcs->udata = JS_GetRuntimeOpaque(ctx->qjs_rt);
}

void duk_gc(duk_context *ctx, unsigned int flags)
{
	(void)flags;
	JS_RunGC(ctx->qjs_rt);
}

/* ------------------------------------------------------------------ */
/* def_prop                                                            */
/* ------------------------------------------------------------------ */

void duk_def_prop(duk_context *ctx, duk_idx_t obj_idx, unsigned int flags)
{
	int norm = duk__norm_idx(ctx, obj_idx);

	/* Stack layout depends on flags:
	 * If HAVE_VALUE: ... key value
	 * If HAVE_GETTER + HAVE_SETTER: ... key getter setter
	 * etc.
	 * Minimum: ... key */

	int qjs_flags = 0;

	if (flags & DUK_DEFPROP_HAVE_CONFIGURABLE) {
		qjs_flags |= JS_PROP_HAS_CONFIGURABLE;
		if (flags & DUK_DEFPROP_CONFIGURABLE)
			qjs_flags |= JS_PROP_CONFIGURABLE;
	}
	if (flags & DUK_DEFPROP_HAVE_ENUMERABLE) {
		qjs_flags |= JS_PROP_HAS_ENUMERABLE;
		if (flags & DUK_DEFPROP_ENUMERABLE)
			qjs_flags |= JS_PROP_ENUMERABLE;
	}
	if (flags & DUK_DEFPROP_HAVE_WRITABLE) {
		qjs_flags |= JS_PROP_HAS_WRITABLE;
		if (flags & DUK_DEFPROP_WRITABLE)
			qjs_flags |= JS_PROP_WRITABLE;
	}

	if (flags & DUK_DEFPROP_HAVE_VALUE) {
		/* Stack: ... key value */
		assert(ctx->top >= 2);
		JSValue val = ctx->vstack[ctx->top - 1];
		JSValue key = ctx->vstack[ctx->top - 2];
		ctx->vstack[ctx->top - 1] = JS_UNDEFINED;
		ctx->vstack[ctx->top - 2] = JS_UNDEFINED;
		ctx->top -= 2;

		const char *str = JS_ToCString(ctx->qjs, key);
		if (str) {
			JSAtom atom = JS_NewAtom(ctx->qjs, str);
			qjs_flags |= JS_PROP_HAS_VALUE;
			JS_DefineProperty(ctx->qjs, ctx->vstack[norm],
					  atom, val, JS_UNDEFINED,
					  JS_UNDEFINED, qjs_flags);
			JS_FreeAtom(ctx->qjs, atom);
			JS_FreeCString(ctx->qjs, str);
		}
		JS_FreeValue(ctx->qjs, key);
		JS_FreeValue(ctx->qjs, val);
	} else if ((flags & DUK_DEFPROP_HAVE_GETTER) &&
		   (flags & DUK_DEFPROP_HAVE_SETTER)) {
		/* Stack: ... key getter setter */
		assert(ctx->top >= 3);
		JSValue setter = ctx->vstack[ctx->top - 1];
		JSValue getter = ctx->vstack[ctx->top - 2];
		JSValue key = ctx->vstack[ctx->top - 3];
		ctx->vstack[ctx->top - 1] = JS_UNDEFINED;
		ctx->vstack[ctx->top - 2] = JS_UNDEFINED;
		ctx->vstack[ctx->top - 3] = JS_UNDEFINED;
		ctx->top -= 3;

		const char *str = JS_ToCString(ctx->qjs, key);
		if (str) {
			JSAtom atom = JS_NewAtom(ctx->qjs, str);
			qjs_flags |= JS_PROP_HAS_GET | JS_PROP_HAS_SET;
			JS_DefineProperty(ctx->qjs, ctx->vstack[norm],
					  atom, JS_UNDEFINED, getter,
					  setter, qjs_flags);
			JS_FreeAtom(ctx->qjs, atom);
			JS_FreeCString(ctx->qjs, str);
		}
		JS_FreeValue(ctx->qjs, key);
		JS_FreeValue(ctx->qjs, getter);
		JS_FreeValue(ctx->qjs, setter);
	} else if (flags & DUK_DEFPROP_HAVE_GETTER) {
		/* Stack: ... key getter (no setter) */
		assert(ctx->top >= 2);
		JSValue getter = ctx->vstack[ctx->top - 1];
		JSValue key = ctx->vstack[ctx->top - 2];
		ctx->vstack[ctx->top - 1] = JS_UNDEFINED;
		ctx->vstack[ctx->top - 2] = JS_UNDEFINED;
		ctx->top -= 2;

		const char *str = JS_ToCString(ctx->qjs, key);
		if (str) {
			JSAtom atom = JS_NewAtom(ctx->qjs, str);
			qjs_flags |= JS_PROP_HAS_GET;
			JS_DefineProperty(ctx->qjs, ctx->vstack[norm],
					  atom, JS_UNDEFINED, getter,
					  JS_UNDEFINED, qjs_flags);
			JS_FreeAtom(ctx->qjs, atom);
			JS_FreeCString(ctx->qjs, str);
		}
		JS_FreeValue(ctx->qjs, key);
		JS_FreeValue(ctx->qjs, getter);
	} else if (flags & DUK_DEFPROP_HAVE_SETTER) {
		/* Stack: ... key setter (no getter) */
		assert(ctx->top >= 2);
		JSValue setter = ctx->vstack[ctx->top - 1];
		JSValue key = ctx->vstack[ctx->top - 2];
		ctx->vstack[ctx->top - 1] = JS_UNDEFINED;
		ctx->vstack[ctx->top - 2] = JS_UNDEFINED;
		ctx->top -= 2;

		const char *str = JS_ToCString(ctx->qjs, key);
		if (str) {
			JSAtom atom = JS_NewAtom(ctx->qjs, str);
			qjs_flags |= JS_PROP_HAS_SET;
			JS_DefineProperty(ctx->qjs, ctx->vstack[norm],
					  atom, JS_UNDEFINED, JS_UNDEFINED,
					  setter, qjs_flags);
			JS_FreeAtom(ctx->qjs, atom);
			JS_FreeCString(ctx->qjs, str);
		}
		JS_FreeValue(ctx->qjs, key);
		JS_FreeValue(ctx->qjs, setter);
	} else {
		/* Just a key with attribute changes */
		assert(ctx->top >= 1);
		JSValue key = ctx->vstack[ctx->top - 1];
		ctx->vstack[ctx->top - 1] = JS_UNDEFINED;
		ctx->top--;

		const char *str = JS_ToCString(ctx->qjs, key);
		if (str) {
			JSAtom atom = JS_NewAtom(ctx->qjs, str);
			JS_DefineProperty(ctx->qjs, ctx->vstack[norm],
					  atom, JS_UNDEFINED, JS_UNDEFINED,
					  JS_UNDEFINED, qjs_flags);
			JS_FreeAtom(ctx->qjs, atom);
			JS_FreeCString(ctx->qjs, str);
		}
		JS_FreeValue(ctx->qjs, key);
	}
}

/* ------------------------------------------------------------------ */
/* Stash                                                               */
/* ------------------------------------------------------------------ */

static const char ns_stash_key[] = "\xFF\xFF__ns_stash";
static const char ns_heap_stash_key[] = "\xFF\xFF__ns_heap_stash";

void duk_push_global_stash(duk_context *ctx)
{
	duk_get_global_string(ctx, ns_stash_key);
	if (duk_is_undefined(ctx, -1)) {
		duk_pop(ctx);
		duk_push_object(ctx);
		duk_dup(ctx, -1);
		duk_put_global_string(ctx, ns_stash_key);
	}
}

void duk_push_heap_stash(duk_context *ctx)
{
	duk_get_global_string(ctx, ns_heap_stash_key);
	if (duk_is_undefined(ctx, -1)) {
		duk_pop(ctx);
		duk_push_object(ctx);
		duk_dup(ctx, -1);
		duk_put_global_string(ctx, ns_heap_stash_key);
	}
}

/* ------------------------------------------------------------------ */
/* Context dump (debug)                                                */
/* ------------------------------------------------------------------ */

void duk_push_context_dump(duk_context *ctx)
{
	char buf[256];
	snprintf(buf, sizeof(buf), "[enhanced stack: top=%d]", ctx->top);
	duk_push_string(ctx, buf);
}

/* ------------------------------------------------------------------ */
/* Require (coercion with type assertion)                              */
/* ------------------------------------------------------------------ */

const char *duk_require_string(duk_context *ctx, duk_idx_t idx)
{
	return duk_get_string(ctx, idx);
}

const char *duk_require_lstring(duk_context *ctx, duk_idx_t idx,
				duk_size_t *len)
{
	return duk_get_lstring(ctx, idx, len);
}

/* ------------------------------------------------------------------ */
/* To-coercion variants                                                */
/* ------------------------------------------------------------------ */

const char *duk_to_lstring(duk_context *ctx, duk_idx_t idx, duk_size_t *len)
{
	return duk_get_lstring(ctx, idx, len);
}

/* ------------------------------------------------------------------ */
/* lstring property access                                             */
/* ------------------------------------------------------------------ */

duk_bool_t duk_get_prop_lstring(duk_context *ctx, duk_idx_t obj_idx,
				const char *key, duk_size_t key_len)
{
	int norm = duk__norm_idx(ctx, obj_idx);
	JSAtom atom = JS_NewAtomLen(ctx->qjs, key, key_len);
	JSValue result = JS_GetProperty(ctx->qjs, ctx->vstack[norm], atom);
	JS_FreeAtom(ctx->qjs, atom);

	duk__ensure_stack(ctx, 1);
	ctx->vstack[ctx->top++] = result;

	return !JS_IsUndefined(result);
}

void duk_put_prop_lstring(duk_context *ctx, duk_idx_t obj_idx,
			  const char *key, duk_size_t key_len)
{
	int norm = duk__norm_idx(ctx, obj_idx);
	assert(ctx->top > 0);

	JSValue val = ctx->vstack[ctx->top - 1];
	ctx->vstack[ctx->top - 1] = JS_UNDEFINED;
	ctx->top--;

	JSAtom atom = JS_NewAtomLen(ctx->qjs, key, key_len);
	JS_SetProperty(ctx->qjs, ctx->vstack[norm], atom, val);
	JS_FreeAtom(ctx->qjs, atom);
}

duk_bool_t duk_del_prop_lstring(duk_context *ctx, duk_idx_t obj_idx,
				const char *key, duk_size_t key_len)
{
	int norm = duk__norm_idx(ctx, obj_idx);
	JSAtom atom = JS_NewAtomLen(ctx->qjs, key, key_len);
	int ret = JS_DeleteProperty(ctx->qjs, ctx->vstack[norm], atom, 0);
	JS_FreeAtom(ctx->qjs, atom);
	return ret >= 0;
}

duk_bool_t duk_has_prop(duk_context *ctx, duk_idx_t obj_idx)
{
	int norm = duk__norm_idx(ctx, obj_idx);
	assert(ctx->top > 0);

	JSValue key = ctx->vstack[ctx->top - 1];
	ctx->vstack[ctx->top - 1] = JS_UNDEFINED;
	ctx->top--;

	const char *str = JS_ToCString(ctx->qjs, key);
	int ret = 0;
	if (str) {
		JSAtom atom = JS_NewAtom(ctx->qjs, str);
		ret = JS_HasProperty(ctx->qjs, ctx->vstack[norm], atom);
		JS_FreeAtom(ctx->qjs, atom);
		JS_FreeCString(ctx->qjs, str);
	}
	JS_FreeValue(ctx->qjs, key);

	return ret > 0;
}

/* ------------------------------------------------------------------ */
/* Finalizer                                                           */
/* ------------------------------------------------------------------ */

void duk_set_finalizer(duk_context *ctx, duk_idx_t obj_idx)
{
	/* WHY: In Duktape, duk_set_finalizer takes a finalizer function
	 * from the top of the stack and attaches it to the object. QuickJS
	 * uses class-based finalizers (JSClassDef.finalizer) instead.
	 * For the compat shim, we simply consume the finalizer value from
	 * the stack since QuickJS handles cleanup via class finalizers. */
	(void)obj_idx;
	assert(ctx->top > 0);
	duk_pop(ctx); /* consume the finalizer function */
}

/* ------------------------------------------------------------------ */
/* String operations                                                   */
/* ------------------------------------------------------------------ */

void duk_concat(duk_context *ctx, int count)
{
	if (count <= 0)
		return;
	if (count == 1)
		return; /* single value stays as-is */

	/* Concatenate 'count' string values from top of stack */
	int start = ctx->top - count;
	assert(start >= 0);

	/* Build concatenated string */
	size_t total = 0;
	const char **strs = calloc((size_t)count, sizeof(const char *));
	size_t *lens = calloc((size_t)count, sizeof(size_t));
	if (strs == NULL || lens == NULL) {
		free(strs);
		free(lens);
		return;
	}

	for (int i = 0; i < count; i++) {
		size_t len = 0;
		strs[i] = JS_ToCStringLen(ctx->qjs, &len,
					   ctx->vstack[start + i]);
		lens[i] = strs[i] ? len : 0;
		total += lens[i];
	}

	char *buf = malloc(total + 1);
	if (buf) {
		size_t pos = 0;
		for (int i = 0; i < count; i++) {
			if (strs[i]) {
				memcpy(buf + pos, strs[i], lens[i]);
				pos += lens[i];
			}
		}
		buf[pos] = '\0';
	}

	for (int i = 0; i < count; i++) {
		if (strs[i])
			JS_FreeCString(ctx->qjs, strs[i]);
	}
	free(strs);
	free(lens);

	/* Pop count values, push result */
	for (int i = 0; i < count; i++)
		duk_pop(ctx);

	if (buf) {
		duk_push_lstring(ctx, buf, total);
		free(buf);
	} else {
		duk_push_string(ctx, "");
	}
}

void duk_push_sprintf(duk_context *ctx, const char *fmt, ...)
{
	char buf[1024];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	duk_push_string(ctx, buf);
}

/* ------------------------------------------------------------------ */
/* Comparison                                                          */
/* ------------------------------------------------------------------ */

duk_bool_t duk_strict_equals(duk_context *ctx, duk_idx_t a, duk_idx_t b)
{
	int na = duk__norm_idx(ctx, a);
	int nb = duk__norm_idx(ctx, b);
	JSValue va = ctx->vstack[na];
	JSValue vb = ctx->vstack[nb];

	/* Use JS_IsStrictEqual for strict equality (===) */
	return JS_IsStrictEqual(ctx->qjs, va, vb);
}

/* ------------------------------------------------------------------ */
/* Enumeration                                                         */
/* ------------------------------------------------------------------ */

void duk_enum(duk_context *ctx, duk_idx_t obj_idx, unsigned int flags)
{
	int norm = duk__norm_idx(ctx, obj_idx);
	(void)flags;

	/* Get property names as an array and push an iterator-like object.
	 * WHY: Duktape's duk_enum creates an enumerator object that
	 * iterates over keys. We use QuickJS's JS_GetOwnPropertyNames
	 * to get property names, then push an object that duk_next
	 * will consume. */

	JSPropertyEnum *tab = NULL;
	uint32_t len = 0;
	int gflags = JS_GPN_STRING_MASK;
	if (flags & DUK_ENUM_OWN_PROPERTIES_ONLY)
		gflags |= JS_GPN_ENUM_ONLY;

	JS_GetOwnPropertyNames(ctx->qjs, &tab, &len,
				ctx->vstack[norm], gflags);

	/* Create an array of key strings */
	duk__ensure_stack(ctx, 1);
	JSValue arr = JS_NewArray(ctx->qjs);
	for (uint32_t i = 0; i < len; i++) {
		JSValue key = JS_AtomToString(ctx->qjs, tab[i].atom);
		JS_SetPropertyUint32(ctx->qjs, arr, i, key);
	}

	/* Store length and current index as properties */
	JS_SetPropertyStr(ctx->qjs, arr, "__enum_len",
			  JS_NewInt32(ctx->qjs, (int32_t)len));
	JS_SetPropertyStr(ctx->qjs, arr, "__enum_idx",
			  JS_NewInt32(ctx->qjs, 0));

	/* Also store the source object for value lookups */
	JS_SetPropertyStr(ctx->qjs, arr, "__enum_obj",
			  JS_DupValue(ctx->qjs, ctx->vstack[norm]));

	ctx->vstack[ctx->top++] = arr;

	/* Free the property enum tab */
	for (uint32_t i = 0; i < len; i++)
		JS_FreeAtom(ctx->qjs, tab[i].atom);
	js_free(ctx->qjs, tab);
}

duk_bool_t duk_next(duk_context *ctx, duk_idx_t enum_idx,
		    duk_bool_t get_value)
{
	int norm = duk__norm_idx(ctx, enum_idx);
	JSValue arr = ctx->vstack[norm];

	/* Get current index and length */
	JSValue idx_val = JS_GetPropertyStr(ctx->qjs, arr, "__enum_idx");
	JSValue len_val = JS_GetPropertyStr(ctx->qjs, arr, "__enum_len");
	int32_t cur = 0, len = 0;
	JS_ToInt32(ctx->qjs, &cur, idx_val);
	JS_ToInt32(ctx->qjs, &len, len_val);
	JS_FreeValue(ctx->qjs, idx_val);
	JS_FreeValue(ctx->qjs, len_val);

	if (cur >= len)
		return 0; /* no more entries */

	/* Get the key at current index */
	JSValue key = JS_GetPropertyUint32(ctx->qjs, arr, (uint32_t)cur);

	/* Advance the index */
	JS_SetPropertyStr(ctx->qjs, arr, "__enum_idx",
			  JS_NewInt32(ctx->qjs, cur + 1));

	/* Push key (and optionally value) */
	duk__ensure_stack(ctx, get_value ? 2 : 1);
	ctx->vstack[ctx->top++] = key;

	if (get_value) {
		JSValue obj = JS_GetPropertyStr(ctx->qjs, arr, "__enum_obj");
		const char *str = JS_ToCString(ctx->qjs, key);
		JSValue val = JS_UNDEFINED;
		if (str) {
			val = JS_GetPropertyStr(ctx->qjs, obj, str);
			JS_FreeCString(ctx->qjs, str);
		}
		JS_FreeValue(ctx->qjs, obj);
		ctx->vstack[ctx->top++] = val;
	}

	return 1; /* has more */
}

/* ------------------------------------------------------------------ */
/* Buffer API                                                          */
/* ------------------------------------------------------------------ */

/* WHY: Duktape's buffer API stores raw byte data. QuickJS uses
 * ArrayBuffer/TypedArray. We emulate with ArrayBuffer for simplicity. */

void *duk_push_buffer_raw(duk_context *ctx, duk_size_t size,
			  unsigned int flags)
{
	(void)flags;
	duk__ensure_stack(ctx, 1);

	/* Create an ArrayBuffer */
	JSValue ab = JS_NewArrayBufferCopy(ctx->qjs, NULL, size);
	ctx->vstack[ctx->top++] = ab;

	size_t out_size = 0;
	return JS_GetArrayBuffer(ctx->qjs, &out_size, ab);
}

void *duk_push_buffer(duk_context *ctx, duk_size_t size, duk_bool_t dynamic)
{
	unsigned int flags = dynamic ? DUK_BUF_FLAG_DYNAMIC : 0;
	return duk_push_buffer_raw(ctx, size, flags);
}

void *duk_push_fixed_buffer(duk_context *ctx, duk_size_t size)
{
	return duk_push_buffer_raw(ctx, size, 0);
}

void duk_push_buffer_object(duk_context *ctx, duk_idx_t buf_idx,
			    duk_size_t offset, duk_size_t length,
			    unsigned int type)
{
	(void)type; /* always Uint8Array for now */
	int norm = duk__norm_idx(ctx, buf_idx);

	duk__ensure_stack(ctx, 1);

	/* Create a Uint8Array view over the ArrayBuffer */
	JSValue globals = JS_GetGlobalObject(ctx->qjs);
	JSValue u8a_ctor = JS_GetPropertyStr(ctx->qjs, globals,
					     "Uint8Array");
	JSValue args[3];
	args[0] = JS_DupValue(ctx->qjs, ctx->vstack[norm]);
	args[1] = JS_NewInt32(ctx->qjs, (int32_t)offset);
	args[2] = JS_NewInt32(ctx->qjs, (int32_t)length);

	JSValue result = JS_CallConstructor(ctx->qjs, u8a_ctor, 3, args);

	JS_FreeValue(ctx->qjs, args[0]);
	JS_FreeValue(ctx->qjs, args[1]);
	JS_FreeValue(ctx->qjs, args[2]);
	JS_FreeValue(ctx->qjs, u8a_ctor);
	JS_FreeValue(ctx->qjs, globals);

	ctx->vstack[ctx->top++] = result;
}

void *duk_get_buffer_data(duk_context *ctx, duk_idx_t idx, duk_size_t *size)
{
	int norm = duk__norm_idx(ctx, idx);
	JSValue v = ctx->vstack[norm];
	size_t len = 0;
	uint8_t *ptr;

	/* Try ArrayBuffer first */
	ptr = JS_GetArrayBuffer(ctx->qjs, &len, v);
	if (ptr) {
		if (size) *size = len;
		return ptr;
	}

	/* Try TypedArray */
	size_t off = 0, blen = 0;
	JSValue ab = JS_GetTypedArrayBuffer(ctx->qjs, v, &off, &blen, NULL);
	if (!JS_IsException(ab)) {
		ptr = JS_GetArrayBuffer(ctx->qjs, &len, ab);
		JS_FreeValue(ctx->qjs, ab);
		if (ptr) {
			if (size) *size = blen;
			return ptr + off;
		}
	}

	if (size) *size = 0;
	return NULL;
}

duk_bool_t duk_is_buffer_data(duk_context *ctx, duk_idx_t idx)
{
	duk_size_t sz = 0;
	return duk_get_buffer_data(ctx, idx, &sz) != NULL;
}

/* ------------------------------------------------------------------ */
/* Error object creation                                               */
/* ------------------------------------------------------------------ */

duk_idx_t duk_push_error_object(duk_context *ctx, int code,
				const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	duk__ensure_stack(ctx, 1);

	/* Create an Error object with the message */
	JSValue globals = JS_GetGlobalObject(ctx->qjs);
	const char *ctor_name;

	switch (code) {
	case DUK_ERR_TYPE_ERROR:
		ctor_name = "TypeError";
		break;
	case DUK_ERR_RANGE_ERROR:
		ctor_name = "RangeError";
		break;
	case DUK_ERR_REFERENCE_ERROR:
		ctor_name = "ReferenceError";
		break;
	case DUK_ERR_SYNTAX_ERROR:
		ctor_name = "SyntaxError";
		break;
	default:
		ctor_name = "Error";
		break;
	}

	JSValue ctor = JS_GetPropertyStr(ctx->qjs, globals, ctor_name);
	JSValue msg = JS_NewString(ctx->qjs, buf);
	JSValue err = JS_CallConstructor(ctx->qjs, ctor, 1, &msg);

	JS_FreeValue(ctx->qjs, msg);
	JS_FreeValue(ctx->qjs, ctor);
	JS_FreeValue(ctx->qjs, globals);

	ctx->vstack[ctx->top] = err;
	return ctx->top++;
}

duk_ret_t duk_error_raw(duk_context *ctx, int code, const char *filename,
			int line, const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	(void)filename;
	(void)line;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	return duk_error(ctx, code, "%s", buf);
}

/* ------------------------------------------------------------------ */
/* Compilation (additional forms)                                      */
/* ------------------------------------------------------------------ */

duk_int_t duk_pcompile(duk_context *ctx, unsigned int flags)
{
	/* Stack: ... source filename
	 * In Duktape, duk_pcompile compiles source from stack[-2]
	 * with filename from stack[-1]. */
	assert(ctx->top >= 2);

	const char *filename = JS_ToCString(ctx->qjs,
					    ctx->vstack[ctx->top - 1]);
	size_t srclen = 0;
	const char *src = JS_ToCStringLen(ctx->qjs, &srclen,
					   ctx->vstack[ctx->top - 2]);

	/* Pop source and filename */
	duk_pop_2(ctx);

	int eval_flags = JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY;

	JSValue result = JS_UNDEFINED;
	if (src) {
		result = JS_Eval(ctx->qjs, src, srclen,
				 filename ? filename : "<input>",
				 eval_flags);
	}

	if (src)
		JS_FreeCString(ctx->qjs, src);
	if (filename)
		JS_FreeCString(ctx->qjs, filename);

	if (JS_IsException(result)) {
		JSValue exc = JS_GetException(ctx->qjs);
		duk__ensure_stack(ctx, 1);
		ctx->vstack[ctx->top++] = exc;
		return DUK_EXEC_ERROR;
	}

	duk__ensure_stack(ctx, 1);
	ctx->vstack[ctx->top++] = result;
	return DUK_EXEC_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Heap lifecycle (for dukky.c compat)                                 */
/* ------------------------------------------------------------------ */

duk_context *duk_create_heap(void *alloc_func, void *realloc_func,
			     void *free_func, void *heap_udata,
			     duk_fatal_function fatal_handler)
{
	/* WHY: In enhanced mode, heap creation is done by bridge.c's
	 * js_newheap / js_newthread. This function exists only so that
	 * dukky.c compiles. If called, it creates a compat context
	 * from the thread's existing QuickJS context. */
	(void)alloc_func;
	(void)realloc_func;
	(void)free_func;
	(void)fatal_handler;

	/* Retrieve the jsthread from heap_udata (which is the jsheap) */
	duk_context *ctx = calloc(1, sizeof(duk_context));
	if (ctx == NULL)
		return NULL;

	/* This will be properly initialized when bridge.c calls into
	 * the binding code. For now just set up the stack. */
	ctx->capacity = DUK_COMPAT_INITIAL_STACK;
	ctx->vstack = calloc((size_t)ctx->capacity, sizeof(JSValue));
	if (ctx->vstack == NULL) {
		free(ctx);
		return NULL;
	}
	for (int i = 0; i < ctx->capacity; i++)
		ctx->vstack[i] = JS_UNDEFINED;

	/* Store heap_udata for duk_get_memory_functions */
	(void)heap_udata;

	return ctx;
}

void duk_destroy_heap(duk_context *ctx)
{
	if (ctx == NULL)
		return;

	/* Free compat stack values */
	if (ctx->vstack) {
		for (int i = 0; i < ctx->top; i++) {
			if (ctx->qjs)
				JS_FreeValue(ctx->qjs, ctx->vstack[i]);
		}
		free(ctx->vstack);
	}
	free(ctx);
}

/* ------------------------------------------------------------------ */
/* Persistent compat context lifecycle                                 */
/* ------------------------------------------------------------------ */

duk_context *duk_compat_create(JSContext *qjs)
{
	duk_context *ctx = calloc(1, sizeof(duk_context));
	if (ctx == NULL)
		return NULL;

	ctx->qjs = qjs;
	ctx->qjs_rt = JS_GetRuntime(qjs);
	ctx->capacity = DUK_COMPAT_INITIAL_STACK;
	ctx->vstack = calloc((size_t)ctx->capacity, sizeof(JSValue));
	if (ctx->vstack == NULL) {
		free(ctx);
		return NULL;
	}

	for (int i = 0; i < ctx->capacity; i++)
		ctx->vstack[i] = JS_UNDEFINED;

	ctx->this_val = JS_UNDEFINED;

	duk_compat_ensure_pointer_class(qjs);

	return ctx;
}

void duk_compat_destroy(duk_context *ctx)
{
	if (ctx == NULL)
		return;

	if (ctx->vstack) {
		for (int i = 0; i < ctx->top; i++) {
			if (ctx->qjs)
				JS_FreeValue(ctx->qjs, ctx->vstack[i]);
		}
		free(ctx->vstack);
	}

	if (ctx->qjs && !JS_IsUndefined(ctx->this_val))
		JS_FreeValue(ctx->qjs, ctx->this_val);

	free(ctx);
}
