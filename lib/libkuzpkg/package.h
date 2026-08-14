/*
 *  package.h
 *
 *  Copyright (C) 2026 Kuznix
 *  Copyright (c) 2002-2006 by Judd Vinet <jvinet@zeroflux.org>
 *  Copyright (c) 2005 by Aurelien Foret <orelien@chez.com>
 *  Copyright (c) 2006 by David Kimpe <dnaku@frugalware.org>
 *  Copyright (c) 2005, 2006 by Christian Hamar <krics@linuxforum.hu>
 *  Copyright (c) 2005, 2006 by Miklos Vajna <vmiklos@frugalware.org>
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
#ifndef KUZPKG_PACKAGE_H
#define KUZPKG_PACKAGE_H

#include <sys/types.h> /* off_t */

/* libarchive */
#include <archive.h>
#include <archive_entry.h>

#include "klpm.h"
#include "backup.h"
#include "db.h"
#include "signing.h"

/** Package operations struct. This struct contains function pointers to
 * all methods used to access data in a package to allow for things such
 * as lazy package initialization (such as used by the file backend). Each
 * backend is free to define a struct containing pointers to a specific
 * implementation of these methods. Some backends may find using the
 * defined default_pkg_ops struct to work just fine for their needs.
 */
struct pkg_operations {
	const char *(*get_base) (klpm_pkg_t *);
	const char *(*get_desc) (klpm_pkg_t *);
	const char *(*get_url) (klpm_pkg_t *);
	klpm_time_t (*get_builddate) (klpm_pkg_t *);
	klpm_time_t (*get_installdate) (klpm_pkg_t *);
	const char *(*get_packager) (klpm_pkg_t *);
	const char *(*get_arch) (klpm_pkg_t *);
	off_t (*get_isize) (klpm_pkg_t *);
	klpm_pkgreason_t (*get_reason) (klpm_pkg_t *);
	int (*get_validation) (klpm_pkg_t *);
	int (*has_scriptlet) (klpm_pkg_t *);

	klpm_list_t *(*get_licenses) (klpm_pkg_t *);
	klpm_list_t *(*get_groups) (klpm_pkg_t *);
	klpm_list_t *(*get_depends) (klpm_pkg_t *);
	klpm_list_t *(*get_optdepends) (klpm_pkg_t *);
	klpm_list_t *(*get_checkdepends) (klpm_pkg_t *);
	klpm_list_t *(*get_makedepends) (klpm_pkg_t *);
	klpm_list_t *(*get_conflicts) (klpm_pkg_t *);
	klpm_list_t *(*get_provides) (klpm_pkg_t *);
	klpm_list_t *(*get_replaces) (klpm_pkg_t *);
	klpm_filelist_t *(*get_files) (klpm_pkg_t *);
	klpm_list_t *(*get_backup) (klpm_pkg_t *);

	klpm_list_t *(*get_xdata) (klpm_pkg_t *);

	void *(*changelog_open) (klpm_pkg_t *);
	size_t (*changelog_read) (void *, size_t, const klpm_pkg_t *, void *);
	int (*changelog_close) (const klpm_pkg_t *, void *);

	struct archive *(*mtree_open) (klpm_pkg_t *);
	int (*mtree_next) (const klpm_pkg_t *, struct archive *, struct archive_entry **);
	int (*mtree_close) (const klpm_pkg_t *, struct archive *);

	int (*force_load) (klpm_pkg_t *);
};

/** The standard package operations struct. get fields directly from the
 * struct itself with no abstraction layer or any type of lazy loading.
 * The actual definition is in package.c so it can have access to the
 * default accessor functions which are defined there.
 */
extern const struct pkg_operations default_pkg_ops;

struct _klpm_pkg_t {
	unsigned long name_hash;
	char *filename;
	char *base;
	char *name;
	char *version;
	char *desc;
	char *url;
	char *packager;
	char *sha256sum;
	char *base64_sig;
	char *arch;

	klpm_time_t builddate;
	klpm_time_t installdate;

	off_t size;
	off_t isize;
	off_t download_size;

	klpm_handle_t *handle;

	klpm_list_t *licenses;
	klpm_list_t *replaces;
	klpm_list_t *groups;
	klpm_list_t *backup;
	klpm_list_t *depends;
	klpm_list_t *optdepends;
	klpm_list_t *checkdepends;
	klpm_list_t *makedepends;
	klpm_list_t *conflicts;
	klpm_list_t *provides;
	klpm_list_t *removes; /* in transaction targets only */
	klpm_pkg_t *oldpkg; /* in transaction targets only */

	const struct pkg_operations *ops;

	klpm_filelist_t files;

	/* origin == PKG_FROM_FILE, use pkg->origin_data.file
	 * origin == PKG_FROM_*DB, use pkg->origin_data.db */
	union {
		klpm_db_t *db;
		char *file;
	} origin_data;

	klpm_pkgfrom_t origin;
	klpm_pkgreason_t reason;
	int scriptlet;

	klpm_list_t *xdata;

	/* Bitfield from klpm_dbinfrq_t */
	int infolevel;
	/* Bitfield from klpm_pkgvalidation_t */
	int validation;
};

klpm_file_t *_klpm_file_copy(klpm_file_t *dest, const klpm_file_t *src);

klpm_pkg_t *_klpm_pkg_new(void);
int _klpm_pkg_dup(klpm_pkg_t *pkg, klpm_pkg_t **new_ptr);
void _klpm_pkg_free(klpm_pkg_t *pkg);
void _klpm_pkg_free_trans(klpm_pkg_t *pkg);

int _klpm_pkg_validate_internal(klpm_handle_t *handle,
		const char *pkgfile, klpm_pkg_t *syncpkg, int level,
		klpm_siglist_t **sigdata, int *validation);
klpm_pkg_t *_klpm_pkg_load_internal(klpm_handle_t *handle,
		const char *pkgfile, int full);

int _klpm_pkg_cmp(const void *p1, const void *p2);
int _klpm_pkg_compare_versions(klpm_pkg_t *local_pkg, klpm_pkg_t *pkg);

klpm_pkg_xdata_t *_klpm_pkg_parse_xdata(const char *string);
void _klpm_pkg_xdata_free(klpm_pkg_xdata_t *pd);

int _klpm_pkg_check_meta(klpm_pkg_t *pkg);

#endif /* KUZPKG_PACKAGE_H */
