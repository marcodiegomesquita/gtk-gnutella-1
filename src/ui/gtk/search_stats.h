/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Richard Eckart
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

#ifndef _gtk_search_stats_h_
#define _gtk_search_stats_h_

#include "common.h"

/**
 * Stats types for search_stats_gui_set_type().
 */
enum {
    NO_SEARCH_STATS,
    WORD_SEARCH_STATS,
    WHOLE_SEARCH_STATS,
    ROUTED_SEARCH_STATS
};

void search_stats_gui_init(void);
void search_stats_gui_shutdown(void);
void search_stats_gui_reset(void);
void search_stats_gui_set_type(gint type);

#endif /* _gtk_search_stats_h_ */
