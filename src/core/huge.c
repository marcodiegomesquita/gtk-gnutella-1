/*
 * Copyright (c) 2002-2003, Ch. Tronche & Raphael Manfredi
 *
 * Started by Ch. Tronche (http://tronche.com/) 28/04/2002
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
 * HUGE support (Hash/URN Gnutella Extension).
 *
 * @author Raphael Manfredi
 * @date 2002-2003
 * @author Ch. Tronche (http://tronche.com/)
 * @date 2002-04-28
 */

#include "common.h"

#include "huge.h"

#include "dmesh.h"
#include "gmsg.h"
#include "nodes.h"
#include "settings.h"
#include "share.h"
#include "spam.h"
#include "verify_sha1.h"
#include "verify_tth.h"
#include "version.h"

#include "lib/atoms.h"
#include "lib/base32.h"
#include "lib/file.h"
#include "lib/gnet_host.h"
#include "lib/halloc.h"
#include "lib/hashing.h"
#include "lib/header.h"
#include "lib/hikset.h"
#include "lib/parse.h"
#include "lib/pattern.h"
#include "lib/sha1.h"
#include "lib/stringify.h"
#include "lib/tm.h"
#include "lib/urn.h"
#include "lib/walloc.h"

#include "if/gnet_property.h"
#include "if/gnet_property_priv.h"

#include "lib/override.h"		/* Must be the last header included */

/***
 *** Server side: computation of SHA1 hash digests and replies.
 *** SHA1 is defined in RFC 3174.
 ***/

/**
 * There's an in-core cache (the hash table ``sha1_cache''), and a
 * persistent copy (normally in ~/.gtk-gnutella/sha1_cache). The
 * in-core cache is filled with the persistent one at launch. When the
 * "shared_file" (the records describing the shared files, see
 * share.h) are created, a call is made to sha1_set_digest to fill the
 * SHA1 digest part of the shared_file. If the digest isn't found in
 * the in-core cache, it's computed, stored in the in-core cache and
 * appended at the end of the persistent cache. If the digest is found
 * in the cache, a check is made based on the file size and last
 * modification time. If they're identical to the ones in the cache,
 * the digest is considered to be accurate, and is used. If the file
 * size or last modification time don't match, the digest is computed
 * again and stored in the in-core cache, but it isn't stored in the
 * persistent one. Instead, the cache is marked as dirty, and will be
 * entirely overwritten by dump_cache, called when everything has been
 * computed.
 */

struct sha1_cache_entry {
    const char *file_name;		/**< Full path name (atom)          */
	const struct sha1 *sha1;	/**< SHA-1 (binary; atom)			*/
	const struct tth *tth;		/**< TTH (binary; atom)				*/
    filesize_t  size;			/**< File size                      */
    time_t mtime;				/**< Last modification time         */
    bool shared;				/**< There's a known entry for this
                                     file in the share library      */
};

static hikset_t *sha1_cache;

/**
 * cache_dirty = TRUE means that in-core cache is different from the disk one.
 */
static bool cache_dirty;
static time_t cache_dumped;

static cpattern_t *has_http_urls;

/**
 ** Handling of persistent buffer
 **/

/* In-memory cache */

/**
 * Takes an in-memory cached entry, and update its content.
 */
static void update_volatile_cache(
	struct sha1_cache_entry *item,
	filesize_t size,
	time_t mtime,
	const struct sha1 *sha1,
	const struct tth *tth)
{
	g_assert(sha1);	/* tth may be NULL but sha1 not */

	item->shared = TRUE;
	item->size = size;
	item->mtime = mtime;
	atom_sha1_change(&item->sha1, sha1);
	atom_tth_change(&item->tth, tth);
}

/**
 * Add a new entry to the in-memory cache.
 */
static void
add_volatile_cache_entry(const char *filename, filesize_t size, time_t mtime,
	const struct sha1 *sha1, const struct tth *tth, bool known_to_be_shared)
{
	struct sha1_cache_entry *item;
   
	WALLOC(item);
	item->file_name = atom_str_get(filename);
	item->size = size;
	item->mtime = mtime;
	item->sha1 = atom_sha1_get(sha1);
	item->tth = tth ? atom_tth_get(tth) : NULL;
	item->shared = known_to_be_shared;
	hikset_insert_key(sha1_cache, &item->file_name);
}

/* Disk cache */

static const char sha1_persistent_cache_file_header[] =
"#\n"
"# gtk-gnutella SHA1 cache file.\n"
"# This file is automatically generated.\n"
"# Format is: URN<TAB>file_size<TAB>file_mtime<TAB>file_name\n"
"# Comment lines start with a sharp (#)\n"
"#\n"
"\n";

static void
cache_entry_print(FILE *f, const char *filename,
	const struct sha1 *sha1, const struct tth *tth,
	filesize_t size, time_t mtime)
{
	char size_buf[UINT64_DEC_BUFLEN], mtime_buf[UINT64_DEC_BUFLEN];

	g_return_if_fail(f);
	g_return_if_fail(filename);
	g_return_if_fail(sha1);
	g_return_if_fail(size > 0);

	uint64_to_string_buf(size, size_buf, sizeof size_buf);
	uint64_to_string_buf(mtime, mtime_buf, sizeof mtime_buf);

	fprintf(f, "%s\t%s\t%s\t%s\n", bitprint_to_urn_string(sha1, tth),
		size_buf, mtime_buf, filename);
}

/**
 * Add an entry to the persistent cache.
 */
static void
add_persistent_cache_entry(const char *filename, filesize_t size,
	time_t mtime, const struct sha1 *sha1, const struct tth *tth)
{
	char *pathname;
	FILE *f;

	pathname = make_pathname(settings_config_dir(), "sha1_cache");
	f = file_fopen(pathname, "a");
	if (f) {
		filestat_t sb;

		/*
		 * If we're adding the very first entry (file empty), then emit header.
		 */

		if (fstat(fileno(f), &sb)) {
			g_warning("%s(): could not stat \"%s\": %m", G_STRFUNC, pathname);
		} else {
			if (0 == sb.st_size) {
				fputs(sha1_persistent_cache_file_header, f);
			}
			cache_entry_print(f, filename, sha1, tth, size, mtime);
		}
		fclose(f);
	} else {
		g_warning("%s(): could not open \"%s\": %m", G_STRFUNC, pathname);
	}
	HFREE_NULL(pathname);
}

struct dump_cache_context {
	FILE *f;
	bool forced;
};

/**
 * Dump one (in-memory) cache into the persistent cache. This is a callback
 * called by dump_cache to dump the whole in-memory cache onto disk.
 */
static void
dump_cache_one_entry(void *value, void *udata)
{
	struct sha1_cache_entry *e = value;
	struct dump_cache_context *ctx = udata;

	if (ctx->forced || e->shared) {
		cache_entry_print(ctx->f,
			e->file_name, e->sha1, e->tth, e->size, e->mtime);
	}
}

/**
 * Dump the whole in-memory cache onto disk.
 */
static void
dump_cache(bool force)
{
	FILE *f;
	file_path_t fp;

	if (!force && !cache_dirty)
		return;
	
	file_path_set(&fp, settings_config_dir(), "sha1_cache");
	f = file_config_open_write("SHA-1 cache", &fp);
	if (f) {
		struct dump_cache_context ctx;
		
		fputs(sha1_persistent_cache_file_header, f);
		ctx.f = f;
		ctx.forced = force;
		hikset_foreach(sha1_cache, dump_cache_one_entry, &ctx);
		if (file_config_close(f, &fp)) {
			cache_dirty = FALSE;
		}
	}

	/*
	 * Update the timestamp even on failure to avoid that we retry this
	 * too frequently.
	 */
	cache_dumped = tm_time();
}

/**
 * This function is used to read the disk cache into memory.
 *
 * It must be passed one line from the cache (ending with '\n'). It
 * performs all the syntactic processing to extract the fields from
 * the line and calls add_volatile_cache_entry() to append the record
 * to the in-memory cache.
 */
static G_GNUC_COLD void
parse_and_append_cache_entry(char *line)
{
	const char *p, *end; /* pointers to scan the line */
	int c, error;
	filesize_t size;
	time_t mtime;
	struct sha1 sha1;
	struct tth tth;
	bool has_tth;

	/* Skip comments and blank lines */
	if (file_line_is_skipable(line))
		return;

	/* Scan until file size */

	p = line;
	while ((c = *p) != '\0' && c != '\t') {
		p++;
	}

	if (urn_get_bitprint(line, p - line, &sha1, &tth)) {
		has_tth = TRUE;
	} else if (urn_get_sha1(line, &sha1)) {
		has_tth = FALSE;
	} else {
		const char *sha1_digest_ascii;

		has_tth = FALSE;
		sha1_digest_ascii = line; /* SHA1 digest is the first field. */

		if (
			*p != '\t' ||
			(p - sha1_digest_ascii) != SHA1_BASE32_SIZE ||
			SHA1_RAW_SIZE != base32_decode(sha1.data, sizeof sha1.data,
								sha1_digest_ascii, SHA1_BASE32_SIZE)
		) {
			goto failure;
		}
	}
	p++; /* Skip \t */

	/* p is now supposed to point to the beginning of the file size */

	size = parse_uint64(p, &end, 10, &error);
	if (error || *end != '\t') {
		goto failure;
	}

	p = ++end;

	/*
	 * p is now supposed to point to the beginning of the file last
	 * modification time.
	 */

	mtime = parse_uint64(p, &end, 10, &error);
	if (error || *end != '\t') {
		goto failure;
	}

	p = ++end;

	/* p is now supposed to point to the file name */

	if (strchr(p, '\t') != NULL)
		goto failure;

	add_volatile_cache_entry(p, size, mtime,
		&sha1, has_tth ? &tth : NULL, FALSE);
	return;

failure:
	g_warning("malformed line in SHA1 cache file: %s", line);
}

/**
 * Read the whole persistent cache into memory.
 */
static G_GNUC_COLD void
sha1_read_cache(void)
{
	FILE *f;
	file_path_t fp[1];
	bool truncated = FALSE;

	g_return_if_fail(settings_config_dir());

	file_path_set(fp, settings_config_dir(), "sha1_cache");
	f = file_config_open_read("SHA-1 cache", fp, G_N_ELEMENTS(fp));
	if (f) {
		for (;;) {
			char buffer[4096];

			if (NULL == fgets(buffer, sizeof buffer, f))
				break;

			if (!file_line_chomp_tail(buffer, sizeof buffer, NULL)) {
				truncated = TRUE;
			} else if (truncated) {
				truncated = FALSE;
			} else {
				parse_and_append_cache_entry(buffer);
			}
		}
		fclose(f);
		dump_cache(TRUE);
	}
}

static bool
huge_spam_check(shared_file_t *sf, const struct sha1 *sha1)
{
	if (NULL != sha1 && spam_sha1_check(sha1)) {
		g_warning("file \"%s\" is listed as spam (SHA1)", shared_file_path(sf));
		return TRUE;
	}

	if (
		spam_check_filename_size(shared_file_name_nfc(sf),
			shared_file_size(sf))
	) {
		g_warning("file \"%s\" is listed as spam (Name)", shared_file_path(sf));
		return TRUE;
	}
	return FALSE;
}

/**
 ** Asynchronous computation of hash value
 **/

bool
huge_update_hashes(shared_file_t *sf,
	const struct sha1 *sha1, const struct tth *tth)
{
	struct sha1_cache_entry *cached;
	filestat_t sb;

	shared_file_check(sf);
	g_return_val_if_fail(sha1, FALSE);

	/*
	 * Make sure the file's timestamp is still accurate.
	 */

	if (-1 == stat(shared_file_path(sf), &sb)) {
		g_warning("discarding SHA1 for file \"%s\": can't stat(): %m",
			shared_file_path(sf));
		shared_file_remove(sf);
		return TRUE;
	}

	if (sb.st_mtime != shared_file_modification_time(sf)) {
		g_warning("file \"%s\" was modified whilst SHA1 was computed",
			shared_file_path(sf));
		shared_file_set_modification_time(sf, sb.st_mtime);
		request_sha1(sf);					/* Retry! */
		return TRUE;
	}

	if (huge_spam_check(sf, sha1)) {
		shared_file_remove(sf);
		return FALSE;
	}

	shared_file_set_sha1(sf, sha1);
	shared_file_set_tth(sf, tth);

	/* Update cache */

	cached = hikset_lookup(sha1_cache, shared_file_path(sf));

	if (cached) {
		update_volatile_cache(cached, shared_file_size(sf),
			shared_file_modification_time(sf), sha1, tth);
		cache_dirty = TRUE;

		/* Dump the cache at most about once per minute. */
		if (!cache_dumped || delta_time(tm_time(), cache_dumped) > 60) {
			dump_cache(FALSE);
		}
	} else {
		add_volatile_cache_entry(shared_file_path(sf),
			shared_file_size(sf), shared_file_modification_time(sf),
			sha1, tth, TRUE);
		add_persistent_cache_entry(shared_file_path(sf),
			shared_file_size(sf), shared_file_modification_time(sf),
			sha1, tth);
	}
	return TRUE;
}

/**
 * Look whether we still need to compute the SHA1 of the given shared file
 * by looking into our in-core cache to see whether the entry we have is
 * up-to-date.
 *
 * @param sf	the shared file for which we want to compute the SHA1
 *
 * @return TRUE if the file need SHA1 recomputation.
 */
static bool
huge_need_sha1(shared_file_t *sf)
{
	struct sha1_cache_entry *cached;

	shared_file_check(sf);

	/*
	 * After a rescan, there might be files in the queue which are
	 * no longer shared.
	 */

	if (!shared_file_indexed(sf))
		return FALSE;

	if G_UNLIKELY(NULL == sha1_cache)
		return FALSE;		/* Shutdown occurred (processing TEQ event?) */

	cached = hikset_lookup(sha1_cache, shared_file_path(sf));

	if (cached != NULL) {
		filestat_t sb;

		if (-1 == stat(shared_file_path(sf), &sb)) {
			g_warning("ignoring SHA1 recomputation request for \"%s\": %m",
				shared_file_path(sf));
			return FALSE;
		}
		if (
			cached->size + (fileoffset_t) 0 == sb.st_size + (filesize_t) 0 &&
			cached->mtime == sb.st_mtime
		) {
			if (GNET_PROPERTY(share_debug) > 1) {
				g_warning("ignoring duplicate SHA1 work for \"%s\"",
					shared_file_path(sf));
			}
			return FALSE;
		}
	}
	return TRUE;
}

/**
 ** External interface
 **/

/* This is the external interface. During the share library building,
 * computation of SHA1 values for shared_file is repeatedly requested
 * through sha1_set_digest. If the value is found in the cache (and
 * the cache is up to date), it's set immediately. Otherwise, the file
 * is put in a queue for it's SHA1 digest to be computed.
 */

static bool
huge_verify_callback(const struct verify *ctx, enum verify_status status,
	void *user_data)
{
	shared_file_t *sf = user_data;

	shared_file_check(sf);
	switch (status) {
	case VERIFY_START:
		if (!huge_need_sha1(sf))
			return FALSE;
		gnet_prop_set_boolean_val(PROP_SHA1_REBUILDING, TRUE);
		return TRUE;
	case VERIFY_PROGRESS:
		return 0 != (SHARE_F_INDEXED & shared_file_flags(sf));
	case VERIFY_DONE:
		huge_update_hashes(sf, verify_sha1_digest(ctx), NULL);
		request_tigertree(sf, TRUE);
		/* FALL THROUGH */
	case VERIFY_ERROR:
	case VERIFY_SHUTDOWN:
		gnet_prop_set_boolean_val(PROP_SHA1_REBUILDING, FALSE);
		shared_file_unref(&sf);
		return TRUE;
	case VERIFY_INVALID:
		break;
	}
	g_assert_not_reached();
	return FALSE;
}

/**
 * Put the shared file on the stack of the things to do.
 *
 * We first begin with the computation of the SHA1, and when completed we
 * will continue with the TTH computation.
 */
static void
queue_shared_file_for_sha1_computation(shared_file_t *sf)
{
	int inserted;
	
 	shared_file_check(sf);

	inserted = verify_sha1_enqueue(FALSE, shared_file_path(sf),
					shared_file_size(sf), huge_verify_callback,
					shared_file_ref(sf));

	if (!inserted)
		shared_file_unref(&sf);
}

/**
 * Check to see if an (in-memory) entry cache is up to date.
 *
 * @return true (in the C sense) if it is, or false otherwise.
 */
static bool
cached_entry_up_to_date(const struct sha1_cache_entry *cache_entry,
	const shared_file_t *sf)
{
	return cache_entry->size == shared_file_size(sf)
		&& cache_entry->mtime == shared_file_modification_time(sf);
}

/**
 * External interface to check whether the sha1 for shared_file is known.
 */
bool
sha1_is_cached(const shared_file_t *sf)
{
	const struct sha1_cache_entry *cached;

	cached = hikset_lookup(sha1_cache, shared_file_path(sf));
	return cached && cached_entry_up_to_date(cached, sf);
}


/**
 * External interface to call for getting the hash for a shared_file.
 */
void
request_sha1(shared_file_t *sf)
{
	struct sha1_cache_entry *cached;

	shared_file_check(sf);

	if (!shared_file_indexed(sf))
		return;		/* "stale" shared file, has been superseded or removed */

	cached = hikset_lookup(sha1_cache, shared_file_path(sf));

	if (cached && cached_entry_up_to_date(cached, sf)) {
		cache_dirty = TRUE;
		cached->shared = TRUE;
		shared_file_set_sha1(sf, cached->sha1);
		shared_file_set_tth(sf, cached->tth);
		request_tigertree(sf, NULL == cached->tth);
	} else {

		if (GNET_PROPERTY(share_debug) > 1) {
			if (cached)
				g_debug("cached SHA1 entry for \"%s\" outdated: "
					"had mtime %lu, now %lu",
					shared_file_path(sf),
					(ulong) cached->mtime,
					(ulong) shared_file_modification_time(sf));
			else
				g_debug("queuing \"%s\" for SHA1 computation",
						shared_file_path(sf));
		}

		queue_shared_file_for_sha1_computation(sf);
	}
}

/**
 * Test whether the SHA1 in its base32/binary form is improbable.
 *
 * This is used to detect "urn:sha1:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" and
 * things using the same pattern with other letters, as being rather
 * improbable hashes.
 */
bool
huge_improbable_sha1(const char *buf, size_t len)
{
	size_t ilen = 0;			/* Length of the improbable sequence */
	size_t i, longest = 0;

	for (i = 1; i < len; i++) {
		uchar previous, c;
		
		previous = buf[i - 1];
		c = buf[i];

		if (c == previous || (c + 1 == previous) || (c - 1 == previous)) {
			ilen++;
		} else {
			longest = MAX(longest, ilen);
			ilen = 0;		/* Reset sequence, we broke out of the pattern */
		}
	}

	return (longest >= len / 2) ? TRUE : FALSE;
}

/**
 * Validate `len' bytes starting from `buf' as a proper base32 encoding
 * of a SHA1 hash, and write decoded value in `sha1'.
 * Also make sure that the SHA1 is not an improbable value.
 *
 * `n' is the node receiving the packet where we found the SHA1, so that we
 * may trace errors if needed.
 *
 * When `check_old' is true, check the encoding against an earlier version
 * of the base32 alphabet.
 *
 * @return TRUE if the SHA1 was valid and properly decoded, FALSE on error.
 */
bool
huge_sha1_extract32(const char *buf, size_t len, struct sha1 *sha1,
	const gnutella_node_t *n)
{
	if (len != SHA1_BASE32_SIZE || huge_improbable_sha1(buf, len))
		goto bad;

	if (SHA1_RAW_SIZE != base32_decode(sha1->data, sizeof sha1->data, buf, len))
		goto bad;

	/*
	 * Make sure the decoded value in `sha1' is "valid".
	 */

	if (huge_improbable_sha1(sha1->data, sizeof sha1->data)) {
		if (GNET_PROPERTY(share_debug)) {
			if (is_printable(buf, len)) {
				g_warning("%s has improbable SHA1 (len=%lu): %.*s, hex: %s",
					gmsg_node_infostr(n),
					(unsigned long) len,
					(int) MIN(len, (size_t) INT_MAX),
					buf, data_hex_str(sha1->data, sizeof sha1->data));
			} else
				goto bad;		/* SHA1 should be printable originally */
		}
		return FALSE;
	}

	return TRUE;

bad:
	if (GNET_PROPERTY(share_debug)) {
		if (is_printable(buf, len)) {
			g_warning("%s has bad SHA1 (len=%u): %.*s",
				gmsg_node_infostr(n),
				(unsigned) len,
				(int) MIN(len, (size_t) INT_MAX),
				buf);
		} else {
			g_warning("%s has bad SHA1 (len=%u)",
				gmsg_node_infostr(n), (unsigned) len);
			if (len)
				dump_hex(stderr, "Base32 SHA1", buf, len);
		}
	}

	return FALSE;
}

bool
huge_tth_extract32(const char *buf, size_t len, struct tth *tth,
	const gnutella_node_t *n)
{
	if (len != TTH_BASE32_SIZE)
		goto bad;

	if (TTH_RAW_SIZE != base32_decode(tth->data, sizeof tth->data, buf, len))
		goto bad;

	return TRUE;

bad:
	if (GNET_PROPERTY(share_debug)) {
		if (is_printable(buf, len)) {
			g_warning("%s has bad TTH (len=%u): %.*s",
				gmsg_node_infostr(n),
				(unsigned) len,
				(int) MIN(len, (size_t) INT_MAX),
				buf);
		} else {
			g_warning("%s has bad TTH (len=%u",
				gmsg_node_infostr(n), (unsigned) len);
			if (len)
				dump_hex(stderr, "Base32 TTH", buf, len);
		}
	}
	return FALSE;
}

/**
 * Is the X-Alt header really holding a collection of IP:port (with possible
 * push alt-locs containing a prefixing GUID) or is it mistakenly called
 * X-Alt but is really an old X-Gnutella-Alternate-Location containing a
 * list of HTTP URLs.
 */
static bool
huge_is_pure_xalt(const char *value, size_t len)
{
	host_addr_t addr;

	/*
	 * This is pure heuristics, knowing that if we return TRUE, we'll parse
	 * the X-Alt header in the format that is emitted by the majority of
	 * vendors.
	 *
	 * We try to avoid the more costly pattern_qsearch() call if we can,
	 * and our heuristic should catch 99% of the cases.  And pattern_qsearch()
	 * should quickly return true if the X-Alt is not a collection of IP:port.
	 */

	if (is_strcaseprefix(value, "tls="))
		return TRUE;

	if (string_to_host_addr(value, NULL, &addr))
		return TRUE;

	if (pattern_qsearch(has_http_urls, value, len, 0, qs_any))
		return FALSE;

	return TRUE;
}

/**
 * Parse the "X-Gnutella-Alternate-Location" header if present to learn
 * about other sources for this file.
 *
 * Also knows about "Alternate-Location", "Alt-Location", "X-Alt" and "X-Falt".
 *
 * @param sha1		the SHA1 for which we're parsing alt-locs
 * @param header	the headers supplied by the remote host
 * @param origin	if non-NULL, this is the host supplying the alt-locs
 */
void
huge_collect_locations(const sha1_t *sha1, const header_t *header,
	const gnet_host_t * origin)
{
	char *alt;
	size_t len;
	const char *user_agent;

	g_return_if_fail(sha1);
	g_return_if_fail(header);

	/*
	 * This code can be invoked on the download path (when we analyse
	 * locations sent by the server) or on the upload path (when we
	 * analyze those sent by the user to whom we are uploading).
	 *
	 * Therefore the user agent name can be held in the Server or
	 * User-Agent header, depending.
	 */

	user_agent = header_get(header, "User-Agent");	/* Uploading */

	if (NULL == user_agent)
		user_agent = header_get(header, "Server");	/* Downloading */

	alt = header_get(header, "X-Gnutella-Alternate-Location");

	/*
	 * Unfortunately, clueless people broke the HUGE specs and made up their
	 * own headers.  They should learn about header continuations, and
	 * that "X-Gnutella-Alternate-Location" does not need to be repeated.
	 */

	if (alt == NULL)
		alt = header_get(header, "Alternate-Location");
	if (alt == NULL)
		alt = header_get(header, "Alt-Location");

	if (alt != NULL) {
		dmesh_collect_locations(sha1, alt, origin, user_agent);
		return;
	}

	alt = header_get_extended(header, "X-Alt", &len);

	if (alt != NULL) {
		/*
		 * Wonderful Shareaza now uses X-Alt but does not pass compact
		 * locations.  In essence, they renamed Alt-Location to X-Alt
		 * without changing the format of the value.  Great job.
		 *		--RAM, 2010-02-22
		 */

		if (huge_is_pure_xalt(alt, len))
			dmesh_collect_compact_locations(sha1, alt, origin);
		else
			dmesh_collect_locations(sha1, alt, origin, user_agent);
    }

	/*
	 * Firewalled locations.
	 */

	alt = header_get(header, "X-Falt");

	if (alt != NULL) {
		dmesh_collect_fw_hosts(sha1, alt);
	}
}

/**
 * Initialize the HUGE layer.
 */
void
huge_init(void)
{
	sha1_cache = hikset_create(		/* Keys are atoms */
		offsetof(struct sha1_cache_entry, file_name), HASH_KEY_SELF, 0);
	sha1_read_cache();
	has_http_urls = pattern_compile("http://");
}

/**
 * Free SHA1 cache entry.
 */
static void
cache_free_entry(void *v, void *unused_udata)
{
	struct sha1_cache_entry *e = v;

	(void) unused_udata;

	atom_str_free_null(&e->file_name);
	atom_sha1_free_null(&e->sha1);
	atom_tth_free_null(&e->tth);
	WFREE(e);
}

/**
 * Called when servent is shutdown.
 */
void
huge_close(void)
{
	dump_cache(FALSE);

	hikset_foreach(sha1_cache, cache_free_entry, NULL);
	hikset_free_null(&sha1_cache);

	pattern_free(has_http_urls);
	has_http_urls = NULL;
}

/*
 * Emacs stuff:
 * Local Variables: ***
 * c-indentation-style: "bsd" ***
 * fill-column: 80 ***
 * tab-width: 4 ***
 * indent-tabs-mode: nil ***
 * End: ***
 * vi: set ts=4 sw=4 cindent:
 */
