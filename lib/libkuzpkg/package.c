/*
 *  package.c
 *
 *  Copyright (C) 2026 Kuznix
 *  Copyright (c) 2002-2006 by Judd Vinet <jvinet@zeroflux.org>
 *  Copyright (c) 2005 by Aurelien Foret <orelien@chez.com>
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

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

/* libkuzpkg */
#include "package.h"
#include "klpm_list.h"
#include "log.h"
#include "util.h"
#include "db.h"
#include "handle.h"
#include "deps.h"

int SYMEXPORT klpm_pkg_free(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return -1);

	/* Only free packages loaded in user space */
	if(pkg->origin == KUZPKG_PKG_FROM_FILE) {
		_klpm_pkg_free(pkg);
	}

	return 0;
}

/* Default package accessor functions. These will get overridden by any
 * backend logic that needs lazy access, such as the local database through
 * a lazy-load cache. However, the defaults will work just fine for fully-
 * populated package structures. */
static const char *_pkg_get_base(klpm_pkg_t *pkg)        { return pkg->base; }
static const char *_pkg_get_desc(klpm_pkg_t *pkg)        { return pkg->desc; }
static const char *_pkg_get_url(klpm_pkg_t *pkg)         { return pkg->url; }
static klpm_time_t _pkg_get_builddate(klpm_pkg_t *pkg)   { return pkg->builddate; }
static klpm_time_t _pkg_get_installdate(klpm_pkg_t *pkg) { return pkg->installdate; }
static const char *_pkg_get_packager(klpm_pkg_t *pkg)    { return pkg->packager; }
static const char *_pkg_get_arch(klpm_pkg_t *pkg)        { return pkg->arch; }
static off_t _pkg_get_isize(klpm_pkg_t *pkg)             { return pkg->isize; }
static klpm_pkgreason_t _pkg_get_reason(klpm_pkg_t *pkg) { return pkg->reason; }
static int _pkg_get_validation(klpm_pkg_t *pkg) { return pkg->validation; }
static int _pkg_has_scriptlet(klpm_pkg_t *pkg)           { return pkg->scriptlet; }

static klpm_list_t *_pkg_get_licenses(klpm_pkg_t *pkg)   { return pkg->licenses; }
static klpm_list_t *_pkg_get_groups(klpm_pkg_t *pkg)     { return pkg->groups; }
static klpm_list_t *_pkg_get_depends(klpm_pkg_t *pkg)    { return pkg->depends; }
static klpm_list_t *_pkg_get_optdepends(klpm_pkg_t *pkg) { return pkg->optdepends; }
static klpm_list_t *_pkg_get_checkdepends(klpm_pkg_t *pkg) { return pkg->checkdepends; }
static klpm_list_t *_pkg_get_makedepends(klpm_pkg_t *pkg) { return pkg->makedepends; }
static klpm_list_t *_pkg_get_conflicts(klpm_pkg_t *pkg)  { return pkg->conflicts; }
static klpm_list_t *_pkg_get_provides(klpm_pkg_t *pkg)   { return pkg->provides; }
static klpm_list_t *_pkg_get_replaces(klpm_pkg_t *pkg)   { return pkg->replaces; }
static klpm_filelist_t *_pkg_get_files(klpm_pkg_t *pkg)  { return &(pkg->files); }
static klpm_list_t *_pkg_get_backup(klpm_pkg_t *pkg)     { return pkg->backup; }
static klpm_list_t *_pkg_get_xdata(klpm_pkg_t *pkg)      { return pkg->xdata; }

static void *_pkg_changelog_open(klpm_pkg_t UNUSED *pkg)
{
	return NULL;
}

static size_t _pkg_changelog_read(void UNUSED *ptr, size_t UNUSED size,
		const klpm_pkg_t UNUSED *pkg, UNUSED void *fp)
{
	return 0;
}

static int _pkg_changelog_close(const klpm_pkg_t UNUSED *pkg,
		void UNUSED *fp)
{
	return EOF;
}

static struct archive *_pkg_mtree_open(klpm_pkg_t UNUSED *pkg)
{
	return NULL;
}

static int _pkg_mtree_next(const klpm_pkg_t UNUSED *pkg,
		struct archive UNUSED *archive, struct archive_entry UNUSED **entry)
{
	return -1;
}

static int _pkg_mtree_close(const klpm_pkg_t UNUSED *pkg,
		struct archive UNUSED *archive)
{
	return -1;
}

static int _pkg_force_load(klpm_pkg_t UNUSED *pkg) { return 0; }

/** The standard package operations struct. Get fields directly from the
 * struct itself with no abstraction layer or any type of lazy loading.
 */
const struct pkg_operations default_pkg_ops = {
	.get_base        = _pkg_get_base,
	.get_desc        = _pkg_get_desc,
	.get_url         = _pkg_get_url,
	.get_builddate   = _pkg_get_builddate,
	.get_installdate = _pkg_get_installdate,
	.get_packager    = _pkg_get_packager,
	.get_arch        = _pkg_get_arch,
	.get_isize       = _pkg_get_isize,
	.get_reason      = _pkg_get_reason,
	.get_validation  = _pkg_get_validation,
	.has_scriptlet   = _pkg_has_scriptlet,

	.get_licenses    = _pkg_get_licenses,
	.get_groups      = _pkg_get_groups,
	.get_depends     = _pkg_get_depends,
	.get_optdepends  = _pkg_get_optdepends,
	.get_checkdepends = _pkg_get_checkdepends,
	.get_makedepends = _pkg_get_makedepends,
	.get_conflicts   = _pkg_get_conflicts,
	.get_provides    = _pkg_get_provides,
	.get_replaces    = _pkg_get_replaces,
	.get_files       = _pkg_get_files,
	.get_backup      = _pkg_get_backup,
	.get_xdata       = _pkg_get_xdata,

	.changelog_open  = _pkg_changelog_open,
	.changelog_read  = _pkg_changelog_read,
	.changelog_close = _pkg_changelog_close,

	.mtree_open      = _pkg_mtree_open,
	.mtree_next      = _pkg_mtree_next,
	.mtree_close     = _pkg_mtree_close,

	.force_load      = _pkg_force_load,
};

/* Public functions for getting package information. These functions
 * delegate the hard work to the function callbacks attached to each
 * package, which depend on where the package was loaded from. */
const char SYMEXPORT *klpm_pkg_get_filename(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return NULL);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->filename;
}

const char SYMEXPORT *klpm_pkg_get_base(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return NULL);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->ops->get_base(pkg);
}

klpm_handle_t SYMEXPORT *klpm_pkg_get_handle(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return NULL);
	return pkg->handle;
}

const char SYMEXPORT *klpm_pkg_get_name(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return NULL);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->name;
}

const char SYMEXPORT *klpm_pkg_get_version(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return NULL);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->version;
}

klpm_pkgfrom_t SYMEXPORT klpm_pkg_get_origin(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return -1);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->origin;
}

const char SYMEXPORT *klpm_pkg_get_desc(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return NULL);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->ops->get_desc(pkg);
}

const char SYMEXPORT *klpm_pkg_get_url(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return NULL);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->ops->get_url(pkg);
}

klpm_time_t SYMEXPORT klpm_pkg_get_builddate(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return -1);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->ops->get_builddate(pkg);
}

klpm_time_t SYMEXPORT klpm_pkg_get_installdate(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return -1);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->ops->get_installdate(pkg);
}

const char SYMEXPORT *klpm_pkg_get_packager(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return NULL);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->ops->get_packager(pkg);
}

const char SYMEXPORT *klpm_pkg_get_sha256sum(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return NULL);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->sha256sum;
}

const char SYMEXPORT *klpm_pkg_get_base64_sig(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return NULL);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->base64_sig;
}

int SYMEXPORT klpm_pkg_get_sig(klpm_pkg_t *pkg, unsigned char **sig, size_t *sig_len)
{
	ASSERT(pkg != NULL, return -1);

	if(pkg->base64_sig) {
		int ret = klpm_decode_signature(pkg->base64_sig, sig, sig_len);
		if(ret != 0) {
			RET_ERR(pkg->handle, KUZPKG_ERR_SIG_INVALID, -1);
		}
		return 0;
	} else {
		char *pkgpath = NULL, *sigpath = NULL;
		klpm_errno_t err;
		int ret = -1;

		pkgpath = _klpm_filecache_find(pkg->handle, pkg->filename);
		if(!pkgpath) {
			GOTO_ERR(pkg->handle, KUZPKG_ERR_PKG_NOT_FOUND, cleanup);
		}
		sigpath = _klpm_sigpath(pkg->handle, pkgpath);
		if(!sigpath || _klpm_access(pkg->handle, NULL, sigpath, R_OK)) {
			GOTO_ERR(pkg->handle, KUZPKG_ERR_SIG_MISSING, cleanup);
		}
		err = _klpm_read_file(sigpath, sig, sig_len);
		if(err == KUZPKG_ERR_OK) {
			_klpm_log(pkg->handle, KUZPKG_LOG_DEBUG, "found detached signature %s with size %ld\n",
				sigpath, *sig_len);
		} else {
			GOTO_ERR(pkg->handle, err, cleanup);
		}
		ret = 0;
cleanup:
		FREE(pkgpath);
		FREE(sigpath);
		return ret;
	}
}

const char SYMEXPORT *klpm_pkg_get_arch(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return NULL);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->ops->get_arch(pkg);
}

off_t SYMEXPORT klpm_pkg_get_size(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return -1);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->size;
}

off_t SYMEXPORT klpm_pkg_get_isize(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return -1);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->ops->get_isize(pkg);
}

klpm_pkgreason_t SYMEXPORT klpm_pkg_get_reason(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return -1);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->ops->get_reason(pkg);
}

int SYMEXPORT klpm_pkg_get_validation(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return -1);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->ops->get_validation(pkg);
}

klpm_list_t SYMEXPORT *klpm_pkg_get_licenses(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return NULL);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->ops->get_licenses(pkg);
}

klpm_list_t SYMEXPORT *klpm_pkg_get_groups(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return NULL);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->ops->get_groups(pkg);
}

klpm_list_t SYMEXPORT *klpm_pkg_get_depends(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return NULL);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->ops->get_depends(pkg);
}

klpm_list_t SYMEXPORT *klpm_pkg_get_optdepends(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return NULL);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->ops->get_optdepends(pkg);
}

klpm_list_t SYMEXPORT *klpm_pkg_get_checkdepends(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return NULL);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->ops->get_checkdepends(pkg);
}

klpm_list_t SYMEXPORT *klpm_pkg_get_makedepends(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return NULL);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->ops->get_makedepends(pkg);
}

klpm_list_t SYMEXPORT *klpm_pkg_get_conflicts(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return NULL);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->ops->get_conflicts(pkg);
}

klpm_list_t SYMEXPORT *klpm_pkg_get_provides(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return NULL);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->ops->get_provides(pkg);
}

klpm_list_t SYMEXPORT *klpm_pkg_get_replaces(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return NULL);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->ops->get_replaces(pkg);
}

klpm_filelist_t SYMEXPORT *klpm_pkg_get_files(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return NULL);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->ops->get_files(pkg);
}

klpm_list_t SYMEXPORT *klpm_pkg_get_backup(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return NULL);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->ops->get_backup(pkg);
}

klpm_db_t SYMEXPORT *klpm_pkg_get_db(klpm_pkg_t *pkg)
{
	/* Sanity checks */
	ASSERT(pkg != NULL, return NULL);
	ASSERT(pkg->origin != KUZPKG_PKG_FROM_FILE, return NULL);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;

	return pkg->origin_data.db;
}

void SYMEXPORT *klpm_pkg_changelog_open(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return NULL);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->ops->changelog_open(pkg);
}

size_t SYMEXPORT klpm_pkg_changelog_read(void *ptr, size_t size,
		const klpm_pkg_t *pkg, void *fp)
{
	ASSERT(pkg != NULL, return 0);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->ops->changelog_read(ptr, size, pkg, fp);
}

int SYMEXPORT klpm_pkg_changelog_close(const klpm_pkg_t *pkg, void *fp)
{
	ASSERT(pkg != NULL, return -1);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->ops->changelog_close(pkg, fp);
}

struct archive SYMEXPORT *klpm_pkg_mtree_open(klpm_pkg_t * pkg)
{
	ASSERT(pkg != NULL, return NULL);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->ops->mtree_open(pkg);
}

int SYMEXPORT klpm_pkg_mtree_next(const klpm_pkg_t * pkg, struct archive *archive,
	struct archive_entry **entry)
{
	ASSERT(pkg != NULL, return -1);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->ops->mtree_next(pkg, archive, entry);
}

int SYMEXPORT klpm_pkg_mtree_close(const klpm_pkg_t * pkg, struct archive *archive)
{
	ASSERT(pkg != NULL, return -1);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->ops->mtree_close(pkg, archive);
}

int SYMEXPORT klpm_pkg_has_scriptlet(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return -1);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->ops->has_scriptlet(pkg);
}

klpm_list_t SYMEXPORT *klpm_pkg_get_xdata(klpm_pkg_t *pkg)
{
	ASSERT(pkg != NULL, return NULL);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;
	return pkg->ops->get_xdata(pkg);
}

static void find_requiredby(klpm_pkg_t *pkg, klpm_db_t *db, klpm_list_t **reqs,
		int optional)
{
	const klpm_list_t *i;
	pkg->handle->pm_errno = KUZPKG_ERR_OK;

	for(i = _klpm_db_get_pkgcache(db); i; i = i->next) {
		klpm_pkg_t *cachepkg = i->data;
		klpm_list_t *j;

		if(optional == 0) {
			j = klpm_pkg_get_depends(cachepkg);
		} else {
			j = klpm_pkg_get_optdepends(cachepkg);
		}

		for(; j; j = j->next) {
			if(_klpm_depcmp(pkg, j->data)) {
				const char *cachepkgname = cachepkg->name;
				if(klpm_list_find_str(*reqs, cachepkgname) == NULL) {
					*reqs = klpm_list_add(*reqs, strdup(cachepkgname));
				}
			}
		}
	}
}

static klpm_list_t *compute_requiredby(klpm_pkg_t *pkg, int optional)
{
	const klpm_list_t *i;
	klpm_list_t *reqs = NULL;
	klpm_db_t *db;

	ASSERT(pkg != NULL, return NULL);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;

	if(pkg->origin == KUZPKG_PKG_FROM_FILE) {
		/* The sane option; search locally for things that require this. */
		find_requiredby(pkg, pkg->handle->db_local, &reqs, optional);
	} else {
		/* We have a DB package. if it is a local package, then we should
		 * only search the local DB; else search all known sync databases. */
		db = pkg->origin_data.db;
		if(db->status & DB_STATUS_LOCAL) {
			find_requiredby(pkg, db, &reqs, optional);
		} else {
			for(i = pkg->handle->dbs_sync; i; i = i->next) {
				db = i->data;
				find_requiredby(pkg, db, &reqs, optional);
			}
			reqs = klpm_list_msort(reqs, klpm_list_count(reqs), _klpm_str_cmp);
		}
	}
	return reqs;
}

klpm_list_t SYMEXPORT *klpm_pkg_compute_requiredby(klpm_pkg_t *pkg)
{
	return compute_requiredby(pkg, 0);
}

klpm_list_t SYMEXPORT *klpm_pkg_compute_optionalfor(klpm_pkg_t *pkg)
{
	return compute_requiredby(pkg, 1);
}

klpm_file_t *_klpm_file_copy(klpm_file_t *dest,
		const klpm_file_t *src)
{
	STRDUP(dest->name, src->name, return NULL);
	dest->size = src->size;
	dest->mode = src->mode;

	return dest;
}

klpm_pkg_t *_klpm_pkg_new(void)
{
	klpm_pkg_t *pkg;

	CALLOC(pkg, 1, sizeof(klpm_pkg_t), return NULL);

	return pkg;
}

static klpm_list_t *list_depdup(klpm_list_t *old)
{
	klpm_list_t *i, *new = NULL;
	for(i = old; i; i = i->next) {
		new = klpm_list_add(new, _klpm_dep_dup(i->data));
	}
	return new;
}

/**
 * Duplicate a package data struct.
 * @param pkg the package to duplicate
 * @param new_ptr location to store duplicated package pointer
 * @return 0 on success, -1 on fatal error, 1 on non-fatal error
 */
int _klpm_pkg_dup(klpm_pkg_t *pkg, klpm_pkg_t **new_ptr)
{
	klpm_pkg_t *newpkg;
	klpm_list_t *i;
	int ret = 0;

	if(!pkg || !pkg->handle) {
		return -1;
	}

	if(!new_ptr) {
		RET_ERR(pkg->handle, KUZPKG_ERR_WRONG_ARGS, -1);
	}

	if(pkg->ops->force_load(pkg)) {
		_klpm_log(pkg->handle, KUZPKG_LOG_WARNING,
				_("could not fully load metadata for package %s-%s\n"),
				pkg->name, pkg->version);
		ret = 1;
		pkg->handle->pm_errno = KUZPKG_ERR_PKG_INVALID;
	}

	CALLOC(newpkg, 1, sizeof(klpm_pkg_t), goto cleanup);

	newpkg->name_hash = pkg->name_hash;
	STRDUP(newpkg->filename, pkg->filename, goto cleanup);
	STRDUP(newpkg->base, pkg->base, goto cleanup);
	STRDUP(newpkg->name, pkg->name, goto cleanup);
	STRDUP(newpkg->version, pkg->version, goto cleanup);
	STRDUP(newpkg->desc, pkg->desc, goto cleanup);
	STRDUP(newpkg->url, pkg->url, goto cleanup);
	newpkg->builddate = pkg->builddate;
	newpkg->installdate = pkg->installdate;
	STRDUP(newpkg->packager, pkg->packager, goto cleanup);
	STRDUP(newpkg->sha256sum, pkg->sha256sum, goto cleanup);
	STRDUP(newpkg->arch, pkg->arch, goto cleanup);
	newpkg->size = pkg->size;
	newpkg->isize = pkg->isize;
	newpkg->scriptlet = pkg->scriptlet;
	newpkg->reason = pkg->reason;
	newpkg->validation = pkg->validation;

	newpkg->licenses   = klpm_list_strdup(pkg->licenses);
	newpkg->replaces   = list_depdup(pkg->replaces);
	newpkg->groups     = klpm_list_strdup(pkg->groups);
	for(i = pkg->backup; i; i = i->next) {
		newpkg->backup = klpm_list_add(newpkg->backup, _klpm_backup_dup(i->data));
	}
	newpkg->depends    = list_depdup(pkg->depends);
	newpkg->optdepends = list_depdup(pkg->optdepends);
	newpkg->conflicts  = list_depdup(pkg->conflicts);
	newpkg->provides   = list_depdup(pkg->provides);

	if(pkg->files.count) {
		size_t filenum;
		size_t len = sizeof(klpm_file_t) * pkg->files.count;
		MALLOC(newpkg->files.files, len, goto cleanup);
		for(filenum = 0; filenum < pkg->files.count; filenum++) {
			if(!_klpm_file_copy(newpkg->files.files + filenum,
						pkg->files.files + filenum)) {
				goto cleanup;
			}
		}
		newpkg->files.count = pkg->files.count;
	}

	/* internal */
	newpkg->infolevel = pkg->infolevel;
	newpkg->origin = pkg->origin;
	if(newpkg->origin == KUZPKG_PKG_FROM_FILE) {
		STRDUP(newpkg->origin_data.file, pkg->origin_data.file, goto cleanup);
	} else {
		newpkg->origin_data.db = pkg->origin_data.db;
	}
	newpkg->ops = pkg->ops;
	newpkg->handle = pkg->handle;

	*new_ptr = newpkg;
	return ret;

cleanup:
	_klpm_pkg_free(newpkg);
	RET_ERR(pkg->handle, KUZPKG_ERR_MEMORY, -1);
}

static void free_deplist(klpm_list_t *deps)
{
	klpm_list_free_inner(deps, (klpm_list_fn_free)klpm_dep_free);
	klpm_list_free(deps);
}

klpm_pkg_xdata_t *_klpm_pkg_parse_xdata(const char *string)
{
	klpm_pkg_xdata_t *pd;
	const char *sep;
	if(string == NULL || (sep = strchr(string, '=')) == NULL) {
		return NULL;
	}

	CALLOC(pd, 1, sizeof(klpm_pkg_xdata_t), return NULL);
	STRNDUP(pd->name, string, sep - string, FREE(pd); return NULL);
	STRDUP(pd->value, sep + 1, FREE(pd->name); FREE(pd); return NULL);

	return pd;
}

void _klpm_pkg_xdata_free(klpm_pkg_xdata_t *pd)
{
	if(pd) {
		free(pd->name);
		free(pd->value);
		free(pd);
	}
}

void _klpm_pkg_free(klpm_pkg_t *pkg)
{
	if(pkg == NULL) {
		return;
	}

	FREE(pkg->filename);
	FREE(pkg->base);
	FREE(pkg->name);
	FREE(pkg->version);
	FREE(pkg->desc);
	FREE(pkg->url);
	FREE(pkg->packager);
	FREE(pkg->sha256sum);
	FREE(pkg->base64_sig);
	FREE(pkg->arch);

	FREELIST(pkg->licenses);
	free_deplist(pkg->replaces);
	FREELIST(pkg->groups);
	if(pkg->files.count) {
		size_t i;
		for(i = 0; i < pkg->files.count; i++) {
			FREE(pkg->files.files[i].name);
		}
		free(pkg->files.files);
	}
	klpm_list_free_inner(pkg->backup, (klpm_list_fn_free)_klpm_backup_free);
	klpm_list_free(pkg->backup);
	klpm_list_free_inner(pkg->xdata, (klpm_list_fn_free)_klpm_pkg_xdata_free);
	klpm_list_free(pkg->xdata);
	free_deplist(pkg->depends);
	free_deplist(pkg->optdepends);
	free_deplist(pkg->checkdepends);
	free_deplist(pkg->makedepends);
	free_deplist(pkg->conflicts);
	free_deplist(pkg->provides);
	klpm_list_free(pkg->removes);
	_klpm_pkg_free(pkg->oldpkg);

	if(pkg->origin == KUZPKG_PKG_FROM_FILE) {
		FREE(pkg->origin_data.file);
	}
	FREE(pkg);
}

/* This function should be used when removing a target from upgrade/sync target list
 * Case 1: If pkg is a loaded package file (KUZPKG_PKG_FROM_FILE), it will be freed.
 * Case 2: If pkg is a pkgcache entry (KUZPKG_PKG_FROM_CACHE), it won't be freed,
 *         only the transaction specific fields of pkg will be freed.
 */
void _klpm_pkg_free_trans(klpm_pkg_t *pkg)
{
	if(pkg == NULL) {
		return;
	}

	if(pkg->origin == KUZPKG_PKG_FROM_FILE) {
		_klpm_pkg_free(pkg);
		return;
	}

	klpm_list_free(pkg->removes);
	pkg->removes = NULL;
	_klpm_pkg_free(pkg->oldpkg);
	pkg->oldpkg = NULL;
}

/* Is spkg an upgrade for localpkg? */
int _klpm_pkg_compare_versions(klpm_pkg_t *spkg, klpm_pkg_t *localpkg)
{
	return klpm_pkg_vercmp(spkg->version, localpkg->version);
}

/* Helper function for comparing packages
 */
int _klpm_pkg_cmp(const void *p1, const void *p2)
{
	const klpm_pkg_t *pkg1 = p1;
	const klpm_pkg_t *pkg2 = p2;
	return strcmp(pkg1->name, pkg2->name);
}

/* Test for existence of a package in a klpm_list_t*
 * of klpm_pkg_t*
 */
klpm_pkg_t SYMEXPORT *klpm_pkg_find(klpm_list_t *haystack, const char *needle)
{
	klpm_list_t *lp;
	unsigned long needle_hash;

	if(needle == NULL || haystack == NULL) {
		return NULL;
	}

	needle_hash = _klpm_hash_sdbm(needle);

	for(lp = haystack; lp; lp = lp->next) {
		klpm_pkg_t *info = lp->data;

		if(info) {
			if(info->name_hash != needle_hash) {
				continue;
			}

			/* finally: we had hash match, verify string match */
			if(strcmp(info->name, needle) == 0) {
				return info;
			}
		}
	}
	return NULL;
}

int SYMEXPORT klpm_pkg_should_ignore(klpm_handle_t *handle, klpm_pkg_t *pkg)
{
	klpm_list_t *groups = NULL;

	/* first see if the package is ignored */
	if(klpm_list_find(handle->ignorepkg, pkg->name, _klpm_fnmatch)) {
		return 1;
	}

	/* next see if the package is in a group that is ignored */
	for(groups = klpm_pkg_get_groups(pkg); groups; groups = groups->next) {
		char *grp = groups->data;
		if(klpm_list_find(handle->ignoregroup, grp, _klpm_fnmatch)) {
			return 1;
		}
	}

	return 0;
}

/* check that package metadata meets our requirements */
int _klpm_pkg_check_meta(klpm_pkg_t *pkg)
{
	char *c;
	int error_found = 0;

#define EPKGMETA(error) do { \
	error_found = -1; \
	_klpm_log(pkg->handle, KUZPKG_LOG_ERROR, error, pkg->name, pkg->version); \
} while(0)

	/* sanity check */
	if(pkg->handle == NULL) {
		return -1;
	}

	/* immediate bail if package doesn't have name or version */
	if(pkg->name == NULL || pkg->name[0] == '\0'
			|| pkg->version == NULL || pkg->version[0] == '\0') {
		_klpm_log(pkg->handle, KUZPKG_LOG_ERROR,
				_("invalid package metadata (name or version missing)"));
		return -1;
	}

	if(pkg->name[0] == '-' || pkg->name[0] == '.') {
		EPKGMETA(_("invalid metadata for package %s-%s "
					"(package name cannot start with '.' or '-')\n"));
	}
	if(_klpm_fnmatch(pkg->name, "[![:alnum:]+_.@-]") == 0) {
		EPKGMETA(_("invalid metadata for package %s-%s "
					"(package name contains invalid characters)\n"));
	}

	/* multiple '-' in pkgver can cause local db entries for different packages
	 * to overlap (e.g. foo-1=2-3 and foo=1-2-3 both give foo-1-2-3) */
	if((c = strchr(pkg->version, '-')) && (strchr(c + 1, '-'))) {
		EPKGMETA(_("invalid metadata for package %s-%s "
					"(package version contains invalid characters)\n"));
	}
	if(strchr(pkg->version, '/')) {
		EPKGMETA(_("invalid metadata for package %s-%s "
					"(package version contains invalid characters)\n"));
	}

	/* local db entry is <pkgname>-<pkgver> */
	if(strlen(pkg->name) + strlen(pkg->version) + 1 > NAME_MAX) {
		EPKGMETA(_("invalid metadata for package %s-%s "
					"(package name and version too long)\n"));
	}

#undef EPKGMETA

	return error_found;
}
