/*
 * Copyright 2012 Vincent Sanders <vince@netsurf-browser.org>
 * Copyright 2015 Daniel Dilverstone <dsilvers@netsurf-browser.org>
 * Copyright 2016 Michael Drake <tlsa@netsurf-browser.org>
 * Copyright 2016 John-Mark Bell <jmb@netsurf-browser.org>
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
 * Real implementations of dukky helper functions for enhanced mode.
 *
 * WHY: Ported from duktape/dukky.c. The nsgenbind-generated binding code
 * calls these dukky_* helpers. They use the duk_compat shim to interact
 * with QuickJS-NG through the emulated Duktape stack API.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>

#include <nsutils/time.h>
#include <dom/dom.h>

#include "utils/log.h"
#include "utils/errors.h"
#include "utils/utils.h"
#include "utils/nsurl.h"
#include "utils/corestrings.h"

#include "javascript/duktape/duktape.h"
#include "duktape/binding.h"
#include "duktape/private.h"
#include "javascript/duktape/dukky.h"

#include "content/handlers/javascript/enhanced/bridge.h"
#include "content/handlers/javascript/enhanced/duk_compat.h"

/* Magic string aliases matching dukky.c */
#define EVENT_MAGIC MAGIC(EVENT_MAP)
#define HANDLER_LISTENER_MAGIC MAGIC(HANDLER_LISTENER_MAP)
#define HANDLER_MAGIC MAGIC(HANDLER_MAP)
#define EVENT_LISTENER_JS_MAGIC MAGIC(EVENT_LISTENER_JS_MAP)
#define GENERICS_MAGIC MAGIC(GENERICS_TABLE)

/* Forward declarations for functions called from bridge.c.
 * These are static in dukky.c (standard mode) but non-static here
 * because bridge.c references them for js_newthread initialization. */
duk_ret_t dukky_url_constructor(duk_context *ctx);
duk_ret_t dukky_urlsearchparams_constructor(duk_context *ctx);
duk_ret_t dukky_url_tostring(duk_context *ctx);
duk_ret_t dukky_url_tojson(duk_context *ctx);
duk_ret_t dukky_urlsearchparams_tostring(duk_context *ctx);

/* ------------------------------------------------------------------ */
/* Object creation and prototype management                            */
/* ------------------------------------------------------------------ */

static duk_ret_t dukky_populate_object(duk_context *ctx, void *udata)
{
	(void)udata;
	/* ... obj args protoname nargs */
	int nargs = duk_get_int(ctx, -1);
	duk_pop(ctx);
	/* ... obj args protoname */
	duk_get_global_string(ctx, PROTO_MAGIC);
	/* .. obj args protoname prototab */
	duk_dup(ctx, -2);
	/* ... obj args protoname prototab protoname */
	duk_get_prop(ctx, -2);
	/* ... obj args protoname prototab {proto/undefined} */
	if (duk_is_undefined(ctx, -1)) {
		NSLOG(dukky, WARNING,
		      "Unable to find dukky prototype for `%s` - falling back to HTMLUnknownElement",
		      duk_get_string(ctx, -3) + 2 /* Skip the two unprintables */);
		duk_pop(ctx);
		duk_push_string(ctx, PROTO_NAME(HTMLUNKNOWNELEMENT));
		duk_get_prop(ctx, -2);
	}
	/* ... obj args protoname prototab proto */
	duk_remove(ctx, -3);
	/* ... obj args prototab proto */
	duk_dup(ctx, -1);
	/* ... obj args prototab proto proto */
	duk_set_prototype(ctx, -(nargs+4));
	/* ... obj[proto] args prototab proto */
	duk_get_prop_string(ctx, -1, INIT_MAGIC);
	/* ... obj[proto] args prototab proto initfn */
	duk_insert(ctx, -(nargs+4));
	/* ... initfn obj[proto] args prototab proto */
	duk_pop_2(ctx);
	/* ... initfn obj[proto] args */
	NSLOG(dukky, DEEPDEBUG, "Call the init function");
	duk_call(ctx, nargs + 1);
	return 1; /* The object */
}

duk_ret_t dukky_create_object(duk_context *ctx, const char *name, int args)
{
	duk_ret_t ret;
	NSLOG(dukky, DEEPDEBUG, "name=%s nargs=%d", name + 2, args);
	/* ... args */
	duk_push_object(ctx);
	/* ... args obj */
	duk_push_object(ctx);
	/* ... args obj handlers */
	duk_put_prop_string(ctx, -2, HANDLER_LISTENER_MAGIC);
	/* ... args obj */
	duk_push_object(ctx);
	/* ... args obj handlers */
	duk_put_prop_string(ctx, -2, HANDLER_MAGIC);
	/* ... args obj */
	duk_insert(ctx, -(args+1));
	/* ... obj args */
	duk_push_string(ctx, name);
	/* ... obj args name */
	duk_push_int(ctx, args);
	/* ... obj args name nargs */
	if ((ret = duk_safe_call(ctx, dukky_populate_object, NULL, args + 3, 1))
	    != DUK_EXEC_SUCCESS)
		return ret;
	NSLOG(dukky, DEEPDEBUG, "created");
	return DUK_EXEC_SUCCESS;
}

static duk_ret_t
dukky_bad_constructor(duk_context *ctx)
{
	return duk_error(ctx, DUK_ERR_ERROR, "Bad constructor");
}

void
dukky_inject_not_ctr(duk_context *ctx, int idx, const char *name)
{
	/* ... p[idx] ... proto */
	duk_push_c_function(ctx, dukky_bad_constructor, 0);
	/* ... p[idx] ... proto cons */
	duk_insert(ctx, -2);
	/* ... p[idx] ... cons proto */
	duk_put_prop_string(ctx, -2, "prototype");
	/* ... p[idx] ... cons[proto] */
	duk_put_prop_string(ctx, idx, name);
	/* ... p ... */
}

/* ------------------------------------------------------------------ */
/* Node push / identity map                                            */
/* ------------------------------------------------------------------ */

#define SET_HTML_CLASS(CLASS) \
		*html_class = PROTO_NAME(HTML##CLASS##ELEMENT); \
		*html_class_len = \
				SLEN(PROTO_NAME(HTML)) + \
				SLEN(#CLASS) + \
				SLEN("ELEMENT");

static void dukky_html_element_class_from_tag_type(dom_html_element_type type,
		const char **html_class, size_t *html_class_len)
{
	switch(type) {
	case DOM_HTML_ELEMENT_TYPE_HTML:
		SET_HTML_CLASS(HTML)
		break;
	case DOM_HTML_ELEMENT_TYPE_HEAD:
		SET_HTML_CLASS(HEAD)
		break;
	case DOM_HTML_ELEMENT_TYPE_META:
		SET_HTML_CLASS(META)
		break;
	case DOM_HTML_ELEMENT_TYPE_BASE:
		SET_HTML_CLASS(BASE)
		break;
	case DOM_HTML_ELEMENT_TYPE_TITLE:
		SET_HTML_CLASS(TITLE)
		break;
	case DOM_HTML_ELEMENT_TYPE_BODY:
		SET_HTML_CLASS(BODY)
		break;
	case DOM_HTML_ELEMENT_TYPE_DIV:
		SET_HTML_CLASS(DIV)
		break;
	case DOM_HTML_ELEMENT_TYPE_FORM:
		SET_HTML_CLASS(FORM)
		break;
	case DOM_HTML_ELEMENT_TYPE_LINK:
		SET_HTML_CLASS(LINK)
		break;
	case DOM_HTML_ELEMENT_TYPE_BUTTON:
		SET_HTML_CLASS(BUTTON)
		break;
	case DOM_HTML_ELEMENT_TYPE_INPUT:
		SET_HTML_CLASS(INPUT)
		break;
	case DOM_HTML_ELEMENT_TYPE_TEXTAREA:
		SET_HTML_CLASS(TEXTAREA)
		break;
	case DOM_HTML_ELEMENT_TYPE_OPTGROUP:
		SET_HTML_CLASS(OPTGROUP)
		break;
	case DOM_HTML_ELEMENT_TYPE_OPTION:
		SET_HTML_CLASS(OPTION)
		break;
	case DOM_HTML_ELEMENT_TYPE_SELECT:
		SET_HTML_CLASS(SELECT)
		break;
	case DOM_HTML_ELEMENT_TYPE_HR:
		SET_HTML_CLASS(HR)
		break;
	case DOM_HTML_ELEMENT_TYPE_DL:
		SET_HTML_CLASS(DLIST)
		break;
	case DOM_HTML_ELEMENT_TYPE_DIR:
		SET_HTML_CLASS(DIRECTORY)
		break;
	case DOM_HTML_ELEMENT_TYPE_MENU:
		SET_HTML_CLASS(MENU)
		break;
	case DOM_HTML_ELEMENT_TYPE_FIELDSET:
		SET_HTML_CLASS(FIELDSET)
		break;
	case DOM_HTML_ELEMENT_TYPE_LEGEND:
		SET_HTML_CLASS(LEGEND)
		break;
	case DOM_HTML_ELEMENT_TYPE_P:
		SET_HTML_CLASS(PARAGRAPH)
		break;
	case DOM_HTML_ELEMENT_TYPE_H1:
	case DOM_HTML_ELEMENT_TYPE_H2:
	case DOM_HTML_ELEMENT_TYPE_H3:
	case DOM_HTML_ELEMENT_TYPE_H4:
	case DOM_HTML_ELEMENT_TYPE_H5:
	case DOM_HTML_ELEMENT_TYPE_H6:
		SET_HTML_CLASS(HEADING)
		break;
	case DOM_HTML_ELEMENT_TYPE_BLOCKQUOTE:
	case DOM_HTML_ELEMENT_TYPE_Q:
		SET_HTML_CLASS(QUOTE)
		break;
	case DOM_HTML_ELEMENT_TYPE_PRE:
		SET_HTML_CLASS(PRE)
		break;
	case DOM_HTML_ELEMENT_TYPE_BR:
		SET_HTML_CLASS(BR)
		break;
	case DOM_HTML_ELEMENT_TYPE_LABEL:
		SET_HTML_CLASS(LABEL)
		break;
	case DOM_HTML_ELEMENT_TYPE_UL:
		SET_HTML_CLASS(ULIST)
		break;
	case DOM_HTML_ELEMENT_TYPE_OL:
		SET_HTML_CLASS(OLIST)
		break;
	case DOM_HTML_ELEMENT_TYPE_LI:
		SET_HTML_CLASS(LI)
		break;
	case DOM_HTML_ELEMENT_TYPE_FONT:
		SET_HTML_CLASS(FONT)
		break;
	case DOM_HTML_ELEMENT_TYPE_DEL:
	case DOM_HTML_ELEMENT_TYPE_INS:
		SET_HTML_CLASS(MOD)
		break;
	case DOM_HTML_ELEMENT_TYPE_A:
		SET_HTML_CLASS(ANCHOR)
		break;
	case DOM_HTML_ELEMENT_TYPE_BASEFONT:
		SET_HTML_CLASS(BASEFONT)
		break;
	case DOM_HTML_ELEMENT_TYPE_IMG:
		SET_HTML_CLASS(IMAGE)
		break;
	case DOM_HTML_ELEMENT_TYPE_OBJECT:
		SET_HTML_CLASS(OBJECT)
		break;
	case DOM_HTML_ELEMENT_TYPE_PARAM:
		SET_HTML_CLASS(PARAM)
		break;
	case DOM_HTML_ELEMENT_TYPE_APPLET:
		SET_HTML_CLASS(APPLET)
		break;
	case DOM_HTML_ELEMENT_TYPE_MAP:
		SET_HTML_CLASS(MAP)
		break;
	case DOM_HTML_ELEMENT_TYPE_AREA:
		SET_HTML_CLASS(AREA)
		break;
	case DOM_HTML_ELEMENT_TYPE_SCRIPT:
		SET_HTML_CLASS(SCRIPT)
		break;
	case DOM_HTML_ELEMENT_TYPE_CAPTION:
		SET_HTML_CLASS(TABLECAPTION)
		break;
	case DOM_HTML_ELEMENT_TYPE_TD:
	case DOM_HTML_ELEMENT_TYPE_TH:
		SET_HTML_CLASS(TABLECELL)
		break;
	case DOM_HTML_ELEMENT_TYPE_COL:
	case DOM_HTML_ELEMENT_TYPE_COLGROUP:
		SET_HTML_CLASS(TABLECOL)
		break;
	case DOM_HTML_ELEMENT_TYPE_THEAD:
	case DOM_HTML_ELEMENT_TYPE_TBODY:
	case DOM_HTML_ELEMENT_TYPE_TFOOT:
		SET_HTML_CLASS(TABLESECTION)
		break;
	case DOM_HTML_ELEMENT_TYPE_TABLE:
		SET_HTML_CLASS(TABLE)
		break;
	case DOM_HTML_ELEMENT_TYPE_TR:
		SET_HTML_CLASS(TABLEROW)
		break;
	case DOM_HTML_ELEMENT_TYPE_STYLE:
		SET_HTML_CLASS(STYLE)
		break;
	case DOM_HTML_ELEMENT_TYPE_FRAMESET:
		SET_HTML_CLASS(FRAMESET)
		break;
	case DOM_HTML_ELEMENT_TYPE_FRAME:
		SET_HTML_CLASS(FRAME)
		break;
	case DOM_HTML_ELEMENT_TYPE_IFRAME:
		SET_HTML_CLASS(IFRAME)
		break;
	case DOM_HTML_ELEMENT_TYPE_ISINDEX:
		SET_HTML_CLASS(ISINDEX)
		break;
	case DOM_HTML_ELEMENT_TYPE_CANVAS:
		SET_HTML_CLASS(CANVAS)
		break;
	case DOM_HTML_ELEMENT_TYPE__COUNT:
		assert(type != DOM_HTML_ELEMENT_TYPE__COUNT);
		fallthrough;
	case DOM_HTML_ELEMENT_TYPE__UNKNOWN:
		SET_HTML_CLASS(UNKNOWN)
		break;
	default:
		/* Known HTML element without a specialisation */
		*html_class = PROTO_NAME(HTMLELEMENT);
		*html_class_len =
				SLEN(PROTO_NAME(HTML)) +
				SLEN("ELEMENT");
		break;
	}
}

#undef SET_HTML_CLASS

static void
dukky_push_node_klass(duk_context *ctx, struct dom_node *node)
{
	dom_node_type nodetype;
	dom_exception err;

	err = dom_node_get_node_type(node, &nodetype);
	if (err != DOM_NO_ERR) {
		duk_push_string(ctx, PROTO_NAME(NODE));
		return;
	}

	switch(nodetype) {
	case DOM_ELEMENT_NODE: {
		dom_string *namespace;
		dom_html_element_type type;
		const char *html_class;
		size_t html_class_len;
		err = dom_node_get_namespace(node, &namespace);
		if (err != DOM_NO_ERR) {
			NSLOG(dukky, ERROR,
			      "dom_node_get_namespace() failed");
			duk_push_string(ctx, PROTO_NAME(ELEMENT));
			break;
		}
		if (namespace == NULL) {
			NSLOG(dukky, DEBUG, "no namespace");
			duk_push_string(ctx, PROTO_NAME(ELEMENT));
			break;
		}

		if (dom_string_isequal(namespace, corestring_dom_html_namespace) == false) {
			duk_push_string(ctx, PROTO_NAME(ELEMENT));
			dom_string_unref(namespace);
			break;
		}
		dom_string_unref(namespace);

		err = dom_html_element_get_tag_type(node, &type);
		if (err != DOM_NO_ERR) {
			type = DOM_HTML_ELEMENT_TYPE__UNKNOWN;
		}

		dukky_html_element_class_from_tag_type(type,
				&html_class, &html_class_len);

		duk_push_lstring(ctx, html_class, html_class_len);
		break;
	}
	case DOM_TEXT_NODE:
		duk_push_string(ctx, PROTO_NAME(TEXT));
		break;
	case DOM_COMMENT_NODE:
		duk_push_string(ctx, PROTO_NAME(COMMENT));
		break;
	case DOM_DOCUMENT_NODE:
		duk_push_string(ctx, PROTO_NAME(DOCUMENT));
		break;
	case DOM_DOCUMENT_FRAGMENT_NODE:
		duk_push_string(ctx, PROTO_NAME(DOCUMENTFRAGMENT));
		break;
	case DOM_ATTRIBUTE_NODE:
	case DOM_PROCESSING_INSTRUCTION_NODE:
	case DOM_DOCUMENT_TYPE_NODE:
	case DOM_NOTATION_NODE:
	case DOM_ENTITY_REFERENCE_NODE:
	case DOM_ENTITY_NODE:
	case DOM_CDATA_SECTION_NODE:
	default:
		duk_push_string(ctx, PROTO_NAME(NODE));
	}
}

duk_bool_t
dukky_push_node_stacked(duk_context *ctx)
{
	int top_at_fail = duk_get_top(ctx) - 2;
	/* ... nodeptr klass */
	duk_get_global_string(ctx, NODE_MAGIC);
	/* ... nodeptr klass nodes */
	duk_dup(ctx, -3);
	/* ... nodeptr klass nodes nodeptr */
	duk_get_prop(ctx, -2);
	/* ... nodeptr klass nodes node/undefined */
	if (duk_is_undefined(ctx, -1)) {
		/* ... nodeptr klass nodes undefined */
		duk_pop(ctx);
		/* ... nodeptr klass nodes */
		duk_push_object(ctx);
		/* ... nodeptr klass nodes obj */
		duk_push_object(ctx);
		/* ... nodeptr klass nodes obj handlers */
		duk_put_prop_string(ctx, -2, HANDLER_LISTENER_MAGIC);
		/* ... nodeptr klass nodes obj */
		duk_push_object(ctx);
		/* ... nodeptr klass nodes obj handlers */
		duk_put_prop_string(ctx, -2, HANDLER_MAGIC);
		/* ... nodeptr klass nodes obj */
		duk_dup(ctx, -4);
		/* ... nodeptr klass nodes obj nodeptr */
		duk_dup(ctx, -4);
		/* ... nodeptr klass nodes obj nodeptr klass */
		duk_push_int(ctx, 1);
		/* ... nodeptr klass nodes obj nodeptr klass 1 */
		if (duk_safe_call(ctx, dukky_populate_object, NULL, 4, 1)
		    != DUK_EXEC_SUCCESS) {
			duk_set_top(ctx, top_at_fail);
			NSLOG(dukky, ERROR, "Failed to populate object prototype");
			return false;
		}
		/* ... nodeptr klass nodes node */
		duk_dup(ctx, -4);
		/* ... nodeptr klass nodes node nodeptr */
		duk_dup(ctx, -2);
		/* ... nodeptr klass nodes node nodeptr node */
		duk_put_prop(ctx, -4);
		/* ... nodeptr klass nodes node */
	}
	/* ... nodeptr klass nodes node */
	duk_insert(ctx, -4);
	/* ... node nodeptr klass nodes */
	duk_pop_3(ctx);
	/* ... node */
	if (NSLOG_COMPILED_MIN_LEVEL <= NSLOG_LEVEL_DEEPDEBUG) {
		duk_dup(ctx, -1);
		const char * what = duk_safe_to_string(ctx, -1);
		NSLOG(dukky, DEEPDEBUG, "Created: %s", what);
		duk_pop(ctx);
	}
	return true;
}

duk_bool_t
dukky_push_node(duk_context *ctx, struct dom_node *node)
{
	NSLOG(dukky, DEEPDEBUG, "Pushing node %p", node);
	/* First check if we can find the node */
	/* ... */
	duk_get_global_string(ctx, NODE_MAGIC);
	/* ... nodes */
	duk_push_pointer(ctx, node);
	/* ... nodes nodeptr */
	duk_get_prop(ctx, -2);
	/* ... nodes node/undefined */
	if (!duk_is_undefined(ctx, -1)) {
		/* ... nodes node */
		duk_insert(ctx, -2);
		/* ... node nodes */
		duk_pop(ctx);
		/* ... node */
		if (NSLOG_COMPILED_MIN_LEVEL <= NSLOG_LEVEL_DEEPDEBUG) {
			duk_dup(ctx, -1);
			const char * what = duk_safe_to_string(ctx, -1);
			NSLOG(dukky, DEEPDEBUG, "Found it memoised: %s", what);
			duk_pop(ctx);
		}
		return true;
	}
	/* ... nodes undefined */
	duk_pop_2(ctx);
	/* ... */
	/* We couldn't, so now we determine the node type and then
	 * we ask for it to be created
	 */
	duk_push_pointer(ctx, node);
	/* ... nodeptr */
	dukky_push_node_klass(ctx, node);
	/* ... nodeptr klass */
	return dukky_push_node_stacked(ctx);
}

/* ------------------------------------------------------------------ */
/* Event handling                                                      */
/* ------------------------------------------------------------------ */

static const char* dukky_event_proto(dom_event *evt)
{
	const char *ret = PROTO_NAME(EVENT);
	dom_string *type = NULL;
	dom_exception err;

	err = dom_event_get_type(evt, &type);
	if (err != DOM_NO_ERR) {
		goto out;
	}

	if (dom_string_isequal(type, corestring_dom_keydown)) {
		ret = PROTO_NAME(KEYBOARDEVENT);
		goto out;
	} else if (dom_string_isequal(type, corestring_dom_keyup)) {
		ret = PROTO_NAME(KEYBOARDEVENT);
		goto out;
	} else if (dom_string_isequal(type, corestring_dom_keypress)) {
		ret = PROTO_NAME(KEYBOARDEVENT);
		goto out;
	}

out:
	if (type != NULL) {
		dom_string_unref(type);
	}

	return ret;
}

void dukky_push_event(duk_context *ctx, dom_event *evt)
{
	/* ... */
	duk_get_global_string(ctx, EVENT_MAGIC);
	/* ... events */
	duk_push_pointer(ctx, evt);
	/* ... events eventptr */
	duk_get_prop(ctx, -2);
	/* ... events event? */
	if (duk_is_undefined(ctx, -1)) {
		/* ... events undefined */
		duk_pop(ctx);
		/* ... events */
		duk_push_pointer(ctx, evt);
		if (dukky_create_object(ctx, dukky_event_proto(evt), 1) != DUK_EXEC_SUCCESS) {
			/* ... events err */
			duk_pop(ctx);
			/* ... events */
			duk_push_object(ctx);
			/* ... events eobj[meh] */
		}
		/* ... events eobj */
		duk_push_pointer(ctx, evt);
		/* ... events eobj eventptr */
		duk_dup(ctx, -2);
		/* ... events eobj eventptr eobj */
		duk_put_prop(ctx, -4);
		/* ... events eobj */
	}
	/* ... events event */
	duk_replace(ctx, -2);
	/* ... event */
}

static void dukky_push_handler_code_(duk_context *ctx, dom_string *name,
				     dom_event_target *et)
{
	dom_string *onname, *val;
	dom_element *ele = (dom_element *)et;
	dom_exception exc;
	dom_node_type ntype;

	if (et == NULL) {
		duk_push_lstring(ctx, "", 0);
		return;
	}

	exc = dom_node_get_node_type(et, &ntype);
	if (exc != DOM_NO_ERR) {
		duk_push_lstring(ctx, "", 0);
		return;
	}

	if (ntype != DOM_ELEMENT_NODE) {
		duk_push_lstring(ctx, "", 0);
		return;
	}

	exc = dom_string_concat(corestring_dom_on, name, &onname);
	if (exc != DOM_NO_ERR) {
		duk_push_lstring(ctx, "", 0);
		return;
	}

	exc = dom_element_get_attribute(ele, onname, &val);
	if ((exc != DOM_NO_ERR) || (val == NULL)) {
		dom_string_unref(onname);
		duk_push_lstring(ctx, "", 0);
		return;
	}

	dom_string_unref(onname);
	duk_push_lstring(ctx, dom_string_data(val), dom_string_length(val));
	dom_string_unref(val);
}

bool dukky_get_current_value_of_event_handler(duk_context *ctx,
					      dom_string *name,
					      dom_event_target *et)
{
	/* Must be entered as:
	 * ... node(et)
	 */
	duk_get_prop_string(ctx, -1, HANDLER_MAGIC);
	/* ... node handlers */
	duk_push_lstring(ctx, dom_string_data(name), dom_string_length(name));
	/* ... node handlers name */
	duk_get_prop(ctx, -2);
	/* ... node handlers handler? */
	if (duk_is_undefined(ctx, -1)) {
		/* ... node handlers undefined */
		duk_pop_2(ctx);
		/* ... node */
		dukky_push_handler_code_(ctx, name, et);
		/* ... node handlercode? */
		/* ... node handlercode */
		duk_push_string(ctx, "function (event) {");
		/* ... node handlercode prefix */
		duk_insert(ctx, -2);
		/* ... node prefix handlercode */
		duk_push_string(ctx, "}");
		/* ... node prefix handlercode suffix */
		duk_concat(ctx, 3);
		/* ... node fullhandlersrc */
		duk_push_string(ctx, "internal raw uncompiled handler");
		/* ... node fullhandlersrc filename */
		if (duk_pcompile(ctx, DUK_COMPILE_FUNCTION) != 0) {
			/* ... node err */
			NSLOG(dukky, DEBUG,
			      "Unable to proceed with handler, could not compile");
			duk_pop_2(ctx);
			return false;
		}
		/* ... node handler */
		duk_insert(ctx, -2);
		/* ... handler node */
	} else {
		/* ... node handlers handler */
		duk_insert(ctx, -3);
		/* ... handler node handlers */
		duk_pop(ctx);
		/* ... handler node */
	}
	/* ... handler node */
	return true;
}

static void dukky_generic_event_handler(dom_event *evt, void *pw)
{
	duk_context *ctx = (duk_context *)pw;
	dom_string *name;
	dom_exception exc;
	dom_event_target *targ;
	dom_event_flow_phase phase;
	duk_uarridx_t idx;
	event_listener_flags flags;

	NSLOG(dukky, DEBUG, "Handling an event in enhanced interface...");
	exc = dom_event_get_type(evt, &name);
	if (exc != DOM_NO_ERR) {
		NSLOG(dukky, DEBUG, "Unable to find the event name");
		return;
	}
	NSLOG(dukky, DEBUG, "Event's name is %*s", (int)dom_string_length(name),
	      dom_string_data(name));
	exc = dom_event_get_event_phase(evt, &phase);
	if (exc != DOM_NO_ERR) {
		NSLOG(dukky, WARNING, "Unable to get event phase");
		return;
	}

	exc = dom_event_get_current_target(evt, &targ);
	if (exc != DOM_NO_ERR) {
		dom_string_unref(name);
		NSLOG(dukky, DEBUG, "Unable to find the event target");
		return;
	}

	/* If we're capturing right now, we skip the 'event handler'
	 * and go straight to the extras
	 */
	if (phase == DOM_CAPTURING_PHASE)
		goto handle_extras;

	/* ... */
	if (dukky_push_node(ctx, (dom_node *)targ) == false) {
		dom_string_unref(name);
		dom_node_unref(targ);
		NSLOG(dukky, DEBUG,
		      "Unable to push JS node representation?!");
		return;
	}
	/* ... node */
	if (dukky_get_current_value_of_event_handler(
		    ctx, name, (dom_event_target *)targ) == false) {
		/* ... */
		goto handle_extras;
	}
	/* ... handler node */
	dukky_push_event(ctx, evt);
	/* ... handler node event */
	if (duk_pcall_method(ctx, 1) != 0) {
		/* Failed to run the method */
		/* ... err */
		NSLOG(dukky, DEBUG,
		      "OH NOES! An error running a callback.  Meh.");
		exc = dom_event_stop_immediate_propagation(evt);
		if (exc != DOM_NO_ERR)
			NSLOG(dukky, DEBUG,
			      "WORSE! could not stop propagation");
		duk_get_prop_string(ctx, -1, "name");
		duk_get_prop_string(ctx, -2, "message");
		duk_get_prop_string(ctx, -3, "fileName");
		duk_get_prop_string(ctx, -4, "lineNumber");
		duk_get_prop_string(ctx, -5, "stack");
		/* ... err name message fileName lineNumber stack */
		NSLOG(dukky, DEBUG, "Uncaught error in JS: %s: %s",
		      duk_safe_to_string(ctx, -5),
		      duk_safe_to_string(ctx, -4));
		NSLOG(dukky, INFO, "              was at: %s line %s",
		      duk_safe_to_string(ctx, -3),
		      duk_safe_to_string(ctx, -2));
		NSLOG(dukky, INFO, "         Stack trace: %s",
		      duk_safe_to_string(ctx, -1));

		duk_pop_n(ctx, 6);
		/* ... */
		goto handle_extras;
	}
	/* ... result */
	if (duk_is_boolean(ctx, -1) &&
	    duk_to_boolean(ctx, -1) == 0) {
		dom_event_prevent_default(evt);
	}
	duk_pop(ctx);
handle_extras:
	/* ... */
	duk_push_lstring(ctx, dom_string_data(name), dom_string_length(name));
	dukky_push_node(ctx, (dom_node *)targ);
	/* ... type node */
	if (dukky_event_target_push_listeners(ctx, true)) {
		/* Nothing to do */
		duk_pop(ctx);
		goto out;
	}
	/* ... sublisteners */
	duk_push_array(ctx);
	/* ... sublisteners copy */
	idx = 0;
	while (duk_get_prop_index(ctx, -2, idx)) {
		/* ... sublisteners copy handler */
		duk_get_prop_index(ctx, -1, 1);
		/* ... sublisteners copy handler flags */
		if ((event_listener_flags)duk_to_int(ctx, -1) & ELF_ONCE) {
			duk_dup(ctx, -4);
			/* ... subl copy handler flags subl */
			dukky_shuffle_array(ctx, idx);
			duk_pop(ctx);
			/* ... subl copy handler flags */
		}
		duk_pop(ctx);
		/* ... sublisteners copy handler */
		duk_put_prop_index(ctx, -2, idx);
		/* ... sublisteners copy */
		idx++;
	}
	/* ... sublisteners copy undefined */
	duk_pop(ctx);
	/* ... sublisteners copy */
	duk_insert(ctx, -2);
	/* ... copy sublisteners */
	duk_pop(ctx);
	/* ... copy */
	idx = 0;
	while (duk_get_prop_index(ctx, -1, idx++)) {
		/* ... copy handler */
		if (duk_get_prop_index(ctx, -1, 2)) {
			/* ... copy handler meh */
			duk_pop_2(ctx);
			continue;
		}
		duk_pop(ctx);
		duk_get_prop_index(ctx, -1, 0);
		duk_get_prop_index(ctx, -2, 1);
		/* ... copy handler callback flags */
		flags = (event_listener_flags)duk_get_int(ctx, -1);
		duk_pop(ctx);
		/* ... copy handler callback */
		if (((phase == DOM_CAPTURING_PHASE) && !(flags & ELF_CAPTURE)) ||
		    ((phase != DOM_CAPTURING_PHASE) && (flags & ELF_CAPTURE))) {
			duk_pop_2(ctx);
			/* ... copy */
			continue;
		}
		/* ... copy handler callback */
		dukky_push_node(ctx, (dom_node *)targ);
		/* ... copy handler callback node */
		dukky_push_event(ctx, evt);
		/* ... copy handler callback node event */
		if (duk_pcall_method(ctx, 1) != 0) {
			/* Failed to run the method */
			/* ... copy handler err */
			NSLOG(dukky, DEBUG,
			      "OH NOES! An error running a callback.  Meh.");
			exc = dom_event_stop_immediate_propagation(evt);
			if (exc != DOM_NO_ERR)
				NSLOG(dukky, DEBUG,
				      "WORSE! could not stop propagation");
			duk_get_prop_string(ctx, -1, "name");
			duk_get_prop_string(ctx, -2, "message");
			duk_get_prop_string(ctx, -3, "fileName");
			duk_get_prop_string(ctx, -4, "lineNumber");
			duk_get_prop_string(ctx, -5, "stack");
			/* ... err name message fileName lineNumber stack */
			NSLOG(dukky, DEBUG, "Uncaught error in JS: %s: %s",
			      duk_safe_to_string(ctx, -5),
			      duk_safe_to_string(ctx, -4));
			NSLOG(dukky, DEBUG,
			      "              was at: %s line %s",
			      duk_safe_to_string(ctx, -3),
			      duk_safe_to_string(ctx, -2));
			NSLOG(dukky, DEBUG, "         Stack trace: %s",
			      duk_safe_to_string(ctx, -1));

			duk_pop_n(ctx, 7);
			/* ... copy */
			continue;
		}
		/* ... copy handler result */
		if (duk_is_boolean(ctx, -1) &&
		    duk_to_boolean(ctx, -1) == 0) {
			dom_event_prevent_default(evt);
		}
		duk_pop_2(ctx);
		/* ... copy */
	}
	duk_pop_2(ctx);
out:
	/* ... */
	dom_node_unref(targ);
	dom_string_unref(name);
}

void dukky_register_event_listener_for(duk_context *ctx,
				       struct dom_element *ele,
				       dom_string *name,
				       bool capture)
{
	dom_event_listener *listen = NULL;
	dom_exception exc;
	struct jsthread *thread;

	/* ... */
	if (ele == NULL) {
		/* A null element is the Window object */
		duk_push_global_object(ctx);
	} else {
		/* Non null elements must be pushed as a node object */
		if (dukky_push_node(ctx, (struct dom_node *)ele) == false)
			return;
	}
	/* ... node */
	duk_get_prop_string(ctx, -1, HANDLER_LISTENER_MAGIC);
	/* ... node handlers */
	duk_push_lstring(ctx, dom_string_data(name), dom_string_length(name));
	/* ... node handlers name */
	if (duk_has_prop(ctx, -2)) {
		/* ... node handlers */
		duk_pop_2(ctx);
		/* ... */
		return;
	}
	/* ... node handlers */
	duk_push_lstring(ctx, dom_string_data(name), dom_string_length(name));
	/* ... node handlers name */
	duk_push_boolean(ctx, true);
	/* ... node handlers name true */
	duk_put_prop(ctx, -3);
	/* ... node handlers */
	duk_pop_2(ctx);
	/* ... */
	if (ele == NULL) {
		/* Nothing more to do, Window doesn't register in the
		 * normal event listener flow
		 */
		return;
	}

	/* Otherwise add an event listener to the element.
	 * WHY: We pass the persistent compat_ctx as the pw argument so
	 * that when the event fires, dukky_generic_event_handler receives
	 * a valid long-lived context rather than a freed trampoline context.
	 */
	thread = enhanced_get_thread(duk_compat_qjs(ctx));
	exc = dom_event_listener_create(dukky_generic_event_handler,
					thread->compat_ctx, &listen);
	if (exc != DOM_NO_ERR) return;
	exc = dom_event_target_add_event_listener(
		ele, name, listen, capture);
	if (exc != DOM_NO_ERR) {
		NSLOG(dukky, DEBUG,
		      "Unable to register listener for %p.%*s", ele,
		      (int)dom_string_length(name), dom_string_data(name));
	} else {
		NSLOG(dukky, DEBUG, "have registered listener for %p.%*s",
		      ele, (int)dom_string_length(name), dom_string_data(name));
	}
	dom_event_listener_unref(listen);
}

/* The sub-listeners are a list of {callback,flags} tuples */
bool dukky_event_target_push_listeners(duk_context *ctx, bool dont_create)
{
	bool ret = false;
	/* ... type this */
	duk_get_prop_string(ctx, -1, EVENT_LISTENER_JS_MAGIC);
	if (duk_is_undefined(ctx, -1)) {
		/* ... type this null */
		duk_pop(ctx);
		duk_push_object(ctx);
		duk_dup(ctx, -1);
		/* ... type this listeners listeners */
		duk_put_prop_string(ctx, -3, EVENT_LISTENER_JS_MAGIC);
		/* ... type this listeners */
	}
	/* ... type this listeners */
	duk_insert(ctx, -3);
	/* ... listeners type this */
	duk_pop(ctx);
	/* ... listeners type */
	duk_dup(ctx, -1);
	/* ... listeners type type */
	duk_get_prop(ctx, -3);
	/* ... listeners type ??? */
	if (duk_is_undefined(ctx, -1)) {
		/* ... listeners type ??? */
		if (dont_create == true) {
			duk_pop_3(ctx);
			duk_push_undefined(ctx);
			return true;
		}
		duk_pop(ctx);
		duk_push_array(ctx);
		duk_dup(ctx, -2);
		duk_dup(ctx, -2);
		/* ... listeners type sublisteners type sublisteners */
		duk_put_prop(ctx, -5);
		/* ... listeners type sublisteners */
		ret = true;
	}
	duk_insert(ctx, -3);
	/* ... sublisteners listeners type */
	duk_pop_2(ctx);
	/* ... sublisteners */
	return ret;
}

void dukky_shuffle_array(duk_context *ctx, duk_uarridx_t idx)
{
	/* ... somearr */
	while (duk_get_prop_index(ctx, -1, idx + 1)) {
		duk_put_prop_index(ctx, -2, idx);
		idx++;
	}
	/* ... somearr undefined */
	duk_del_prop_index(ctx, -2, idx + 1);
	duk_pop(ctx);
}

/* ------------------------------------------------------------------ */
/* Generics                                                            */
/* ------------------------------------------------------------------ */

void dukky_push_generics(duk_context *ctx, const char *generic)
{
	/* ... */
	duk_get_global_string(ctx, GENERICS_MAGIC);
	/* ..., generics */
	duk_get_prop_string(ctx, -1, generic);
	/* ..., generics, generic */
	duk_remove(ctx, -2);
	/* ..., generic */
}

/* ------------------------------------------------------------------ */
/* Utility functions                                                   */
/* ------------------------------------------------------------------ */

static void dukky_dump_error(duk_context *ctx)
{
	/* stack is ..., errobj */
	duk_dup_top(ctx);
	/* ..., errobj, errobj */
	NSLOG(jserrors, WARNING, "Uncaught error in JS: %s", duk_safe_to_stacktrace(ctx, -1));
	/* ..., errobj, errobj.stackstring */
	duk_pop(ctx);
	/* ..., errobj */
}

static void dukky_reset_start_time(duk_context *ctx)
{
	struct jsthread *thread = enhanced_get_thread(duk_compat_qjs(ctx));
	if (thread && thread->heap) {
		(void)nsu_getmonotonic_ms(&thread->heap->exec_start_time);
	}
}

duk_int_t dukky_pcall(duk_context *ctx, duk_size_t argc, bool reset_timeout)
{
	if (reset_timeout) {
		dukky_reset_start_time(ctx);
	}

	duk_int_t ret = duk_pcall(ctx, (int)argc);
	if (ret) {
		/* Something went wrong calling this... */
		dukky_dump_error(ctx);
	}

	return ret;
}

static duk_int_t dukky_push_context_dump(duk_context *ctx, void *udata)
{
	(void)udata;
	duk_push_context_dump(ctx);
	return 1;
}

void dukky_log_stack_frame(duk_context *ctx, const char * reason)
{
	if (duk_safe_call(ctx, dukky_push_context_dump, NULL, 0, 1) != 0) {
		duk_pop(ctx);
		duk_push_string(ctx, "[???]");
	}
	NSLOG(dukky, DEEPDEBUG, "%s, stack is: %s", reason, duk_safe_to_string(ctx, -1));
	duk_pop(ctx);
}

/* ------------------------------------------------------------------ */
/* innerHTML serializer                                                */
/* ------------------------------------------------------------------ */

static bool is_void_element(const char *name)
{
	switch (name[0]) {
	case 'a': return strcmp(name, "area")   == 0;
	case 'b': return strcmp(name, "base")   == 0 || strcmp(name, "br") == 0;
	case 'c': return strcmp(name, "col")    == 0;
	case 'e': return strcmp(name, "embed")  == 0;
	case 'h': return strcmp(name, "hr")     == 0;
	case 'i': return strcmp(name, "img")    == 0 || strcmp(name, "input") == 0;
	case 'l': return strcmp(name, "link")   == 0;
	case 'm': return strcmp(name, "meta")   == 0;
	case 'p': return strcmp(name, "param")  == 0;
	case 's': return strcmp(name, "source") == 0;
	case 't': return strcmp(name, "track")  == 0;
	case 'w': return strcmp(name, "wbr")    == 0;
	default:  return false;
	}
}

static duk_idx_t push_html_escaped(duk_context *ctx,
				   const char *s, size_t len, bool in_attr)
{
	duk_idx_t pushed = 0;
	size_t start = 0;

	for (size_t i = 0; i < len; i++) {
		const char *entity = NULL;

		switch (s[i]) {
		case '&':  entity = "&amp;";  break;
		case '<':  entity = "&lt;";   break;
		case '>':  entity = "&gt;";   break;
		case '"':  if (in_attr) entity = "&quot;"; break;
		default:   break;
		}

		if (entity != NULL) {
			if (i > start) {
				duk_push_lstring(ctx, s + start, i - start);
				pushed++;
			}
			duk_push_string(ctx, entity);
			pushed++;
			start = i + 1;
		}
	}

	if (start < len) {
		duk_push_lstring(ctx, s + start, len - start);
		pushed++;
	}

	return pushed;
}

static duk_idx_t serialize_children(duk_context *ctx, dom_node *parent);

static duk_idx_t serialize_node(duk_context *ctx, dom_node *node)
{
	dom_node_type type;
	dom_exception exc;
	duk_idx_t pushed = 0;

	exc = dom_node_get_node_type(node, &type);
	if (exc != DOM_NO_ERR) {
		return 0;
	}

	switch (type) {
	case DOM_ELEMENT_NODE: {
		dom_string *name_s = NULL;
		exc = dom_node_get_node_name(node, &name_s);
		if (exc != DOM_NO_ERR || name_s == NULL) {
			break;
		}

		size_t name_len = dom_string_byte_length(name_s);
		const char *name_data = dom_string_data(name_s);

		char tag_lower[64];
		size_t copy_len = name_len < 63 ? name_len : 63;
		for (size_t i = 0; i < copy_len; i++) {
			char c = name_data[i];
			tag_lower[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
		}
		tag_lower[copy_len] = '\0';

		duk_push_string(ctx, "<");
		duk_push_lstring(ctx, tag_lower, copy_len);
		pushed += 2;

		dom_namednodemap *attrs = NULL;
		exc = dom_node_get_attributes(node, &attrs);
		if (exc == DOM_NO_ERR && attrs != NULL) {
			uint32_t attr_len = 0;
			exc = dom_namednodemap_get_length(attrs, &attr_len);
			for (uint32_t ai = 0; exc == DOM_NO_ERR && ai < attr_len; ai++) {
				dom_node *attr = NULL;
				exc = dom_namednodemap_item(attrs, ai, &attr);
				if (exc != DOM_NO_ERR || attr == NULL) {
					continue;
				}

				dom_string *aname = NULL;
				dom_string *aval  = NULL;
				dom_node_get_node_name(attr, &aname);
				dom_node_get_node_value(attr, &aval);

				if (aname != NULL) {
					duk_push_string(ctx, " ");
					duk_push_lstring(ctx,
						dom_string_data(aname),
						dom_string_byte_length(aname));
					pushed += 2;
					if (aval != NULL) {
						duk_push_string(ctx, "=\"");
						pushed++;
						pushed += push_html_escaped(ctx,
							dom_string_data(aval),
							dom_string_byte_length(aval),
							true);
						duk_push_string(ctx, "\"");
						pushed++;
					}
				}

				if (aval != NULL)  dom_string_unref(aval);
				if (aname != NULL) dom_string_unref(aname);
				dom_node_unref(attr);
			}
			dom_namednodemap_unref(attrs);
		}

		duk_push_string(ctx, ">");
		pushed++;

		if (!is_void_element(tag_lower)) {
			pushed += serialize_children(ctx, node);

			duk_push_string(ctx, "</");
			duk_push_lstring(ctx, tag_lower, copy_len);
			duk_push_string(ctx, ">");
			pushed += 3;
		}

		dom_string_unref(name_s);
		break;
	}

	case DOM_TEXT_NODE:
	case DOM_CDATA_SECTION_NODE: {
		dom_string *text = NULL;
		exc = dom_node_get_node_value(node, &text);
		if (exc == DOM_NO_ERR && text != NULL) {
			pushed += push_html_escaped(ctx,
				dom_string_data(text),
				dom_string_byte_length(text),
				false);
			dom_string_unref(text);
		}
		break;
	}

	case DOM_COMMENT_NODE: {
		dom_string *data = NULL;
		exc = dom_node_get_node_value(node, &data);
		if (exc == DOM_NO_ERR && data != NULL) {
			duk_push_string(ctx, "<!--");
			duk_push_lstring(ctx,
				dom_string_data(data),
				dom_string_byte_length(data));
			duk_push_string(ctx, "-->");
			pushed += 3;
			dom_string_unref(data);
		}
		break;
	}

	default:
		break;
	}

	return pushed;
}

static duk_idx_t serialize_children(duk_context *ctx, dom_node *parent)
{
	dom_node *child = NULL;
	dom_exception exc;
	duk_idx_t pushed = 0;

	exc = dom_node_get_first_child(parent, &child);
	if (exc != DOM_NO_ERR || child == NULL) {
		return 0;
	}

	while (child != NULL) {
		dom_node *next = NULL;
		pushed += serialize_node(ctx, child);

		exc = dom_node_get_next_sibling(child, &next);
		dom_node_unref(child);
		if (exc != DOM_NO_ERR) {
			break;
		}
		child = next;
	}

	return pushed;
}

duk_ret_t dukky_push_node_innerhtml(duk_context *ctx, struct dom_node *node)
{
	duk_idx_t pieces = serialize_children(ctx, node);

	if (pieces == 0) {
		duk_push_lstring(ctx, "", 0);
		return 1;
	}

	duk_concat(ctx, pieces);
	return 1;
}

/* ------------------------------------------------------------------ */
/* querySelector / querySelectorAll                                    */
/* ------------------------------------------------------------------ */

struct simple_selector {
	const char *tag;
	size_t tag_len;
	const char *id;
	size_t id_len;
	const char *cls;
	size_t cls_len;
	const char *attr;
	size_t attr_len;
	const char *attr_val;
	size_t attr_val_len;
};

enum selector_combinator {
	COMB_NONE,
	COMB_DESCENDANT,
	COMB_CHILD,
};

#define MAX_SELECTOR_PARTS 8
struct compound_selector {
	struct simple_selector parts[MAX_SELECTOR_PARTS];
	enum selector_combinator combinators[MAX_SELECTOR_PARTS];
	int count;
};

static bool
parse_selector(const char *sel, size_t sel_len,
	       struct compound_selector *out)
{
	const char *p = sel;
	const char *end = sel + sel_len;
	int idx = 0;

	memset(out, 0, sizeof(*out));

	while (p < end && (*p == ' ' || *p == '\t'))
		p++;

	if (p >= end)
		return false;

	while (p < end && idx < MAX_SELECTOR_PARTS) {
		struct simple_selector *s = &out->parts[idx];

		while (p < end && *p != ' ' && *p != '\t' && *p != '>' &&
		       *p != ',' && *p != '\0') {
			if (*p == '#') {
				p++;
				s->id = p;
				while (p < end && *p != '.' && *p != '[' &&
				       *p != '#' && *p != ' ' && *p != '\t' &&
				       *p != '>' && *p != ',' && *p != '\0')
					p++;
				s->id_len = (size_t)(p - s->id);
			} else if (*p == '.') {
				p++;
				s->cls = p;
				while (p < end && *p != '.' && *p != '[' &&
				       *p != '#' && *p != ' ' && *p != '\t' &&
				       *p != '>' && *p != ',' && *p != '\0')
					p++;
				s->cls_len = (size_t)(p - s->cls);
			} else if (*p == '[') {
				p++;
				s->attr = p;
				while (p < end && *p != '=' && *p != ']')
					p++;
				s->attr_len = (size_t)(p - s->attr);
				if (p < end && *p == '=') {
					p++;
					char quote = 0;
					if (p < end && (*p == '"' || *p == '\'')) {
						quote = *p;
						p++;
					}
					s->attr_val = p;
					if (quote) {
						while (p < end && *p != quote)
							p++;
						s->attr_val_len = (size_t)(p - s->attr_val);
						if (p < end) p++;
					} else {
						while (p < end && *p != ']')
							p++;
						s->attr_val_len = (size_t)(p - s->attr_val);
					}
				}
				if (p < end && *p == ']')
					p++;
			} else if (*p == '*') {
				p++;
			} else {
				s->tag = p;
				while (p < end && *p != '.' && *p != '[' &&
				       *p != '#' && *p != ' ' && *p != '\t' &&
				       *p != '>' && *p != ',' && *p != '\0')
					p++;
				s->tag_len = (size_t)(p - s->tag);
			}
		}

		idx++;

		while (p < end && (*p == ' ' || *p == '\t'))
			p++;

		if (p >= end || *p == ',' || *p == '\0')
			break;

		if (*p == '>') {
			p++;
			while (p < end && (*p == ' ' || *p == '\t'))
				p++;
			if (idx < MAX_SELECTOR_PARTS)
				out->combinators[idx] = COMB_CHILD;
		} else {
			if (idx < MAX_SELECTOR_PARTS)
				out->combinators[idx] = COMB_DESCENDANT;
		}
	}

	out->count = idx;
	return idx > 0;
}

static bool
strncasematch(const char *a, size_t a_len, dom_string *b)
{
	if (b == NULL)
		return false;
	if (a_len != dom_string_length(b))
		return false;
	return strncasecmp(a, dom_string_data(b), a_len) == 0;
}

static bool
element_matches_simple(dom_element *element, const struct simple_selector *sel)
{
	dom_string *val = NULL;
	dom_exception exc;

	if (sel->tag != NULL) {
		exc = dom_node_get_node_name((dom_node *)element, &val);
		if (exc != DOM_NO_ERR || val == NULL)
			return false;
		bool match = strncasematch(sel->tag, sel->tag_len, val);
		dom_string_unref(val);
		if (!match)
			return false;
	}

	if (sel->id != NULL) {
		exc = dom_element_get_attribute(element,
						corestring_dom_id, &val);
		if (exc != DOM_NO_ERR || val == NULL)
			return false;
		bool match = (sel->id_len == dom_string_length(val) &&
			      strncmp(sel->id, dom_string_data(val),
				      sel->id_len) == 0);
		dom_string_unref(val);
		if (!match)
			return false;
	}

	if (sel->cls != NULL) {
		exc = dom_element_get_attribute(element,
						corestring_dom_class, &val);
		if (exc != DOM_NO_ERR || val == NULL)
			return false;
		const char *haystack = dom_string_data(val);
		size_t haystack_len = dom_string_length(val);
		bool found = false;
		const char *hp = haystack;
		while (hp < haystack + haystack_len) {
			while (hp < haystack + haystack_len && *hp == ' ')
				hp++;
			const char *word_start = hp;
			while (hp < haystack + haystack_len && *hp != ' ')
				hp++;
			size_t word_len = (size_t)(hp - word_start);
			if (word_len == sel->cls_len &&
			    strncmp(word_start, sel->cls, sel->cls_len) == 0) {
				found = true;
				break;
			}
		}
		dom_string_unref(val);
		if (!found)
			return false;
	}

	if (sel->attr != NULL) {
		dom_string *attr_name = NULL;
		exc = dom_string_create((const uint8_t *)sel->attr,
					sel->attr_len, &attr_name);
		if (exc != DOM_NO_ERR)
			return false;

		if (sel->attr_val != NULL) {
			exc = dom_element_get_attribute(element, attr_name, &val);
			dom_string_unref(attr_name);
			if (exc != DOM_NO_ERR || val == NULL)
				return false;
			bool match = (sel->attr_val_len == dom_string_length(val) &&
				      strncmp(sel->attr_val, dom_string_data(val),
					      sel->attr_val_len) == 0);
			dom_string_unref(val);
			if (!match)
				return false;
		} else {
			bool has = false;
			exc = dom_element_has_attribute(element, attr_name, &has);
			dom_string_unref(attr_name);
			if (exc != DOM_NO_ERR || !has)
				return false;
		}
	}

	return true;
}

static bool
element_matches_compound(dom_element *element,
			 const struct compound_selector *sel)
{
	dom_node *node = (dom_node *)element;
	int i = sel->count - 1;

	if (!element_matches_simple((dom_element *)node, &sel->parts[i]))
		return false;

	for (i = sel->count - 2; i >= 0; i--) {
		enum selector_combinator comb = sel->combinators[i + 1];
		dom_node *candidate = NULL;
		dom_exception exc;
		bool found = false;

		if (comb == COMB_CHILD) {
			exc = dom_node_get_parent_node(node, &candidate);
			if (exc != DOM_NO_ERR || candidate == NULL)
				return false;
			dom_node_type ntype;
			exc = dom_node_get_node_type(candidate, &ntype);
			if (exc != DOM_NO_ERR || ntype != DOM_ELEMENT_NODE) {
				dom_node_unref(candidate);
				return false;
			}
			found = element_matches_simple(
				(dom_element *)candidate, &sel->parts[i]);
			node = candidate;
			dom_node_unref(candidate);
			if (!found)
				return false;
		} else if (comb == COMB_DESCENDANT) {
			exc = dom_node_get_parent_node(node, &candidate);
			while (candidate != NULL) {
				dom_node_type ntype;
				exc = dom_node_get_node_type(candidate, &ntype);
				if (exc == DOM_NO_ERR &&
				    ntype == DOM_ELEMENT_NODE &&
				    element_matches_simple(
					    (dom_element *)candidate,
					    &sel->parts[i])) {
					found = true;
					node = candidate;
					dom_node_unref(candidate);
					break;
				}
				dom_node *parent = NULL;
				exc = dom_node_get_parent_node(candidate, &parent);
				dom_node_unref(candidate);
				if (exc != DOM_NO_ERR) {
					candidate = NULL;
				} else {
					candidate = parent;
				}
			}
			if (!found)
				return false;
		}
	}

	return true;
}

static uint32_t
queryselector_walk(duk_context *ctx, dom_node *root,
		   const struct compound_selector *sel,
		   duk_idx_t arr_idx, bool first_only)
{
	dom_node *child = NULL;
	dom_exception exc;
	uint32_t count = 0;

	exc = dom_node_get_first_child(root, &child);
	if (exc != DOM_NO_ERR)
		return 0;

	while (child != NULL) {
		dom_node_type ntype;
		exc = dom_node_get_node_type(child, &ntype);
		if (exc == DOM_NO_ERR && ntype == DOM_ELEMENT_NODE) {
			if (element_matches_compound((dom_element *)child, sel)) {
				dukky_push_node(ctx, child);
				duk_put_prop_index(ctx, arr_idx, count);
				count++;
				if (first_only) {
					dom_node_unref(child);
					return count;
				}
			}
			uint32_t sub = queryselector_walk(
				ctx, child, sel, arr_idx, first_only);
			count += sub;
			if (first_only && count > 0) {
				dom_node_unref(child);
				return count;
			}
		}

		dom_node *next = NULL;
		exc = dom_node_get_next_sibling(child, &next);
		dom_node_unref(child);
		child = (exc == DOM_NO_ERR) ? next : NULL;
	}

	return count;
}

duk_ret_t
dukky_queryselector(duk_context *ctx, dom_node *root, bool all)
{
	struct compound_selector sel;
	duk_size_t sel_len;
	const char *sel_str;

	if (duk_get_top(ctx) < 1) {
		return duk_error(ctx, DUK_ERR_TYPE_ERROR,
				 "querySelector requires a selector string");
	}
	sel_str = duk_safe_to_lstring(ctx, 0, &sel_len);

	const char *comma = memchr(sel_str, ',', sel_len);
	size_t parse_len = comma ? (size_t)(comma - sel_str) : sel_len;

	if (!parse_selector(sel_str, parse_len, &sel)) {
		if (all) {
			duk_push_array(ctx);
			return 1;
		}
		return 0; /* WHY: null for querySelector on unparseable selector */
	}

	if (all) {
		duk_push_array(ctx);
		duk_idx_t arr_idx = duk_get_top_index(ctx);
		uint32_t count = queryselector_walk(ctx, root, &sel,
						    arr_idx, false);
		(void)count;
		return 1;
	} else {
		duk_push_array(ctx);
		duk_idx_t arr_idx = duk_get_top_index(ctx);
		uint32_t count = queryselector_walk(ctx, root, &sel,
						    arr_idx, true);
		if (count > 0) {
			duk_get_prop_index(ctx, arr_idx, 0);
			return 1;
		}
		return 0; /* WHY: null per spec when no match found */
	}
}

bool
dukky_element_matches_selector(dom_element *element,
			       const char *sel_str, size_t sel_len)
{
	struct compound_selector sel;

	const char *comma = memchr(sel_str, ',', sel_len);
	size_t parse_len = comma ? (size_t)(comma - sel_str) : sel_len;

	if (!parse_selector(sel_str, parse_len, &sel)) {
		return false;
	}

	return element_matches_compound(element, &sel);
}

/* ------------------------------------------------------------------ */
/* URL / URLSearchParams                                               */
/* ------------------------------------------------------------------ */

static bool
dukky_construct_with_proto(duk_context *ctx, const char *proto_name,
			   void *priv_ptr)
{
	duk_push_object(ctx);
	duk_push_object(ctx);
	duk_put_prop_string(ctx, -2, MAGIC(HANDLER_LISTENER_MAP));
	duk_push_object(ctx);
	duk_put_prop_string(ctx, -2, MAGIC(HANDLER_MAP));

	duk_get_global_string(ctx, PROTO_MAGIC);
	duk_get_prop_string(ctx, -1, proto_name);
	if (duk_is_undefined(ctx, -1)) {
		duk_pop_3(ctx); /* undef, prototab, obj */
		return false;
	}
	duk_remove(ctx, -2); /* prototab */
	/* ... obj proto */
	duk_set_prototype(ctx, -2);
	/* ... obj[proto] */

	duk_push_pointer(ctx, priv_ptr);
	duk_put_prop_string(ctx, -2, dukky_magic_string_private);

	return true;
}

/* Percent-decode a string in-place (output <= input length) */
static size_t usp_percent_decode(char *s, size_t len)
{
	size_t out = 0;
	for (size_t i = 0; i < len; i++) {
		if (s[i] == '%' && i + 2 < len &&
		    isxdigit((unsigned char)s[i+1]) &&
		    isxdigit((unsigned char)s[i+2])) {
			char hex[3] = { s[i+1], s[i+2], '\0' };
			s[out++] = (char)strtol(hex, NULL, 16);
			i += 2;
		} else if (s[i] == '+') {
			s[out++] = ' ';
		} else {
			s[out++] = s[i];
		}
	}
	return out;
}

duk_ret_t
dukky_urlsearchparams_tostring(duk_context *ctx)
{
	duk_push_this(ctx);
	duk_get_prop_string(ctx, -1, MAGIC(Params));
	duk_size_t len = duk_get_length(ctx, -1);

	if (len == 0) {
		duk_push_lstring(ctx, "", 0);
		return 1;
	}

	duk_idx_t parts = 0;
	for (duk_size_t i = 0; i < len; i++) {
		if (i > 0) {
			duk_push_lstring(ctx, "&", 1);
			parts++;
		}
		duk_get_prop_index(ctx, -1 - parts, (duk_uarridx_t)i);
		duk_get_prop_index(ctx, -1, 0);
		duk_remove(ctx, -2);
		parts++;
		duk_push_lstring(ctx, "=", 1);
		parts++;
		duk_get_prop_index(ctx, -1 - parts, (duk_uarridx_t)i);
		duk_get_prop_index(ctx, -1, 1);
		duk_remove(ctx, -2);
		parts++;
	}

	duk_concat(ctx, (duk_idx_t)parts);
	return 1;
}

duk_ret_t
dukky_url_tostring(duk_context *ctx)
{
	duk_push_this(ctx);
	duk_get_prop_string(ctx, -1, dukky_magic_string_private);
	url_private_t *priv = duk_get_pointer(ctx, -1);
	if (priv == NULL || priv->url == NULL) {
		duk_push_lstring(ctx, "", 0);
		return 1;
	}
	duk_push_string(ctx, nsurl_access(priv->url));
	return 1;
}

duk_ret_t
dukky_url_tojson(duk_context *ctx)
{
	return dukky_url_tostring(ctx);
}

duk_ret_t
dukky_url_constructor(duk_context *ctx)
{
	if (!duk_is_constructor_call(ctx))
		return duk_error(ctx, DUK_ERR_TYPE_ERROR,
				 "URL must be called with new");

	int nargs = duk_get_top(ctx);
	if (nargs < 1)
		return duk_error(ctx, DUK_ERR_TYPE_ERROR,
				 "URL constructor requires at least one argument");

	const char *href = duk_safe_to_string(ctx, 0);
	nsurl *parsed = NULL;
	nserror err;

	if (nargs >= 2 && !duk_is_undefined(ctx, 1)) {
		const char *base_s = duk_safe_to_string(ctx, 1);
		nsurl *base = NULL;
		err = nsurl_create(base_s, &base);
		if (err != NSERROR_OK)
			return duk_error(ctx, DUK_ERR_TYPE_ERROR,
					 "URL: invalid base URL");
		err = nsurl_join(base, href, &parsed);
		nsurl_unref(base);
		if (err != NSERROR_OK)
			return duk_error(ctx, DUK_ERR_TYPE_ERROR,
					 "URL: could not resolve relative URL");
	} else {
		err = nsurl_create(href, &parsed);
		if (err != NSERROR_OK)
			return duk_error(ctx, DUK_ERR_TYPE_ERROR,
					 "URL: invalid URL");
	}

	url_private_t *priv = calloc(1, sizeof(*priv));
	if (priv == NULL) {
		nsurl_unref(parsed);
		return duk_error(ctx, DUK_ERR_ERROR, "URL: out of memory");
	}
	priv->url = parsed;

	if (!dukky_construct_with_proto(ctx, PROTO_NAME(URL), priv)) {
		nsurl_unref(parsed);
		free(priv);
		return duk_error(ctx, DUK_ERR_ERROR,
				 "URL: prototype not found");
	}

	return 1;
}

duk_ret_t
dukky_urlsearchparams_constructor(duk_context *ctx)
{
	if (!duk_is_constructor_call(ctx))
		return duk_error(ctx, DUK_ERR_TYPE_ERROR,
				 "URLSearchParams must be called with new");

	url_search_params_private_t *priv = calloc(1, sizeof(*priv));
	if (priv == NULL)
		return duk_error(ctx, DUK_ERR_ERROR,
				 "URLSearchParams: out of memory");

	int nargs = duk_get_top(ctx);

	if (!dukky_construct_with_proto(ctx, PROTO_NAME(URLSEARCHPARAMS),
					priv)) {
		free(priv);
		return duk_error(ctx, DUK_ERR_ERROR,
				 "URLSearchParams: prototype not found");
	}

	duk_push_array(ctx);

	if (nargs >= 1 && duk_is_string(ctx, 0)) {
		duk_size_t init_len;
		const char *init = duk_safe_to_lstring(ctx, 0, &init_len);
		const char *p = init;
		if (init_len > 0 && p[0] == '?') { p++; init_len--; }

		uint32_t idx = 0;
		while (init_len > 0) {
			const char *amp = memchr(p, '&', init_len);
			size_t pair_len = amp ? (size_t)(amp - p) : init_len;
			const char *eq = memchr(p, '=', pair_len);
			size_t key_len = eq ? (size_t)(eq - p) : pair_len;
			size_t val_len = eq ? pair_len - key_len - 1 : 0;
			const char *val_p = eq ? eq + 1 : p + key_len;

			if (key_len > 0) {
				char *key = malloc(key_len + 1);
				char *val = malloc(val_len + 1);
				if (key != NULL && val != NULL) {
					memcpy(key, p, key_len);
					key[key_len] = '\0';
					size_t dk = usp_percent_decode(
							key, key_len);
					memcpy(val, val_p, val_len);
					val[val_len] = '\0';
					size_t dv = usp_percent_decode(
							val, val_len);
					duk_push_array(ctx);
					duk_push_lstring(ctx, key, dk);
					duk_put_prop_index(ctx, -2, 0);
					duk_push_lstring(ctx, val, dv);
					duk_put_prop_index(ctx, -2, 1);
					duk_put_prop_index(ctx, -2, idx++);
				}
				free(key);
				free(val);
			}

			if (amp == NULL) break;
			p = amp + 1;
			init_len -= pair_len + 1;
		}
	}

	duk_put_prop_string(ctx, -2, MAGIC(Params));
	return 1;
}

duk_ret_t
dukky_push_urlsearchparams(duk_context *ctx, const char *qs, size_t len)
{
	url_search_params_private_t *priv = calloc(1, sizeof(*priv));
	if (priv == NULL)
		return 0;

	if (!dukky_construct_with_proto(ctx, PROTO_NAME(URLSEARCHPARAMS),
					priv)) {
		free(priv);
		return 0;
	}

	duk_push_array(ctx);

	const char *p = qs;
	if (len > 0 && p[0] == '?') { p++; len--; }

	uint32_t idx = 0;
	while (len > 0) {
		const char *amp = memchr(p, '&', len);
		size_t pair_len = amp ? (size_t)(amp - p) : len;
		const char *eq = memchr(p, '=', pair_len);
		size_t key_len = eq ? (size_t)(eq - p) : pair_len;
		size_t val_len = eq ? pair_len - key_len - 1 : 0;
		const char *val_p = eq ? eq + 1 : p + key_len;

		if (key_len > 0) {
			char *key = malloc(key_len + 1);
			char *val = malloc(val_len + 1);
			if (key != NULL && val != NULL) {
				memcpy(key, p, key_len);
				key[key_len] = '\0';
				size_t dk = usp_percent_decode(key, key_len);
				memcpy(val, val_p, val_len);
				val[val_len] = '\0';
				size_t dv = usp_percent_decode(val, val_len);
				duk_push_array(ctx);
				duk_push_lstring(ctx, key, dk);
				duk_put_prop_index(ctx, -2, 0);
				duk_push_lstring(ctx, val, dv);
				duk_put_prop_index(ctx, -2, 1);
				duk_put_prop_index(ctx, -2, idx++);
			}
			free(key);
			free(val);
		}

		if (amp == NULL) break;
		p = amp + 1;
		len -= pair_len + 1;
	}

	duk_put_prop_string(ctx, -2, MAGIC(Params));
	return 1;
}
