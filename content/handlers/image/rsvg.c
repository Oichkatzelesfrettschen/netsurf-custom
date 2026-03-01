/*
 * Copyright 2007 Rob Kendrick <rjek@netsurf-browser.org>
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
 * implementation of content handler for image/svg using librsvg.
 *
 * SVG files are rendered to a NetSurf bitmap by creating a Cairo rendering
 * surface (content_rsvg_data.cs) over the bitmap's data, creating a Cairo
 * drawing context using that surface, and then passing that drawing context
 * to librsvg which then uses Cairo calls to plot the graphic to the bitmap.
 * We store this in content->bitmap, and then use the usual bitmap plotter
 * function to render it for redraw requests.
 */

#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <sys/types.h>

#include <librsvg/rsvg.h>
#ifndef RSVG_CAIRO_H
#include <librsvg/rsvg-cairo.h>
#endif

#include <math.h>

#include <nsutils/endian.h>

#include "utils/log.h"
#include "utils/utils.h"
#include "utils/messages.h"
#include "netsurf/plotters.h"
#include "netsurf/bitmap.h"
#include "netsurf/content.h"
#include "content/llcache.h"
#include "content/content_protected.h"
#include "content/content_factory.h"
#include "desktop/gui_internal.h"
#include "desktop/bitmap.h"

#include "image/rsvg.h"

typedef struct rsvg_content {
	struct content base;

	RsvgHandle *rsvgh;	/**< Context handle for RSVG renderer */
	cairo_surface_t *cs;	/**< The surface built inside a nsbitmap */
	cairo_t *ct;		/**< Cairo drawing context */
	struct bitmap *bitmap;	/**< Created NetSurf bitmap */
} rsvg_content;

static nserror rsvg_create_svg_data(rsvg_content *c)
{
	c->rsvgh = NULL;
	c->cs = NULL;
	c->ct = NULL;
	c->bitmap = NULL;

#if LIBRSVG_CHECK_VERSION(2, 52, 0)
	/* Handle created from full data in rsvg_convert via
	 * rsvg_handle_new_from_data(). No incremental handle needed. */
#else
	if ((c->rsvgh = rsvg_handle_new()) == NULL) {
		NSLOG(netsurf, INFO, "rsvg_handle_new() returned NULL.");
		content_broadcast_error(&c->base, NSERROR_NOMEM, NULL);
		return NSERROR_NOMEM;
	}
#endif

	return NSERROR_OK;
}


static nserror rsvg_create(const content_handler *handler,
		lwc_string *imime_type, const struct http_parameter *params,
		llcache_handle *llcache, const char *fallback_charset,
		bool quirks, struct content **c)
{
	rsvg_content *svg;
	nserror error;

	svg = calloc(1, sizeof(rsvg_content));
	if (svg == NULL)
		return NSERROR_NOMEM;

	error = content__init(&svg->base, handler, imime_type, params,
			llcache, fallback_charset, quirks);
	if (error != NSERROR_OK) {
		free(svg);
		return error;
	}

	error = rsvg_create_svg_data(svg);
	if (error != NSERROR_OK) {
		free(svg);
		return error;
	}

	*c = (struct content *) svg;

	return NSERROR_OK;
}


static bool rsvg_process_data(struct content *c, const char *data,
			unsigned int size)
{
#if LIBRSVG_CHECK_VERSION(2, 52, 0)
	/* Data is buffered by the content framework; handle created from
	 * full data in rsvg_convert() via rsvg_handle_new_from_data(). */
	(void)c;
	(void)data;
	(void)size;
	return true;
#else
	rsvg_content *d = (rsvg_content *) c;
	GError *err = NULL;

	if (rsvg_handle_write(d->rsvgh, (const guchar *)data, (gsize)size,
				&err) == FALSE) {
		NSLOG(netsurf, INFO,
		      "rsvg_handle_write returned an error: %s", err->message);
		content_broadcast_error(c, NSERROR_SVG_ERROR, NULL);
		return false;
	}

	return true;
#endif
}

static bool rsvg_convert(struct content *c)
{
	rsvg_content *d = (rsvg_content *) c;
	GError *err = NULL;

#if LIBRSVG_CHECK_VERSION(2, 52, 0)
	const uint8_t *src;
	size_t src_size;
	gdouble dwidth, dheight;
	RsvgRectangle viewport;

	src = content__get_source_data(c, &src_size);
	if (src == NULL || src_size == 0) {
		NSLOG(netsurf, INFO, "No SVG data to convert.");
		content_broadcast_error(c, NSERROR_SVG_ERROR, NULL);
		return false;
	}

	d->rsvgh = rsvg_handle_new_from_data(src, (gsize)src_size, &err);
	if (d->rsvgh == NULL) {
		NSLOG(netsurf, INFO,
		      "rsvg_handle_new_from_data error: %s",
		      err ? err->message : "unknown");
		if (err)
			g_error_free(err);
		content_broadcast_error(c, NSERROR_SVG_ERROR, NULL);
		return false;
	}

	if (!rsvg_handle_get_intrinsic_size_in_pixels(d->rsvgh,
						      &dwidth, &dheight) ||
	    dwidth <= 0.0 || dheight <= 0.0) {
		/* Fallback: use a default size when intrinsic size is
		 * not available (e.g. SVGs that use only percentages). */
		dwidth = 300.0;
		dheight = 150.0;
	}

	c->width = (int)ceil(dwidth);
	c->height = (int)ceil(dheight);
#else
	RsvgDimensionData rsvgsize;

	if (rsvg_handle_close(d->rsvgh, &err) == FALSE) {
		NSLOG(netsurf, INFO,
		      "rsvg_handle_close returned an error: %s", err->message);
		content_broadcast_error(c, NSERROR_SVG_ERROR, NULL);
		return false;
	}

	assert(err == NULL);

	rsvg_handle_get_dimensions(d->rsvgh, &rsvgsize);
	c->width = rsvgsize.width;
	c->height = rsvgsize.height;
#endif

	if ((d->bitmap = guit->bitmap->create(c->width, c->height,
			BITMAP_NONE)) == NULL) {
		NSLOG(netsurf, INFO,
		      "Failed to create bitmap for rsvg render.");
		content_broadcast_error(c, NSERROR_NOMEM, NULL);
		return false;
	}

	if ((d->cs = cairo_image_surface_create_for_data(
			(unsigned char *)guit->bitmap->get_buffer(d->bitmap),
			CAIRO_FORMAT_ARGB32,
			c->width, c->height,
			guit->bitmap->get_rowstride(d->bitmap))) == NULL) {
		NSLOG(netsurf, INFO,
		      "Failed to create Cairo image surface for rsvg render.");
		content_broadcast_error(c, NSERROR_NOMEM, NULL);
		return false;
	}

	if ((d->ct = cairo_create(d->cs)) == NULL) {
		NSLOG(netsurf, INFO,
		      "Failed to create Cairo drawing context for rsvg render.");
		content_broadcast_error(c, NSERROR_NOMEM, NULL);
		return false;
	}

#if LIBRSVG_CHECK_VERSION(2, 52, 0)
	viewport.x = 0;
	viewport.y = 0;
	viewport.width = (double)c->width;
	viewport.height = (double)c->height;

	if (!rsvg_handle_render_document(d->rsvgh, d->ct, &viewport, &err)) {
		NSLOG(netsurf, INFO,
		      "rsvg_handle_render_document error: %s",
		      err ? err->message : "unknown");
		if (err)
			g_error_free(err);
		content_broadcast_error(c, NSERROR_SVG_ERROR, NULL);
		return false;
	}
#else
	rsvg_handle_render_cairo(d->rsvgh, d->ct);
#endif

	bitmap_format_to_client(d->bitmap, &(bitmap_fmt_t) {
		.layout = BITMAP_LAYOUT_ARGB8888,
	});
	guit->bitmap->modified(d->bitmap);
	content_set_ready(c);
	content_set_done(c);
	/* Done: update status bar */
	content_set_status(c, "");

	return true;
}

static bool rsvg_redraw(struct content *c, struct content_redraw_data *data,
		const struct rect *clip, const struct redraw_context *ctx)
{
	rsvg_content *rsvgcontent = (rsvg_content *) c;
	bitmap_flags_t flags = BITMAPF_NONE;

	assert(rsvgcontent->bitmap != NULL);

	if (data->repeat_x)
		flags |= BITMAPF_REPEAT_X;
	if (data->repeat_y)
		flags |= BITMAPF_REPEAT_Y;

	return (ctx->plot->bitmap(ctx,
				  rsvgcontent->bitmap,
				  data->x, data->y,
				  data->width, data->height,
				  data->background_colour,
				  flags) == NSERROR_OK);
}

static void rsvg_destroy(struct content *c)
{
	rsvg_content *d = (rsvg_content *) c;

	if (d->bitmap != NULL) guit->bitmap->destroy(d->bitmap);
	if (d->rsvgh != NULL) g_object_unref(d->rsvgh);
	if (d->ct != NULL) cairo_destroy(d->ct);
	if (d->cs != NULL) cairo_surface_destroy(d->cs);

	return;
}

static nserror rsvg_clone(const struct content *old, struct content **newc)
{
	rsvg_content *svg;
	nserror error;
	const uint8_t *data;
	size_t size;

	svg = calloc(1, sizeof(rsvg_content));
	if (svg == NULL)
		return NSERROR_NOMEM;

	error = content__clone(old, &svg->base);
	if (error != NSERROR_OK) {
		content_destroy(&svg->base);
		return error;
	}

	/* Simply replay create/process/convert */
	error = rsvg_create_svg_data(svg);
	if (error != NSERROR_OK) {
		content_destroy(&svg->base);
		return error;
	}

	data = content__get_source_data(&svg->base, &size);
	if (size > 0) {
		if (rsvg_process_data(&svg->base, (const char *)data, size) == false) {
			content_destroy(&svg->base);
			return NSERROR_NOMEM;
		}
	}

	if (old->status == CONTENT_STATUS_READY ||
			old->status == CONTENT_STATUS_DONE) {
		if (rsvg_convert(&svg->base) == false) {
			content_destroy(&svg->base);
			return NSERROR_CLONE_FAILED;
		}
	}

	*newc = (struct content *) svg;

	return NSERROR_OK;
}

static void *rsvg_get_internal(const struct content *c, void *context)
{
	rsvg_content *d = (rsvg_content *) c;

	return d->bitmap;
}

static content_type rsvg_content_type(void)
{
	return CONTENT_IMAGE;
}


static bool rsvg_content_is_opaque(struct content *c)
{
	rsvg_content *d = (rsvg_content *) c;

	if (d->bitmap != NULL) {
		return guit->bitmap->get_opaque(d->bitmap);
	}

	return false;
}


static const content_handler rsvg_content_handler = {
	.create = rsvg_create,
	.process_data = rsvg_process_data,
	.data_complete = rsvg_convert,
	.destroy = rsvg_destroy,
	.redraw = rsvg_redraw,
	.clone = rsvg_clone,
	.get_internal = rsvg_get_internal,
	.type = rsvg_content_type,
	.is_opaque = rsvg_content_is_opaque,
	.no_share = false,
};

static const char *rsvg_types[] = {
	"image/svg",
	"image/svg+xml"
};

CONTENT_FACTORY_REGISTER_TYPES(nsrsvg, rsvg_types, rsvg_content_handler);

