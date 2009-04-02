/*
 * $Id$
 *
 * Copyright (c) 2009, Raphael Manfredi
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
 * @ingroup lib
 * @file
 *
 * Interface definition for a sequence (traversable ordered structure).
 *
 * @author Raphael Manfredi
 * @date 2009
 */

#include "common.h"

RCSID("$Id$")

#include "sequence.h"
#include "walloc.h"
#include "override.h"			/* Must be the last header included */

/**
 * Iterator directions.
 */
enum sequence_direction {
	SEQ_ITER_FORWARD = 0,		/**< Forward iteration */
	SEQ_ITER_BACKWARD			/**< Backward iteration */
};

/**
 * A sequence iterator.
 */
struct sequence_iterator {
	union {
		GSList *gsl;
		GList *gl;
		list_iter_t *li;
		slist_iter_t *sli;
		hash_list_iter_t *hli;
	} u;
	enum sequence_type type;
	enum sequence_direction direction;
};

/**
 * Create a sequence out of an existing GSList.
 * Use sequence_release() to discard the sequence encapsulation.
 */
sequence_t *
sequence_create_from_gslist(GSList *gsl)
{
	sequence_t *s;

	s = walloc(sizeof *s);
	return sequence_fill_from_gslist(s, gsl);
}

/**
 * Create a sequence out of an existing GList.
 * Use sequence_release() to discard the sequence encapsulation.
 */
sequence_t *
sequence_create_from_glist(GList *gl)
{
	sequence_t *s;

	s = walloc(sizeof *s);
	return sequence_fill_from_glist(s, gl);
}

/**
 * Create a sequence out of an existing list_t.
 * Use sequence_release() to discard the sequence encapsulation.
 */
sequence_t *
sequence_create_from_list(list_t *l)
{
	sequence_t *s;

	g_assert(l != NULL);

	s = walloc(sizeof *s);
	return sequence_fill_from_list(s, l);
}

/**
 * Create a sequence out of an existing slist_t.
 * Use sequence_release() to discard the sequence encapsulation.
 */
sequence_t *
sequence_create_from_slist(slist_t *sl)
{
	sequence_t *s;

	g_assert(sl != NULL);

	s = walloc(sizeof *s);
	return sequence_fill_from_slist(s, sl);
}

/**
 * Create a sequence out of an existing hash_list_t.
 * Use sequence_release() to discard the sequence encapsulation.
 */
sequence_t *
sequence_create_from_hash_list(hash_list_t *hl)
{
	sequence_t *s;

	g_assert(hl != NULL);

	s = walloc(sizeof *s);
	return sequence_fill_from_hash_list(s, hl);
}

/**
 * Fill sequence object with GSList.
 */
sequence_t *
sequence_fill_from_gslist(sequence_t *s, GSList *gsl)
{
	s->type = SEQUENCE_GSLIST;
	s->u.gsl = gsl;
	return s;
}

/**
 * Fill sequence object with GList.
 */
sequence_t *
sequence_fill_from_glist(sequence_t *s, GList *gl)
{
	s->type = SEQUENCE_GLIST;
	s->u.gl = gl;
	return s;
}

/**
 * Fill sequence object with list_.
 */
sequence_t *
sequence_fill_from_list(sequence_t *s, list_t *l)
{
	g_assert(l != NULL);

	s->type = SEQUENCE_LIST;
	s->u.l = l;
	return s;
}

/**
 * Fill sequence object with slist_t.
 */
sequence_t *
sequence_fill_from_slist(sequence_t *s, slist_t *sl)
{
	g_assert(sl != NULL);

	s->type = SEQUENCE_SLIST;
	s->u.sl = sl;
	return s;
}

/**
 * Fill sequence object with hash_list_t.
 */
sequence_t *
sequence_fill_from_hash_list(sequence_t *s, hash_list_t *hl)
{
	g_assert(hl != NULL);

	s->type = SEQUENCE_HLIST;
	s->u.hl = hl;
	return s;
}

/**
 * Is the sequence empty?
 */
gboolean
sequence_is_empty(const sequence_t *s)
{
	g_assert(s != NULL);

	switch (s->type) {
	case SEQUENCE_GSLIST:
		return NULL == s->u.gsl;
	case SEQUENCE_GLIST:
		return NULL == s->u.gl;
	case SEQUENCE_LIST:
		return 0 == list_length(s->u.l);
	case SEQUENCE_SLIST:
		return 0 == slist_length(s->u.sl);
	case SEQUENCE_HLIST:
		return 0 == hash_list_length(s->u.hl);
	case SEQUENCE_MAXTYPE:
		g_assert_not_reached();
	}

	return FALSE;
}

/**
 * Returns the underlying sequence implementation.
 */
gpointer
sequence_implementation(const sequence_t *s)
{
	g_assert(s != NULL);

	switch (s->type) {
	case SEQUENCE_GSLIST:
		return s->u.gsl;
	case SEQUENCE_GLIST:
		return s->u.gl;
	case SEQUENCE_LIST:
		return s->u.l;
	case SEQUENCE_SLIST:
		return s->u.sl;
	case SEQUENCE_HLIST:
		return s->u.hl;
	case SEQUENCE_MAXTYPE:
		g_assert_not_reached();
	}

	return NULL;
}

/**
 * Release sequence encapsulation, returning the underlying implementation
 * object (will need to be cast back to the proper type for perusal).
 */
gpointer
sequence_release(sequence_t *s)
{
	gpointer implementation;

	g_assert(s != NULL);

	implementation = sequence_implementation(s);

	s->type = SEQUENCE_MAXTYPE;
	wfree(s, sizeof *s);

	return implementation;
}

/**
 * Destroy a sequence and the underlying implementation.
 */
void
sequence_destroy(sequence_t *s)
{
	g_assert(s != NULL);

	switch (s->type) {
	case SEQUENCE_GSLIST:
		g_slist_free(s->u.gsl);
		break;
	case SEQUENCE_GLIST:
		g_list_free(s->u.gl);
		break;
	case SEQUENCE_LIST:
		list_free(&s->u.l);
		break;
	case SEQUENCE_SLIST:
		slist_free(&s->u.sl);
		break;
	case SEQUENCE_HLIST:
		hash_list_free(&s->u.hl);
		break;
	case SEQUENCE_MAXTYPE:
		g_assert_not_reached();
	}

	s->type = SEQUENCE_MAXTYPE;
	wfree(s, sizeof *s);
}

/**
 * Create a forward iterator.
 */
sequence_iter_t *
sequence_forward_iterator(const sequence_t *s)
{
	sequence_iter_t *si;

	g_assert(s != NULL);

	si = walloc(sizeof *si);
	si->direction = SEQ_ITER_FORWARD;
	si->type = s->type;

	switch (s->type) {
	case SEQUENCE_GSLIST:
		si->u.gsl = s->u.gsl;
		break;
	case SEQUENCE_GLIST:
		si->u.gl = s->u.gl;
		break;
	case SEQUENCE_LIST:
		si->u.li = list_iter_before_head(s->u.l);
		break;
	case SEQUENCE_SLIST:
		si->u.sli = slist_iter_before_head(s->u.sl);
		break;
	case SEQUENCE_HLIST:
		si->u.hli = hash_list_iterator(s->u.hl);
		break;
	case SEQUENCE_MAXTYPE:
		g_assert_not_reached();
	}

	return si;
}

/**
 * Check whether we have a next item to be iterated over.
 */
gboolean
sequence_iter_has_next(const sequence_iter_t *si)
{
	if (!si)
		return FALSE;

	g_assert(SEQ_ITER_FORWARD == si->direction);

	switch (si->type) {
	case SEQUENCE_GSLIST:
		return si->u.gsl != NULL;
	case SEQUENCE_GLIST:
		return si->u.gl != NULL;
	case SEQUENCE_LIST:
		return list_iter_has_next(si->u.li);
	case SEQUENCE_SLIST:
		return slist_iter_has_next(si->u.sli);
	case SEQUENCE_HLIST:
		return hash_list_iter_has_next(si->u.hli);
	case SEQUENCE_MAXTYPE:
		g_assert_not_reached();
	}

	return FALSE;
}

/**
 * Get next item, moving iterator cursor by one position.
 */
gpointer
sequence_iter_next(sequence_iter_t *si)
{
	gpointer next = NULL;

	g_assert(si != NULL);
	g_assert(SEQ_ITER_FORWARD == si->direction);

	switch (si->type) {
	case SEQUENCE_GSLIST:
		next = si->u.gsl ? si->u.gsl->data : NULL;
		si->u.gsl = g_slist_next(si->u.gsl);
		break;
	case SEQUENCE_GLIST:
		next = si->u.gl ? si->u.gl->data : NULL;
		si->u.gl = g_list_next(si->u.gl);
		break;
	case SEQUENCE_LIST:
		return list_iter_next(si->u.li);
	case SEQUENCE_SLIST:
		return slist_iter_next(si->u.sli);
	case SEQUENCE_HLIST:
		return hash_list_iter_next(si->u.hli);
	case SEQUENCE_MAXTYPE:
		g_assert_not_reached();
	}

	return next;
}

/**
 * Release the iterator.
 */
void
sequence_iterator_release(sequence_iter_t **iter_ptr)
{
	if (*iter_ptr) {
		sequence_iter_t *si = *iter_ptr;

		switch (si->type) {
		case SEQUENCE_GSLIST:
		case SEQUENCE_GLIST:
			break;
		case SEQUENCE_LIST:
			list_iter_free(&si->u.li);
			break;
		case SEQUENCE_SLIST:
			slist_iter_free(&si->u.sli);
			break;
		case SEQUENCE_HLIST:
			hash_list_iter_release(&si->u.hli);
			break;
		case SEQUENCE_MAXTYPE:
			g_assert_not_reached();
		}

		wfree(si, sizeof *si);
		*iter_ptr = NULL;
	}
}

/* vi: set ts=4 sw=4 cindent: */
