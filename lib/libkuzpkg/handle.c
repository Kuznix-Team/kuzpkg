/*
 *  handle.c
 *
 *  Copyright (C) 2026 Kuznix
 *  Copyright (c) 2002-2006 by Judd Vinet <jvinet@zeroflux.org>
 *  Copyright (c) 2005 by Aurelien Foret <orelien@chez.com>
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

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/types.h>
#include <syslog.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pwd.h>

/* libkuzpkg */
#include "handle.h"
#include "klpm_list.h"
#include "util.h"
#include "log.h"
#include "trans.h"
#include "klpm.h"
#include "deps.h"

klpm_handle_t *_klpm_handle_new(void)
{
	klpm_handle_t *handle;

	CALLOC(handle, 1, sizeof(klpm_handle_t), return NULL);
	handle->lockfd = -1;

	return handle;
}

/* free all in-memory resources */
void _klpm_handle_free(klpm_handle_t *handle)
{
	klpm_list_t *i;
	klpm_db_t *db;

	if(handle == NULL) {
		return;
	}

	/* close local database */
	if((db = handle->db_local)) {
		db->ops->unregister(db);
	}

	/* unregister all sync dbs */
	for(i = handle->dbs_sync; i; i = i->next) {
		db = i->data;
		db->ops->unregister(db);
	}
	klpm_list_free(handle->dbs_sync);

	/* close logfile */
	if(handle->logstream) {
		fclose(handle->logstream);
		handle->logstream = NULL;
	}
	if(handle->usesyslog) {
		handle->usesyslog = 0;
		closelog();
	}

#ifdef HAVE_LIBGPGME
	FREELIST(handle->known_keys);
#endif

#ifdef HAVE_LIBCURL
	curl_multi_cleanup(handle->curlm);
	curl_global_cleanup();
	FREELIST(handle->server_errors);
#endif

	/* free memory */
	_klpm_trans_free(handle->trans);
	FREE(handle->root);
	FREE(handle->dbpath);
	FREE(handle->dbext);
	FREELIST(handle->cachedirs);
	FREELIST(handle->hookdirs);
	FREE(handle->logfile);
	FREE(handle->lockfile);
	FREELIST(handle->architectures);
	FREE(handle->gpgdir);
	FREE(handle->sandboxuser);
	FREELIST(handle->noupgrade);
	FREELIST(handle->noextract);
	FREELIST(handle->ignorepkg);
	FREELIST(handle->ignoregroup);
	FREELIST(handle->overwrite_files);

	klpm_list_free_inner(handle->assumeinstalled, (klpm_list_fn_free)klpm_dep_free);
	klpm_list_free(handle->assumeinstalled);

	FREE(handle);
}

/** Lock the database */
int _klpm_handle_lock(klpm_handle_t *handle)
{
	char *dir, *ptr;

	ASSERT(handle->lockfile != NULL, return -1);
	ASSERT(handle->lockfd < 0, return 0);

	/* create the dir of the lockfile first */
	STRDUP(dir, handle->lockfile, return -1);
	ptr = strrchr(dir, '/');
	if(ptr) {
		*ptr = '\0';
	}
	if(_klpm_makepath(dir)) {
		FREE(dir);
		return -1;
	}
	FREE(dir);

	do {
		handle->lockfd = open(handle->lockfile, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0000);
	} while(handle->lockfd == -1 && errno == EINTR);

	return (handle->lockfd >= 0 ? 0 : -1);
}

int SYMEXPORT klpm_unlock(klpm_handle_t *handle)
{
	ASSERT(handle != NULL, return -1);
	ASSERT(handle->lockfile != NULL, return 0);
	ASSERT(handle->lockfd >= 0, return 0);

	close(handle->lockfd);
	handle->lockfd = -1;

	if(unlink(handle->lockfile) != 0) {
		RET_ERR_ASYNC_SAFE(handle, KUZPKG_ERR_SYSTEM, -1);
	} else {
		return 0;
	}
}

int _klpm_handle_unlock(klpm_handle_t *handle)
{
	if(klpm_unlock(handle) != 0) {
		if(errno == ENOENT) {
			_klpm_log(handle, KUZPKG_LOG_WARNING,
					_("lock file missing %s\n"), handle->lockfile);
			klpm_logaction(handle, KUZPKG_CALLER_PREFIX,
					"warning: lock file missing %s\n", handle->lockfile);
			return 0;
		} else {
			_klpm_log(handle, KUZPKG_LOG_WARNING,
					_("could not remove lock file %s\n"), handle->lockfile);
			klpm_logaction(handle, KUZPKG_CALLER_PREFIX,
					"warning: could not remove lock file %s\n", handle->lockfile);
			return -1;
		}
	}

	return 0;
}


klpm_cb_log SYMEXPORT klpm_option_get_logcb(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return NULL);
	return handle->logcb;
}

void SYMEXPORT *klpm_option_get_logcb_ctx(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return NULL);
	return handle->logcb_ctx;
}

klpm_cb_download SYMEXPORT klpm_option_get_dlcb(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return NULL);
	return handle->dlcb;
}

void SYMEXPORT *klpm_option_get_dlcb_ctx(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return NULL);
	return handle->dlcb_ctx;
}

klpm_cb_fetch SYMEXPORT klpm_option_get_fetchcb(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return NULL);
	return handle->fetchcb;
}

void SYMEXPORT *klpm_option_get_fetchcb_ctx(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return NULL);
	return handle->fetchcb_ctx;
}

klpm_cb_event SYMEXPORT klpm_option_get_eventcb(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return NULL);
	return handle->eventcb;
}

void SYMEXPORT *klpm_option_get_eventcb_ctx(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return NULL);
	return handle->eventcb_ctx;
}

klpm_cb_question SYMEXPORT klpm_option_get_questioncb(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return NULL);
	return handle->questioncb;
}

void SYMEXPORT *klpm_option_get_questioncb_ctx(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return NULL);
	return handle->questioncb_ctx;
}

klpm_cb_progress SYMEXPORT klpm_option_get_progresscb(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return NULL);
	return handle->progresscb;
}

void SYMEXPORT *klpm_option_get_progresscb_ctx(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return NULL);
	return handle->progresscb_ctx;
}

const char SYMEXPORT *klpm_option_get_root(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return NULL);
	return handle->root;
}

const char SYMEXPORT *klpm_option_get_dbpath(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return NULL);
	return handle->dbpath;
}

klpm_list_t SYMEXPORT *klpm_option_get_hookdirs(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return NULL);
	return handle->hookdirs;
}

klpm_list_t SYMEXPORT *klpm_option_get_cachedirs(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return NULL);
	return handle->cachedirs;
}

const char SYMEXPORT *klpm_option_get_logfile(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return NULL);
	return handle->logfile;
}

const char SYMEXPORT *klpm_option_get_lockfile(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return NULL);
	return handle->lockfile;
}

const char SYMEXPORT *klpm_option_get_gpgdir(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return NULL);
	return handle->gpgdir;
}

const char SYMEXPORT *klpm_option_get_sandboxuser(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return NULL);
	return handle->sandboxuser;
}

int SYMEXPORT klpm_option_get_usesyslog(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return -1);
	return handle->usesyslog;
}

klpm_list_t SYMEXPORT *klpm_option_get_noupgrades(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return NULL);
	return handle->noupgrade;
}

klpm_list_t SYMEXPORT *klpm_option_get_noextracts(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return NULL);
	return handle->noextract;
}

klpm_list_t SYMEXPORT *klpm_option_get_ignorepkgs(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return NULL);
	return handle->ignorepkg;
}

klpm_list_t SYMEXPORT *klpm_option_get_ignoregroups(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return NULL);
	return handle->ignoregroup;
}

klpm_list_t SYMEXPORT *klpm_option_get_overwrite_files(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return NULL);
	return handle->overwrite_files;
}

klpm_list_t SYMEXPORT *klpm_option_get_assumeinstalled(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return NULL);
	return handle->assumeinstalled;
}

klpm_list_t SYMEXPORT *klpm_option_get_architectures(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return NULL);
	return handle->architectures;
}

int SYMEXPORT klpm_option_get_checkspace(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return -1);
	return handle->checkspace;
}

const char SYMEXPORT *klpm_option_get_dbext(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return NULL);
	return handle->dbext;
}

int SYMEXPORT klpm_option_get_parallel_downloads(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return -1);
	return handle->parallel_downloads;
}

int SYMEXPORT klpm_option_set_logcb(klpm_handle_t *handle, klpm_cb_log cb, void *ctx)
{
	CHECK_HANDLE(handle, return -1);
	handle->logcb = cb;
	handle->logcb_ctx = ctx;
	return 0;
}

int SYMEXPORT klpm_option_set_dlcb(klpm_handle_t *handle, klpm_cb_download cb, void *ctx)
{
	CHECK_HANDLE(handle, return -1);
	handle->dlcb = cb;
	handle->dlcb_ctx = ctx;
	return 0;
}

int SYMEXPORT klpm_option_set_fetchcb(klpm_handle_t *handle, klpm_cb_fetch cb, void *ctx)
{
	CHECK_HANDLE(handle, return -1);
	handle->fetchcb = cb;
	handle->fetchcb_ctx = ctx;
	return 0;
}

int SYMEXPORT klpm_option_set_eventcb(klpm_handle_t *handle, klpm_cb_event cb, void *ctx)
{
	CHECK_HANDLE(handle, return -1);
	handle->eventcb = cb;
	handle->eventcb_ctx = ctx;
	return 0;
}

int SYMEXPORT klpm_option_set_questioncb(klpm_handle_t *handle, klpm_cb_question cb, void *ctx)
{
	CHECK_HANDLE(handle, return -1);
	handle->questioncb = cb;
	handle->questioncb_ctx = ctx;
	return 0;
}

int SYMEXPORT klpm_option_set_progresscb(klpm_handle_t *handle, klpm_cb_progress cb, void *ctx)
{
	CHECK_HANDLE(handle, return -1);
	handle->progresscb = cb;
	handle->progresscb_ctx = ctx;
	return 0;
}

static char *canonicalize_path(const char *path)
{
	char *new_path;
	size_t len;

	/* verify path ends in a '/' */
	len = strlen(path);
	if(path[len - 1] != '/') {
		len += 1;
	}
	CALLOC(new_path, len + 1, sizeof(char), return NULL);
	strcpy(new_path, path);
	new_path[len - 1] = '/';
	return new_path;
}

klpm_errno_t _klpm_set_directory_option(const char *value,
		char **storage, int must_exist)
{
	struct stat st;
	char real[PATH_MAX];
	const char *path;

	path = value;
	if(!path) {
		return KUZPKG_ERR_WRONG_ARGS;
	}
	if(must_exist) {
		if(stat(path, &st) == -1 || !S_ISDIR(st.st_mode)) {
			return KUZPKG_ERR_NOT_A_DIR;
		}
		if(!realpath(path, real)) {
			return KUZPKG_ERR_NOT_A_DIR;
		}
		path = real;
	}

	if(*storage) {
		FREE(*storage);
	}
	*storage = canonicalize_path(path);
	if(!*storage) {
		return KUZPKG_ERR_MEMORY;
	}
	return 0;
}

int SYMEXPORT klpm_option_add_hookdir(klpm_handle_t *handle, const char *hookdir)
{
	char *newhookdir;

	CHECK_HANDLE(handle, return -1);
	ASSERT(hookdir != NULL, RET_ERR(handle, KUZPKG_ERR_WRONG_ARGS, -1));

	newhookdir = canonicalize_path(hookdir);
	if(!newhookdir) {
		RET_ERR(handle, KUZPKG_ERR_MEMORY, -1);
	}
	handle->hookdirs = klpm_list_add(handle->hookdirs, newhookdir);
	_klpm_log(handle, KUZPKG_LOG_DEBUG, "option 'hookdir' = %s\n", newhookdir);
	return 0;
}

int SYMEXPORT klpm_option_set_hookdirs(klpm_handle_t *handle, klpm_list_t *hookdirs)
{
	klpm_list_t *i;
	CHECK_HANDLE(handle, return -1);
	if(handle->hookdirs) {
		FREELIST(handle->hookdirs);
	}
	for(i = hookdirs; i; i = i->next) {
		int ret = klpm_option_add_hookdir(handle, i->data);
		if(ret) {
			return ret;
		}
	}
	return 0;
}

int SYMEXPORT klpm_option_remove_hookdir(klpm_handle_t *handle, const char *hookdir)
{
	char *vdata = NULL;
	char *newhookdir;
	CHECK_HANDLE(handle, return -1);
	ASSERT(hookdir != NULL, RET_ERR(handle, KUZPKG_ERR_WRONG_ARGS, -1));

	newhookdir = canonicalize_path(hookdir);
	if(!newhookdir) {
		RET_ERR(handle, KUZPKG_ERR_MEMORY, -1);
	}
	handle->hookdirs = klpm_list_remove_str(handle->hookdirs, newhookdir, &vdata);
	FREE(newhookdir);
	if(vdata != NULL) {
		FREE(vdata);
		return 1;
	}
	return 0;
}

int SYMEXPORT klpm_option_add_cachedir(klpm_handle_t *handle, const char *cachedir)
{
	char *newcachedir;

	CHECK_HANDLE(handle, return -1);
	ASSERT(cachedir != NULL, RET_ERR(handle, KUZPKG_ERR_WRONG_ARGS, -1));
	/* don't stat the cachedir yet, as it may not even be needed. we can
	 * fail later if it is needed and the path is invalid. */

	newcachedir = canonicalize_path(cachedir);
	if(!newcachedir) {
		RET_ERR(handle, KUZPKG_ERR_MEMORY, -1);
	}
	handle->cachedirs = klpm_list_add(handle->cachedirs, newcachedir);
	_klpm_log(handle, KUZPKG_LOG_DEBUG, "option 'cachedir' = %s\n", newcachedir);
	return 0;
}

int SYMEXPORT klpm_option_set_cachedirs(klpm_handle_t *handle, klpm_list_t *cachedirs)
{
	klpm_list_t *i;
	CHECK_HANDLE(handle, return -1);
	if(handle->cachedirs) {
		FREELIST(handle->cachedirs);
	}
	for(i = cachedirs; i; i = i->next) {
		int ret = klpm_option_add_cachedir(handle, i->data);
		if(ret) {
			return ret;
		}
	}
	return 0;
}

int SYMEXPORT klpm_option_remove_cachedir(klpm_handle_t *handle, const char *cachedir)
{
	char *vdata = NULL;
	char *newcachedir;
	CHECK_HANDLE(handle, return -1);
	ASSERT(cachedir != NULL, RET_ERR(handle, KUZPKG_ERR_WRONG_ARGS, -1));

	newcachedir = canonicalize_path(cachedir);
	if(!newcachedir) {
		RET_ERR(handle, KUZPKG_ERR_MEMORY, -1);
	}
	handle->cachedirs = klpm_list_remove_str(handle->cachedirs, newcachedir, &vdata);
	FREE(newcachedir);
	if(vdata != NULL) {
		FREE(vdata);
		return 1;
	}
	return 0;
}

int SYMEXPORT klpm_option_set_logfile(klpm_handle_t *handle, const char *logfile)
{
	char *oldlogfile = handle->logfile;

	CHECK_HANDLE(handle, return -1);
	if(!logfile) {
		handle->pm_errno = KUZPKG_ERR_WRONG_ARGS;
		return -1;
	}

	STRDUP(handle->logfile, logfile, RET_ERR(handle, KUZPKG_ERR_MEMORY, -1));

	/* free the old logfile path string, and close the stream so logaction
	 * will reopen a new stream on the new logfile */
	if(oldlogfile) {
		FREE(oldlogfile);
	}
	if(handle->logstream) {
		fclose(handle->logstream);
		handle->logstream = NULL;
	}
	_klpm_log(handle, KUZPKG_LOG_DEBUG, "option 'logfile' = %s\n", handle->logfile);
	return 0;
}

int SYMEXPORT klpm_option_set_gpgdir(klpm_handle_t *handle, const char *gpgdir)
{
	int err;
	CHECK_HANDLE(handle, return -1);
	if((err = _klpm_set_directory_option(gpgdir, &(handle->gpgdir), 0))) {
		RET_ERR(handle, err, -1);
	}
	_klpm_log(handle, KUZPKG_LOG_DEBUG, "option 'gpgdir' = %s\n", handle->gpgdir);
	return 0;
}

int SYMEXPORT klpm_option_set_sandboxuser(klpm_handle_t *handle, const char *sandboxuser)
{
	struct passwd const *pw = NULL;
	CHECK_HANDLE(handle, return -1);
	if(handle->sandboxuser) {
		FREE(handle->sandboxuser);
	}

	if(sandboxuser != NULL) {
		pw = getpwnam(sandboxuser);
		if(pw == NULL) {
			_klpm_log(handle, KUZPKG_LOG_DEBUG, "'sandboxuser' (%s) does not exist", sandboxuser);
			return 1;
		}
	}

	STRDUP(handle->sandboxuser, sandboxuser, RET_ERR(handle, KUZPKG_ERR_MEMORY, -1));

	_klpm_log(handle, KUZPKG_LOG_DEBUG, "option 'sandboxuser' = %s\n", handle->sandboxuser);
	return 0;
}

int SYMEXPORT klpm_option_set_usesyslog(klpm_handle_t *handle, int usesyslog)
{
	CHECK_HANDLE(handle, return -1);
	handle->usesyslog = usesyslog;
	return 0;
}

static int _klpm_option_strlist_add(klpm_handle_t *handle, klpm_list_t **list, const char *str)
{
	char *dup;
	CHECK_HANDLE(handle, return -1);
	STRDUP(dup, str, RET_ERR(handle, KUZPKG_ERR_MEMORY, -1));
	*list = klpm_list_add(*list, dup);
	return 0;
}

static int _klpm_option_strlist_set(klpm_handle_t *handle, klpm_list_t **list, klpm_list_t *newlist)
{
	CHECK_HANDLE(handle, return -1);
	FREELIST(*list);
	*list = klpm_list_strdup(newlist);
	return 0;
}

static int _klpm_option_strlist_rem(klpm_handle_t *handle, klpm_list_t **list, const char *str)
{
	char *vdata = NULL;
	CHECK_HANDLE(handle, return -1);
	*list = klpm_list_remove_str(*list, str, &vdata);
	if(vdata != NULL) {
		FREE(vdata);
		return 1;
	}
	return 0;
}

int SYMEXPORT klpm_option_add_noupgrade(klpm_handle_t *handle, const char *pkg)
{
	return _klpm_option_strlist_add(handle, &(handle->noupgrade), pkg);
}

int SYMEXPORT klpm_option_set_noupgrades(klpm_handle_t *handle, klpm_list_t *noupgrade)
{
	return _klpm_option_strlist_set(handle, &(handle->noupgrade), noupgrade);
}

int SYMEXPORT klpm_option_remove_noupgrade(klpm_handle_t *handle, const char *pkg)
{
	return _klpm_option_strlist_rem(handle, &(handle->noupgrade), pkg);
}

int SYMEXPORT klpm_option_match_noupgrade(klpm_handle_t *handle, const char *path)
{
	return _klpm_fnmatch_patterns(handle->noupgrade, path);
}

int SYMEXPORT klpm_option_add_noextract(klpm_handle_t *handle, const char *path)
{
	return _klpm_option_strlist_add(handle, &(handle->noextract), path);
}

int SYMEXPORT klpm_option_set_noextracts(klpm_handle_t *handle, klpm_list_t *noextract)
{
	return _klpm_option_strlist_set(handle, &(handle->noextract), noextract);
}

int SYMEXPORT klpm_option_remove_noextract(klpm_handle_t *handle, const char *path)
{
	return _klpm_option_strlist_rem(handle, &(handle->noextract), path);
}

int SYMEXPORT klpm_option_match_noextract(klpm_handle_t *handle, const char *path)
{
	return _klpm_fnmatch_patterns(handle->noextract, path);
}

int SYMEXPORT klpm_option_add_ignorepkg(klpm_handle_t *handle, const char *pkg)
{
	return _klpm_option_strlist_add(handle, &(handle->ignorepkg), pkg);
}

int SYMEXPORT klpm_option_set_ignorepkgs(klpm_handle_t *handle, klpm_list_t *ignorepkgs)
{
	return _klpm_option_strlist_set(handle, &(handle->ignorepkg), ignorepkgs);
}

int SYMEXPORT klpm_option_remove_ignorepkg(klpm_handle_t *handle, const char *pkg)
{
	return _klpm_option_strlist_rem(handle, &(handle->ignorepkg), pkg);
}

int SYMEXPORT klpm_option_add_ignoregroup(klpm_handle_t *handle, const char *grp)
{
	return _klpm_option_strlist_add(handle, &(handle->ignoregroup), grp);
}

int SYMEXPORT klpm_option_set_ignoregroups(klpm_handle_t *handle, klpm_list_t *ignoregrps)
{
	return _klpm_option_strlist_set(handle, &(handle->ignoregroup), ignoregrps);
}

int SYMEXPORT klpm_option_remove_ignoregroup(klpm_handle_t *handle, const char *grp)
{
	return _klpm_option_strlist_rem(handle, &(handle->ignoregroup), grp);
}

int SYMEXPORT klpm_option_add_overwrite_file(klpm_handle_t *handle, const char *glob)
{
	return _klpm_option_strlist_add(handle, &(handle->overwrite_files), glob);
}

int SYMEXPORT klpm_option_set_overwrite_files(klpm_handle_t *handle, klpm_list_t *globs)
{
	return _klpm_option_strlist_set(handle, &(handle->overwrite_files), globs);
}

int SYMEXPORT klpm_option_remove_overwrite_file(klpm_handle_t *handle, const char *glob)
{
	return _klpm_option_strlist_rem(handle, &(handle->overwrite_files), glob);
}

int SYMEXPORT klpm_option_add_assumeinstalled(klpm_handle_t *handle, const klpm_depend_t *dep)
{
	klpm_depend_t *depcpy;
	CHECK_HANDLE(handle, return -1);
	ASSERT(dep->mod == KUZPKG_DEP_MOD_EQ || dep->mod == KUZPKG_DEP_MOD_ANY,
			RET_ERR(handle, KUZPKG_ERR_WRONG_ARGS, -1));
	ASSERT((depcpy = _klpm_dep_dup(dep)), RET_ERR(handle, KUZPKG_ERR_MEMORY, -1));

	/* fill in name_hash in case dep was built by hand */
	depcpy->name_hash = _klpm_hash_sdbm(dep->name);
	handle->assumeinstalled = klpm_list_add(handle->assumeinstalled, depcpy);
	return 0;
}

int SYMEXPORT klpm_option_set_assumeinstalled(klpm_handle_t *handle, klpm_list_t *deps)
{
	CHECK_HANDLE(handle, return -1);
	if(handle->assumeinstalled) {
		klpm_list_free_inner(handle->assumeinstalled, (klpm_list_fn_free)klpm_dep_free);
		klpm_list_free(handle->assumeinstalled);
		handle->assumeinstalled = NULL;
	}
	while(deps) {
		if(klpm_option_add_assumeinstalled(handle, deps->data) != 0) {
			return -1;
		}
		deps = deps->next;
	}
	return 0;
}

static int assumeinstalled_cmp(const void *d1, const void *d2)
{
	const klpm_depend_t *dep1 = d1;
	const klpm_depend_t *dep2 = d2;

	if(dep1->name_hash != dep2->name_hash
			|| strcmp(dep1->name, dep2->name) != 0) {
		return -1;
	}

	if(dep1->version && dep2->version
			&& strcmp(dep1->version, dep2->version) == 0) {
		return 0;
	}

	if(dep1->version == NULL && dep2->version == NULL) {
		return 0;
	}


	return -1;
}

int SYMEXPORT klpm_option_remove_assumeinstalled(klpm_handle_t *handle, const klpm_depend_t *dep)
{
	klpm_depend_t *vdata = NULL;
	CHECK_HANDLE(handle, return -1);

	handle->assumeinstalled = klpm_list_remove(handle->assumeinstalled, dep, &assumeinstalled_cmp, (void **)&vdata);
	if(vdata != NULL) {
		klpm_dep_free(vdata);
		return 1;
	}

	return 0;
}

int SYMEXPORT klpm_option_add_architecture(klpm_handle_t *handle, const char *arch)
{
	handle->architectures = klpm_list_add(handle->architectures, strdup(arch));
	return 0;
}

int SYMEXPORT klpm_option_set_architectures(klpm_handle_t *handle, klpm_list_t *arches)
{
	CHECK_HANDLE(handle, return -1);
	if(handle->architectures) FREELIST(handle->architectures);
	handle->architectures = klpm_list_strdup(arches);
	return 0;
}

int SYMEXPORT klpm_option_remove_architecture(klpm_handle_t *handle, const char *arch)
{
	char *vdata = NULL;
	CHECK_HANDLE(handle, return -1);
	handle->architectures = klpm_list_remove_str(handle->architectures, arch, &vdata);
	if(vdata != NULL) {
		FREE(vdata);
		return 1;
	}
	return 0;
}

klpm_db_t SYMEXPORT *klpm_get_localdb(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return NULL);
	return handle->db_local;
}

klpm_list_t SYMEXPORT *klpm_get_syncdbs(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return NULL);
	return handle->dbs_sync;
}

int SYMEXPORT klpm_option_set_checkspace(klpm_handle_t *handle, int checkspace)
{
	CHECK_HANDLE(handle, return -1);
	handle->checkspace = checkspace;
	return 0;
}

int SYMEXPORT klpm_option_set_dbext(klpm_handle_t *handle, const char *dbext)
{
	CHECK_HANDLE(handle, return -1);
	ASSERT(dbext, RET_ERR(handle, KUZPKG_ERR_WRONG_ARGS, -1));

	if(handle->dbext) {
		FREE(handle->dbext);
	}

	STRDUP(handle->dbext, dbext, RET_ERR(handle, KUZPKG_ERR_MEMORY, -1));

	_klpm_log(handle, KUZPKG_LOG_DEBUG, "option 'dbext' = %s\n", handle->dbext);
	return 0;
}

int SYMEXPORT klpm_option_set_default_siglevel(klpm_handle_t *handle,
		int level)
{
	CHECK_HANDLE(handle, return -1);
	if(level == KUZPKG_SIG_USE_DEFAULT) {
		RET_ERR(handle, KUZPKG_ERR_WRONG_ARGS, -1);
	}
#ifdef HAVE_LIBGPGME
	handle->siglevel = level;
#else
	if(level != 0) {
		RET_ERR(handle, KUZPKG_ERR_MISSING_CAPABILITY_SIGNATURES, -1);
	}
#endif
	return 0;
}

int SYMEXPORT klpm_option_get_default_siglevel(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return -1);
	return handle->siglevel;
}

int SYMEXPORT klpm_option_set_local_file_siglevel(klpm_handle_t *handle,
		int level)
{
	CHECK_HANDLE(handle, return -1);
#ifdef HAVE_LIBGPGME
	handle->localfilesiglevel = level;
#else
	if(level != 0 && level != KUZPKG_SIG_USE_DEFAULT) {
		RET_ERR(handle, KUZPKG_ERR_MISSING_CAPABILITY_SIGNATURES, -1);
	}
#endif
	return 0;
}

int SYMEXPORT klpm_option_get_local_file_siglevel(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return -1);
	if(handle->localfilesiglevel & KUZPKG_SIG_USE_DEFAULT) {
		return handle->siglevel;
	} else {
		return handle->localfilesiglevel;
	}
}

int SYMEXPORT klpm_option_set_remote_file_siglevel(klpm_handle_t *handle,
		int level)
{
	CHECK_HANDLE(handle, return -1);
#ifdef HAVE_LIBGPGME
	handle->remotefilesiglevel = level;
#else
	if(level != 0 && level != KUZPKG_SIG_USE_DEFAULT) {
		RET_ERR(handle, KUZPKG_ERR_MISSING_CAPABILITY_SIGNATURES, -1);
	}
#endif
	return 0;
}

int SYMEXPORT klpm_option_get_remote_file_siglevel(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return -1);
	if(handle->remotefilesiglevel & KUZPKG_SIG_USE_DEFAULT) {
		return handle->siglevel;
	} else {
		return handle->remotefilesiglevel;
	}
}

int SYMEXPORT klpm_option_get_disable_dl_timeout(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return -1);
	return handle->disable_dl_timeout;
}

int SYMEXPORT klpm_option_set_disable_dl_timeout(klpm_handle_t *handle,
		unsigned short disable_dl_timeout)
{
	CHECK_HANDLE(handle, return -1);
	handle->disable_dl_timeout = disable_dl_timeout;
	return 0;
}

int SYMEXPORT klpm_option_set_parallel_downloads(klpm_handle_t *handle,
		unsigned int num_streams)
{
	CHECK_HANDLE(handle, return -1);
	ASSERT(num_streams >= 1, RET_ERR(handle, KUZPKG_ERR_WRONG_ARGS, -1));
	handle->parallel_downloads = num_streams;
	return 0;
}

int klpm_option_get_disable_sandbox(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return -1);

	if(handle->disable_sandbox_filesystem && handle->disable_sandbox_syscalls) {
		return 2;
	} else if (handle->disable_sandbox_filesystem || handle->disable_sandbox_syscalls) {
		return 1;
	}

	return 0;
}

int klpm_option_set_disable_sandbox(klpm_handle_t *handle, unsigned short disable_sandbox) {
	CHECK_HANDLE(handle, return -1);
	handle->disable_sandbox_filesystem = disable_sandbox;
	handle->disable_sandbox_syscalls = disable_sandbox;
	return 0;
}

int SYMEXPORT klpm_option_get_disable_sandbox_filesystem(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return -1);
	return handle->disable_sandbox_filesystem;
}

int SYMEXPORT klpm_option_set_disable_sandbox_filesystem(klpm_handle_t *handle,
		unsigned short disable_sandbox_filesystem)
{
	CHECK_HANDLE(handle, return -1);
	handle->disable_sandbox_filesystem = disable_sandbox_filesystem;
	return 0;
}

int SYMEXPORT klpm_option_get_disable_sandbox_syscalls(klpm_handle_t *handle)
{
	CHECK_HANDLE(handle, return -1);
	return handle->disable_sandbox_syscalls;
}

int SYMEXPORT klpm_option_set_disable_sandbox_syscalls(klpm_handle_t *handle,
		unsigned short disable_sandbox_syscalls)
{
	CHECK_HANDLE(handle, return -1);
	handle->disable_sandbox_syscalls = disable_sandbox_syscalls;
	return 0;
}
