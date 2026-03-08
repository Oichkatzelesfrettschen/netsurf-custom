/*
 * Copyright 2012 Vincent Sanders <vince@netsurf-browser.org>
 * Copyright 2015 Daniel Dilverstone <dsilvers@netsurf-browser.org>
 * Copyright 2016 Michael Drake <tlsa@netsurf-browser.org>
 * Copyright 2016 John-Mark Bell <jmb@netsurf-browser.org>
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
 * Duktapeish implementation of javascript engine functions.
 */

#include <stdint.h>
#include <nsutils/time.h>

#include "netsurf/inttypes.h"
#include "utils/utils.h"
#include "utils/nsoption.h"
#include "utils/log.h"
#include "utils/corestrings.h"
#include "content/content.h"

#include "javascript/js.h"
#include "javascript/content.h"

#include "duktape/binding.h"
#include "duktape/generics.js.inc"
#include "duktape/polyfill.js.inc"
#include "duktape/private.h"

#include "duktape.h"
#include "dukky.h"

#include <dom/dom.h>
#include <ctype.h>

#define EVENT_MAGIC MAGIC(EVENT_MAP)
#define HANDLER_LISTENER_MAGIC MAGIC(HANDLER_LISTENER_MAP)
#define HANDLER_MAGIC MAGIC(HANDLER_MAP)
#define EVENT_LISTENER_JS_MAGIC MAGIC(EVENT_LISTENER_JS_MAP)
#define GENERICS_MAGIC MAGIC(GENERICS_TABLE)
#define THREAD_MAP MAGIC(THREAD_MAP)

/**
 * dukky javascript heap
 */
struct jsheap {
	duk_context *ctx; /**< duktape base context */
	duk_uarridx_t next_thread; /**< monotonic thread counter */
	bool pending_destroy; /**< Whether this heap is pending destruction */
	unsigned int live_threads; /**< number of live threads */
	uint64_t exec_start_time;
	size_t heap_allocated; /**< current total bytes allocated */
	size_t heap_limit; /**< maximum bytes allowed (0 = unlimited) */
};

/**
 * dukky javascript thread
 */
struct jsthread {
	bool pending_destroy; /**< Whether this thread is pending destruction */
	unsigned int in_use; /**< The number of times this thread is in use */
	jsheap *heap; /**< The heap this thread belongs to */
	duk_context *ctx; /**< The duktape thread context */
	duk_uarridx_t thread_idx; /**< The thread number */
};

static duk_ret_t dukky_populate_object(duk_context *ctx, void *udata)
{
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
	return;
}

#undef SET_HTML_CLASS

static void
dukky_push_node_klass(duk_context *ctx, struct dom_node *node)
{
	dom_node_type nodetype;
	dom_exception err;

	err = dom_node_get_node_type(node, &nodetype);
	if (err != DOM_NO_ERR) {
		/* Oh bum, just node then */
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
			/* Feck it, element */
			NSLOG(dukky, ERROR,
			      "dom_node_get_namespace() failed");
			duk_push_string(ctx, PROTO_NAME(ELEMENT));
			break;
		}
		if (namespace == NULL) {
			/* No namespace, -> element */
			NSLOG(dukky, DEBUG, "no namespace");
			duk_push_string(ctx, PROTO_NAME(ELEMENT));
			break;
		}

		if (dom_string_isequal(namespace, corestring_dom_html_namespace) == false) {
			/* definitely not an HTML element of some kind */
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
		/* Oh bum, just node then */
		duk_push_string(ctx, PROTO_NAME(NODE));
	}
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
	return;
}

/* WHY: nsgenbind registers all classes with dukky_inject_not_ctr, which
 * throws "Bad constructor". URL and URLSearchParams need real constructors
 * so that `new URL(href)` and `new URLSearchParams(init)` work.
 *
 * WHY we cannot use dukky_create_object: nsgenbind registers the
 * __constructor function via duk_push_c_function(ctx, fn, 1), hard-coding
 * nargs=1. When dukky_populate_object calls duk_call(ctx, nargs+1), Duktape
 * truncates the argument list to 1 because the target function was registered
 * with nargs=1. This means the URL/USP init body never sees the constructor
 * arguments. Instead, we build the object manually: create object, set
 * prototype, allocate private data, and run the init logic inline. */

/* Helper: create object with prototype from proto table, set up handlers
 * and private data. Returns the object on top of the Duktape stack. */
static bool
dukky_construct_with_proto(duk_context *ctx, const char *proto_name,
			   void *priv_ptr)
{
	/* Push a new object */
	duk_push_object(ctx);
	/* Set up handler maps */
	duk_push_object(ctx);
	duk_put_prop_string(ctx, -2, MAGIC(HANDLER_LISTENER_MAP));
	duk_push_object(ctx);
	duk_put_prop_string(ctx, -2, MAGIC(HANDLER_MAP));

	/* Get prototype from proto table */
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

	/* Attach private data */
	duk_push_pointer(ctx, priv_ptr);
	duk_put_prop_string(ctx, -2, dukky_magic_string_private);

	/* Object with prototype and private data is on the stack */
	return true;
}

static duk_ret_t
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

/* WHY: nsgenbind does not generate toString for URLSearchParams because
 * adding it to the WebIDL conflicts with the generic toString installed by
 * dukky_create_prototype in binding.c. Instead, we install this proper
 * toString on the prototype after creation. */
static duk_ret_t
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

/* WHY: Same issue as URLSearchParams -- URL toString/toJSON are not in the
 * WebIDL (adding them conflicts with binding.c generic toString). */
static duk_ret_t
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

static duk_ret_t
dukky_url_tojson(duk_context *ctx)
{
	return dukky_url_tostring(ctx);
}

static duk_ret_t
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
	/* ... obj is on top of stack */

	/* Create internal params array */
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
	/* obj is still on top */
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

	/* Create params array and parse query string */
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
	return 1; /* USP object on stack */
}

/* Duktape heap utility functions */

/* We need to override the defaults because not all platforms are fully ANSI
 * compatible.  E.g. RISC OS gets upset if we malloc or realloc a zero byte
 * block, as do debugging tools such as Electric Fence by Bruce Perens.
 */

/* WHY: We prefix each allocation with a size_t header so free/realloc
 * can update the heap_allocated counter without a separate lookup table.
 * The header is hidden before the pointer returned to Duktape. */
#define ALLOC_HDR_SIZE (sizeof(size_t))
#define ALLOC_TO_HDR(p) ((size_t *)(p) - 1)
#define HDR_TO_ALLOC(h) ((void *)((size_t *)(h) + 1))

static void dukky_free_function(void *udata, void *ptr);

static void *dukky_alloc_function(void *udata, duk_size_t size)
{
	jsheap *heap = (jsheap *)udata;
	size_t *hdr;

	if (size == 0)
		return NULL;

	if (heap->heap_limit > 0 &&
	    heap->heap_allocated + size + ALLOC_HDR_SIZE > heap->heap_limit) {
		NSLOG(dukky, WARNING,
		      "JS heap limit reached (%zu / %zu), rejecting %zu byte alloc",
		      heap->heap_allocated, heap->heap_limit, (size_t)size);
		return NULL;
	}

	hdr = malloc(ALLOC_HDR_SIZE + size);
	if (hdr == NULL)
		return NULL;

	*hdr = size;
	heap->heap_allocated += size + ALLOC_HDR_SIZE;
	return HDR_TO_ALLOC(hdr);
}

static void *dukky_realloc_function(void *udata, void *ptr, duk_size_t size)
{
	jsheap *heap = (jsheap *)udata;
	size_t *hdr;
	size_t *new_hdr;
	size_t old_size;

	if (ptr == NULL && size == 0)
		return NULL;

	if (ptr == NULL)
		return dukky_alloc_function(udata, size);

	if (size == 0) {
		dukky_free_function(udata, ptr);
		return NULL;
	}

	hdr = ALLOC_TO_HDR(ptr);
	old_size = *hdr;

	if (size > old_size && heap->heap_limit > 0 &&
	    heap->heap_allocated + (size - old_size) > heap->heap_limit) {
		NSLOG(dukky, WARNING,
		      "JS heap limit reached (%zu / %zu), rejecting %zu byte realloc",
		      heap->heap_allocated, heap->heap_limit, (size_t)size);
		return NULL;
	}

	new_hdr = realloc(hdr, ALLOC_HDR_SIZE + size);
	if (new_hdr == NULL)
		return NULL;
	hdr = new_hdr;

	heap->heap_allocated -= old_size + ALLOC_HDR_SIZE;
	*hdr = size;
	heap->heap_allocated += size + ALLOC_HDR_SIZE;
	return HDR_TO_ALLOC(hdr);
}


static void dukky_free_function(void *udata, void *ptr)
{
	jsheap *heap = (jsheap *)udata;
	size_t *hdr;

	if (ptr == NULL)
		return;

	hdr = ALLOC_TO_HDR(ptr);
	heap->heap_allocated -= *hdr + ALLOC_HDR_SIZE;
	free(hdr);
}

/* exported interface documented in js.h */
void js_initialise(void)
{
	/** TODO: Forces JS on for our testing, needs changing before a release
	 * lest we incur the wrath of others.
	 */
	/* Disabled force-on for forthcoming release */
	/* nsoption_set_bool(enable_javascript, true);
	 */
	javascript_init();
}


/* exported interface documented in js.h */
void js_finalise(void)
{
	/* NADA for now */
}


/* exported interface documented in js.h */
nserror
js_newheap(int timeout, jsheap **heap)
{
	duk_context *ctx;
	jsheap *ret = calloc(1, sizeof(*ret));
	*heap = NULL;
	NSLOG(dukky, DEBUG, "Creating new duktape javascript heap");
	if (ret == NULL) return NSERROR_NOMEM;
	ret->heap_allocated = 0;
	ret->heap_limit = (size_t)nsoption_int(js_heap_limit);
	ctx = ret->ctx = duk_create_heap(
		dukky_alloc_function,
		dukky_realloc_function,
		dukky_free_function,
		ret,
		NULL);
	if (ret->ctx == NULL) { free(ret); return NSERROR_NOMEM; }
	/* Create the prototype stuffs */
	duk_push_global_object(ctx);
	duk_push_boolean(ctx, true);
	duk_put_prop_string(ctx, -2, "protos");
	duk_put_global_string(ctx, PROTO_MAGIC);
	/* Create prototypes here */
	dukky_create_prototypes(ctx);
	/* Now create the thread map */
	duk_push_object(ctx);
	duk_put_global_string(ctx, THREAD_MAP);

	*heap = ret;
	return NSERROR_OK;
}


static void dukky_destroyheap(jsheap *heap)
{
	assert(heap->pending_destroy == true);
	assert(heap->live_threads == 0);
	NSLOG(dukky, DEBUG, "Destroying duktape javascript context");
	duk_destroy_heap(heap->ctx);
	free(heap);
}

/* exported interface documented in js.h */
void js_destroyheap(jsheap *heap)
{
	heap->pending_destroy = true;
	if (heap->live_threads == 0) {
		dukky_destroyheap(heap);
	}
}

/* Just for here, the CTX is in ret, not thread */
#define CTX (ret->ctx)

/* exported interface documented in js.h */
nserror js_newthread(jsheap *heap, void *win_priv, void *doc_priv, jsthread **thread)
{
	jsthread *ret;
	assert(heap != NULL);
	assert(heap->pending_destroy == false);

	ret = calloc(1, sizeof (*ret));
	if (ret == NULL) {
		NSLOG(dukky, ERROR, "Unable to allocate new JS thread structure");
		return NSERROR_NOMEM;
	}

	NSLOG(dukky, DEBUG,
	      "New javascript/duktape thread, win_priv=%p, doc_priv=%p",
	      win_priv, doc_priv);

	/* create new thread */
	duk_get_global_string(heap->ctx, THREAD_MAP); /* ... threads */
	duk_push_thread(heap->ctx); /* ... threads thread */
	ret->heap = heap;
	ret->ctx = duk_require_context(heap->ctx, -1);
	ret->thread_idx = heap->next_thread++;
	duk_put_prop_index(heap->ctx, -2, ret->thread_idx);
	heap->live_threads++;
	duk_pop(heap->ctx); /* ... */
	duk_push_int(CTX, 0);
	duk_push_int(CTX, 1);
	duk_push_int(CTX, 2);
	/* Manufacture a Window object */
	/* win_priv is a browser_window, doc_priv is an html content struct */
	duk_push_pointer(CTX, win_priv);
	duk_push_pointer(CTX, doc_priv);
	dukky_create_object(CTX, PROTO_NAME(WINDOW), 2);
	duk_push_global_object(CTX);
	duk_put_prop_string(CTX, -2, PROTO_MAGIC);
	duk_set_global_object(CTX);

	/* Now we need to prepare our node mapping table */
	duk_push_object(CTX);
	duk_push_pointer(CTX, NULL);
	duk_push_null(CTX);
	duk_put_prop(CTX, -3);
	duk_put_global_string(CTX, NODE_MAGIC);

	/* And now the event mapping table */
	duk_push_object(CTX);
	duk_put_global_string(CTX, EVENT_MAGIC);

	/* Now load the polyfills */
	/* ... */
	duk_push_string(CTX, "polyfill.js");
	/* ..., polyfill.js */
	if (duk_pcompile_lstring_filename(CTX, DUK_COMPILE_EVAL,
					  (const char *)polyfill_js, polyfill_js_len) != 0) {
		NSLOG(dukky, CRITICAL, "%s", duk_safe_to_string(CTX, -1));
		NSLOG(dukky, CRITICAL, "Unable to compile polyfill.js, thread aborted");
		js_destroythread(ret);
		return NSERROR_INIT_FAILED;
	}
	/* ..., (generics.js) */
	if (dukky_pcall(CTX, 0, true) != 0) {
		NSLOG(dukky, CRITICAL, "Unable to run polyfill.js, thread aborted");
		js_destroythread(ret);
		return NSERROR_INIT_FAILED;
	}
	/* ..., result */
	duk_pop(CTX);
	/* ... */

	/* WHY: polyfill.js stores Map, Set, etc. on globalThis, which in
	 * Duktape after duk_set_global_object is the OLD Duktape global
	 * (stored as PROTO_MAGIC on Window), not the Window itself. Variable
	 * resolution uses Window (the current global), so bare `Map` fails.
	 *
	 * Fix: copy polyfill-defined names from the old Duktape global
	 * (PROTO_MAGIC) onto the Window, and register real constructors
	 * for URL and URLSearchParams.
	 */
	{
		duk_push_global_object(CTX);
		/* ... Win */

		/* Get old Duktape global (stored as PROTO_MAGIC on Window) */
		duk_get_prop_string(CTX, -1, PROTO_MAGIC);
		/* ... Win oldGlobal */

		/* Copy ES6 polyfills from old global onto Window */
		static const char * const es6_names[] = {
			"Map", "Set", "WeakMap", "WeakSet", "Promise", NULL
		};
		for (const char * const *p = es6_names; *p != NULL; p++) {
			if (duk_get_prop_string(CTX, -1, *p)) {
				/* ... Win oldGlobal value */
				duk_put_prop_string(CTX, -3, *p);
				/* ... Win oldGlobal */
			} else {
				duk_pop(CTX); /* undefined */
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
		duk_remove(CTX, -2); /* prototypes table */
		duk_put_prop_string(CTX, -2, "prototype");
		duk_put_prop_string(CTX, -2, "URL");

		/* Register real URLSearchParams constructor */
		duk_push_c_function(CTX, dukky_urlsearchparams_constructor,
				    DUK_VARARGS);
		duk_get_global_string(CTX, dukky_magic_string_prototypes);
		duk_get_prop_string(CTX, -1,
			"\xFF\xFFNETSURF_DUKTAPE_PROTOTYPE_URLSEARCHPARAMS");
		duk_remove(CTX, -2); /* prototypes table */
		duk_put_prop_string(CTX, -2, "prototype");
		duk_put_prop_string(CTX, -2, "URLSearchParams");

		/* WHY: Install real toString/toJSON on URL and USP prototypes.
		 * dukky_create_prototype installs a generic toString that
		 * returns "[object ClassName]". We need proper implementations.
		 * Use duk_def_prop with FORCE to override regardless of
		 * writability. */
		duk_get_global_string(CTX, dukky_magic_string_prototypes);
		/* ... Win prototab */

		/* URL prototype: toString and toJSON */
		duk_get_prop_string(CTX, -1,
			"\xFF\xFFNETSURF_DUKTAPE_PROTOTYPE_URL");
		/* ... Win prototab url_proto */
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

		/* WHY: Node.ELEMENT_NODE etc. are const members in the WebIDL
		 * but nsgenbind doesn't register them as properties on the
		 * constructor function. Install them on the Node constructor
		 * and prototype for spec compliance. */
		duk_get_prop_string(CTX, -1, "Node");
		/* ... Win Node */
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
		/* ... */
	}

	/* Now load the NetSurf table in */
	/* ... */
	duk_push_string(CTX, "generics.js");
	/* ..., generics.js */
	if (duk_pcompile_lstring_filename(CTX, DUK_COMPILE_EVAL,
					  (const char *)generics_js, generics_js_len) != 0) {
		NSLOG(dukky, CRITICAL, "%s", duk_safe_to_string(CTX, -1));
		NSLOG(dukky, CRITICAL, "Unable to compile generics.js, thread aborted");
		js_destroythread(ret);
		return NSERROR_INIT_FAILED;
	}
	/* ..., (generics.js) */
	if (dukky_pcall(CTX, 0, true) != 0) {
		NSLOG(dukky, CRITICAL, "Unable to run generics.js, thread aborted");
		js_destroythread(ret);
		return NSERROR_INIT_FAILED;
	}
	/* ..., result */
	duk_pop(CTX);
	/* ... */
	duk_push_global_object(CTX);
	/* ..., Win */
	duk_get_prop_string(CTX, -1, "NetSurf");
	/* ..., Win, NetSurf */
	duk_put_global_string(CTX, GENERICS_MAGIC);
	/* ..., Win */
	duk_del_prop_string(CTX, -1, "NetSurf");
	duk_pop(CTX);
	/* ... */

	dukky_log_stack_frame(CTX, "New thread created");
	NSLOG(dukky, DEBUG, "New thread is %p in heap %p", thread, heap);
	*thread = ret;

	return NSERROR_OK;
}

/* Now switch to the long term CTX behaviour */
#undef CTX
#define CTX (thread->ctx)

/* exported interface documented in js.h */
nserror js_closethread(jsthread *thread)
{
	/* We can always close down a thread, it might just confuse
	 * the code running, though we don't mind since we're in the
	 * process of destruction at this point
	 */
	duk_int_t top = duk_get_top(CTX);

	/* Closing down the extant thread */
	NSLOG(dukky, DEBUG, "Closing down extant thread %p in heap %p", thread, thread->heap);
	duk_get_global_string(CTX, MAGIC(closedownThread));
	dukky_pcall(CTX, 0, true);

	/* Restore whatever stack we had */
	duk_set_top(CTX, top);

	return NSERROR_OK;
}

/**
 * Destroy a Duktape thread
 */
static void dukky_destroythread(jsthread *thread)
{
	jsheap *heap = thread->heap;

	assert(thread->in_use == 0);
	assert(thread->pending_destroy == true);

	/* Closing down the extant thread */
	NSLOG(dukky, DEBUG, "Closing down extant thread %p in heap %p", thread, heap);
	duk_get_global_string(CTX, MAGIC(closedownThread));
	dukky_pcall(CTX, 0, true);

	/* Now delete the thread from the heap */
	duk_get_global_string(heap->ctx, THREAD_MAP); /* ... threads */
	duk_del_prop_index(heap->ctx, -1, thread->thread_idx);
	duk_pop(heap->ctx); /* ... */

	/* We can now free the thread object */
	free(thread);

	/* Finally give the heap a chance to clean up */
	duk_gc(heap->ctx, 0);
	duk_gc(heap->ctx, DUK_GC_COMPACT);
	heap->live_threads--;

	/* And if the heap should now go, blow it away */
	if (heap->pending_destroy == true && heap->live_threads == 0) {
		dukky_destroyheap(heap);
	}
}

/* exported interface documented in js.h */
void js_destroythread(jsthread *thread)
{
	thread->pending_destroy = true;
	if (thread->in_use == 0) {
		dukky_destroythread(thread);
	}
}

static void dukky_enter_thread(jsthread *thread)
{
	assert(thread != NULL);
	thread->in_use++;
}

static void dukky_leave_thread(jsthread *thread)
{
	assert(thread != NULL);
	assert(thread->in_use > 0);

	thread->in_use--;
	if (thread->in_use == 0 && thread->pending_destroy == true) {
		dukky_destroythread(thread);
	}
}

duk_bool_t dukky_check_timeout(void *udata)
{
#define JS_EXEC_TIMEOUT_MS 10000 /* 10 seconds */
	jsheap *heap = (jsheap *) udata;
	uint64_t now;

	(void) nsu_getmonotonic_ms(&now);

	/* This function may be called during duk heap construction,
	 * so only test for execution timeout if we've recorded a
	 * start time.
	 */
	return heap->exec_start_time != 0 &&
			now > (heap->exec_start_time + JS_EXEC_TIMEOUT_MS);
}

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
	duk_memory_functions funcs;
	jsheap *heap;
	duk_get_memory_functions(ctx, &funcs);
	heap = funcs.udata;
	(void) nsu_getmonotonic_ms(&heap->exec_start_time);
}

duk_int_t dukky_pcall(duk_context *ctx, duk_size_t argc, bool reset_timeout)
{
	if (reset_timeout) {
		dukky_reset_start_time(ctx);
	}

	duk_int_t ret = duk_pcall(ctx, argc);
	if (ret) {
		/* Something went wrong calling this... */
		dukky_dump_error(ctx);
	}

	return ret;
}


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

static duk_int_t dukky_push_context_dump(duk_context *ctx, void *udata)
{
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


/* exported interface documented in js.h */
bool
js_exec(jsthread *thread, const uint8_t *txt, size_t txtlen, const char *name)
{
	bool ret = false;
	assert(thread);

	if (txt == NULL || txtlen == 0) {
		return false;
	}

	if (thread->pending_destroy) {
		NSLOG(dukky, DEEPDEBUG, "Skipping exec call because thread is dead");
		return false;
	}

	dukky_enter_thread(thread);

	duk_set_top(CTX, 0);
	NSLOG(dukky, DEEPDEBUG, "Running %"PRIsizet" bytes from %s", txtlen, name);
	/* NSLOG(dukky, DEEPDEBUG, "\n%s\n", txt); */

	dukky_reset_start_time(CTX);
	if (name != NULL) {
		duk_push_string(CTX, name);
	} else {
		duk_push_string(CTX, "?unknown source?");
	}
	if (duk_pcompile_lstring_filename(CTX,
					  DUK_COMPILE_EVAL,
					  (const char *)txt,
					  txtlen) != 0) {
		NSLOG(dukky, DEBUG, "Failed to compile JavaScript input");
		goto handle_error;
	}

	if (duk_pcall(CTX, 0/*nargs*/) == DUK_EXEC_ERROR) {
		NSLOG(dukky, DEBUG, "Failed to execute JavaScript");
		goto handle_error;
	}

	if (duk_get_top(CTX) == 0) duk_push_boolean(CTX, false);
	NSLOG(dukky, DEEPDEBUG, "Returning %s",
	      duk_get_boolean(CTX, 0) ? "true" : "false");
	ret = duk_get_boolean(CTX, 0);
	goto out;

handle_error:
	dukky_dump_error(CTX);
out:
	dukky_leave_thread(thread);
	return ret;
}

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

/*** New style event handling ***/

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

	/* If et is NULL, then we're actually dealing with the Window object
	 * which has no default handlers and no way to assign handlers
	 * which aren't directly stored in the HANDLER_MAGIC
	 */
	if (et == NULL) {
		duk_push_lstring(ctx, "", 0);
		return;
	}

	/* The rest of this assumes et is a proper event target and expands
	 * out from there based on the assumption that all valid event targets
	 * are nodes.
	 */
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
		/* TODO: If this is null, clean up and propagate */
		/* ... node handlercode */
		/** @todo This is entirely wrong, but it's hard to get right */
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

	NSLOG(dukky, DEBUG, "Handling an event in duktape interface...");
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
	NSLOG(dukky, DEBUG, "Event phase is: %s (%d)",
	      phase == DOM_CAPTURING_PHASE ? "capturing" : phase == DOM_AT_TARGET ? "at-target" : phase == DOM_BUBBLING_PHASE ? "bubbling" : "unknown",
	      (int)phase);

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
	dukky_reset_start_time(ctx);
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
		dukky_reset_start_time(ctx);
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

	/* Otherwise add an event listener to the element */
	exc = dom_event_listener_create(dukky_generic_event_handler, ctx,
					&listen);
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
/* We return true if we created a new sublistener table */
/* If we're told to not create, but we want to, we still return true */
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

/* Shuffle a duktape array "down" one.  This involves iterating from
 * the index provided, shuffling elements down, until we reach an
 * undefined
 */
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


void js_handle_new_element(jsthread *thread, struct dom_element *node)
{
	assert(thread);
	assert(node);
	dom_namednodemap *map;
	dom_exception exc;
	dom_ulong idx;
	dom_ulong siz;
	dom_attr *attr = NULL;
	dom_string *key = NULL;
	dom_string *nodename;
	duk_bool_t is_body = false;

	exc = dom_node_get_node_name(node, &nodename);
	if (exc != DOM_NO_ERR) return;

	if (nodename == corestring_dom_BODY)
		is_body = true;

	dom_string_unref(nodename);

	exc = dom_node_get_attributes(node, &map);
	if (exc != DOM_NO_ERR) return;
	if (map == NULL) return;

	dukky_enter_thread(thread);

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
			/* This is a forwarded event, it doesn't matter,
			 * we should skip registering for it and later
			 * we will register it for Window itself
			 */
			goto skip_register;
		}
		if (dom_string_length(key) > 2) {
			/* Can be on* */
			const uint8_t *data = (const uint8_t *)dom_string_data(key);
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

	dukky_leave_thread(thread);
}

void js_event_cleanup(jsthread *thread, struct dom_event *evt)
{
	assert(thread);
	dukky_enter_thread(thread);
	/* ... */
	duk_get_global_string(CTX, EVENT_MAGIC);
	/* ... EVENT_MAP */
	duk_push_pointer(CTX, evt);
	/* ... EVENT_MAP eventptr */
	duk_del_prop(CTX, -2);
	/* ... EVENT_MAP */
	duk_pop(CTX);
	/* ... */
	dukky_leave_thread(thread);
}

bool js_fire_event(jsthread *thread, const char *type, struct dom_document *doc, struct dom_node *target)
{
	dom_exception exc;
	dom_event *evt;
	dom_event_target *body;

	NSLOG(dukky, DEBUG, "Event: %s (doc=%p, target=%p)", type, doc,
	      target);

	/** @todo Make this more generic, this only handles load and only
	 * targetting the window, so that we actually stand a chance of
	 * getting 3.4 out.
	 */

	if (target != NULL)
		/* Swallow non-Window-targetted events quietly */
		return true;

	if (strcmp(type, "load") != 0)
		/* Swallow non-load events quietly */
		return true;

	/* Okay, we're processing load, targetted at Window, do the single
	 * thing which gets us there, which is to find the appropriate event
	 * handler and call it.  If we have no event handler on Window then
	 * we divert to the body, and if there's no event handler there
	 * we swallow the event silently
	 */

	exc = dom_event_create(&evt);
	if (exc != DOM_NO_ERR) return true;
	exc = dom_event_init(evt, corestring_dom_load, false, false);
	if (exc != DOM_NO_ERR) {
		dom_event_unref(evt);
		return true;
	}
	dukky_enter_thread(thread);
	/* ... */
	duk_get_global_string(CTX, HANDLER_MAGIC);
	/* ... handlers */
	duk_push_lstring(CTX, "load", 4);
	/* ... handlers "load" */
	duk_get_prop(CTX, -2);
	/* ... handlers handler? */
	if (duk_is_undefined(CTX, -1)) {
		/* No handler here, *try* and retrieve a handler from
		 * the body
		 */
		duk_pop(CTX);
		/* ... handlers */
		exc = dom_html_document_get_body(doc, &body);
		if (exc != DOM_NO_ERR) {
			dom_event_unref(evt);
			dukky_leave_thread(thread);
			return true;
		}
		dukky_push_node(CTX, (struct dom_node *)body);
		/* ... handlers bodynode */
		if (dukky_get_current_value_of_event_handler(
			    CTX, corestring_dom_load, body) == false) {
			/* Unref the body, we don't need it any more */
			dom_node_unref(body);
			/* ... handlers */
			duk_pop(CTX);
			dukky_leave_thread(thread);
			return true;
		}
		/* Unref the body, we don't need it any more */
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
	dukky_reset_start_time(CTX);
	if (duk_pcall_method(CTX, 1) != 0) {
		/* Failed to run the handler */
		/* ... err */
		NSLOG(dukky, DEBUG,
		      "OH NOES! An error running a handler.  Meh.");
		duk_get_prop_string(CTX, -1, "name");
		duk_get_prop_string(CTX, -2, "message");
		duk_get_prop_string(CTX, -3, "fileName");
		duk_get_prop_string(CTX, -4, "lineNumber");
		duk_get_prop_string(CTX, -5, "stack");
		/* ... err name message fileName lineNumber stack */
		NSLOG(dukky, DEBUG, "Uncaught error in JS: %s: %s",
		      duk_safe_to_string(CTX, -5),
		      duk_safe_to_string(CTX, -4));
		NSLOG(dukky, DEBUG, "              was at: %s line %s",
		      duk_safe_to_string(CTX, -3),
		      duk_safe_to_string(CTX, -2));
		NSLOG(dukky, DEBUG, "         Stack trace: %s",
		      duk_safe_to_string(CTX, -1));

		duk_pop_n(CTX, 6);
		/* ... */
		js_event_cleanup(thread, evt);
		dom_event_unref(evt);
		dukky_leave_thread(thread);
		return true;
	}
	/* ... result */
	duk_pop(CTX);
	/* ... */
	js_event_cleanup(thread, evt);
	dom_event_unref(evt);
	dukky_leave_thread(thread);
	return true;
}

/* ---- innerHTML serializer ------------------------------------------------ */

/**
 * Return true if \a name (lower-case element name) is a void element.
 *
 * Uses a first-character dispatch to reduce worst-case comparisons from
 * 14 (linear scan) to at most 2.  Covers all 14 WHATWG void elements:
 * area, base, br, col, embed, hr, img, input, link, meta, param,
 * source, track, wbr.
 */
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

/**
 * Append a text-content string to the Duktape stack, HTML-escaping
 * '<', '>', '&', and optionally '"' (for attribute values).
 *
 * Pushes pieces as individual strings; caller must concatenate them.
 *
 * \param ctx        Duktape context
 * \param s          UTF-8 text to escape
 * \param len        Byte length of \a s
 * \param in_attr    true if inside an attribute value (escape '"' too)
 * \return Number of strings pushed onto the stack
 */
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

/**
 * Forward declaration for mutual recursion with serialize_element.
 */
static duk_idx_t serialize_children(duk_context *ctx, dom_node *parent);

/**
 * Serialize a single DOM node to HTML, pushing string pieces onto the stack.
 *
 * COMPLEXITY NOTE:
 *   O(N * M) where N = total nodes in subtree, M = attributes per element.
 *   Each element generates 5 + 2*M duk_push_* calls, then a duk_concat.
 *   All pieces are buffered on the Duktape value stack; there is no streaming.
 *
 * FUTURE OPTIMIZATION:
 *   Cache the serialized string and invalidate via MutationObserver when the
 *   subtree changes.  This would make repeated elem.innerHTML reads free.
 *   MutationObserver is not yet implemented (XL effort; requires libdom hooks).
 *
 * \param ctx   Duktape context
 * \param node  Node to serialize (not ref'd by this function)
 * \return Number of string pieces pushed
 */
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
		/* Get tag name (lower-case for HTML documents) */
		dom_string *name_s = NULL;
		exc = dom_node_get_node_name(node, &name_s);
		if (exc != DOM_NO_ERR || name_s == NULL) {
			break;
		}

		/* Convert to lower-case C string for void-element check */
		size_t name_len = dom_string_byte_length(name_s);
		const char *name_data = dom_string_data(name_s);

		/* Use a fixed-size buffer for tag names; truncate silently */
		char tag_lower[64];
		size_t copy_len = name_len < 63 ? name_len : 63;
		for (size_t i = 0; i < copy_len; i++) {
			char c = name_data[i];
			tag_lower[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
		}
		tag_lower[copy_len] = '\0';

		/* Opening tag */
		duk_push_string(ctx, "<");
		duk_push_lstring(ctx, tag_lower, copy_len);
		pushed += 2;

		/* Serialize attributes */
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
			/* Serialize children */
			pushed += serialize_children(ctx, node);

			/* Closing tag */
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
		/* Processing instructions, document types etc. are omitted per
		 * the HTML5 serialization spec's inner-HTML algorithm. */
		break;
	}

	return pushed;
}

/**
 * Serialize all children of \a parent, pushing string pieces onto the stack.
 *
 * \param ctx     Duktape context
 * \param parent  Parent node whose children to walk
 * \return Total number of string pieces pushed
 */
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

/* exported interface documented in dukky.h */
duk_ret_t dukky_push_node_innerhtml(duk_context *ctx, struct dom_node *node)
{
	duk_idx_t pieces = serialize_children(ctx, node);

	if (pieces == 0) {
		duk_push_lstring(ctx, "", 0);
		return 1;
	}

	/* Concatenate all pieces into one string */
	duk_concat(ctx, pieces);
	return 1;
}


/* -- querySelector / querySelectorAll implementation --
 *
 * WHY: querySelector is the single most impactful missing API. jQuery,
 * React, Vue, and virtually all modern JS use it as the primary DOM
 * query mechanism.
 *
 * HOW: We implement a subset CSS selector parser (Option B from the
 * roadmap) covering: #id, .class, tag, tag.class, tag#id, [attr],
 * [attr=val], and descendant/child/sibling combinators. This covers
 * 90%+ of real-world querySelector usage.
 */

/**
 * Parsed simple selector (one segment of a compound selector).
 */
struct simple_selector {
	const char *tag;         /**< tag name (NULL = any) */
	size_t tag_len;
	const char *id;          /**< id value (NULL = none) */
	size_t id_len;
	const char *cls;         /**< class value (NULL = none) */
	size_t cls_len;
	const char *attr;        /**< attribute name (NULL = none) */
	size_t attr_len;
	const char *attr_val;    /**< attribute value (NULL = presence-only) */
	size_t attr_val_len;
};

/** Combinator between compound selectors */
enum selector_combinator {
	COMB_NONE,         /**< first selector */
	COMB_DESCENDANT,   /**< whitespace: ancestor descendant */
	COMB_CHILD,        /**< >: parent > child */
};

/** A compound selector: a chain of simple selectors with combinators */
#define MAX_SELECTOR_PARTS 8
struct compound_selector {
	struct simple_selector parts[MAX_SELECTOR_PARTS];
	enum selector_combinator combinators[MAX_SELECTOR_PARTS];
	int count;
};

/**
 * Parse a CSS selector string into a compound_selector structure.
 *
 * Supports: tag, #id, .class, tag#id, tag.class, [attr], [attr=val],
 * tag[attr=val], descendant (space) and child (>) combinators.
 *
 * \return true on success, false on parse error
 */
static bool
parse_selector(const char *sel, size_t sel_len,
	       struct compound_selector *out)
{
	const char *p = sel;
	const char *end = sel + sel_len;
	int idx = 0;

	memset(out, 0, sizeof(*out));

	/* Skip leading whitespace */
	while (p < end && (*p == ' ' || *p == '\t'))
		p++;

	if (p >= end)
		return false;

	while (p < end && idx < MAX_SELECTOR_PARTS) {
		struct simple_selector *s = &out->parts[idx];

		/* Parse one simple selector */
		while (p < end && *p != ' ' && *p != '\t' && *p != '>' &&
		       *p != ',' && *p != '\0') {
			if (*p == '#') {
				p++; /* skip '#' */
				s->id = p;
				while (p < end && *p != '.' && *p != '[' &&
				       *p != '#' && *p != ' ' && *p != '\t' &&
				       *p != '>' && *p != ',' && *p != '\0')
					p++;
				s->id_len = (size_t)(p - s->id);
			} else if (*p == '.') {
				p++; /* skip '.' */
				s->cls = p;
				while (p < end && *p != '.' && *p != '[' &&
				       *p != '#' && *p != ' ' && *p != '\t' &&
				       *p != '>' && *p != ',' && *p != '\0')
					p++;
				s->cls_len = (size_t)(p - s->cls);
			} else if (*p == '[') {
				p++; /* skip '[' */
				s->attr = p;
				while (p < end && *p != '=' && *p != ']')
					p++;
				s->attr_len = (size_t)(p - s->attr);
				if (p < end && *p == '=') {
					p++; /* skip '=' */
					/* Skip optional quotes */
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
						if (p < end) p++; /* skip closing quote */
					} else {
						while (p < end && *p != ']')
							p++;
						s->attr_val_len = (size_t)(p - s->attr_val);
					}
				}
				if (p < end && *p == ']')
					p++;
			} else if (*p == '*') {
				/* Universal selector: tag stays NULL */
				p++;
			} else {
				/* Tag name */
				s->tag = p;
				while (p < end && *p != '.' && *p != '[' &&
				       *p != '#' && *p != ' ' && *p != '\t' &&
				       *p != '>' && *p != ',' && *p != '\0')
					p++;
				s->tag_len = (size_t)(p - s->tag);
			}
		}

		idx++;

		/* Skip whitespace and check for combinator */
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
			/* Descendant combinator (whitespace already consumed) */
			if (idx < MAX_SELECTOR_PARTS)
				out->combinators[idx] = COMB_DESCENDANT;
		}
	}

	out->count = idx;
	return idx > 0;
}

/**
 * Case-insensitive comparison of a string against a dom_string.
 */
static bool
strncasematch(const char *a, size_t a_len, dom_string *b)
{
	if (b == NULL)
		return false;
	if (a_len != dom_string_length(b))
		return false;
	return strncasecmp(a, dom_string_data(b), a_len) == 0;
}

/**
 * Test whether a DOM element matches a simple selector.
 */
static bool
element_matches_simple(dom_element *element, const struct simple_selector *sel)
{
	dom_string *val = NULL;
	dom_exception exc;

	/* Match tag name (case-insensitive) */
	if (sel->tag != NULL) {
		exc = dom_node_get_node_name((dom_node *)element, &val);
		if (exc != DOM_NO_ERR || val == NULL)
			return false;
		bool match = strncasematch(sel->tag, sel->tag_len, val);
		dom_string_unref(val);
		if (!match)
			return false;
	}

	/* Match id */
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

	/* Match class (checks if the element's class list contains the value) */
	if (sel->cls != NULL) {
		exc = dom_element_get_attribute(element,
						corestring_dom_class, &val);
		if (exc != DOM_NO_ERR || val == NULL)
			return false;
		/* Search for the class name as a whole word */
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

	/* Match attribute */
	if (sel->attr != NULL) {
		dom_string *attr_name = NULL;
		exc = dom_string_create((const uint8_t *)sel->attr,
					sel->attr_len, &attr_name);
		if (exc != DOM_NO_ERR)
			return false;

		if (sel->attr_val != NULL) {
			/* Attribute value match */
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
			/* Attribute presence check */
			bool has = false;
			exc = dom_element_has_attribute(element, attr_name, &has);
			dom_string_unref(attr_name);
			if (exc != DOM_NO_ERR || !has)
				return false;
		}
	}

	return true;
}

/**
 * Test whether an element matches a compound selector (all parts).
 */
static bool
element_matches_compound(dom_element *element,
			 const struct compound_selector *sel)
{
	/* Walk backward through selector parts, checking combinators */
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

/**
 * Walk the DOM subtree rooted at \a root, calling element_matches_compound
 * on each element node. Pushes matching nodes onto a Duktape array.
 *
 * \param ctx     Duktape context
 * \param root    Root node to search from (not tested itself)
 * \param sel     Parsed compound selector
 * \param arr_idx Duktape stack index of the results array
 * \param first_only If true, stop after first match
 * \return number of matches found
 */
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
			/* Recurse into children */
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

/* exported interface documented in dukky.h */
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

	/* Handle comma-separated selectors (selector list) for qSA */
	/* For simplicity, we support only the first selector in a list */
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

/* exported interface documented in dukky.h */
bool
dukky_element_matches_selector(dom_element *element,
			       const char *sel_str, size_t sel_len)
{
	struct compound_selector sel;

	/* Handle comma-separated selectors: match if any sub-selector matches */
	const char *comma = memchr(sel_str, ',', sel_len);
	size_t parse_len = comma ? (size_t)(comma - sel_str) : sel_len;

	if (!parse_selector(sel_str, parse_len, &sel)) {
		return false;
	}

	return element_matches_compound(element, &sel);
}
