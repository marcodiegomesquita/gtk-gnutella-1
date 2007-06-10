/*
 * $Id$
 *
 * Copyright (c) 2007, Christian Biere
 *
 *----------------------------------------------------------------------
 * This file is part of gtk-gnutella.
 *
 *  gtk-gnutella is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  gtk-gnutella is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with gtk-gnutella; if not, write to the Free Software
 *  Foundation, Inc.:
 *      59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *----------------------------------------------------------------------
 */

/**
 * @ingroup gtk
 * @file
 *
 * Drag support - no dropping, just dragging.
 *
 * @author Christian Biere
 * @date 2007
 */

#include "gui.h"

RCSID("$Id$")

#include "drag.h"

#include "lib/override.h"		/* Must be the last header included */

/*
 * Public functions
 */

struct drag_context {
	drag_get_text_cb get_text;
	char *text;
};

#if GTK_CHECK_VERSION(2,0,0)
gboolean 
drag_get_iter(GtkTreeView *tv, GtkTreeModel **model, GtkTreeIter *iter)
{
	gboolean ret = FALSE;
	GtkTreePath *path;

	g_return_val_if_fail(model, FALSE);
	g_return_val_if_fail(iter, FALSE);
	
	gtk_tree_view_get_cursor(tv, &path, NULL);
	if (path) {
		*model = gtk_tree_view_get_model(tv);
		ret = gtk_tree_model_get_iter(*model, iter, path);
		gtk_tree_path_free(path);
	}
	return ret; 
}
#define signal_connect(widget, name, func, data) \
	g_signal_connect((widget), (name), G_CALLBACK(func), (data))

#define signal_stop_emission_by_name g_signal_stop_emission_by_name

static inline void
selection_set_text(GtkSelectionData *data, const char *text)
{
	size_t len = strlen(text);

	len = len < INT_MAX ? len : 0;
	gtk_selection_data_set_text(data, text, len);
}

#else	/* Gtk < 2 */

#define signal_connect(widget, name, func, data) \
	gtk_signal_connect(GTK_OBJECT(widget), (name), (func), (data))

#define signal_stop_emission_by_name(widget, name) \
G_STMT_START { \
	(void) (widget); \
	(void) (name); \
} G_STMT_END

static inline void
selection_set_text(GtkSelectionData *data, const char *text)
{
	size_t len = strlen(text);
	
	len = len < INT_MAX ? len : 0;
   	gtk_selection_data_set(data, GDK_SELECTION_TYPE_STRING, 8 /* CHAR_BIT */,
		cast_to_gconstpointer(text), len);
}

#endif /* Gtk+ >= 2 */

static void
drag_begin(GtkWidget *widget, GdkDragContext *unused_drag_ctx, void *udata)
{
	struct drag_context *ctx = udata;

	(void) unused_drag_ctx;

	signal_stop_emission_by_name(widget, "drag-begin");

	g_return_if_fail(ctx);
	g_return_if_fail(ctx->get_text);

	G_FREE_NULL(ctx->text);
	ctx->text = ctx->get_text(widget);
}


static void
drag_data_get(GtkWidget *widget, GdkDragContext *unused_drag_ctx,
	GtkSelectionData *data, unsigned unused_info, unsigned unused_stamp,
	void *udata)
{
	struct drag_context *ctx = udata;

	(void) unused_drag_ctx;
	(void) unused_info;
	(void) unused_stamp;

	signal_stop_emission_by_name(widget, "drag-data-get");

	g_return_if_fail(ctx);
	g_return_if_fail(ctx->get_text);

	if (ctx->text) {
		selection_set_text(data, ctx->text);
		G_FREE_NULL(ctx->text);
	}
}

static void
drag_end(GtkWidget *widget, GdkDragContext *unused_drag_ctx, void *udata)
{
	struct drag_context *ctx = udata;

	(void) unused_drag_ctx;

	signal_stop_emission_by_name(widget, "drag-end");

	g_return_if_fail(ctx);
	g_return_if_fail(ctx->get_text);

	G_FREE_NULL(ctx->text);
}

/**
 * Allocates a new drag context, to be freed with drag_free().
 * @return a drag context.
 */
struct drag_context *
drag_new(void)
{
	static const struct drag_context zero_ctx;
	struct drag_context *ctx;

	ctx = g_malloc(sizeof *ctx);
	*ctx = zero_ctx;
	return ctx;
}


/**
 * Attaches a drag context to a widget, so that user can drag data from
 * the widget as text. The context can be attached to multiple widgets.
 */
void 
drag_attach(struct drag_context *ctx,
	GtkWidget *widget, drag_get_text_cb callback)
{
    static const GtkTargetEntry targets[] = {
        { "STRING",			0, 1 },
        { "text/plain",		0, 2 },
#if GTK_CHECK_VERSION(2,0,0)
        { "UTF8_STRING",	0, 3 },
        { "text/plain;charset=utf-8",	0, 4 },
#endif	/* Gtk+ >= 2.0 */
    };

	g_return_if_fail(ctx);
	g_return_if_fail(widget);
	g_return_if_fail(callback);

	ctx->get_text = callback;

	/* Initialize drag support */
	gtk_drag_source_set(widget,
		GDK_BUTTON1_MASK | GDK_BUTTON2_MASK, targets, G_N_ELEMENTS(targets),
		GDK_ACTION_DEFAULT | GDK_ACTION_COPY | GDK_ACTION_ASK);

    signal_connect(widget, "drag-data-get", drag_data_get, ctx);
    signal_connect(widget, "drag-begin",	drag_begin, ctx);
    signal_connect(widget, "drag-end",	  	drag_end, ctx);
}

/**
 * Frees a drag context.
 */
void
drag_free(struct drag_context **ptr)
{
	struct drag_context *ctx = *ptr;

	if (ctx) {
		G_FREE_NULL(ctx);
		*ptr = NULL;
	}
}

/* vi: set ts=4 sw=4 cindent: */
