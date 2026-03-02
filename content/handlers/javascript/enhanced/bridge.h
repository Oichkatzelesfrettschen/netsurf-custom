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
 * Enhanced JavaScript engine bridge internals.
 *
 * WHY: This header declares internal types and helpers shared between
 * bridge.c and duk_compat.c. It is NOT part of the js.h public API.
 */

#ifndef NETSURF_ENHANCED_BRIDGE_H
#define NETSURF_ENHANCED_BRIDGE_H

#include "content/handlers/javascript/enhanced/engine.h"

struct dom_node;
struct dom_element;
struct dom_document;
struct dom_event;
struct duk_context;

/**
 * Enhanced JS heap (one per browser window).
 *
 * Maps directly to QuickJS-NG's JSRuntime, plus NetSurf-specific state
 * for timeout control and memory limiting.
 */
struct jsheap {
	JSRuntime *rt;
	int timeout;
	bool pending_destroy;
	unsigned int live_threads;
	uint64_t exec_start_time;
	size_t heap_limit;
};

/**
 * Enhanced JS thread (one per browsing context / HTML content).
 *
 * Maps to a QuickJS-NG JSContext sharing the parent heap's JSRuntime.
 */
struct jsthread {
	struct jsheap *heap;
	JSContext *ctx;
	JSValue global;
	struct duk_context *compat_ctx; /**< persistent compat context for bindings */
	bool pending_destroy;
	unsigned int in_use;
};

/**
 * Get the jsthread associated with a QuickJS context.
 */
struct jsthread *enhanced_get_thread(JSContext *ctx);

/**
 * Enter/leave thread usage tracking (prevents premature destruction).
 */
void enhanced_enter_thread(struct jsthread *thread);
void enhanced_leave_thread(struct jsthread *thread);

#endif /* NETSURF_ENHANCED_BRIDGE_H */
