/*
 * $Id$
 *
 * Copyright (c) 2002, Raphael Manfredi
 *
 * Version management.
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

#ifndef _version_h_
#define _version_h_

#include <glib.h>

/*
 * A decompiled version descriptor.
 * In our comments below, we are assuming a value of "0.90.3b2".
 */
typedef struct version {
	guint major;				/* Major version number (0) */
	guint minor;				/* Minor version number (90) */
	guint patchlevel;			/* Patch level (3) */
	guchar tag;					/* Code letter after version number (b) */
	guint taglevel;				/* Value after code letter (2) */
	time_t timestamp;
} version_t;

/*
 * Public interface.
 */

void version_init(void);
void version_close(void);
void version_ancient_warn(void);
void version_check(guchar *str);
gboolean version_is_too_old(gchar *vendor);

gchar *version_str(version_t *ver);

extern gchar *version_string;
extern gchar *version_number;

#endif	/* _version_h_ */

/* vi: set ts=4: */

