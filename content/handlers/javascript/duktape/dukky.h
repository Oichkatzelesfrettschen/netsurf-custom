/*
 * Copyright 2012 Vincent Sanders <vince@netsurf-browser.org>
 * Copyright 2015 Daniel Dilverstone <dsilvers@netsurf-browser.org>
 * Copyright 2016 Michael Drake <tlsa@netsurf-browser.org>
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

/** \file
 * Duktapeish implementation of javascript engine functions, prototypes.
 */

#ifndef DUKKY_H
#define DUKKY_H

/*
 * ERROR CONVENTION FOR .bnd BINDINGS
 *
 * On programming error (wrong arg count, null state that should never be
 * null, type mismatch from caller):
 *   return duk_error(ctx, DUK_ERR_TYPE_ERROR, "descriptive message");
 *
 * On security violation (cross-origin access, forbidden operation):
 *   return duk_error(ctx, DUK_ERR_ERROR, "descriptive message");
 *
 * On intentional silent no-op (spec-defined benign case, e.g. back() with
 * no history, go(0) when already reloading, unref on null pointer):
 *   return 0;  // WHY: <reason the spec permits silence here>
 *
 * Never return 0 silently for a condition that is a caller mistake.
 * Document every silent return 0 with a WHY comment so reviewers can
 * verify the silence is intentional.
 */

duk_ret_t dukky_create_object(duk_context *ctx, const char *name, int args);
duk_bool_t dukky_push_node_stacked(duk_context *ctx);
duk_bool_t dukky_push_node(duk_context *ctx, struct dom_node *node);
void dukky_inject_not_ctr(duk_context *ctx, int idx, const char *name);
void dukky_register_event_listener_for(duk_context *ctx,
				       struct dom_element *ele,
				       dom_string *name,
				       bool capture);
bool dukky_get_current_value_of_event_handler(duk_context *ctx,
					      dom_string *name,
					      dom_event_target *et);
void dukky_push_event(duk_context *ctx, dom_event *evt);
bool dukky_event_target_push_listeners(duk_context *ctx, bool dont_create);

typedef enum {
	ELF_CAPTURE = 1 << 0,
	ELF_PASSIVE = 1 << 1,
	ELF_ONCE    = 1 << 2,
	ELF_NONE    = 0
} event_listener_flags;

void dukky_shuffle_array(duk_context *ctx, duk_uarridx_t idx);

/* pcall something, and if it errored, also dump the error to the log */
duk_int_t dukky_pcall(duk_context *ctx, duk_size_t argc, bool reset_timeout);

/* Push a generics function onto the stack */
void dukky_push_generics(duk_context *ctx, const char *generic);

/* Log the current stack frame if possible */
void dukky_log_stack_frame(duk_context *ctx, const char * reason);

/**
 * Perform querySelector or querySelectorAll on a DOM subtree.
 *
 * Parses a CSS selector string (subset: tag, #id, .class, [attr],
 * [attr=val], descendant and child combinators) and walks the subtree
 * rooted at \a root to find matching elements.
 *
 * \param ctx   Duktape context (selector string at stack index 0)
 * \param root  DOM node whose subtree to search
 * \param all   If true, return an array of all matches (querySelectorAll);
 *              if false, return the first match or null (querySelector)
 * \return 1 (result pushed) or 0 (null/undefined)
 */
duk_ret_t dukky_queryselector(duk_context *ctx, struct dom_node *root,
			      bool all);

/**
 * Serialize the inner HTML of a DOM node and push it as a JS string.
 *
 * Walks the children of \a node and serialises them as HTML5, following
 * the "serialize a subtree" algorithm (simplified: handles element, text,
 * comment, and CDATA section nodes; void elements are serialised without
 * a closing tag).
 *
 * On success, a JS string is pushed onto the Duktape stack and 1 is returned.
 * On error, a JS error may be thrown (the caller should use duk_safe_call if
 * needed).
 *
 * \param ctx   Duktape context
 * \param node  DOM node whose children to serialise
 * \return 1 (one string pushed onto the stack), or 0 on fatal error
 */
duk_ret_t dukky_push_node_innerhtml(duk_context *ctx, struct dom_node *node);

/**
 * Test whether an element matches a CSS selector string.
 *
 * Parses the selector and tests the element against it. Used by
 * Element.matches() and Element.closest().
 *
 * \param element  DOM element to test
 * \param sel_str  CSS selector string
 * \param sel_len  Length of selector string
 * \return true if the element matches, false otherwise
 */
bool dukky_element_matches_selector(struct dom_element *element,
				    const char *sel_str, size_t sel_len);

#endif
