/*
 * $Id$
 *
 * Copyright (c) 2002-2003, Raphael Manfredi
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
 * @ingroup core
 * @file
 *
 * Download mesh.
 *
 * @author Raphael Manfredi
 * @date 2002-2003
 */

#include "common.h"

RCSID("$Id$");

#include "gnutella.h"
#include "downloads.h"
#include "uploads.h"		/* For upload_is_enabled() */
#include "dmesh.h"
#include "huge.h"
#include "http.h"
#include "hostiles.h"
#include "guid.h"
#include "share.h"
#include "fileinfo.h"
#include "settings.h"
#include "hosts.h"

#include "if/gnet_property_priv.h"

#include "lib/atoms.h"
#include "lib/base32.h"
#include "lib/cq.h"
#include "lib/endian.h"
#include "lib/file.h"
#include "lib/fuzzy.h"
#include "lib/getdate.h"
#include "lib/glib-missing.h"
#include "lib/header.h"
#include "lib/tm.h"
#include "lib/url.h"
#include "lib/urn.h"
#include "lib/walloc.h"

#include "lib/override.h"	/* Must be the last header included */

dmesh_url_error_t dmesh_url_errno;	/**< Error from dmesh_url_parse() */

/**
 * The download mesh records all the known sources for a given SHA1.
 * It is implemented as a big hash table, where SHA1 are keys, each value
 * being a struct dmesh pointer.
 */
static GHashTable *mesh = NULL;

struct dmesh {				/**< A download mesh bucket */
	GSList *entries;		/**< The download mesh entries, dmesh_entry data */
	time_t last_update;		/**< Timestamp of last insertion in the mesh */
	gint count;				/**< Amount of entries in list */
};

struct dmesh_entry {
	time_t inserted;		/**< When entry was inserted in mesh */
	time_t stamp;			/**< When entry was last seen */
	dmesh_urlinfo_t url;	/**< URL info */
};

#define MAX_LIFETIME	86400		/**< 1 day */
#define MAX_ENTRIES		64			/**< Max amount of entries kept in list */
#define MAX_STAMP		((time_t) -1)

#define MIN_PFSP_SIZE	524288		/**< 512K, min size for PFSP advertising */
#define MIN_PFSP_PCT	10			/**< 10%, min available data for PFSP */

/** If not at least 60% alike, dump! */
#define FUZZY_DROP		((60 << FUZZY_SHIFT) / 100)
/** If more than 80% alike, equal! */
#define FUZZY_MATCH		((80 << FUZZY_SHIFT) / 100)

static const gchar dmesh_file[] = "dmesh";

/**
 * If we get a "bad" URL into the mesh ("bad" = gives 404 or other error when
 * trying to download it), we must remember it for some time and prevent it
 * from re-entering the mesh again within that period to prevent rescheduling
 * for download and a further failure: that would be hammering the poor host,
 * and we're wasting our time and bandwidth.
 *
 * Therefore, each time we get a "bad" URL, we insert it in a hash table.
 * The table entry is then scheduled to be removed after some grace period
 * occurs.  The table is keyed by the dmesh_urlinfo_t, and points to a
 * dmesh_banned structure.
 *
 * The table is persisted at regular intervals.
 */
static GHashTable *ban_mesh = NULL;

struct dmesh_banned {
	dmesh_urlinfo_t *info;	/**< The banned URL (same as key) */
	gpointer cq_ev;			/**< Scheduled callout event */
	const gchar *sha1;		/**< The SHA1, if any */
	time_t ctime;			/**< Last time we saw this banned URL */
};

/**
 * This table stores the banned entries by SHA1.
 */
static GHashTable *ban_mesh_by_sha1 = NULL;

#define BAN_LIFETIME	7200		/**< 2 hours */

static const gchar dmesh_ban_file[] = "dmesh_ban";

static void dmesh_retrieve(void);
static void dmesh_ban_retrieve(void);
static gchar *dmesh_urlinfo_to_string(const dmesh_urlinfo_t *info);

/**
 * Hash a URL info.
 */
static guint
urlinfo_hash(gconstpointer key)
{
	const dmesh_urlinfo_t *info = key;
	guint hash;

	hash = host_addr_hash(info->addr);
	hash ^= ((guint32) info->port << 16) | info->port;
	hash ^= info->idx;
	hash ^= g_str_hash(info->name);

	return hash;
}

/**
 * Test equality of two URL infos.
 */
static gint
urlinfo_eq(gconstpointer a, gconstpointer b)
{
	const dmesh_urlinfo_t *ia = a, *ib = b;

	return ia->port == ib->port &&
		ia->idx == ib->idx &&
		host_addr_equal(ia->addr, ib->addr) &&
		(ia->name == ib->name || 0 == strcmp(ia->name, ib->name));
}

/**
 * Initialize the download mesh.
 */
void
dmesh_init(void)
{
	mesh = g_hash_table_new(sha1_hash, sha1_eq);
	ban_mesh = g_hash_table_new(urlinfo_hash, urlinfo_eq);
	ban_mesh_by_sha1 = g_hash_table_new(sha1_hash, sha1_eq);
	dmesh_retrieve();
	dmesh_ban_retrieve();
}

/**
 * Compare two dmesh_entry, based on the timestamp.  The greater the time
 * stamp, the samller the entry (i.e. the more recent).
 */
static gint
dmesh_entry_cmp(gconstpointer a, gconstpointer b)
{
	const struct dmesh_entry *ae = a, *be = b;

	return delta_time(be->stamp, ae->stamp);
}

/**
 * Free download mesh entry.
 */
static void
dmesh_entry_free(struct dmesh_entry *dme)
{
	g_assert(dme);

	if (dme->url.name)
		atom_str_free(dme->url.name);

	wfree(dme, sizeof *dme);
}

/**
 * Free a dmesh_urlinfo_t structure.
 */
static void
dmesh_urlinfo_free(dmesh_urlinfo_t *info)
{
	g_assert(info);

	atom_str_free(info->name);
	wfree(info, sizeof *info);
}

/**
 * Called from callout queue when it's time to expire the URL ban.
 */
static void
dmesh_ban_expire(cqueue_t *unused_cq, gpointer obj)
{
	struct dmesh_banned *dmb = obj;

	(void) unused_cq;
	g_assert(dmb);
	g_assert(dmb == g_hash_table_lookup(ban_mesh, dmb->info));

	/*
	 * Also remove the banned entry from the IP list by SHA1 which is ussed
	 * by X-NAlt
	 *		-- JA 24/10/2003
	 */
	if (dmb->sha1 != NULL) {
		GSList *by_addr;
		GSList *head;
		gpointer key;			/* The SHA1 atom used for key in table */
		gpointer x;
		gboolean found;

		found = g_hash_table_lookup_extended(
			ban_mesh_by_sha1, dmb->sha1, &key, &x);
		g_assert(found);
		head = by_addr = x;
		by_addr = g_slist_remove(by_addr, dmb);

		if (by_addr == NULL) {
			g_hash_table_remove(ban_mesh_by_sha1, key);
			atom_sha1_free(key);
		} else if (by_addr != head)
			g_hash_table_insert(ban_mesh_by_sha1, key, by_addr);

		atom_sha1_free(dmb->sha1);
	}

	g_hash_table_remove(ban_mesh, dmb->info);
	dmesh_urlinfo_free(dmb->info);
	wfree(dmb, sizeof *dmb);
}

/**
 * Add new URL to the banned hash.
 * If stamp is 0, the current timestamp is used.
 */
static void
dmesh_ban_add(const gchar *sha1, dmesh_urlinfo_t *info, time_t stamp)
{
	time_t now = tm_time();
	struct dmesh_banned *dmb;
	time_delta_t lifetime = BAN_LIFETIME;

	if (stamp == 0)
		stamp = now;

	/*
	 * If expired, don't insert.
	 */

	lifetime -= delta_time(now, stamp);
	lifetime = MIN(lifetime, INT_MAX / 1000);

	if (lifetime <= 0)
		return;

	/*
	 * Insert new entry, or update old entry if the new one is more recent.
	 */

	dmb = g_hash_table_lookup(ban_mesh, info);

	if (dmb == NULL) {
		dmesh_urlinfo_t *ui;

		ui = walloc(sizeof *ui);
		ui->addr = info->addr;
		ui->port = info->port;
		ui->idx = info->idx;
		ui->name = atom_str_get(info->name);

		dmb = walloc(sizeof *dmb);
		dmb->info = ui;
		dmb->ctime = stamp;
		dmb->cq_ev = cq_insert(callout_queue,
			lifetime * 1000, dmesh_ban_expire, dmb);
		dmb->sha1 = NULL;

		g_hash_table_insert(ban_mesh, dmb->info, dmb);

		/*
		 * Keep record of banned hosts by SHA1 Hash. We will use this to send
		 * out X-Nalt locations.
		 *		-- JA, 1/11/2003.
		 */

		if (sha1 != NULL) {
			GSList *by_addr;
			gboolean existed;

			dmb->sha1 = atom_sha1_get(sha1);

			/*
             * Don't fear for duplicates here. The dmb lookup above
             * makes sure that if a XNalt with the IP already exists,
             * the appropriate dmb will be updates (else-case below).
             *     -- BLUE 16/01/2004
             */
			by_addr = g_hash_table_lookup(ban_mesh_by_sha1, sha1);
			existed = by_addr != NULL;
			by_addr = g_slist_append(by_addr, dmb);

			if (!existed)
				g_hash_table_insert(ban_mesh_by_sha1,
					atom_sha1_get(sha1), by_addr);
		}
	}
	else if (delta_time(dmb->ctime, stamp) < 0) {
		dmb->ctime = stamp;
		cq_resched(callout_queue, dmb->cq_ev, lifetime * 1000);
	}
}

/**
 * Check whether URL is banned from the mesh.
 */
static gboolean
dmesh_is_banned(const dmesh_urlinfo_t *info)
{
	return NULL != g_hash_table_lookup(ban_mesh, info);
}

/***
 *** Mesh URL parsing.
 ***/

static const gchar * const parse_errstr[] = {
	"OK",									/**< DMESH_URL_OK */
	"HTTP parsing error",					/**< DMESH_URL_HTTP_PARSER */
	"File prefix neither /uri-res nor /get",/**< DMESH_URL_BAD_FILE_PREFIX */
	"Index in /get/index is reserved",		/**< DMESH_URL_RESERVED_INDEX */
	"No filename after /get/index",			/**< DMESH_URL_NO_FILENAME */
	"Bad URL encoding",						/**< DMESH_URL_BAD_ENCODING */
	"Malformed /uri-res/N2R?",				/**< DMESH_URL_BAD_URI_RES */
};

/**
 * @return human-readable error string corresponding to error code `errnum'.
 */
const gchar *
dmesh_url_strerror(dmesh_url_error_t errnum)
{
	if ((gint) errnum < 0 || errnum >= G_N_ELEMENTS(parse_errstr))
		return "Invalid error code";

	if (errnum == DMESH_URL_HTTP_PARSER) {
		static gchar http_error_str[128];

		concat_strings(http_error_str, sizeof http_error_str,
			parse_errstr[errnum], ": ", http_url_strerror(http_url_errno),
			(void *) 0);
		return http_error_str;
	}

	return parse_errstr[errnum];
}

/**
 * Parse URL `url', and fill a structure `info' representing this URL.
 *
 * @return TRUE if OK, FALSE if we could not parse it.
 * The variable `dmesh_url_errno' is set accordingly.
 */
gboolean
dmesh_url_parse(const gchar *url, dmesh_urlinfo_t *info)
{
	host_addr_t addr;
	guint16 port;
	guint idx;
	const gchar *endptr, *file, *host = NULL, *path = NULL;

	if (!http_url_parse(url, &port, &host, &path)) {
		dmesh_url_errno = DMESH_URL_HTTP_PARSER;
		return FALSE;
	}

	/* FIXME:	This can block; we should never keep resolved hostnames as IP
	 *			addresses around but always resolve hostnames just in time.
	 */
	addr = name_to_single_host_addr(host, settings_dns_net());
	if (!is_host_addr(addr))
		return FALSE;

	/*
	 * Test the first form of resource naming:
	 *
	 *    /get/1/name.txt
	 */

	if (NULL != (endptr = is_strprefix(path, "/get/"))) {
		gint error;

		idx = parse_uint32(endptr, &endptr, 10, &error);
		if (!error && URN_INDEX == idx) {
			dmesh_url_errno = DMESH_URL_RESERVED_INDEX;
			return FALSE;				/* Index 0xffffffff is our mark */
		}

		if (error || *endptr != '/') {
			dmesh_url_errno = DMESH_URL_NO_FILENAME;
			return FALSE;				/* Did not have "/get/234/" */
		}

		/* Ok, `file' points after the "/", at beginning of filename */
		file = ++endptr;
	} else if (NULL != (endptr = is_strprefix(path, "/uri-res/N2R?"))) {

		/*
		 * Test the second form of resource naming:
		 *
		 *    /uri-res/N2R?urn:sha1:ABCDEFGHIJKLMN....
		 */

		idx = URN_INDEX;		/* Identifies second form */
		file = endptr;
	} else {
		dmesh_url_errno = DMESH_URL_BAD_FILE_PREFIX;
		return FALSE;
	}

	g_assert(file != NULL);

	info->addr = addr;
	info->port = port;
	info->idx = idx;

	/*
	 * If we have an URL with a filename, it is URL-escaped.
	 *
	 * We're unescaping it now, meaning there cannot be embedded '/' in the
	 * filename.  This is a reasonable assumption.
	 * NB: when most servents understand URL-escaped queries, we won't need
	 * to do this anymore and will keep the file URL-escaped.
	 */

	if (idx != URN_INDEX) {
		gchar *unescaped = url_unescape(deconstify_gchar(file), FALSE);
		if (!unescaped) {
			dmesh_url_errno = DMESH_URL_BAD_ENCODING;
			return FALSE;
		}
		info->name = atom_str_get(unescaped);
		if (unescaped != file)
			G_FREE_NULL(unescaped);
	} else {
		gchar digest[SHA1_RAW_SIZE];
		
		if (!urn_get_sha1(file, digest)) {
			dmesh_url_errno = DMESH_URL_BAD_URI_RES;
			return FALSE;
		}
		info->name = atom_str_get(file);
	}


	dmesh_url_errno = DMESH_URL_OK;

	return TRUE;
}

/**
 * Expire entries older than `agemax' in a given mesh bucket `dm'.
 * `sha1' is only passed in case we want to log the removal.
 */
static void
dm_expire(struct dmesh *dm, glong agemax, const gchar *sha1)
{
	GSList *l;
	GSList *prev;
	time_t now = tm_time();

	for (prev = NULL, l = dm->entries; l; /* empty */) {
		struct dmesh_entry *dme = l->data;
		GSList *next;

		if (delta_time(now, dme->stamp) <= agemax) {
			prev = l;
			l = l->next;
			continue;
		}

		/*
		 * Remove the entry.
		 *
		 * XXX instead of removing, maybe we can schedule a HEAD refresh
		 * XXX to see whether the entry is still valid.
		 */

		g_assert(dm->count > 0);

		if (dmesh_debug > 4)
			g_message("MESH %s: EXPIRED \"%s\", age=%d",
				sha1 ? sha1_base32(sha1) : "<no SHA-1 known>",
				dmesh_urlinfo_to_string(&dme->url),
				(gint) delta_time(now, dme->stamp));

		dmesh_entry_free(dme);
		dm->count--;

		next = l->next;
		l->next = NULL;
		g_slist_free_1(l);

		if (prev == NULL)				/* At the head of the list */
			dm->entries = next;
		else
			prev->next = next;

		l = next;
	}
}

/**
 * Remove specified entry from mesh bucket, if it is older than `stamp'.
 *
 * @return TRUE if entry was removed or not found, FALSE otherwise.
 */
static gboolean
dm_remove(struct dmesh *dm, const host_addr_t addr,
	guint16 port, guint idx, const gchar *name, time_t stamp)
{
	GSList *sl;

	g_assert(dm);
	g_assert(dm->count > 0);

	for (sl = dm->entries; sl; sl = g_slist_next(sl)) {
		struct dmesh_entry *dme = sl->data;

		if (
			host_addr_equal(dme->url.addr, addr) &&
			dme->url.port == port	&&
			dme->url.idx == idx		&&
			0 == strcmp(dme->url.name, name)
		) {
			/*
			 * Found entry, remove it if older than `stamp'.
			 *
			 * If it's equal, we don't remove it either, to prevent addition
			 * of an entry we already have.
			 */

			if (MAX_STAMP != stamp && delta_time(dme->stamp, stamp) >= 0)
				return FALSE;

			dm->entries = g_slist_remove(dm->entries, dme);
			dm->count--;
			dmesh_entry_free(dme);
			break;
		}
	}

	return TRUE;
}

/**
 * Dispose of the entry slot, which must be empty.
 */
static void
dmesh_dispose(const gchar *sha1)
{
	gpointer key;
	gpointer value;
	gboolean found;
	struct dmesh *dm;

	found = g_hash_table_lookup_extended(mesh, sha1, &key, &value);

	dm = value;
	g_assert(found);
	g_assert(dm->count == 0);
	g_assert(dm->entries == NULL);

	/* Remove it from the hashtable before freeing key, just in case
	 * that sha1 == key. */
	g_hash_table_remove(mesh, sha1);
	atom_sha1_free(key);
	wfree(dm, sizeof *dm);
}

/**
 * Fill URL info from externally supplied sha1, addr, port, idx and name.
 * If sha1 is NULL, we use the name, otherwise the urn:sha1.
 */
static void
dmesh_fill_info(dmesh_urlinfo_t *info,
	const gchar *sha1, const host_addr_t addr,
	guint16 port, guint idx, const gchar *name)
{
	static const gchar urnsha1[] = "urn:sha1:";
	static gchar urn[SHA1_BASE32_SIZE + sizeof urnsha1];

	info->addr = addr;
	info->port = port;
	info->idx = idx;

	if (sha1) {
		concat_strings(urn, sizeof urn, urnsha1, sha1_base32(sha1), (void *) 0);
		info->name = urn;
	} else {
		info->name = name;
	}
}

/**
 * Remove entry from mesh due to a failed download attempt.
 */
gboolean
dmesh_remove(const gchar *sha1, const host_addr_t addr, guint16 port,
	guint idx, const gchar *name)
{
	struct dmesh *dm;
	dmesh_urlinfo_t info;

	/*
	 * We're called because the download failed, so we must ban the URL
	 * to prevent further insertion in the mesh.
	 */

	dmesh_fill_info(&info, sha1, addr, port, idx, name);
	dmesh_ban_add(sha1, &info, 0);

	/*
	 * Lookup SHA1 in the mesh to see if we already have entries for it.
	 */

	dm = g_hash_table_lookup(mesh, sha1);

	if (dm == NULL)				/* Nothing for this SHA1 key */
		return FALSE;

	(void) dm_remove(dm, addr, port, idx, info.name, MAX_STAMP);

	/*
	 * If there is nothing left, clear the mesh entry.
	 */

	if (dm->count == 0) {
		g_assert(dm->entries == NULL);
		dmesh_dispose(sha1);
	}

    return TRUE;
}

/**
 * Get the number of dmesh entries for a given SHA1.
 *
 * @return the number of dmesh entries
 */
gint
dmesh_count(const gchar *sha1)
{
	struct dmesh *dm;

	g_assert(sha1);

	dm = g_hash_table_lookup(mesh, sha1);

	if (dm)
		return dm->count;

	return 0;
}

/**
 * Add entry to the download mesh, indexed by the binary `sha1' digest.
 * If `stamp' is 0, then the current time is used.
 *
 * If `idx' is URN_INDEX, then we can access this file only through an
 * /uri-res request, the URN being given as `name'.
 *
 * @return whether the entry was added in the mesh, or was discarded because
 * it was the oldest record and we have enough already.
 */
static gboolean
dmesh_raw_add(const gchar *sha1, const dmesh_urlinfo_t *info, time_t stamp)
{
	struct dmesh_entry *dme;
	struct dmesh *dm;
	time_t now = tm_time();
	host_addr_t addr = info->addr;
	guint16 port = info->port;
	guint idx = info->idx;
	const gchar *name = info->name;

	if (stamp == 0 || delta_time(stamp, now) > 0)
		stamp = now;

	if (delta_time(now, stamp) > MAX_LIFETIME)
		return FALSE;

	/*
	 * Reject if this is for our host, or if the host is a private/hostile IP.
	 */

	if (is_my_address(addr, port))
		return FALSE;

	if (!host_is_valid(addr, port))
		return FALSE;

	if (hostiles_check(addr))
		return FALSE;

	/*
	 * See whether this URL is banned from the mesh.
	 */

	if (dmesh_is_banned(info))
		return FALSE;

	/*
	 * Lookup SHA1 in the mesh to see if we already have entries for it.
	 *
	 * If we don't, create a new structure and insert it in the table.
	 *
	 * If we have, make sure we remove any existing older entry first,
	 * to avoid storing duplicates (entry is removed only if found and older
	 * than the one we're trying to add).
	 */

	dm = sha1 ? g_hash_table_lookup(mesh, sha1) : NULL;

	if (dm == NULL) {
		dm = walloc(sizeof *dm);

		dm->count = 0;
		dm->entries = NULL;

		g_hash_table_insert(mesh, atom_sha1_get(sha1), dm);
	} else {
		g_assert(dm->count > 0);

		dm_expire(dm, MAX_LIFETIME, sha1);

		/*
		 * If dm_remove() returns FALSE, it means that we found the entry
		 * in the mesh, but it is not older than the supplied stamp.  So
		 * we have the entry already, and reject this duplicate.
		 */

		if (dm->count && !dm_remove(dm, addr, port, idx, name, stamp))
			return FALSE;
	}

	/*
	 * Allocate new entry.
	 */

	dme = walloc(sizeof *dme);

	dme->inserted = now;
	dme->stamp = stamp;
	dme->url.addr = addr;
	dme->url.port = port;
	dme->url.idx = idx;
	dme->url.name = atom_str_get(name);

    if (dmesh_debug)
		g_message("dmesh entry created, name %p: %s",
				dme->url.name, dme->url.name);

	/*
	 * The entries are sorted by time.  We're going to unconditionally add
	 * the new entry, and then we'll prune the last item (oldest) if we
	 * reached our maximum capacity.
	 */

	dm->entries = g_slist_insert_sorted(dm->entries, dme, dmesh_entry_cmp);
	dm->last_update = now;

	if (dm->count == MAX_ENTRIES) {
		struct dmesh_entry *last = g_slist_last(dm->entries)->data;

		dm->entries = g_slist_remove(dm->entries, last);

		dmesh_entry_free(last);

		if (last == dme)		/* New entry turned out to be the oldest */
			dme = NULL;
	} else
		dm->count++;

	/*
	 * We got a new entry that could be used for swarming if we are
	 * downloading that file.
	 */

	if (dme != NULL) {
		/*
		 * If this is from a uri-res URI, don't use the SHA1 as
		 * filename, so that the existing name is used instead.
		 */
		file_info_try_to_swarm_with(URN_INDEX == idx && sha1 ? NULL : name,
			idx, addr, port, sha1);
	}

	return dme != NULL;			/* TRUE means we added the entry */
}

/**
 * Same as dmesh_raw_add(), but this is for public consumption.
 */
gboolean
dmesh_add(gchar *sha1, const host_addr_t addr, guint16 port, guint idx,
	const gchar *name, time_t stamp)
{
	dmesh_urlinfo_t info;

	/*
	 * Translate the supplied arguments: if idx is URN_INDEX, then `name'
	 * is the filename but we must use the urn:sha1 instead, as URN_INDEX
	 * is our mark to indicate an /uri-res/N2R? URL (with an urn:sha1).
	 */

	dmesh_fill_info(&info, sha1, addr, port, idx, name);
	return dmesh_raw_add(sha1, &info, stamp);
}

/**
 * Format the URL described by `info' into the provided buffer `buf', which
 * can hold `len' bytes.
 *
 * @returns length of formatted entry, -1 if the URL would be larger than
 * the buffer.  If `quoting' is non-NULL, set it to indicate whether the
 * formatted URL should be quoted if emitted in a header, because it
 * contains a "," character.
 */
static size_t
dmesh_urlinfo(const dmesh_urlinfo_t *info, gchar *buf,
	size_t len, gboolean *quoting)
{
	size_t rw;
	size_t maxslen = len - 1;			/* Account for trailing NUL */
	const gchar *host;

	g_assert(len > 0);
	g_assert(len <= INT_MAX);
	g_assert(info->name != NULL);

	host = info->port == HTTP_PORT
			? host_addr_to_string(info->addr)
			: host_addr_port_to_string(info->addr, info->port);
	rw = concat_strings(buf, len, "http://", host, (void *) 0);
	if (rw >= maxslen)
		return (size_t) -1;

	if (info->idx == URN_INDEX) {
		rw += gm_snprintf(&buf[rw], len - rw, "/uri-res/N2R?%s", info->name);
		if (quoting != NULL)
			*quoting = FALSE;			/* No "," in the generated URL */
	} else {
		rw += gm_snprintf(&buf[rw], len - rw, "/get/%u/", info->idx);

		/*
		 * Write filename, URL-escaping it directly into the buffer.
		 */

		if (rw < maxslen) {
			gint re = url_escape_into(info->name, &buf[rw], len - rw);

			if (re < 0)
				return (size_t) -1;

			rw += re;
			if (rw < len)
				buf[rw] = '\0';
		}

		/*
		 * If `quoting' is non-NULL, look whether there is a "," in the
		 * filename.  Since "," is not URL-escaped, we look directly in
		 * the info->name field.
		 */

		if (quoting != NULL)
			*quoting = NULL != strchr(info->name, ',');
	}

	return rw < maxslen ? rw : (size_t) -1;
}

/**
 * Format the `info' URL and return pointer to static string.
 */
static gchar *
dmesh_urlinfo_to_string(const dmesh_urlinfo_t *info)
{
	static gchar urlstr[1024];

	(void) dmesh_urlinfo(info, urlstr, sizeof urlstr, NULL);

	return urlstr;
}

/**
 * Format mesh_entry in the provided buffer, as a compact addr:port address.
 * The port is even omitted if it is the standard Gnutella one.
 *
 * @returns length of formatted entry, -1 if the address would be larger than
 * the buffer, or if no compact form can be derived for this entry (not an
 * URN_INDEX kind).
 */
static size_t
dmesh_entry_compact(const struct dmesh_entry *dme, gchar *buf, size_t size)
{
	const dmesh_urlinfo_t *info = &dme->url;
	const gchar *host;
	size_t rw;

	g_assert(size > 0);
	g_assert(size <= INT_MAX);

	if (info->idx != URN_INDEX)
		return (size_t) -1;

	host = info->port == GTA_PORT
		? host_addr_to_string(info->addr)
		: host_addr_port_to_string(info->addr, info->port);

	rw = g_strlcpy(buf, host, size);
	return rw < size ? rw : (size_t) -1;
}

/**
 * Format dmesh_entry in the provided buffer, as an URL with an appended
 * timestamp in ISO format, GMT time.
 *
 * @return length of formatted entry, -1 if the URL would be larger than
 * the buffer.
 */
static size_t
dmesh_entry_url_stamp(const struct dmesh_entry *dme, gchar *buf, size_t size)
{
	size_t rw;
	gboolean quoting;

	g_assert(size > 0);
	g_assert(size <= INT_MAX);

	/*
	 * Format the URL info first.
	 */

	rw = dmesh_urlinfo(&dme->url, buf, size, &quoting);
	if ((size_t) -1 == rw)
		return (size_t) -1;

	/*
	 * If quoting is required, we need to surround the already formatted
	 * string into "quotes".
	 */

	if (quoting) {
		if (rw + 2 >= size)		/* Not enough room for 2 quotes */
			return (size_t) -1;

		g_memmove(buf + 1, buf, rw);
		buf[0] = '"';
		buf[++rw] = '"';
		buf[++rw] = '\0';
	}

	/*
	 * Append timestamp.
	 */

	rw += concat_strings(&buf[rw], size - rw,
			" ", timestamp_utc_to_string(dme->stamp), (void *) 0);

	return rw < size ? rw : (size_t) -1;
}

/**
 * Format the `dme' mesh entry as "URL timestamp".
 *
 * @return pointer to static string.
 */
static const gchar *
dmesh_entry_to_string(const struct dmesh_entry *dme)
{
	static gchar urlstr[1024];

	(void) dmesh_entry_url_stamp(dme, urlstr, sizeof urlstr);
	return urlstr;
}

/**
 * Fill supplied vector `hvec' whose size is `hcnt' with some alternate
 * locations for a given SHA1 key, that can be requested by hash directly.
 *
 * @return the amount of locations filled.
 */
gint
dmesh_fill_alternate(const gchar *sha1, gnet_host_t *hvec, gint hcnt)
{
	struct dmesh *dm;
	struct dmesh_entry *selected[MAX_ENTRIES];
	gint nselected;
	gint i;
	gint j;
	GSList *l;

	/*
	 * Fetch the mesh entry for this SHA1.
	 */

	dm = (struct dmesh *) g_hash_table_lookup(mesh, sha1);

	if (dm == NULL)						/* SHA1 unknown */
		return 0;

	/*
	 * First pass: identify entries that can be requested by hash only.
	 */

	for (i = 0, l = dm->entries; l; l = l->next) {
		struct dmesh_entry *dme = (struct dmesh_entry *) l->data;

		if (dme->url.idx != URN_INDEX)
			continue;

		g_assert(i < MAX_ENTRIES);

		selected[i++] = dme;
	}

	nselected = i;

	if (nselected == 0)
		return 0;

	g_assert(nselected <= dm->count);

	/*
	 * Second pass: choose at most `hcnt' entries at random.
	 */

	for (i = j = 0; i < nselected && j < hcnt; i++) {
		struct dmesh_entry *dme;
		gint nleft = nselected - i;
		gint npick = random_value(nleft - 1);
		gint k;
		gint n;

		/*
		 * The `npick' variable is the index of the selected entry, all
		 * NULL pointers we can encounter on our path not-withstanding.
		 */

		for (k = 0, n = npick; n >= 0; /* empty */) {
			g_assert(k < nselected);
			if (selected[k] == NULL) {
				k++;
				continue;
			}
			n--;
		}

		g_assert(k < nselected);

		dme = selected[k];
		selected[k] = NULL;				/* Can't select same entry twice */

		g_assert(j < hcnt);

		hvec[j].addr = dme->url.addr;
		hvec[j++].port = dme->url.port;
	}

	return j;		/* Amount we filled in vector */
}

/**
 * Build alternate location header for a given SHA1 key.  We generate at
 * most `size' bytes of data into `alt'.
 *
 * @param `sha1'	no brief description.
 * @param `buf'		no brief description.
 * @param 'size'	no brief description.
 *
 * @param `addr' is the host to which those alternate locations are meant:
 * we skip any data pertaining to that host.
 *
 * @param `last_sent' is the time at which we sent some previous alternate
 * locations. If there has been no change to the mesh since then, we'll
 * return an empty string.  Otherwise we return entries inserted after
 * `last_sent'.
 *
 * @param `vendor' is given to determine whether it is apt to read our
 * X-Alt and X-Nalt fields formatted with continuations or not.
 *
 * @param `fi' when it is non-NULL, it means we're sharing that file and
 * we're sending alternate locations to remote servers: include ourselves
 * in the list of alternate locations if PFSP-server is enabled.
 *
 * @param `request' if it is true, then the mesh entries are generated in
 * an HTTP request; otherwise it's for an HTTP reply.
 *
 * unless the `vendor' is GTKG, don't use continuation: most
 * servent authors don't bother with a proper HTTP header parsing layer.
 *
 * @return amount of generated data.
 */
gint
dmesh_alternate_location(const gchar *sha1,
	gchar *buf, size_t size, const host_addr_t addr,
	time_t last_sent, const gchar *vendor,
	fileinfo_t *fi, gboolean request)
{
	gchar url[1024];
	struct dmesh *dm;
	size_t len = 0;
	GSList *l;
	gint nselected = 0;
	struct dmesh_entry *selected[MAX_ENTRIES];
	gint i;
	size_t maxslen;			/* Account for trailing NUL + "\r\n" */
	GSList *by_addr;
	size_t maxlinelen = 0;
	gpointer fmt;
	gboolean added;

	g_assert(sha1);
	g_assert(buf);
	g_assert((gint) size >= 0);
	g_assert(size <= INT_MAX);

	if (size <= 3)
		return 0;
	maxslen = size - 3;		/* Account for trailing NUL + "\r\n" */

	/*
	 * Shall we emit continuations?
	 *
	 * When sending a request, unless we know the vendor is GTKG, don't.
	 * When sending a reply, do so but be nice with older BearShare versions.
	 *		--RAM, 04/01/2004.
	 */

	if (request) {
		/* We're sending the request: assume they can't read continuations */
		if (
			vendor == NULL || *vendor != 'g' ||
			!is_strprefix(vendor, "gtk-gnutella/")
		)
			maxlinelen = 100000;	/* In practice, no continuations! */
	} else {
		/* We're sending a reply: assume they can read continuations */
		if (
			vendor != NULL && *vendor == 'B' &&
			is_strprefix(vendor, "BearShare ")
		) {
			/*
			 * Only versions newer than (included) BS 4.3.4 and BS 4.4b25
			 * will properly support continuations.
			 *
			 * XXX for now disable for all.
			 */

			maxlinelen = 100000;	/* In practice, no continuations! */
		}
	}

	/*
	 * Get the X-Nalts and fill this header. Only fill up to a maximum of 33%
	 * of the total buffer size.
	 *		 -- JA, 1/11/2003
	 */

	by_addr = g_hash_table_lookup(ban_mesh_by_sha1, sha1);

	if (by_addr != NULL) {
		fmt = header_fmt_make("X-Nalt", ", ", size);
		if (maxlinelen)
			header_fmt_set_line_length(fmt, maxlinelen);
		added = FALSE;

		/* Loop through the X-Nalts */
		for (l = by_addr; l != NULL; l = g_slist_next(l)) {
			struct dmesh_banned *banned = l->data;
			dmesh_urlinfo_t *info = banned->info;

			if (info->idx != URN_INDEX)
				continue;

			if (delta_time(banned->ctime, last_sent) > 0) {
				const gchar *value;

				value = host_addr_port_to_string(info->addr, info->port);

				if (!header_fmt_value_fits(fmt, strlen(value), size / 3))
					break;

				header_fmt_append_value(fmt, value);
				added = TRUE;
			}
		}

		if (added) {
			size_t length;

			header_fmt_end(fmt);
			length = header_fmt_length(fmt);
			g_assert(length < size);
			strncpy(buf, header_fmt_string(fmt), length + 1); /* + final NUL */
			len += length;
		}

		header_fmt_free(fmt);
	}

	/* Find mesh entry for this SHA1 */
	dm = (struct dmesh *) g_hash_table_lookup(mesh, sha1);

	/*
	 * Start filling the buffer.
	 */

	fmt = header_fmt_make("X-Alt", ", ", size);
	if (maxlinelen)
		header_fmt_set_line_length(fmt, maxlinelen);
	added = FALSE;
	maxslen -= len;		/* `len' is non-zero if X-Nalt was generated */

	/*
	 * PFSP-server: If we have to list ourselves in the mesh, do so
	 * at the first position.
	 *
	 * We only introduce ourselves if we have at least MIN_PFSP_SIZE bytes
	 * of the file available, or MIN_PFSP_PCT percents of it.
	 */

	if (
		fi != NULL &&
		fi->size != 0 &&
		fi->file_size_known &&
		fi->done != 0 &&
		pfsp_server &&
		!is_firewalled &&
		is_host_addr(listen_addr()) &&
		(
			fi->done >= MIN_PFSP_SIZE ||
			fi->done * 100 / fi->size > MIN_PFSP_PCT
		) && upload_is_enabled()
	) {
		size_t url_len;
		struct dmesh_entry ourselves;
		time_t now = tm_time();

		file_info_check(fi);

		ourselves.inserted = now;
		ourselves.stamp = now;
		ourselves.url.addr = listen_addr();
		ourselves.url.port = listen_port;
		ourselves.url.idx = URN_INDEX;
		ourselves.url.name = NULL;

		url_len = dmesh_entry_compact(&ourselves, url, sizeof url);
		g_assert((size_t) -1 != url_len && url_len < sizeof url);

		if (!header_fmt_value_fits(fmt, url_len, maxslen))
			goto nomore;

		header_fmt_append_value(fmt, url);
		added = TRUE;
	}

	/*
	 * Check whether we have anything (new).
	 */

	if (dm == NULL)						/* SHA1 unknown */
		goto nomore;

	if (delta_time(dm->last_update, last_sent) <= 0)	/* No new insertion */
		goto nomore;

	/*
	 * Expire old entries.  If none remain, free entry and return.
	 */

	dm_expire(dm, MAX_LIFETIME, sha1);

	if (dm->count == 0) {
		g_assert(dm->entries == NULL);
		dmesh_dispose(sha1);
		goto nomore;
	}

	/*
	 * Go through the list, selecting new entries that can fit.
	 * We'll do two passes.  The first pass identifies the candidates.
	 * The second pass randomly selects items until we fill the room
	 * allocated.
	 */

	memset(selected, 0, sizeof(selected));

	/*
	 * First pass.
	 */

	for (i = 0, l = dm->entries; l; l = l->next) {
		struct dmesh_entry *dme = (struct dmesh_entry *) l->data;

		if (delta_time(dme->inserted, last_sent) <= 0)
			continue;

		if (host_addr_equal(dme->url.addr, addr))
			continue;

		if (dme->url.idx != URN_INDEX)
			continue;

		g_assert(i < MAX_ENTRIES);

		selected[i++] = dme;
	}

	nselected = i;

	if (nselected == 0)
		goto nomore;

	g_assert(nselected <= dm->count);

	/*
	 * Second pass.
	 */

	for (i = 0; i < nselected; i++) {
		struct dmesh_entry *dme;
		gint nleft = nselected - i;
		gint npick = random_value(nleft - 1);
		gint j;
		gint n;
		size_t url_len;

		/*
		 * The `npick' variable is the index of the selected entry, all
		 * NULL pointers we can encounter on our path not-withstanding.
		 */

		for (j = 0, n = npick; n >= 0; /* empty */) {
			g_assert(j < nselected);
			if (selected[j] == NULL) {
				j++;
				continue;
			}
			n--;
		}

		g_assert(j < nselected);

		dme = selected[j];
		selected[j] = NULL;				/* Can't select same entry twice */

		g_assert(delta_time(dme->inserted, last_sent) > 0);

		url_len = dmesh_entry_compact(dme, url, sizeof url);

		/* Buffer was large enough */
		g_assert((size_t) -1 != url_len && url_len < sizeof url);

		if (!header_fmt_value_fits(fmt, url_len, maxslen))
			continue;

		header_fmt_append_value(fmt, url);
		added = TRUE;
	}

nomore:
	if (added) {
		size_t length;

		header_fmt_end(fmt);
		length = header_fmt_length(fmt);
		g_assert(length + len < size);
		/* Add +1 for final NUL */
		strncpy(&buf[len], header_fmt_string(fmt), length + 1);
		len += length;
	}
	header_fmt_free(fmt);

	return len;
}

/**
 * A simple container for the dmesh info that the deferred checking
 * code needs, although to be honest it may be worth refactoring the
 * dmesh code so it all works on dmesh_entries?
 */

typedef struct {
    dmesh_urlinfo_t *dmesh_url;	/**< The URL details */
    time_t stamp;				/**< Timestamp */
} dmesh_deferred_url_t;

/**
 * Create a list of nonurn alternative locations for a given sha1.
 * This is used in preference to self-consitancy checking as these
 * urls are already "known" to be valid and hence we do not waste
 * potentially valid urls on a "all or nothing" approach.
 */
static GSList *
dmesh_get_nonurn_altlocs(const gchar *sha1)
{
    struct dmesh *dm;
    GSList *sl, *nonurn_altlocs = NULL;

    g_assert(sha1);

    dm = g_hash_table_lookup(mesh, sha1);
    if (dm == NULL)				/* SHA1 unknown */
		return NULL;

	for (sl = dm->entries; sl; sl = g_slist_next(sl)) {
		struct dmesh_entry *dme = sl->data;

		if (dme->url.idx != URN_INDEX)
			nonurn_altlocs = g_slist_append(nonurn_altlocs, dme);
	}

    return nonurn_altlocs;
}

/**
 * This function defers adding a proposed alternate location that has been
 * given as an old style string (i.e. not a URN). After
 * dmesh_collect_locations() has parsed all the alt locations another
 * function (dmesh_check_deferred_altlocs) will be called to make a
 * judgement on the quality of these results, and determine whether they
 * should be added to the dmesh.
 */
static GSList *
dmesh_defer_nonurn_altloc(GSList *list, dmesh_urlinfo_t *url, time_t stamp)
{
    static const dmesh_deferred_url_t zero_defer;
    dmesh_deferred_url_t *defer;

    /* Allocate a structure */
    defer = walloc(sizeof *defer);
	*defer = zero_defer;

    /*
	 * Copy the structure, beware of atoms.
	 */

    defer->dmesh_url = wcopy(url, sizeof *url);
    defer->dmesh_url->name = atom_str_get(url->name);
    defer->stamp = stamp;

	if (dmesh_debug)
		g_message("defering nonurn altloc str=%px:%s",
			defer->dmesh_url->name, defer->dmesh_url->name);

    /* Add to list */
    return g_slist_append(list, defer);
}

/**
 * Called to deallocate the dmesh_urlinfo and de-reference names
 * from the GSList. Intended to be called from a g_slist_foreach. The
 * list itself needs to be freed afterwards
 */
static inline void
dmesh_free_deferred_altloc(dmesh_deferred_url_t *info)
{
	dmesh_urlinfo_free(info->dmesh_url);
	wfree(info, sizeof *info);
}

/**
 * This routine checks deferred dmesh entries against any existing
 * nonurn alternative locations. The two factors that control the
 * quality of hits queued are:
 *
 * a) Fuzzy factor of compares
 * b) Number of the altlocs they have to match (threshold)
 *
 * These factors are currently semi-empirical guesses and hardwired.
 *
 * @note
 * This is an O(m*n) process, when `m' is the amount of new entries
 * and `n' the amount of existing entries.
 */
static void
dmesh_check_deferred_against_existing(const gchar *sha1,
	GSList *existing_urls, GSList *deferred_urls)
{
    GSList *ex, *def;
	GSList *adding = NULL;
    gulong score;
    gint matches;
    gint threshold = g_slist_length(existing_urls);
	time_t now = tm_time();

    /* We want to match at least 2 or more entries, going for 50% */
    threshold = (threshold < 3) ? 2 : (threshold / 2);

	for (def = deferred_urls; def; def = def->next) {
		dmesh_deferred_url_t *d = def->data;
		matches = 0;

		if (dmesh_debug > 4)
			g_message("checking deferred url %p (str=%p:%s)",
				cast_to_gconstpointer(d),
				cast_to_gconstpointer(d->dmesh_url->name),
				d->dmesh_url->name);

		for (ex = existing_urls; ex; ex = ex->next) {
			struct dmesh_entry *dme = ex->data;
			score = fuzzy_compare(dme->url.name, d->dmesh_url->name);
			if (score > FUZZY_MATCH)
				matches++;
		}

		/*
		 * We can't add the entry in the mesh in the middle of the
		 * traversal: if we reach the max amount of entries in the mesh,
		 * we'll free some of them, and since our `existing_urls' items
		 * directly refer the dmesh_entry structures, that would be horrible!
		 *		--RAM, 05/02/2003
		 */

		if (matches >= threshold)
			adding = g_slist_prepend(adding, d);
		else {
			dmesh_urlinfo_t *url = d->dmesh_url;
			if (dmesh_debug)
				g_warning("dumped potential dmesh entry:\n%s\n\t"
					"(only matched %d of the others, needed %d)",
					url->name, matches, threshold);
		}
	} /* for def */

	for (def = adding; def; def = def->next) {
		dmesh_deferred_url_t *d = def->data;
		dmesh_urlinfo_t *url = d->dmesh_url;
		gboolean ok;

		ok = dmesh_raw_add(sha1, url, d->stamp);

		if (dmesh_debug > 4) {
			g_message("MESH %s: %s deferred \"%s\", stamp=%u age=%d",
				sha1_base32(sha1),
				ok ? "added" : "rejected",
				dmesh_urlinfo_to_string(url), (guint) d->stamp,
				(gint) delta_time(now, d->stamp));
		}
	}

	g_slist_free(adding);
}

/**
 * This routine checks deferred dmesh entries against themselves. They
 * have to be 100% consistent with what was being passed otherwise we
 * are conservative a throw them away. The main factor is the fuzzy
 * factor of the compare.
 *
 * Apologies for the returns but this function doesn't need to clean up
 * after itself yet.
 */
static void
dmesh_check_deferred_against_themselves(const gchar *sha1,
	GSList *deferred_urls)
{
    dmesh_deferred_url_t *first;
    GSList *sl = deferred_urls;

    first = sl->data;
    sl = g_slist_next(sl);

	if (NULL == sl) { /* It's probably correct, should we bin it? */
		if (dmesh_debug > 4)
			g_message("only one altloc to check, currently dumping:\n%s",
				first->dmesh_url->name);
		return;
	}

	for (/* NOTHING */; sl; sl = g_slist_next(sl)) {
    	dmesh_deferred_url_t *current = sl->data;
    	gulong score;

		score = fuzzy_compare(first->dmesh_url->name, current->dmesh_url->name);
		if (score < FUZZY_DROP) {
			/* When anything fails, it's all over */
			if (dmesh_debug > 4)
				g_message("dmesh_check_deferred_against_themselves failed with:"
					" %s\n\t"
					"(only scoring %lu against:\n\t"
					"%s\n",
					current->dmesh_url->name, score, first->dmesh_url->name);
			return;
		}
	}

    /* We made it this far, they must all match, lets add them to the dmesh */
	for (sl = deferred_urls; sl; sl = g_slist_next(sl)) {
		dmesh_deferred_url_t *def = sl->data;
		dmesh_urlinfo_t *url = def->dmesh_url;
		gboolean ok;

		ok = dmesh_raw_add(sha1, url, def->stamp);

		if (dmesh_debug > 4) {
			g_message("MESH %s: %s consistent deferred \"%s\", stamp=%u age=%d",
				sha1_base32(sha1),
				ok ? "added" : "rejected",
				dmesh_urlinfo_to_string(url), (guint32) def->stamp,
				(gint) delta_time(tm_time(), def->stamp));
		}
	}

    return;
}

/**
 * This function is called once dmesh_collect_locations is done and
 * makes it then decides how its going to vet the quality of the
 * deferred nonurn altlocs before decing if they are going to be added
 * for the given sha1.
 *
 * A couple of approaches are taken:
 *
 * a) if any non-urn urls already exist compare against them and add
 *    the ones that match
 * a) otherwise only add urls if they are all the same(ish)
 *
 * Another possible algorithm (majority wins) has been tried but
 * empirically did allow the occasional dmesh pollution. As we are
 * going for  a non-polluted dmesh we shall be conservative. Besides
 * using urn's for files is generally a better idea.
 *
 */
static void
dmesh_check_deferred_altlocs(const gchar *sha1, GSList *deferred_urls)
{
    GSList *existing_urls;

    if (NULL == deferred_urls)		/* Nothing to do */
		return;

    existing_urls = dmesh_get_nonurn_altlocs(sha1);

    if (existing_urls) {
		dmesh_check_deferred_against_existing(sha1,
			existing_urls, deferred_urls);
		g_slist_free(existing_urls);
	} else
		dmesh_check_deferred_against_themselves(sha1, deferred_urls);

	G_SLIST_FOREACH(deferred_urls, dmesh_free_deferred_altloc);
    g_slist_free(deferred_urls);
}

/**
 * Parse the value of the X-Gnutella-Content-URN header in `value', looking
 * for a SHA1.  When found, the SHA1 is extracted and placed into the given
 * `digest' buffer.
 *
 * @return whether we successfully extracted the SHA1.
 */
gboolean
dmesh_collect_sha1(const gchar *value, gchar *digest)
{
	const gchar *p;

	for (p = value; NULL != p && '\0' != *p; /* NOTHING */) {

		/*
		 * Skip leading spaces, if any.
		 */

		p = skip_ascii_spaces(p);
		if (urn_get_sha1(p, digest))
			return TRUE;

		/*
		 * Advance past the next ',', if any.
		 */

		p = strchr(p, ',');
		if (p)
			p++;
	}

	return FALSE;
}

/**
 * Parse the value of the "X-Alt" header to extract alternate sources
 * for a given SHA1 key given in the new compact form.
 */
void
dmesh_collect_compact_locations(const gchar *sha1, const gchar *value)
{
	time_t now = tm_time();
	const gchar *p = value;

	do {
		const gchar *start, *endptr;
		host_addr_t addr;
		guint16 port;
		gboolean ok;

		start = skip_ascii_blanks(p);
		if ('\0' == *start)
			break;

		endptr = strchr(start, ',');
		if (!endptr) {
			endptr = strchr(start, ';');
			if (!endptr)
				endptr = strchr(start, '\0');
		}

		/*
		 * There could be a GUID here if the host is not directly connectible
		 * but we ignore this apparently.
		 */	
		ok = string_to_host_addr(start, &endptr, &addr);
		if (ok && ':' == *endptr) {
			gint error;
				
			port = parse_uint16(&endptr[1], &endptr, 10, &error);
			ok = !error && port > 0; 
		} else {
			port = GTA_PORT;
		}
		
		if (ok) {
			dmesh_urlinfo_t info;

			dmesh_fill_info(&info, sha1, addr, port, URN_INDEX, NULL);
			ok = dmesh_raw_add(sha1, &info, now);

			if (dmesh_debug > 4)
				g_message("MESH %s: %s compact \"%s\", stamp=%u",
						sha1_base32(sha1),
						ok ? "added" : "rejected",
						dmesh_urlinfo_to_string(&info), (guint) now);
		} else if (dmesh_debug) {
			g_warning("ignoring invalid compact alt-loc \"%s\"", start);
		}

		p = '\0' != *endptr ? &endptr[1] : NULL;
	} while (p);
}

/**
 * Parse value of the "X-Gnutella-Alternate-Location" to extract alternate
 * sources for a given SHA1 key.
 */
void
dmesh_collect_locations(const gchar *sha1, gchar *value, gboolean defer)
{
	gchar *p = value;
	guchar c;
	time_t now = tm_time();
    GSList *nonurn_altlocs = NULL;
	gboolean finished = FALSE;

	do {
		gchar *url;
		gchar *date;
		time_t stamp;
		gboolean ok;
		dmesh_urlinfo_t info;
		gboolean skip_date;
		gboolean in_quote;

		/*
		 * Find next space, colon or EOS (End of String).
		 * Everything from now to there will be an URL.
		 * All leading spaces are skipped.
		 */

		in_quote = FALSE;
		info.name = NULL;
		info.addr = zero_host_addr;

		p = skip_ascii_spaces(p);
		if ('\0' == *p) {				/* Only seen spaces */
			finished = TRUE;
			goto free_urlinfo;
		}

		for (url = p; '\0' != (c = *p); p++) {
			/*
			 * Quoted identifiers are one big token.
			 */

			if (in_quote && c == '\\' && p[1] == '"')
				g_warning("unsupported \\\" escape sequence in quoted section "
					"for Alternate-Location: should use URL escaping instead!");

			if (c == '"') {
				in_quote = !in_quote;
				if (!in_quote)
					break;			/* Space MUST follow after end quote */
			}

			if (in_quote)
				continue;

			/*
			 * The "," may appear un-escaped in the URL.
			 *
			 * We know we're no longer in an URL if the character after is a
			 * space (should be escaped).  Our header parsing code will
			 * concatenate lines with a ", " separation.
			 *
			 * If the character after the "," is an 'h' and we're seeing
			 * the string "http://" coming, then we've reached the end
			 * of the current URL (all URLs were given on one big happy line).
			 */

			if (c == ',') {
				if (is_strcaseprefix(&p[1], "http://"))
					break;
				if (!is_ascii_space(p[1]))
					continue;
			}

			if (is_ascii_space(c) || c == ',')
				break;
		}

		/*
		 * Parse URL.
		 */

		g_assert((guchar) *p == c);

		if (*url == '"') {				/* URL enclosed in quotes? */
			url++;						/* Skip that needless quote */
			if (c != '"')
				g_warning("Alternate-Location URL \"%s\" started with leading "
					"quote, but did not end with one!", url);
		}

		/*
		 * Once dmesh_url_parse() has been called and returned `ok', we'll
		 * have a non-NULL `info.name' field.  This is an atom that must
		 * get freed: instead of saying `continue', we must `goto free_urlinfo'
		 * so that this atom can be freed.
		 */

		*p = '\0';
		ok = dmesh_url_parse(url, &info);

		if (dmesh_debug > 6)
			g_message("MESH (parsed=%d): \"%s\"", ok, url);

		if (!ok)
			g_warning("cannot parse Alternate-Location URL \"%s\": %s",
				url, dmesh_url_strerror(dmesh_url_errno));

		*p = c;

		if (c == '"')				/* URL ended with a quote, skip it */
			c = *(++p);

		/*
		 * Maybe there is no date following the URL?
		 */

		if (c == ',') {				/* There's no following date then */
			p++;					/* Skip separator */
			goto free_urlinfo;		/* continue */
		}

		skip_date = !ok;			/* Skip date if we did not parse the URL */

		/*
		 * Advance to next ',', expecting a date.
		 */

		if (c != '\0')
			p++;

		date = p;

	more_date:
		for (/* NOTHING */; '\0' != (c = *p); p++) {
            /*
             * Limewire has a bug not to use the ',' separator, so
             * we assume a new urn is starting with "http://"
             *      -Richard 23/11/2002
             */

            if (c == ',' || is_strcaseprefix(p, "http://"))
				break;
		}

		/*
		 * Disambiguate "Mon, 17 Jun 2002 07:53:14 +0200"
		 */

		if (c == ',' && p - date == 3) {
			p++;
			goto more_date;
		}

		if (skip_date) {				/* URL was not parsed, just skipping */
			if (c == '\0')				/* Reached end of string */
				finished = TRUE;
			else if (c == ',')
                p++;					/* Skip the "," separator */
			goto free_urlinfo;			/* continue */
		}

		/*
		 * Parse date, if present.
		 */

		if (p != date) {
			g_assert((guchar) *p == c);

			*p = '\0';
			stamp = date2time(date, now);

			if (dmesh_debug > 6)
				g_message("MESH (stamp=%u): \"%s\"", (guint) stamp, date);

			if (stamp == (time_t) -1) {
				g_warning("cannot parse Alternate-Location date: %s", date);
				stamp = 0;
			}

			*p = c;
		} else
			stamp = 0;

		/*
		 * If we have a /uri-res/N2R?urn:sha1, make sure it's matching
		 * the SHA1 of the entry for which we're keeping those alternate
		 * locations.
		 */

		if (info.idx == URN_INDEX) {
			gchar digest[SHA1_RAW_SIZE];
		
			ok = urn_get_sha1(info.name, digest);
			g_assert(ok);

			ok = sha1_eq(sha1, digest);
			if (!ok) {
				g_assert(sha1);
				g_warning("mismatch in /uri-res/N2R? Alternate-Location "
					"for SHA1=%s: got %s", sha1_base32(sha1), info.name);
				goto skip_add;
			}

			/* FALL THROUGH */

			/*
			 * Enter URL into mesh - only if it's a URN_INDEX
			 * to avoid dmesh pollution.
			 */

			ok = dmesh_raw_add(sha1, &info, stamp);

		} else {
			if (fuzzy_filter_dmesh && defer) {
				/*
				 * For named altlocs, defer so we can check they are ok,
				 * all in one block.
				 */
				nonurn_altlocs = dmesh_defer_nonurn_altloc(nonurn_altlocs,
									&info, stamp);
				goto nolog;
			} else
				ok = dmesh_raw_add(sha1, &info, stamp);
		}

	skip_add:
		if (dmesh_debug > 4)
			g_message("MESH %s: %s \"%s\", stamp=%u age=%d",
				sha1_base32(sha1),
				ok ? "added" : "rejected",
				dmesh_urlinfo_to_string(&info), (guint) stamp,
				(gint) delta_time(now, stamp));

	nolog:
		if (c == '\0')				/* Reached end of string */
			finished = TRUE;
		else if (c == ',')
            p++;					/* Skip separator */

	free_urlinfo:
		if (info.name)
			atom_str_free(info.name);

	} while (!finished);

	/*
	 * Once everyone is done we can sort out deferred urls, if any.
	 */

	if (fuzzy_filter_dmesh && defer) {
		/*
		 * We only defer when collection from live headers.
		 */

		dmesh_check_deferred_altlocs(sha1, nonurn_altlocs);
	}

    return;
}

/**
 * Fill buffer with at most `count' alternative locations for sha1.
 *
 * @returns the amount of locations inserted.
 */
static gint
dmesh_alt_loc_fill(const gchar *sha1, dmesh_urlinfo_t *buf, gint count)
{
	struct dmesh *dm;
	GSList *sl;
	gint i;

	g_assert(sha1);
	g_assert(buf);
	g_assert(count > 0);

	dm = g_hash_table_lookup(mesh, sha1);
	if (dm == NULL)					/* SHA1 unknown */
		return 0;

	for (i = 0, sl = dm->entries; sl && i < count; sl = g_slist_next(sl)) {
		struct dmesh_entry *dme = sl->data;
		dmesh_urlinfo_t *from;

		g_assert(i < MAX_ENTRIES);

		from = &dme->url;
		buf[i++] = *from;
	}

	return i;
}

/**
 * Parse query hit (result set) for entries whose SHA1 match something
 * we have into the mesh or share, and insert them if needed.
 */
void
dmesh_check_results_set(gnet_results_set_t *rs)
{
	GSList *sl;
	time_t now = tm_time();

	for (sl = rs->records; sl; sl = g_slist_next(sl)) {
		gnet_record_t *rc = sl->data;
		dmesh_urlinfo_t info;
		gboolean has = FALSE;

		if (rc->sha1 == NULL)
			continue;

		/*
		 * If we have an entry for this SHA1 in the mesh already,
		 * then we can update it for that entry.
		 *
		 * If the entry is not in the mesh already, look whether we're
		 * sharing this SHA1.
		 */

		has = NULL != g_hash_table_lookup(mesh, rc->sha1);

		if (!has) {
			struct shared_file *sf = shared_file_by_sha1(rc->sha1);
			if (sf != NULL && sf != SHARE_REBUILDING && sf->fi == NULL)
				has = TRUE;
		}

		if (has) {
			dmesh_fill_info(&info, rc->sha1, rs->addr, rs->port,
				URN_INDEX, NULL);
			(void) dmesh_raw_add(rc->sha1, &info, now);

			/*
			 * If we have further alt-locs specified in the query hit, add
			 * them to the mesh and dispose of them.
			 */

			if (rc->alt_locs != NULL) {
				gint i;
				gnet_host_vec_t *alt = rc->alt_locs;

				for (i = alt->hvcnt - 1; i >= 0; i--) {
					struct gnutella_host *h = &alt->hvec[i];

					dmesh_fill_info(&info, rc->sha1, h->addr, h->port,
						URN_INDEX, NULL);
					(void) dmesh_raw_add(rc->sha1, &info, now);
				}

				search_free_alt_locs(rc);		/* Read them, free them! */
			}

			g_assert(rc->alt_locs == NULL);
		}
	}
}

#define DMESH_MAX	MAX_ENTRIES

/**
 * This is called when swarming is first requested to get a list of all the
 * servers with the requested file known by dmesh.
 * It creates a new download for every server found.
 *
 * @param `sha1' (atom) the SHA1 of the file.
 * @param `size' the original file size.
 * @param `fi' no brief description.
 */
void
dmesh_multiple_downloads(const gchar *sha1, filesize_t size, fileinfo_t *fi)
{
	dmesh_urlinfo_t buffer[DMESH_MAX], *p;
	gint n;
	time_t now;

	n = dmesh_alt_loc_fill(sha1, buffer, DMESH_MAX);
	if (n == 0)
		return;

	now = tm_time();

	for (p = buffer; n > 0; n--, p++) {
		const gchar *filename;

		if (dmesh_debug > 2)
			g_message("ALT-LOC queuing from MESH: %s",
				dmesh_urlinfo_to_string(p));

		filename = URN_INDEX == p->idx && fi && fi->file_name
			? fi->file_name
			: p->name;
		download_auto_new(p->name, size, p->idx, p->addr, p->port,
			blank_guid, NULL, sha1, now,
			fi->file_size_known, fi, NULL, /* XXX: TLS? */ 0);
	}
}

/**
 * Store key/value pair in file.
 */
static void
dmesh_store_kv(gpointer key, gpointer value, gpointer udata)
{
	const struct dmesh *dm = value;
	FILE *out = udata;
	GSList *sl;

	g_assert(key);
	fprintf(out, "%s\n", sha1_base32(key));

	for (sl = dm->entries; sl; sl = g_slist_next(sl)) {
		const struct dmesh_entry *dme = sl->data;
		fprintf(out, "%s\n", dmesh_entry_to_string(dme));
	}

	fputs("\n", out);
}

/* XXX add dmesh_store_if_dirty() and export that only */

typedef void (*header_func_t)(FILE *out);

/**
 * Store hash table `hash' into `file'.
 * The file header is emitted by `header_cb'.
 * The storing callback for each item is `store_cb'.
 */
static void
dmesh_store_hash(const gchar *what, GHashTable *hash, const gchar *file,
	header_func_t header_cb, GHFunc store_cb)
{
	FILE *out;
	file_path_t fp;

	file_path_set(&fp, settings_config_dir(), file);
	out = file_config_open_write(what, &fp);

	if (!out)
		return;

	header_cb(out);
	g_hash_table_foreach(hash, store_cb, out);

	file_config_close(out, &fp);
}

/**
 * Prints header to dmesh store file.
 */
static void
dmesh_header_print(FILE *out)
{
	file_config_preamble(out, "Download mesh");

	fputs(	"#\n# Format is:\n"
			"#   SHA1\n"
			"#   URL1 timestamp1\n"
			"#   URL2 timestamp2\n"
			"#   <blank line>\n"
			"#\n\n",
			out);
}

/**
 * Store download mesh onto file.
 * The download mesh is normally stored in ~/.gtk-gnutella/dmesh.
 */
void
dmesh_store(void)
{
	dmesh_store_hash("download mesh",
		mesh, dmesh_file, dmesh_header_print, dmesh_store_kv);
}

/**
 * Retrieve download mesh and add entries that have not expired yet.
 * The mesh is normally retrieved from ~/.gtk-gnutella/dmesh.
 */
static void
dmesh_retrieve(void)
{
	FILE *f;
	gchar tmp[4096];
	gchar sha1[SHA1_RAW_SIZE];
	gboolean has_sha1 = FALSE;
	gboolean skip = FALSE, truncated = FALSE;
	gint line = 0;
	file_path_t fp[1];

	file_path_set(fp, settings_config_dir(), dmesh_file);
	f = file_config_open_read("download mesh", fp, G_N_ELEMENTS(fp));
	if (!f)
		return;

	/*
	 * Retrieval algorithm:
	 *
	 * Lines starting with a # are skipped.
	 *
	 * We read the SHA1 first, validate it.  The remaining line up to a
	 * blank line are attached sources for this SHA1.
	 */

	while (fgets(tmp, sizeof tmp, f)) {
		if (NULL == strchr(tmp, '\n')) {
			truncated = TRUE;
			continue;
		}
		line++;
		if (truncated) {
			truncated = FALSE;
			continue;
		}

		if (tmp[0] == '#')
			continue;			/* Skip comments */

		if (tmp[0] == '\n') {
			if (has_sha1)
				has_sha1 = FALSE;
			skip = FALSE;		/* Synchronization point */
			continue;
		}

		if (skip)
			continue;

		str_chomp(tmp, 0);		/* Remove final "\n" */

		if (has_sha1)
			dmesh_collect_locations(sha1, tmp, FALSE);
		else {
			if (
				strlen(tmp) < SHA1_BASE32_SIZE ||
				!base32_decode_into(tmp, SHA1_BASE32_SIZE, sha1, sizeof sha1)
			) {
				g_warning("dmesh_retrieve: "
					"bad base32 SHA1 '%.32s' at line #%d, ignoring", tmp, line);
				skip = TRUE;
			} else
				has_sha1 = TRUE;
		}
	}

	fclose(f);
	dmesh_store();			/* Persist what we have retrieved */
}

/**
 * Store key/value pair in file.
 */
static void
dmesh_ban_store_kv(gpointer key, gpointer value, gpointer udata)
{
	const struct dmesh_banned *dmb = value;
	FILE *out = udata;

	g_assert(key == dmb->info);

	fprintf(out, "%lu %s\n",
		(gulong) dmb->ctime, dmesh_urlinfo_to_string(dmb->info));
}

/**
 * Prints header to banned mesh store file.
 */
static void
dmesh_ban_header_print(FILE *out)
{
	file_config_preamble(out, "Banned mesh");

	fputs(	"#\n# Format is:\n"
			"#  timestamp URL\n"
			"#\n\n", out);
}

/**
 * Store banned mesh onto file.
 * The banned mesh is normally stored in ~/.gtk-gnutella/dmesh_ban.
 */
void
dmesh_ban_store(void)
{
	dmesh_store_hash("banned mesh",
		ban_mesh, dmesh_ban_file, dmesh_ban_header_print, dmesh_ban_store_kv);
}

/**
 * Retrieve banned mesh and add entries that have not expired yet.
 * The mesh is normally retrieved from ~/.gtk-gnutella/dmesh_ban.
 */
static void
dmesh_ban_retrieve(void)
{
	FILE *in;
	gchar tmp[1024];
	gint line = 0;
	time_t stamp;
	const gchar *p;
	gint error;
	dmesh_urlinfo_t info;
	file_path_t fp;

	file_path_set(&fp, settings_config_dir(), dmesh_ban_file);
	in = file_config_open_read("banned mesh", &fp, 1);

	if (!in)
		return;

	/*
	 * Retrieval algorithm:
	 *
	 * Lines starting with a # are skipped.
	 */

	while (fgets(tmp, sizeof tmp, in)) {
		line++;

		if (tmp[0] == '#')
			continue;			/* Skip comments */

		if (tmp[0] == '\n')
			continue;			/* Skip empty lines */

		str_chomp(tmp, 0);		/* Remove final "\n" */

		stamp = parse_uint64(tmp, &p, 10, &error);
		if (error || *p != ' ') {
			g_warning("malformed stamp at line #%d in banned mesh: %s",
				line, tmp);
			continue;
		}

		p++;					/* Now points at the start of the URL */
		if (!dmesh_url_parse(p, &info)) {
			g_warning("malformed URL at line #%d in banned mesh: %s",
				line, tmp);
			continue;
		}

		/* FIXME: Save SHA1 for banning */
		dmesh_ban_add(NULL, &info, stamp);
		atom_str_free(info.name);
	}

	fclose(in);
	dmesh_ban_store();			/* Persist what we have retrieved */
}

/**
 * Free key/value pair in download mesh hash.
 */
static gboolean
dmesh_free_kv(gpointer key, gpointer value, gpointer unused_udata)
{
	struct dmesh *dm = value;
	GSList *sl;

	(void) unused_udata;
	atom_sha1_free(key);

	for (sl = dm->entries; sl; sl = g_slist_next(sl))
		dmesh_entry_free(sl->data);

	g_slist_free(dm->entries);
	wfree(dm, sizeof *dm);

	return TRUE;
}

/**
 * Prepend the value to the list, given by reference.
 */
static void
dmesh_ban_prepend_list(gpointer key, gpointer value, gpointer user)
{
	struct dmesh_banned *dmb = value;
	GSList **listref = user;

	g_assert(key == dmb->info);

	*listref = g_slist_prepend(*listref, dmb);
}

/**
 * Called at servent shutdown time.
 */
void
dmesh_close(void)
{
	GSList *banned = NULL;
	GSList *sl;

	dmesh_store();
	dmesh_ban_store();

	g_hash_table_foreach_remove(mesh, dmesh_free_kv, NULL);
	g_hash_table_destroy(mesh);

	/*
	 * Construct a list of banned mesh entries to remove, then manually
	 * expire all the entries, which will remove entries from `ban_mesh'
	 * and `ban_mesh_by_sha1' as well.
	 */

	g_hash_table_foreach(ban_mesh, dmesh_ban_prepend_list, &banned);

	for (sl = banned; sl; sl = g_slist_next(sl)) {
		struct dmesh_banned *dmb = sl->data;
		cq_cancel(callout_queue, dmb->cq_ev);
		dmesh_ban_expire(callout_queue, dmb);
	}

	g_slist_free(banned);

	g_hash_table_destroy(ban_mesh);
	g_hash_table_destroy(ban_mesh_by_sha1);
}

/* vi: set ts=4 sw=4 cindent: */
