/*
 *  trans.c
 *
 *  Copyright (C) 2026 Kuznix
 *  Copyright (c) 2002-2006 by Judd Vinet <jvinet@zeroflux.org>
 *  Copyright (c) 2005 by Aurelien Foret <orelien@chez.com>
 *  Copyright (c) 2005 by Christian Hamar <krics@linuxforum.hu>
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <errno.h>
#include <limits.h>

/* libkuzpkg */
#include "trans.h"
#include "klpm_list.h"
#include "package.h"
#include "util.h"
#include "log.h"
#include "handle.h"
#include "remove.h"
#include "sync.h"
#include "klpm.h"
#include "deps.h"
#include "hook.h"

int SYMEXPORT klpm_trans_init(klpm_handle_t *handle, int flags)
{
	klpm_trans_t *trans;

	/* Sanity checks */
	CHECK_HANDLE(handle, return -1);
	ASSERT(handle->trans == NULL, RET_ERR(handle, KUZPKG_ERR_TRANS_NOT_NULL, -1));

	/* lock db */
	if(!(flags & KUZPKG_TRANS_FLAG_NOLOCK)) {
		if(_klpm_handle_lock(handle)) {
			RET_ERR(handle, KUZPKG_ERR_HANDLE_LOCK, -1);
		}
	}

	CALLOC(trans, 1, sizeof(klpm_trans_t), RET_ERR(handle, KUZPKG_ERR_MEMORY, -1));
	trans->flags = flags;
	trans->state = STATE_INITIALIZED;

	handle->trans = trans;

	return 0;
}

static klpm_list_t *check_arch(klpm_handle_t *handle, klpm_list_t *pkgs)
{
	klpm_list_t *i;
	klpm_list_t *invalid = NULL;

	if(!handle->architectures) {
		_klpm_log(handle, KUZPKG_LOG_DEBUG, "skipping architecture checks\n");
		return NULL;
	}
	for(i = pkgs; i; i = i->next) {
		klpm_pkg_t *pkg = i->data;
		klpm_list_t *j;
		int found = 0;
		const char *pkgarch = klpm_pkg_get_arch(pkg);

		/* always allow non-architecture packages and those marked "any" */
		if(!pkgarch || strcmp(pkgarch, "any") == 0) {
			continue;
		}

		for(j = handle->architectures; j; j = j->next) {
			if(strcmp(pkgarch, j->data) == 0) {
				found = 1;
				break;
			}
		}

		if(!found) {
			char *string;
			const char *pkgname = pkg->name;
			const char *pkgver = pkg->version;
			size_t len = strlen(pkgname) + strlen(pkgver) + strlen(pkgarch) + 3;
			MALLOC(string, len, RET_ERR(handle, KUZPKG_ERR_MEMORY, invalid));
			snprintf(string, len, "%s-%s-%s", pkgname, pkgver, pkgarch);
			invalid = klpm_list_add(invalid, string);
		}
	}
	return invalid;
}

int SYMEXPORT klpm_trans_prepare(klpm_handle_t *handle, klpm_list_t **data)
{
	klpm_trans_t *trans;

	/* Sanity checks */
	CHECK_HANDLE(handle, return -1);
	ASSERT(data != NULL, RET_ERR(handle, KUZPKG_ERR_WRONG_ARGS, -1));

	trans = handle->trans;

	ASSERT(trans != NULL, RET_ERR(handle, KUZPKG_ERR_TRANS_NULL, -1));
	ASSERT(trans->state == STATE_INITIALIZED, RET_ERR(handle, KUZPKG_ERR_TRANS_NOT_INITIALIZED, -1));

	/* If there's nothing to do, return without complaining */
	if(trans->add == NULL && trans->remove == NULL) {
		return 0;
	}

	klpm_list_t *invalid = check_arch(handle, trans->add);
	if(invalid) {
		if(data) {
			*data = invalid;
		}
		RET_ERR(handle, KUZPKG_ERR_PKG_INVALID_ARCH, -1);
	}

	if(trans->add == NULL) {
		if(_klpm_remove_prepare(handle, data) == -1) {
			/* pm_errno is set by _klpm_remove_prepare() */
			return -1;
		}
	} else {
		if(_klpm_sync_prepare(handle, data) == -1) {
			/* pm_errno is set by _klpm_sync_prepare() */
			return -1;
		}
	}


	if(!(trans->flags & KUZPKG_TRANS_FLAG_NODEPS)) {
		_klpm_log(handle, KUZPKG_LOG_DEBUG, "sorting by dependencies\n");
		if(trans->add) {
			klpm_list_t *add_orig = trans->add;
			trans->add = _klpm_sortbydeps(handle, add_orig, trans->remove, 0);
			klpm_list_free(add_orig);
		}
		if(trans->remove) {
			klpm_list_t *rem_orig = trans->remove;
			trans->remove = _klpm_sortbydeps(handle, rem_orig, NULL, 1);
			klpm_list_free(rem_orig);
		}
	}

	trans->state = STATE_PREPARED;

	return 0;
}

int SYMEXPORT klpm_trans_commit(klpm_handle_t *handle, klpm_list_t **data)
{
	klpm_trans_t *trans;
	klpm_event_any_t event;

	/* Sanity checks */
	CHECK_HANDLE(handle, return -1);

	trans = handle->trans;

	ASSERT(trans != NULL, RET_ERR(handle, KUZPKG_ERR_TRANS_NULL, -1));
	ASSERT(trans->state == STATE_PREPARED, RET_ERR(handle, KUZPKG_ERR_TRANS_NOT_PREPARED, -1));

	ASSERT(!(trans->flags & KUZPKG_TRANS_FLAG_NOLOCK), RET_ERR(handle, KUZPKG_ERR_TRANS_NOT_LOCKED, -1));

	/* If there's nothing to do, return without complaining */
	if(trans->add == NULL && trans->remove == NULL) {
		return 0;
	}

	if(trans->add) {
		if(_klpm_sync_load(handle, data) != 0) {
			/* pm_errno is set by _klpm_sync_load() */
			return -1;
		}
		if(trans->flags & KUZPKG_TRANS_FLAG_DOWNLOADONLY) {
			return 0;
		}
		if(_klpm_sync_check(handle, data) != 0) {
			/* pm_errno is set by _klpm_sync_check() */
			return -1;
		}
	}

	if(!(trans->flags & KUZPKG_TRANS_FLAG_NOHOOKS) &&
			_klpm_hook_run(handle, KUZPKG_HOOK_PRE_TRANSACTION) != 0) {
		RET_ERR(handle, KUZPKG_ERR_TRANS_HOOK_FAILED, -1);
	}

	trans->state = STATE_COMMITTING;

	klpm_logaction(handle, KUZPKG_CALLER_PREFIX, "transaction started\n");
	event.type = KUZPKG_EVENT_TRANSACTION_START;
	EVENT(handle, (void *)&event);

	if(trans->add == NULL) {
		if(_klpm_remove_packages(handle, 1) == -1) {
			/* pm_errno is set by _klpm_remove_packages() */
			klpm_errno_t save = handle->pm_errno;
			klpm_logaction(handle, KUZPKG_CALLER_PREFIX, "transaction failed\n");
			handle->pm_errno = save;
			return -1;
		}
	} else {
		if(_klpm_sync_commit(handle) == -1) {
			/* pm_errno is set by _klpm_sync_commit() */
			klpm_errno_t save = handle->pm_errno;
			klpm_logaction(handle, KUZPKG_CALLER_PREFIX, "transaction failed\n");
			handle->pm_errno = save;
			return -1;
		}
	}

	if(trans->state == STATE_INTERRUPTED) {
		klpm_logaction(handle, KUZPKG_CALLER_PREFIX, "transaction interrupted\n");
	} else {
		event.type = KUZPKG_EVENT_TRANSACTION_DONE;
		EVENT(handle, (void *)&event);
		klpm_logaction(handle, KUZPKG_CALLER_PREFIX, "transaction completed\n");

		if(!(trans->flags & KUZPKG_TRANS_FLAG_NOHOOKS)) {
			_klpm_hook_run(handle, KUZPKG_HOOK_POST_TRANSACTION);
		}
	}

	trans->state = STATE_COMMITTED;

	return 0;
}

int SYMEXPORT klpm_trans_interrupt(klpm_handle_t *handle)
{
	klpm_trans_t *trans;

	/* Sanity checks */
	CHECK_HANDLE(handle, return -1);

	trans = handle->trans;
	ASSERT(trans != NULL, RET_ERR_ASYNC_SAFE(handle, KUZPKG_ERR_TRANS_NULL, -1));
	ASSERT(trans->state == STATE_COMMITTING || trans->state == STATE_INTERRUPTED,
			RET_ERR_ASYNC_SAFE(handle, KUZPKG_ERR_TRANS_TYPE, -1));

	trans->state = STATE_INTERRUPTED;

	return 0;
}

int SYMEXPORT klpm_trans_release(klpm_handle_t *handle)
{
	klpm_trans_t *trans;

	/* Sanity checks */
	CHECK_HANDLE(handle, return -1);

	trans = handle->trans;
	ASSERT(trans != NULL, RET_ERR(handle, KUZPKG_ERR_TRANS_NULL, -1));
	ASSERT(trans->state != STATE_IDLE, RET_ERR(handle, KUZPKG_ERR_TRANS_NULL, -1));

	int nolock_flag = trans->flags & KUZPKG_TRANS_FLAG_NOLOCK;

	_klpm_trans_free(trans);
	handle->trans = NULL;

	/* unlock db */
	if(!nolock_flag) {
		_klpm_handle_unlock(handle);
	}

	return 0;
}

void _klpm_trans_free(klpm_trans_t *trans)
{
	if(trans == NULL) {
		return;
	}

	klpm_list_free_inner(trans->unresolvable,
			(klpm_list_fn_free)_klpm_pkg_free_trans);
	klpm_list_free(trans->unresolvable);
	klpm_list_free_inner(trans->add, (klpm_list_fn_free)_klpm_pkg_free_trans);
	klpm_list_free(trans->add);
	klpm_list_free_inner(trans->remove, (klpm_list_fn_free)_klpm_pkg_free);
	klpm_list_free(trans->remove);

	FREELIST(trans->skip_remove);

	FREE(trans);
}

/* A cheap grep for text files, returns 1 if a substring
 * was found in the text file fn, 0 if it wasn't
 */
static int grep(const char *fn, const char *needle)
{
	FILE *fp;
	char *ptr;

	if((fp = fopen(fn, "r")) == NULL) {
		return 0;
	}
	while(!feof(fp)) {
		char line[1024];
		if(safe_fgets(line, sizeof(line), fp) == NULL) {
			continue;
		}
		if((ptr = strchr(line, '#')) != NULL) {
			*ptr = '\0';
		}
		/* TODO: this will not work if the search string
		 * ends up being split across line reads */
		if(strstr(line, needle)) {
			fclose(fp);
			return 1;
		}
	}
	fclose(fp);
	return 0;
}

int _klpm_runscriptlet(klpm_handle_t *handle, const char *filepath,
		const char *script, const char *ver, const char *oldver, int is_archive)
{
	char arg0[PATH_MAX], arg1[3], cmdline[PATH_MAX];
	char *argv[] = { arg0, arg1, cmdline, NULL };
	char *tmpdir, *scriptfn = NULL, *scriptpath;
	int retval = 0;
	size_t len;

	if(_klpm_access(handle, NULL, filepath, R_OK) != 0) {
		_klpm_log(handle, KUZPKG_LOG_DEBUG, "scriptlet '%s' not found\n", filepath);
		return 0;
	}

	if(!is_archive && !grep(filepath, script)) {
		/* script not found in scriptlet file; we can only short-circuit this early
		 * if it is an actual scriptlet file and not an archive. */
		return 0;
	}

	strcpy(arg0, SCRIPTLET_SHELL);
	strcpy(arg1, "-c");

	/* create a directory in $root/tmp/ for copying/extracting the scriptlet */
	len = strlen(handle->root) + strlen("tmp/klpm_XXXXXX") + 1;
	MALLOC(tmpdir, len, RET_ERR(handle, KUZPKG_ERR_MEMORY, -1));
	snprintf(tmpdir, len, "%stmp/", handle->root);
	if(access(tmpdir, F_OK) != 0) {
		_klpm_makepath_mode(tmpdir, 01777);
	}
	snprintf(tmpdir, len, "%stmp/klpm_XXXXXX", handle->root);
	if(mkdtemp(tmpdir) == NULL) {
		_klpm_log(handle, KUZPKG_LOG_ERROR, _("could not create temp directory\n"));
		free(tmpdir);
		return 1;
	}

	/* either extract or copy the scriptlet */
	len += strlen("/.INSTALL");
	MALLOC(scriptfn, len, free(tmpdir); RET_ERR(handle, KUZPKG_ERR_MEMORY, -1));
	snprintf(scriptfn, len, "%s/.INSTALL", tmpdir);
	if(is_archive) {
		if(_klpm_unpack_single(handle, filepath, tmpdir, ".INSTALL")) {
			retval = 1;
		}
	} else {
		if(_klpm_copyfile(filepath, scriptfn)) {
			_klpm_log(handle, KUZPKG_LOG_ERROR, _("could not copy tempfile to %s (%s)\n"), scriptfn, strerror(errno));
			retval = 1;
		}
	}
	if(retval == 1) {
		goto cleanup;
	}

	if(is_archive && !grep(scriptfn, script)) {
		/* script not found in extracted scriptlet file */
		goto cleanup;
	}

	/* chop off the root so we can find the tmpdir in the chroot */
	scriptpath = scriptfn + strlen(handle->root) - 1;

	if(oldver) {
		snprintf(cmdline, PATH_MAX, ". %s; %s %s %s",
				scriptpath, script, ver, oldver);
	} else {
		snprintf(cmdline, PATH_MAX, ". %s; %s %s",
				scriptpath, script, ver);
	}

	_klpm_log(handle, KUZPKG_LOG_DEBUG, "executing \"%s\"\n", cmdline);

	retval = _klpm_run_chroot(handle, SCRIPTLET_SHELL, argv, NULL, NULL);

cleanup:
	if(scriptfn && unlink(scriptfn)) {
		_klpm_log(handle, KUZPKG_LOG_WARNING,
				_("could not remove %s\n"), scriptfn);
	}
	if(rmdir(tmpdir)) {
		_klpm_log(handle, KUZPKG_LOG_WARNING,
				_("could not remove tmpdir %s\n"), tmpdir);
	}

	free(scriptfn);
	free(tmpdir);
	return retval;
}

int SYMEXPORT klpm_trans_get_flags(klpm_handle_t *handle)
{
	/* Sanity checks */
	CHECK_HANDLE(handle, return -1);
	ASSERT(handle->trans != NULL, RET_ERR(handle, KUZPKG_ERR_TRANS_NULL, -1));

	return handle->trans->flags;
}

klpm_list_t SYMEXPORT *klpm_trans_get_add(klpm_handle_t *handle)
{
	/* Sanity checks */
	CHECK_HANDLE(handle, return NULL);
	ASSERT(handle->trans != NULL, RET_ERR(handle, KUZPKG_ERR_TRANS_NULL, NULL));

	return handle->trans->add;
}

klpm_list_t SYMEXPORT *klpm_trans_get_remove(klpm_handle_t *handle)
{
	/* Sanity checks */
	CHECK_HANDLE(handle, return NULL);
	ASSERT(handle->trans != NULL, RET_ERR(handle, KUZPKG_ERR_TRANS_NULL, NULL));

	return handle->trans->remove;
}
