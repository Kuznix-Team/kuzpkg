/*
 *  db.h
 *
 *  Copyright (C) 2026 Kuznix
 *  Copyright (c) 2002-2006 by Judd Vinet <jvinet@zeroflux.org>
 *  Copyright (c) 2005 by Aurelien Foret <orelien@chez.com>
 *  Copyright (c) 2006 by Miklos Vajna <vmiklos@frugalware.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef KUZPKG_DB_H
#define KUZPKG_DB_H

/* libarchive */
#include <archive.h>
#include <archive_entry.h>

#include "klpm.h"
#include "pkghash.h"
#include "signing.h"

/* Database entries */
typedef enum _klpm_dbinfrq_t {
	INFRQ_BASE = (1 << 0),
	INFRQ_DESC = (1 << 1),
	INFRQ_FILES = (1 << 2),
	INFRQ_SCRIPTLET = (1 << 3),
	INFRQ_DSIZE = (1 << 4),
	/* ALL should be info stored in the package or database */
	INFRQ_ALL = INFRQ_BASE | INFRQ_DESC | INFRQ_FILES |
		INFRQ_SCRIPTLET | INFRQ_DSIZE,
	INFRQ_ERROR = (1 << 30)
} klpm_dbinfrq_t;

/** Database status. Bitflags. */
enum _klpm_dbstatus_t {
	DB_STATUS_VALID = (1 << 0),
	DB_STATUS_INVALID = (1 << 1),
	DB_STATUS_EXISTS = (1 << 2),
	DB_STATUS_MISSING = (1 << 3),

	DB_STATUS_LOCAL = (1 << 10),
	DB_STATUS_PKGCACHE = (1 << 11),
	DB_STATUS_GRPCACHE = (1 << 12)
};

struct db_operations {
	int (*validate) (klpm_db_t *);
	int (*populate) (klpm_db_t *);
	void (*unregister) (klpm_db_t *);
};

/* Database */
struct _klpm_db_t {
	klpm_handle_t *handle;
	char *treename;
	/* do not access directly, use _klpm_db_path(db) for lazy access */
	char *_path;
	klpm_pkghash_t *pkgcache;
	klpm_list_t *grpcache;
	klpm_list_t *cache_servers;
	klpm_list_t *servers;
	const struct db_operations *ops;

	/* bitfields for validity, local, loaded caches, etc. */
	/* From _klpm_dbstatus_t */
	int status;
	/* klpm_siglevel_t */
	int siglevel;
	/* klpm_db_usage_t */
	int usage;
};


/* db.c, database general calls */
klpm_db_t *_klpm_db_new(const char *treename, int is_local);
void _klpm_db_free(klpm_db_t *db);
const char *_klpm_db_path(klpm_db_t *db);
int _klpm_db_cmp(const void *d1, const void *d2);
int _klpm_db_search(klpm_db_t *db, const klpm_list_t *needles,
		klpm_list_t **ret);
klpm_db_t *_klpm_db_register_local(klpm_handle_t *handle);
klpm_db_t *_klpm_db_register_sync(klpm_handle_t *handle, const char *treename,
		int level);
void _klpm_db_unregister(klpm_db_t *db);

/* be_*.c, backend specific calls */
int _klpm_local_db_prepare(klpm_db_t *db, klpm_pkg_t *info);
int _klpm_local_db_write(klpm_db_t *db, klpm_pkg_t *info, int inforeq);
int _klpm_local_db_remove(klpm_db_t *db, klpm_pkg_t *info);
char *_klpm_local_db_pkgpath(klpm_db_t *db, klpm_pkg_t *info, const char *filename);

/* cache bullshit */
/* packages */
void _klpm_db_free_pkgcache(klpm_db_t *db);
int _klpm_db_add_pkgincache(klpm_db_t *db, klpm_pkg_t *pkg);
int _klpm_db_remove_pkgfromcache(klpm_db_t *db, klpm_pkg_t *pkg);
klpm_pkghash_t *_klpm_db_get_pkgcache_hash(klpm_db_t *db);
klpm_list_t *_klpm_db_get_pkgcache(klpm_db_t *db);
klpm_pkg_t *_klpm_db_get_pkgfromcache(klpm_db_t *db, const char *target);
/* groups */
klpm_list_t *_klpm_db_get_groupcache(klpm_db_t *db);
klpm_group_t *_klpm_db_get_groupfromcache(klpm_db_t *db, const char *target);

#endif /* KUZPKG_DB_H */
