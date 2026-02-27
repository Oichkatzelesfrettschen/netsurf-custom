/*
 * Copyright 2009 John-Mark Bell <jmb@netsurf-browser.org>
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

#ifndef NETSURF_CSS_SELECT_H_
#define NETSURF_CSS_SELECT_H_

#include <stdint.h>

#include <dom/dom.h>

#include <libcss/libcss.h>

struct content;
struct nsurl;

/**
 * Selection context
 *
 * Carries per-document state used by the libcss selection callbacks,
 * including live interactive state so that the :hover, :active, and
 * :focus pseudo-class callbacks can return accurate results.
 *
 * hover_node  -- DOM node currently under the pointer, or NULL
 * active_node -- DOM node with mouse button held down, or NULL
 * focus_node  -- DOM node with keyboard focus, or NULL
 *
 * These are not ref-counted; their lifetimes are the document lifetime.
 * The html handler sets them on mouse-track / focus-change events.
 */
typedef struct nscss_select_ctx
{
	css_select_ctx *ctx;
	bool quirks;
	struct nsurl *base_url;
	lwc_string *universal;
	const css_computed_style *root_style;
	const css_computed_style *parent_style;
	/** DOM node currently under the pointer (:hover), or NULL. */
	struct dom_node *hover_node;
	/** DOM node with mouse button held down (:active), or NULL. */
	struct dom_node *active_node;
	/** DOM node with keyboard focus (:focus), or NULL. */
	struct dom_node *focus_node;
} nscss_select_ctx;

css_stylesheet *nscss_create_inline_style(const uint8_t *data, size_t len,
		const char *charset, const char *url, bool allow_quirks);

css_select_results *nscss_get_style(nscss_select_ctx *ctx, dom_node *n,
		const css_media *media,
		const css_unit_ctx *unit_len_ctx,
		const css_stylesheet *inline_style);

css_computed_style *nscss_get_blank_style(nscss_select_ctx *ctx,
		const css_unit_ctx *unit_len_ctx,
		const css_computed_style *parent);


css_error named_ancestor_node(void *pw, void *node,
		const css_qname *qname, void **ancestor);

css_error node_is_visited(void *pw, void *node, bool *match);

#endif
