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
 * Enhanced JavaScript engine bridge: implements js.h using QuickJS-NG.
 *
 * WHY: QuickJS-NG provides ES2023 support (arrow functions, classes,
 * template literals, destructuring, modules, async/await) that Duktape
 * 2.7.0 (ES5.1) cannot offer. This bridge maps NetSurf's js.h API
 * to QuickJS-NG's value-based API.
 *
 * WHAT: Implements the 10 js.h functions plus the js_dom_event_add_listener
 * helper. Full DOM binding integration is provided through the duk_compat
 * shim and nsgenbind-generated bindings, matching the Duktape engine's
 * feature set.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <nsutils/time.h>
#include <dom/dom.h>

#include "utils/errors.h"
#include "utils/nsoption.h"
#include "utils/log.h"
#include "utils/corestrings.h"
#include "content/content.h"

#include "javascript/js.h"
#include "javascript/content.h"

#include "content/handlers/javascript/enhanced/engine.h"
#include "content/handlers/javascript/enhanced/bridge.h"
#include "content/handlers/javascript/enhanced/duk_compat.h"

#include "javascript/duktape/duktape.h"
#include "duktape/binding.h"
#include "duktape/private.h"
#include "javascript/duktape/dukky.h"

/* Embedded JS sources (generated from .js -> .inc by xxd) */
#include "duktape/generics.js.inc"
#include "duktape/polyfill.js.inc"

/* Magic string aliases matching dukky.c */
#define EVENT_MAGIC MAGIC(EVENT_MAP)
#define HANDLER_MAGIC MAGIC(HANDLER_MAP)
#define GENERICS_MAGIC MAGIC(GENERICS_TABLE)

/* Forward declarations for functions defined in dukky_stubs.c (enhanced mode).
 * These are static in dukky.c (standard mode) so cannot go in dukky.h. */
duk_ret_t dukky_url_constructor(duk_context *ctx);
duk_ret_t dukky_urlsearchparams_constructor(duk_context *ctx);
duk_ret_t dukky_url_tostring(duk_context *ctx);
duk_ret_t dukky_url_tojson(duk_context *ctx);
duk_ret_t dukky_urlsearchparams_tostring(duk_context *ctx);

/* Execution timeout in milliseconds */
#define JS_EXEC_TIMEOUT_MS 10000

/* ------------------------------------------------------------------ */
/* Thread tracking                                                     */
/* ------------------------------------------------------------------ */

jsthread *enhanced_get_thread(JSContext *ctx)
{
	return (jsthread *)JS_GetContextOpaque(ctx);
}

void enhanced_enter_thread(jsthread *thread)
{
	assert(thread != NULL);
	thread->in_use++;
}

void enhanced_leave_thread(jsthread *thread)
{
	assert(thread != NULL);
	assert(thread->in_use > 0);
	thread->in_use--;
	if (thread->in_use == 0 && thread->pending_destroy) {
		js_destroythread(thread);
	}
}

/* ------------------------------------------------------------------ */
/* Microtask flush (Promise reactions)                                  */
/* ------------------------------------------------------------------ */

/**
 * Drain all pending microtask jobs (Promise reactions).
 *
 * WHY: QuickJS-NG queues Promise reactions via JS_EnqueueJob.
 * The embedder must drain after each top-level JS entry point
 * (js_exec, js_fire_event).  Without this call, Promise .then /
 * .catch / .finally callbacks never fire.
 *
 * The enhanced_interrupt_handler timeout applies to microtask
 * execution too, preventing infinite loops in Promise chains.
 */
static void enhanced_flush_microtasks(jsthread *thread)
{
	JSContext *pctx;
	int ret;

	for (;;) {
		ret = JS_ExecutePendingJob(thread->heap->rt, &pctx);
		if (ret <= 0) {
			if (ret < 0) {
				JSValue exc = JS_GetException(pctx);
				const char *str = JS_ToCString(pctx, exc);
				if (str) {
					NSLOG(jserrors, WARNING,
					      "Uncaught error in Promise job: %s",
					      str);
					JS_FreeCString(pctx, str);
				}
				JS_FreeValue(pctx, exc);
				/* continue draining -- other jobs may be queued */
			} else {
				break; /* ret == 0: no more jobs */
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* Interrupt handler for execution timeout                             */
/* ------------------------------------------------------------------ */

static int enhanced_interrupt_handler(JSRuntime *rt, void *opaque)
{
	(void)rt;
	jsheap *heap = (jsheap *)opaque;
	uint64_t now;

	if (heap->exec_start_time == 0)
		return 0;

	(void)nsu_getmonotonic_ms(&now);
	if (now > (heap->exec_start_time + JS_EXEC_TIMEOUT_MS)) {
		NSLOG(jserrors, WARNING,
		      "JavaScript execution timeout (%d ms)",
		      JS_EXEC_TIMEOUT_MS);
		return 1; /* interrupt */
	}
	return 0;
}

static void enhanced_reset_start_time(jsheap *heap)
{
	(void)nsu_getmonotonic_ms(&heap->exec_start_time);
}

/* ------------------------------------------------------------------ */
/* Console implementation (native QuickJS, routes through monkey)      */
/* ------------------------------------------------------------------ */

/* WHY: The binding-based Console (Console.bnd) uses dukky_push_generics
 * and browser_window_console_log to route output through the monkey
 * protocol. However, the binding Console throws in enhanced mode
 * (generics/formatter issues). This native console implementation
 * calls browser_window_console_log directly, ensuring output goes
 * through the monkey protocol for test harness visibility. */

#include "netsurf/console.h"
#include "netsurf/browser_window.h"

static JSValue enhanced_console_method(JSContext *ctx, JSValueConst this_val,
				       int argc, JSValueConst *argv,
				       int level)
{
	(void)this_val;

	/* Build the concatenated message */
	char buf[4096];
	int pos = 0;
	for (int i = 0; i < argc && pos < (int)sizeof(buf) - 1; i++) {
		if (i != 0 && pos < (int)sizeof(buf) - 1)
			buf[pos++] = ' ';
		const char *str = JS_ToCString(ctx, argv[i]);
		if (str) {
			int slen = (int)strlen(str);
			if (slen > (int)sizeof(buf) - 1 - pos)
				slen = (int)sizeof(buf) - 1 - pos;
			memcpy(buf + pos, str, slen);
			pos += slen;
			JS_FreeCString(ctx, str);
		}
	}
	buf[pos] = '\0';

	/* Log through NSLOG for stderr visibility */
	switch (level) {
	case 0: NSLOG(jserrors, INFO, "%s", buf); break;
	case 1: NSLOG(jserrors, WARNING, "%s", buf); break;
	case 2: NSLOG(jserrors, ERROR, "%s", buf); break;
	}

	/* Route through browser_window_console_log for monkey protocol.
	 * Get the browser_window from the jsthread. */
	struct jsthread *thread = enhanced_get_thread(ctx);
	if (thread != NULL && thread->compat_ctx != NULL) {
		duk_context *dctx = thread->compat_ctx;
		duk_push_global_object(dctx);
		duk_get_prop_string(dctx, -1, MAGIC(PRIVATE));
		window_private_t *priv_win = duk_get_pointer(dctx, -1);
		duk_pop_2(dctx);
		if (priv_win != NULL && priv_win->win != NULL &&
		    !priv_win->closed_down) {
			browser_window_console_flags flags;
			switch (level) {
			case 1:  flags = BW_CS_FLAG_LEVEL_WARN; break;
			case 2:  flags = BW_CS_FLAG_LEVEL_ERROR; break;
			default: flags = BW_CS_FLAG_LEVEL_LOG; break;
			}
			browser_window_console_log(priv_win->win,
						   BW_CS_SCRIPT_CONSOLE,
						   buf, (size_t)pos,
						   flags);
		}
	}

	return JS_UNDEFINED;
}

static JSValue enhanced_console_log(JSContext *ctx, JSValueConst this_val,
				    int argc, JSValueConst *argv)
{
	return enhanced_console_method(ctx, this_val, argc, argv, 0);
}

static JSValue enhanced_console_warn(JSContext *ctx, JSValueConst this_val,
				     int argc, JSValueConst *argv)
{
	return enhanced_console_method(ctx, this_val, argc, argv, 1);
}

static JSValue enhanced_console_error(JSContext *ctx, JSValueConst this_val,
				      int argc, JSValueConst *argv)
{
	return enhanced_console_method(ctx, this_val, argc, argv, 2);
}

static void enhanced_install_console(JSContext *ctx)
{
	JSValue global, console;

	global = JS_GetGlobalObject(ctx);
	console = JS_NewObject(ctx);

	JS_SetPropertyStr(ctx, console, "log",
			  JS_NewCFunction(ctx, enhanced_console_log,
					  "log", 0));
	JS_SetPropertyStr(ctx, console, "info",
			  JS_NewCFunction(ctx, enhanced_console_log,
					  "info", 0));
	JS_SetPropertyStr(ctx, console, "debug",
			  JS_NewCFunction(ctx, enhanced_console_log,
					  "debug", 0));
	JS_SetPropertyStr(ctx, console, "warn",
			  JS_NewCFunction(ctx, enhanced_console_warn,
					  "warn", 0));
	JS_SetPropertyStr(ctx, console, "error",
			  JS_NewCFunction(ctx, enhanced_console_error,
					  "error", 0));

	JS_SetPropertyStr(ctx, global, "console", console);
	JS_FreeValue(ctx, global);
}

/* ------------------------------------------------------------------ */
/* js.h API implementation                                             */
/* ------------------------------------------------------------------ */

/* exported interface documented in js.h */
void js_initialise(void)
{
	javascript_init();
}

/* exported interface documented in js.h */
void js_finalise(void)
{
	/* Nothing to do globally */
}

/* exported interface documented in js.h */
nserror js_newheap(int timeout, jsheap **heap)
{
	jsheap *ret;

	NSLOG(dukky, DEBUG, "Creating new enhanced javascript heap");

	ret = calloc(1, sizeof(*ret));
	if (ret == NULL) {
		*heap = NULL;
		return NSERROR_NOMEM;
	}

	ret->rt = JS_NewRuntime();
	if (ret->rt == NULL) {
		free(ret);
		*heap = NULL;
		return NSERROR_NOMEM;
	}

	ret->timeout = timeout;
	ret->heap_limit = (size_t)nsoption_int(js_heap_limit);

	if (ret->heap_limit > 0) {
		JS_SetMemoryLimit(ret->rt, ret->heap_limit);
	}

	/* 8 MB max stack size (matching Duktape defaults) */
	JS_SetMaxStackSize(ret->rt, 8 * 1024 * 1024);

	JS_SetRuntimeOpaque(ret->rt, ret);
	JS_SetInterruptHandler(ret->rt, enhanced_interrupt_handler, ret);

	*heap = ret;
	return NSERROR_OK;
}

static void enhanced_destroyheap(jsheap *heap)
{
	assert(heap->pending_destroy);
	assert(heap->live_threads == 0);
	NSLOG(dukky, DEBUG, "Destroying enhanced javascript heap");
	JS_FreeRuntime(heap->rt);
	free(heap);
}

/* exported interface documented in js.h */
void js_destroyheap(jsheap *heap)
{
	heap->pending_destroy = true;
	if (heap->live_threads == 0) {
		enhanced_destroyheap(heap);
	}
}

/* Convenience macro: CTX refers to the persistent compat context during init.
 * Matches dukky.c convention where CTX is the duk_context for the thread. */
#define CTX (ret->compat_ctx)

/* exported interface documented in js.h */
nserror js_newthread(jsheap *heap, void *win_priv, void *doc_priv,
		     jsthread **thread)
{
	jsthread *ret;

	assert(heap != NULL);
	assert(!heap->pending_destroy);

	ret = calloc(1, sizeof(*ret));
	if (ret == NULL) {
		NSLOG(dukky, ERROR,
		      "Unable to allocate new enhanced JS thread");
		return NSERROR_NOMEM;
	}

	NSLOG(dukky, DEBUG,
	      "New enhanced JS thread, win_priv=%p, doc_priv=%p",
	      win_priv, doc_priv);

	ret->heap = heap;
	ret->ctx = JS_NewContext(heap->rt);
	if (ret->ctx == NULL) {
		free(ret);
		return NSERROR_NOMEM;
	}

	JS_SetContextOpaque(ret->ctx, ret);

	/* Store the global object for convenient access */
	ret->global = JS_GetGlobalObject(ret->ctx);

	/* Install console before bindings so it's available during init.
	 * WHY: The binding Console (Console.bnd) uses generics/formatter
	 * which may not work in enhanced mode. This native console routes
	 * output through browser_window_console_log for monkey protocol
	 * visibility AND through NSLOG for stderr. */
	enhanced_install_console(ret->ctx);

	/* Create persistent compat context for bindings */
	ret->compat_ctx = duk_compat_create(ret->ctx);
	if (ret->compat_ctx == NULL) {
		JS_FreeValue(ret->ctx, ret->global);
		JS_FreeContext(ret->ctx);
		free(ret);
		return NSERROR_NOMEM;
	}

	/* Set up the prototype table (PROTO_MAGIC).
	 * WHY: dukky_populate_object looks up prototypes via
	 * duk_get_global_string(ctx, PROTO_MAGIC). In Duktape, PROTO_MAGIC
	 * is the global object stored as a property of itself.
	 * dukky_create_prototype stores each prototype as a global property,
	 * so looking them up via PROTO_MAGIC (= global) works.
	 */
	duk_push_global_object(CTX);
	duk_push_boolean(CTX, 1);
	duk_put_prop_string(CTX, -2, "protos");
	duk_put_global_string(CTX, PROTO_MAGIC);

	/* Create prototypes -- registers ~150 binding prototypes */
	dukky_create_prototypes(CTX);

	/* Manufacture a Window object.
	 * win_priv is a browser_window, doc_priv is an html content struct.
	 * dukky_create_object pops win_priv and doc_priv pointers from the
	 * compat stack and stores them as the Window's private data. */
	duk_push_pointer(CTX, win_priv);
	duk_push_pointer(CTX, doc_priv);
	dukky_create_object(CTX, PROTO_NAME(WINDOW), 2);
	/* ... Window */

	/* Store the old global (QuickJS native) as PROTO_MAGIC on Window,
	 * then make Window the effective global object. */
	duk_push_global_object(CTX);
	/* ... Window oldGlobal */
	duk_put_prop_string(CTX, -2, PROTO_MAGIC);
	/* ... Window */
	duk_set_global_object(CTX);
	/* ... (Window is now the global) */

	/* Create the node identity map (NODE_MAGIC) */
	duk_push_object(CTX);
	duk_push_pointer(CTX, NULL);
	duk_push_null(CTX);
	duk_put_prop(CTX, -3);
	duk_put_global_string(CTX, NODE_MAGIC);

	/* Create the event cache map (EVENT_MAGIC) */
	duk_push_object(CTX);
	duk_put_global_string(CTX, EVENT_MAGIC);

	/* Load polyfill.js.
	 * WHY: QuickJS-NG has native Map/Set/Promise, but the polyfill
	 * also provides Array.from, Object.assign, and other helpers that
	 * the binding code and generics.js expect. Keep it for now. */
	duk_push_string(CTX, "polyfill.js");
	if (duk_pcompile_lstring_filename(CTX, DUK_COMPILE_EVAL,
					  (const char *)polyfill_js,
					  polyfill_js_len) != 0) {
		NSLOG(dukky, CRITICAL, "%s", duk_safe_to_string(CTX, -1));
		NSLOG(dukky, CRITICAL,
		      "Unable to compile polyfill.js, thread aborted");
		duk_pop(CTX);
		duk_compat_destroy(ret->compat_ctx);
		JS_FreeValue(ret->ctx, ret->global);
		JS_FreeContext(ret->ctx);
		free(ret);
		return NSERROR_INIT_FAILED;
	}
	if (dukky_pcall(CTX, 0, true) != 0) {
		NSLOG(dukky, CRITICAL, "%s",
		      duk_safe_to_string(CTX, -1));
		NSLOG(dukky, CRITICAL,
		      "Unable to run polyfill.js, thread aborted");
		duk_pop(CTX);
		duk_compat_destroy(ret->compat_ctx);
		JS_FreeValue(ret->ctx, ret->global);
		JS_FreeContext(ret->ctx);
		free(ret);
		return NSERROR_INIT_FAILED;
	}
	duk_pop(CTX); /* result */

	/* Copy polyfill-defined names from old global onto Window.
	 * WHY: polyfill.js stores Map, Set, etc. on globalThis, which
	 * after duk_set_global_object is the old QuickJS global (stored
	 * as PROTO_MAGIC on Window). Variable resolution uses Window (the
	 * current global), so bare `Map` would fail without this copy. */
	{
		duk_push_global_object(CTX);
		/* ... Win */
		duk_get_prop_string(CTX, -1, PROTO_MAGIC);
		/* ... Win oldGlobal */

		static const char * const es6_names[] = {
			"Map", "Set", "WeakMap", "WeakSet", "Promise",
			"console", NULL
		};
		for (const char * const *p = es6_names; *p != NULL; p++) {
			if (duk_get_prop_string(CTX, -1, *p)) {
				duk_put_prop_string(CTX, -3, *p);
			} else {
				duk_pop(CTX);
			}
		}

		duk_pop(CTX); /* oldGlobal */
		/* ... Win */

		/* Register real URL constructor */
		duk_push_c_function(CTX, dukky_url_constructor,
				    DUK_VARARGS);
		duk_get_global_string(CTX, dukky_magic_string_prototypes);
		duk_get_prop_string(CTX, -1,
			"\xFF\xFFNETSURF_DUKTAPE_PROTOTYPE_URL");
		duk_remove(CTX, -2);
		duk_put_prop_string(CTX, -2, "prototype");
		duk_put_prop_string(CTX, -2, "URL");

		/* Register real URLSearchParams constructor */
		duk_push_c_function(CTX, dukky_urlsearchparams_constructor,
				    DUK_VARARGS);
		duk_get_global_string(CTX, dukky_magic_string_prototypes);
		duk_get_prop_string(CTX, -1,
			"\xFF\xFFNETSURF_DUKTAPE_PROTOTYPE_URLSEARCHPARAMS");
		duk_remove(CTX, -2);
		duk_put_prop_string(CTX, -2, "prototype");
		duk_put_prop_string(CTX, -2, "URLSearchParams");

		/* Override toString/toJSON on URL and USP prototypes */
		duk_get_global_string(CTX, dukky_magic_string_prototypes);
		/* ... Win prototab */

		/* URL prototype: toString and toJSON */
		duk_get_prop_string(CTX, -1,
			"\xFF\xFFNETSURF_DUKTAPE_PROTOTYPE_URL");
		duk_push_string(CTX, "toString");
		duk_push_c_function(CTX, dukky_url_tostring, 0);
		duk_def_prop(CTX, -3,
			     DUK_DEFPROP_HAVE_VALUE |
			     DUK_DEFPROP_HAVE_WRITABLE |
			     DUK_DEFPROP_WRITABLE |
			     DUK_DEFPROP_HAVE_CONFIGURABLE |
			     DUK_DEFPROP_CONFIGURABLE |
			     DUK_DEFPROP_FORCE);
		duk_push_string(CTX, "toJSON");
		duk_push_c_function(CTX, dukky_url_tojson, 0);
		duk_def_prop(CTX, -3,
			     DUK_DEFPROP_HAVE_VALUE |
			     DUK_DEFPROP_HAVE_WRITABLE |
			     DUK_DEFPROP_WRITABLE |
			     DUK_DEFPROP_HAVE_CONFIGURABLE |
			     DUK_DEFPROP_CONFIGURABLE);
		duk_pop(CTX); /* url_proto */

		/* URLSearchParams prototype: toString */
		duk_get_prop_string(CTX, -1,
			"\xFF\xFFNETSURF_DUKTAPE_PROTOTYPE_URLSEARCHPARAMS");
		duk_push_string(CTX, "toString");
		duk_push_c_function(CTX, dukky_urlsearchparams_tostring, 0);
		duk_def_prop(CTX, -3,
			     DUK_DEFPROP_HAVE_VALUE |
			     DUK_DEFPROP_HAVE_WRITABLE |
			     DUK_DEFPROP_WRITABLE |
			     DUK_DEFPROP_HAVE_CONFIGURABLE |
			     DUK_DEFPROP_CONFIGURABLE |
			     DUK_DEFPROP_FORCE);
		duk_pop(CTX); /* usp_proto */

		duk_pop(CTX); /* prototab */

		/* Install Node type constants on the Node constructor */
		duk_get_prop_string(CTX, -1, "Node");
		if (duk_is_function(CTX, -1)) {
			static const struct {
				const char *name;
				unsigned short value;
			} node_consts[] = {
				{"ELEMENT_NODE", 1},
				{"ATTRIBUTE_NODE", 2},
				{"TEXT_NODE", 3},
				{"CDATA_SECTION_NODE", 4},
				{"PROCESSING_INSTRUCTION_NODE", 7},
				{"COMMENT_NODE", 8},
				{"DOCUMENT_NODE", 9},
				{"DOCUMENT_TYPE_NODE", 10},
				{"DOCUMENT_FRAGMENT_NODE", 11},
				{NULL, 0}
			};
			for (int i = 0; node_consts[i].name != NULL; i++) {
				duk_push_uint(CTX, node_consts[i].value);
				duk_put_prop_string(CTX, -2,
						    node_consts[i].name);
			}
		}
		duk_pop(CTX); /* Node */
		duk_pop(CTX); /* Win */
	}

	/* Load generics.js */
	duk_push_string(CTX, "generics.js");
	if (duk_pcompile_lstring_filename(CTX, DUK_COMPILE_EVAL,
					  (const char *)generics_js,
					  generics_js_len) != 0) {
		NSLOG(dukky, CRITICAL, "%s", duk_safe_to_string(CTX, -1));
		NSLOG(dukky, CRITICAL,
		      "Unable to compile generics.js, thread aborted");
		duk_pop(CTX);
		duk_compat_destroy(ret->compat_ctx);
		JS_FreeValue(ret->ctx, ret->global);
		JS_FreeContext(ret->ctx);
		free(ret);
		return NSERROR_INIT_FAILED;
	}
	if (dukky_pcall(CTX, 0, true) != 0) {
		NSLOG(dukky, CRITICAL,
		      "Unable to run generics.js, thread aborted");
		duk_pop(CTX);
		duk_compat_destroy(ret->compat_ctx);
		JS_FreeValue(ret->ctx, ret->global);
		JS_FreeContext(ret->ctx);
		free(ret);
		return NSERROR_INIT_FAILED;
	}
	duk_pop(CTX); /* result */

	/* Store NetSurf generics table as GENERICS_MAGIC and remove from
	 * the visible global namespace. */
	duk_push_global_object(CTX);
	duk_get_prop_string(CTX, -1, "NetSurf");
	duk_put_global_string(CTX, GENERICS_MAGIC);
	duk_del_prop_string(CTX, -1, "NetSurf");
	duk_pop(CTX); /* Win */

	dukky_log_stack_frame(CTX, "New thread created");
	NSLOG(dukky, DEBUG, "New enhanced thread is %p in heap %p",
	      thread, heap);

	heap->live_threads++;
	*thread = ret;
	return NSERROR_OK;
}

#undef CTX
#define CTX (thread->compat_ctx)

/* exported interface documented in js.h */
nserror js_closethread(jsthread *thread)
{
	if (thread == NULL)
		return NSERROR_OK;

	NSLOG(dukky, DEBUG, "Closing enhanced JS thread %p", thread);

	/* Run the closedownThread callback if it exists.
	 * WHY: generics.js registers a cleanup function as MAGIC(closedownThread)
	 * that releases binding resources before the thread is destroyed. */
	if (thread->compat_ctx != NULL) {
		duk_int_t top = duk_get_top(CTX);
		duk_get_global_string(CTX, MAGIC(closedownThread));
		dukky_pcall(CTX, 0, true);
		duk_set_top(CTX, top);
	}

	thread->pending_destroy = true;
	return NSERROR_OK;
}

static void enhanced_destroythread(jsthread *thread)
{
	jsheap *heap;

	assert(thread->in_use == 0);
	assert(thread->pending_destroy);

	heap = thread->heap;

	NSLOG(dukky, DEBUG, "Destroying enhanced JS thread %p in heap %p",
	      thread, heap);

	/* Destroy persistent compat context (frees emulated stack) */
	if (thread->compat_ctx != NULL) {
		duk_compat_destroy(thread->compat_ctx);
		thread->compat_ctx = NULL;
	}

	JS_FreeValue(thread->ctx, thread->global);
	JS_FreeContext(thread->ctx);
	free(thread);

	heap->live_threads--;

	if (heap->pending_destroy && heap->live_threads == 0) {
		enhanced_destroyheap(heap);
	}
}

/* exported interface documented in js.h */
void js_destroythread(jsthread *thread)
{
	thread->pending_destroy = true;
	if (thread->in_use == 0) {
		enhanced_destroythread(thread);
	}
}

/* exported interface documented in js.h */
bool js_exec(jsthread *thread, const uint8_t *txt, size_t txtlen,
	     const char *name)
{
	JSValue result;
	bool ret = false;

	assert(thread);

	if (txt == NULL || txtlen == 0)
		return false;

	if (thread->pending_destroy) {
		NSLOG(dukky, DEEPDEBUG,
		      "Skipping exec call because thread is dead");
		return false;
	}

	enhanced_enter_thread(thread);

	NSLOG(dukky, DEEPDEBUG, "Running %zu bytes from %s", txtlen,
	      name ? name : "?unknown?");

	enhanced_reset_start_time(thread->heap);

	result = JS_Eval(thread->ctx, (const char *)txt, txtlen,
			 name ? name : "<input>",
			 JS_EVAL_TYPE_GLOBAL);

	if (JS_IsException(result)) {
		JSValue exc = JS_GetException(thread->ctx);
		const char *str = JS_ToCString(thread->ctx, exc);
		if (str) {
			NSLOG(jserrors, WARNING,
			      "Uncaught error in JS: %s", str);
			JS_FreeCString(thread->ctx, str);
		}

		/* Try to get stack trace */
		if (JS_IsObject(exc)) {
			JSValue stack = JS_GetPropertyStr(thread->ctx,
							  exc, "stack");
			if (!JS_IsUndefined(stack)) {
				const char *st = JS_ToCString(thread->ctx,
							      stack);
				if (st) {
					NSLOG(jserrors, WARNING,
					      "Stack trace: %s", st);
					JS_FreeCString(thread->ctx, st);
				}
			}
			JS_FreeValue(thread->ctx, stack);
		}
		JS_FreeValue(thread->ctx, exc);
		goto out;
	}

	/* Match Duktape bridge: return the boolean value of the result */
	ret = JS_ToBool(thread->ctx, result) > 0;
	JS_FreeValue(thread->ctx, result);

out:
	enhanced_flush_microtasks(thread);
	enhanced_leave_thread(thread);
	return ret;
}

/* exported interface documented in js.h */
bool js_fire_event(jsthread *thread, const char *type,
		   struct dom_document *doc, struct dom_node *target)
{
	dom_exception exc;
	dom_event *evt;
	dom_event_target *body;

	NSLOG(dukky, DEBUG, "Event: %s (doc=%p, target=%p)", type, doc,
	      target);

	/* Only handle load events targeted at Window (target==NULL).
	 * Matches Duktape engine behaviour. */
	if (target != NULL)
		return true;

	if (strcmp(type, "load") != 0)
		return true;

	exc = dom_event_create(&evt);
	if (exc != DOM_NO_ERR) return true;
	exc = dom_event_init(evt, corestring_dom_load, false, false);
	if (exc != DOM_NO_ERR) {
		dom_event_unref(evt);
		return true;
	}

	enhanced_enter_thread(thread);
	/* ... */
	duk_get_global_string(CTX, HANDLER_MAGIC);
	/* ... handlers */
	duk_push_lstring(CTX, "load", 4);
	/* ... handlers "load" */
	duk_get_prop(CTX, -2);
	/* ... handlers handler? */
	if (duk_is_undefined(CTX, -1)) {
		/* No handler on Window, try the body element */
		duk_pop(CTX);
		/* ... handlers */
		exc = dom_html_document_get_body(doc, &body);
		if (exc != DOM_NO_ERR) {
			dom_event_unref(evt);
			duk_pop(CTX);
			enhanced_leave_thread(thread);
			return true;
		}
		dukky_push_node(CTX, (struct dom_node *)body);
		/* ... handlers bodynode */
		if (dukky_get_current_value_of_event_handler(
			    CTX, corestring_dom_load, body) == false) {
			dom_node_unref(body);
			/* ... handlers */
			duk_pop(CTX);
			enhanced_leave_thread(thread);
			return true;
		}
		dom_node_unref(body);
		/* ... handlers handler bodynode */
		duk_pop(CTX);
	}
	/* ... handlers handler */
	duk_insert(CTX, -2);
	/* ... handler handlers */
	duk_pop(CTX);
	/* ... handler */
	duk_push_global_object(CTX);
	/* ... handler Window */
	dukky_push_event(CTX, evt);
	/* ... handler Window event */
	enhanced_reset_start_time(thread->heap);
	if (duk_pcall_method(CTX, 1) != 0) {
		/* ... err */
		NSLOG(dukky, DEBUG,
		      "OH NOES! An error running a handler.  Meh.");
		duk_get_prop_string(CTX, -1, "name");
		duk_get_prop_string(CTX, -2, "message");
		duk_get_prop_string(CTX, -3, "fileName");
		duk_get_prop_string(CTX, -4, "lineNumber");
		duk_get_prop_string(CTX, -5, "stack");
		NSLOG(dukky, DEBUG, "Uncaught error in JS: %s: %s",
		      duk_safe_to_string(CTX, -5),
		      duk_safe_to_string(CTX, -4));
		NSLOG(dukky, DEBUG, "              was at: %s line %s",
		      duk_safe_to_string(CTX, -3),
		      duk_safe_to_string(CTX, -2));
		NSLOG(dukky, DEBUG, "         Stack trace: %s",
		      duk_safe_to_string(CTX, -1));
		duk_pop_n(CTX, 6);
		js_event_cleanup(thread, evt);
		dom_event_unref(evt);
		enhanced_flush_microtasks(thread);
		enhanced_leave_thread(thread);
		return true;
	}
	/* ... result */
	duk_pop(CTX);
	js_event_cleanup(thread, evt);
	dom_event_unref(evt);
	enhanced_flush_microtasks(thread);
	enhanced_leave_thread(thread);
	return true;
}

/* exported interface documented in js.h */
bool js_dom_event_add_listener(jsthread *thread,
			       struct dom_document *document,
			       struct dom_node *node,
			       struct dom_string *event_type_dom,
			       void *js_funcval)
{
	/* WHY: This function is called from content layer to register
	 * event listeners from HTML parsing. The binding layer handles
	 * addEventListener calls from JS. For now, delegate to the
	 * standard registration mechanism. */
	(void)document;
	(void)js_funcval;

	if (thread == NULL || thread->compat_ctx == NULL)
		return true;

	dukky_register_event_listener_for(CTX, (struct dom_element *)node,
					  event_type_dom, false);
	return true;
}

/* exported interface documented in js.h */
void js_handle_new_element(jsthread *thread, struct dom_element *node)
{
	dom_namednodemap *map;
	dom_exception exc;
	dom_ulong idx;
	dom_ulong siz;
	dom_attr *attr = NULL;
	dom_string *key = NULL;
	dom_string *nodename;
	duk_bool_t is_body = false;

	assert(thread);
	assert(node);

	if (thread->compat_ctx == NULL)
		return;

	exc = dom_node_get_node_name(node, &nodename);
	if (exc != DOM_NO_ERR) return;

	if (nodename == corestring_dom_BODY)
		is_body = true;

	dom_string_unref(nodename);

	exc = dom_node_get_attributes(node, &map);
	if (exc != DOM_NO_ERR) return;
	if (map == NULL) return;

	enhanced_enter_thread(thread);

	exc = dom_namednodemap_get_length(map, &siz);
	if (exc != DOM_NO_ERR) goto out;

	for (idx = 0; idx < siz; idx++) {
		exc = dom_namednodemap_item(map, idx, &attr);
		if (exc != DOM_NO_ERR) goto out;
		exc = dom_attr_get_name(attr, &key);
		if (exc != DOM_NO_ERR) goto out;
		if (is_body && (
			    key == corestring_dom_onblur ||
			    key == corestring_dom_onerror ||
			    key == corestring_dom_onfocus ||
			    key == corestring_dom_onload ||
			    key == corestring_dom_onresize ||
			    key == corestring_dom_onscroll)) {
			/* Forwarded event -- skip, will register on Window */
			goto skip_register;
		}
		if (dom_string_length(key) > 2) {
			const uint8_t *data =
				(const uint8_t *)dom_string_data(key);
			if (data[0] == 'o' && data[1] == 'n') {
				dom_string *sub = NULL;
				exc = dom_string_substr(
					key, 2, dom_string_length(key),
					&sub);
				if (exc == DOM_NO_ERR) {
					dukky_register_event_listener_for(
						CTX, node, sub, false);
					dom_string_unref(sub);
				}
			}
		}
	skip_register:
		dom_string_unref(key); key = NULL;
		dom_node_unref(attr); attr = NULL;
	}

out:
	if (key != NULL)
		dom_string_unref(key);

	if (attr != NULL)
		dom_node_unref(attr);

	dom_namednodemap_unref(map);

	enhanced_leave_thread(thread);
}

/* exported interface documented in js.h */
void js_event_cleanup(jsthread *thread, struct dom_event *evt)
{
	assert(thread);

	if (thread->compat_ctx == NULL)
		return;

	enhanced_enter_thread(thread);
	/* ... */
	duk_get_global_string(CTX, EVENT_MAGIC);
	/* ... EVENT_MAP */
	duk_push_pointer(CTX, evt);
	/* ... EVENT_MAP eventptr */
	duk_del_prop(CTX, -2);
	/* ... EVENT_MAP */
	duk_pop(CTX);
	/* ... */
	enhanced_leave_thread(thread);
}
