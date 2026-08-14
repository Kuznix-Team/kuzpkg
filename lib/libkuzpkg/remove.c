/*
 *  remove.c
 *
 *  Copyright (C) 2026 Kuznix
 *  Copyright (c) 2002-2006 by Judd Vinet <jvinet@zeroflux.org>
 *  Copyright (c) 2005 by Aurelien Foret <orelien@chez.com>
 *  Copyright (c) 2005 by Christian Hamar <krics@linuxforum.hu>
 *  Copyright (c) 2006 by David Kimpe <dnaku@frugalware.org>
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
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <dirent.h>
#include <regex.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

/* libkuzpkg */
#include "remove.h"
#include "klpm_list.h"
#include "klpm.h"
#include "trans.h"
#include "util.h"
#include "log.h"
#include "backup.h"
#include "package.h"
#include "db.h"
#include "deps.h"
#include "handle.h"
#include "filelist.h"

int SYMEXPORT klpm_remove_pkg(klpm_handle_t *handle, klpm_pkg_t *pkg)
{
	const char *pkgname;
	klpm_trans_t *trans;
	klpm_pkg_t *copy;

	/* Sanity checks */
	CHECK_HANDLE(handle, return -1);
	ASSERT(pkg != NULL, RET_ERR(handle, KUZPKG_ERR_WRONG_ARGS, -1));
	ASSERT(pkg->origin == KUZPKG_PKG_FROM_LOCALDB,
			RET_ERR(handle, KUZPKG_ERR_WRONG_ARGS, -1));
	ASSERT(handle == pkg->handle, RET_ERR(handle, KUZPKG_ERR_WRONG_ARGS, -1));
	trans = handle->trans;
	ASSERT(trans != NULL, RET_ERR(handle, KUZPKG_ERR_TRANS_NULL, -1));
	ASSERT(trans->state == STATE_INITIALIZED,
			RET_ERR(handle, KUZPKG_ERR_TRANS_NOT_INITIALIZED, -1));

	pkgname = pkg->name;

	if(klpm_pkg_find(trans->remove, pkgname)) {
		_klpm_log(handle, KUZPKG_LOG_DEBUG, "skipping duplicate target: %s\n", pkgname);
		return 0;
	}

	_klpm_log(handle, KUZPKG_LOG_DEBUG, "adding package %s to the transaction remove list\n",
			pkgname);
	if(_klpm_pkg_dup(pkg, &copy) == -1) {
		return -1;
	}
	trans->remove = klpm_list_add(trans->remove, copy);
	return 0;
}

/**
 * @brief Add dependencies to the removal transaction for cascading.
 *
 * @param handle the context handle
 * @param lp list of missing dependencies caused by the removal transaction
 *
 * @return 0 on success, -1 on error
 */
static int remove_prepare_cascade(klpm_handle_t *handle, klpm_list_t *lp)
{
	klpm_trans_t *trans = handle->trans;

	while(lp) {
		klpm_list_t *i;
		for(i = lp; i; i = i->next) {
			klpm_depmissing_t *miss = i->data;
			klpm_pkg_t *info = _klpm_db_get_pkgfromcache(handle->db_local, miss->target);
			if(info) {
				klpm_pkg_t *copy;
				if(!klpm_pkg_find(trans->remove, info->name)) {
					_klpm_log(handle, KUZPKG_LOG_DEBUG, "pulling %s in target list\n",
							info->name);
					if(_klpm_pkg_dup(info, &copy) == -1) {
						return -1;
					}
					trans->remove = klpm_list_add(trans->remove, copy);
				}
			} else {
				_klpm_log(handle, KUZPKG_LOG_ERROR,
						_("could not find %s in database -- skipping\n"), miss->target);
			}
		}
		klpm_list_free_inner(lp, (klpm_list_fn_free)klpm_depmissing_free);
		klpm_list_free(lp);
		lp = klpm_checkdeps(handle, _klpm_db_get_pkgcache(handle->db_local),
				trans->remove, NULL, 1);
	}
	return 0;
}

/**
 * @brief Remove needed packages from the removal transaction.
 *
 * @param handle the context handle
 * @param lp list of missing dependencies caused by the removal transaction
 */
static void remove_prepare_keep_needed(klpm_handle_t *handle, klpm_list_t *lp)
{
	klpm_trans_t *trans = handle->trans;

	/* Remove needed packages (which break dependencies) from target list */
	while(lp != NULL) {
		klpm_list_t *i;
		for(i = lp; i; i = i->next) {
			klpm_depmissing_t *miss = i->data;
			void *vpkg;
			klpm_pkg_t *pkg = klpm_pkg_find(trans->remove, miss->causingpkg);
			if(pkg == NULL) {
				continue;
			}
			trans->remove = klpm_list_remove(trans->remove, pkg, _klpm_pkg_cmp,
					&vpkg);
			pkg = vpkg;
			if(pkg) {
				_klpm_log(handle, KUZPKG_LOG_WARNING, _("removing %s from target list\n"),
						pkg->name);
				_klpm_pkg_free(pkg);
			}
		}
		klpm_list_free_inner(lp, (klpm_list_fn_free)klpm_depmissing_free);
		klpm_list_free(lp);
		lp = klpm_checkdeps(handle, _klpm_db_get_pkgcache(handle->db_local),
				trans->remove, NULL, 1);
	}
}

/**
 * @brief Send a callback for any optdepend being removed.
 *
 * @param handle the context handle
 * @param lp list of packages to be removed
 */
static void remove_notify_needed_optdepends(klpm_handle_t *handle, klpm_list_t *lp)
{
	klpm_list_t *i;

	for(i = _klpm_db_get_pkgcache(handle->db_local); i; i = klpm_list_next(i)) {
		klpm_pkg_t *pkg = i->data;
		klpm_list_t *optdeps = klpm_pkg_get_optdepends(pkg);

		if(optdeps && !klpm_pkg_find(lp, pkg->name)) {
			klpm_list_t *j;
			for(j = optdeps; j; j = klpm_list_next(j)) {
				klpm_depend_t *optdep = j->data;
				char *optstring = klpm_dep_compute_string(optdep);
				if(klpm_find_satisfier(lp, optstring)) {
					klpm_event_optdep_removal_t event = {
						.type = KUZPKG_EVENT_OPTDEP_REMOVAL,
						.pkg = pkg,
						.optdep = optdep
					};
					EVENT(handle, &event);
				}
				free(optstring);
			}
		}
	}
}

/**
 * @brief Transaction preparation for remove actions.
 *
 * This functions takes a pointer to a klpm_list_t which will be
 * filled with a list of klpm_depmissing_t* objects representing
 * the packages blocking the transaction.
 *
 * @param handle the context handle
 * @param data a pointer to an klpm_list_t* to fill
 *
 * @return 0 on success, -1 on error
 */
int _klpm_remove_prepare(klpm_handle_t *handle, klpm_list_t **data)
{
	klpm_list_t *lp;
	klpm_trans_t *trans = handle->trans;
	klpm_db_t *db = handle->db_local;
	klpm_event_t event;

	if((trans->flags & KUZPKG_TRANS_FLAG_RECURSE)
			&& !(trans->flags & KUZPKG_TRANS_FLAG_CASCADE)) {
		_klpm_log(handle, KUZPKG_LOG_DEBUG, "finding removable dependencies\n");
		if(_klpm_recursedeps(db, &trans->remove,
				trans->flags & KUZPKG_TRANS_FLAG_RECURSEALL)) {
			return -1;
		}
	}

	if(!(trans->flags & KUZPKG_TRANS_FLAG_NODEPS)) {
		event.type = KUZPKG_EVENT_CHECKDEPS_START;
		EVENT(handle, &event);

		_klpm_log(handle, KUZPKG_LOG_DEBUG, "looking for unsatisfied dependencies\n");
		lp = klpm_checkdeps(handle, _klpm_db_get_pkgcache(db), trans->remove, NULL, 1);
		if(lp != NULL) {

			if(trans->flags & KUZPKG_TRANS_FLAG_CASCADE) {
				if(remove_prepare_cascade(handle, lp)) {
					return -1;
				}
			} else if(trans->flags & KUZPKG_TRANS_FLAG_UNNEEDED) {
				/* Remove needed packages (which would break dependencies)
				 * from target list */
				remove_prepare_keep_needed(handle, lp);
			} else {
				if(data) {
					*data = lp;
				} else {
					klpm_list_free_inner(lp,
							(klpm_list_fn_free)klpm_depmissing_free);
					klpm_list_free(lp);
				}
				RET_ERR(handle, KUZPKG_ERR_UNSATISFIED_DEPS, -1);
			}
		}
	}

	/* -Rcs == -Rc then -Rs */
	if((trans->flags & KUZPKG_TRANS_FLAG_CASCADE)
			&& (trans->flags & KUZPKG_TRANS_FLAG_RECURSE)) {
		_klpm_log(handle, KUZPKG_LOG_DEBUG, "finding removable dependencies\n");
		if(_klpm_recursedeps(db, &trans->remove,
					trans->flags & KUZPKG_TRANS_FLAG_RECURSEALL)) {
			return -1;
		}
	}

	/* Note packages being removed that are optdepends for installed packages */
	if(!(trans->flags & KUZPKG_TRANS_FLAG_NODEPS)) {
		remove_notify_needed_optdepends(handle, trans->remove);
	}

	if(!(trans->flags & KUZPKG_TRANS_FLAG_NODEPS)) {
		event.type = KUZPKG_EVENT_CHECKDEPS_DONE;
		EVENT(handle, &event);
	}

	return 0;
}

/**
 * @brief Test if a directory is being used as a mountpoint.
 *
 * @param handle context handle
 * @param directory path to test, must be absolute and include trailing '/'
 * @param stbuf stat result for @a directory, may be NULL
 *
 * @return 0 if @a directory is not a mountpoint or on error, 1 if @a directory
 * is a mountpoint
 */
static int dir_is_mountpoint(klpm_handle_t *handle, const char *directory,
		const struct stat *stbuf)
{
	char parent_dir[PATH_MAX];
	struct stat parent_stbuf;
	dev_t dir_st_dev;

	if(stbuf == NULL) {
		struct stat dir_stbuf;
		if(stat(directory, &dir_stbuf) < 0) {
			_klpm_log(handle, KUZPKG_LOG_DEBUG,
					"failed to stat directory %s: %s\n",
					directory, strerror(errno));
			return 0;
		}
		dir_st_dev = dir_stbuf.st_dev;
	} else {
		dir_st_dev = stbuf->st_dev;
	}

	snprintf(parent_dir, PATH_MAX, "%s..", directory);
	if(stat(parent_dir, &parent_stbuf) < 0) {
		_klpm_log(handle, KUZPKG_LOG_DEBUG,
				"failed to stat parent of %s: %s: %s\n",
				directory, parent_dir, strerror(errno));
		return 0;
	}

	return dir_st_dev != parent_stbuf.st_dev;
}

/**
 * @brief Check if klpm can delete a file.
 *
 * @param handle the context handle
 * @param file file to be removed
 *
 * @return 1 if the file can be deleted, 0 if it cannot be deleted
 */
static int can_remove_file(klpm_handle_t *handle, const klpm_file_t *file)
{
	char filepath[PATH_MAX];

	snprintf(filepath, PATH_MAX, "%s%s", handle->root, file->name);

	if(file->name[strlen(file->name) - 1] == '/' &&
			dir_is_mountpoint(handle, filepath, NULL)) {
		/* we do not remove mountpoints */
		return 1;
	}

	/* If we fail write permissions due to a read-only filesystem, abort.
	 * Assume all other possible failures are covered somewhere else */
	if(_klpm_access(handle, NULL, filepath, W_OK) == -1) {
		if(errno != EACCES && errno != ETXTBSY && _klpm_access(handle, NULL, filepath, F_OK) == 0) {
			/* only return failure if the file ACTUALLY exists and we can't write to
			 * it - ignore "chmod -w" simple permission failures */
			_klpm_log(handle, KUZPKG_LOG_ERROR, _("cannot remove file '%s': %s\n"),
					filepath, strerror(errno));
			return 0;
		}
	}

	return 1;
}

static void shift_pacsave(klpm_handle_t *handle, const char *file)
{
	DIR *dir = NULL;
	struct dirent *ent;
	struct stat st;
	regex_t reg;

	const char *basename;
	char *dirname;
	char oldfile[PATH_MAX];
	char newfile[PATH_MAX];
	char regstr[PATH_MAX];

	unsigned long log_max = 0;
	size_t basename_len;

	dirname = mdirname(file);
	if(!dirname) {
		return;
	}

	basename = mbasename(file);
	basename_len = strlen(basename);

	snprintf(regstr, PATH_MAX, "^%s\\.pacsave\\.([[:digit:]]+)$", basename);
	if(regcomp(&reg, regstr, REG_EXTENDED | REG_NEWLINE) != 0) {
		goto cleanup;
	}

	dir = opendir(dirname);
	if(dir == NULL) {
		_klpm_log(handle, KUZPKG_LOG_ERROR, _("could not open directory: %s: %s\n"),
							dirname, strerror(errno));
		goto cleanup;
	}

	while((ent = readdir(dir)) != NULL) {
		if(strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
			continue;
		}

		if(regexec(&reg, ent->d_name, 0, 0, 0) == 0) {
			unsigned long cur_log;
			cur_log = strtoul(ent->d_name + basename_len + strlen(".pacsave."), NULL, 10);
			if(cur_log > log_max) {
				log_max = cur_log;
			}
		}
	}

	/* Shift pacsaves */
	unsigned long i;
	for(i = log_max + 1; i > 1; i--) {
		if(snprintf(oldfile, PATH_MAX, "%s.pacsave.%lu", file, i-1) >= PATH_MAX
				|| snprintf(newfile, PATH_MAX, "%s.pacsave.%lu", file, i) >= PATH_MAX) {
			_klpm_log(handle, KUZPKG_LOG_ERROR,
					_("could not backup %s due to PATH_MAX overflow\n"), file);
			goto cleanup;
		}
		rename(oldfile, newfile);
	}

	if(snprintf(oldfile, PATH_MAX, "%s.pacsave", file) >= PATH_MAX
			|| snprintf(newfile, PATH_MAX, "%s.1", oldfile) >= PATH_MAX) {
		_klpm_log(handle, KUZPKG_LOG_ERROR,
				_("could not backup %s due to PATH_MAX overflow\n"), file);
		goto cleanup;
	}
	if(stat(oldfile, &st) == 0) {
		rename(oldfile, newfile);
	}

	regfree(&reg);

cleanup:
	free(dirname);
	if(dir != NULL) {
		closedir(dir);
	}
}


/**
 * @brief Unlink a package file, backing it up if necessary.
 *
 * @param handle the context handle
 * @param oldpkg the package being removed
 * @param newpkg the package replacing \a oldpkg
 * @param fileobj file to remove
 * @param nosave whether files should be backed up
 *
 * @return 0 on success, -1 if there was an error unlinking the file, 1 if the
 * file was skipped or did not exist
 */
static int unlink_file(klpm_handle_t *handle, klpm_pkg_t *oldpkg,
		klpm_pkg_t *newpkg, const klpm_file_t *fileobj, int nosave)
{
	struct stat buf;
	char file[PATH_MAX];
	int file_len;

	file_len = snprintf(file, PATH_MAX, "%s%s", handle->root, fileobj->name);
	if(file_len <= 0 || file_len >= PATH_MAX) {
		/* 0 is a valid value from snprintf, but should be impossible here */
		_klpm_log(handle, KUZPKG_LOG_DEBUG, "path too long to unlink %s%s\n",
				handle->root, fileobj->name);
		return -1;
	} else if(file[file_len - 1] == '/') {
		/* trailing slashes cause errors and confusing messages if the user has
		 * replaced a directory with a symlink */
		file[file_len - 1] = '\0';
		file_len--;
	}

	if(llstat(file, &buf)) {
		_klpm_log(handle, KUZPKG_LOG_DEBUG, "file %s does not exist\n", file);
		return 1;
	}

	if(S_ISDIR(buf.st_mode)) {
		ssize_t files;

		/* restore/add trailing slash */
		if(file_len < PATH_MAX - 1) {
			file[file_len] = '/';
			file_len++;
			file[file_len] = '\0';
		} else {
			_klpm_log(handle, KUZPKG_LOG_DEBUG, "path too long to unlink %s%s\n",
					handle->root, fileobj->name);
			return -1;
		}

		files = _klpm_files_in_directory(handle, file, 0);
		if(files > 0) {
			/* if we have files, no need to remove the directory */
			_klpm_log(handle, KUZPKG_LOG_DEBUG, "keeping directory %s (contains files)\n",
					file);
		} else if(files < 0) {
			_klpm_log(handle, KUZPKG_LOG_DEBUG,
					"keeping directory %s (could not count files)\n", file);
		} else if(newpkg && klpm_filelist_contains(klpm_pkg_get_files(newpkg),
					fileobj->name)) {
			_klpm_log(handle, KUZPKG_LOG_DEBUG,
					"keeping directory %s (in new package)\n", file);
		} else if(dir_is_mountpoint(handle, file, &buf)) {
			_klpm_log(handle, KUZPKG_LOG_DEBUG,
					"keeping directory %s (mountpoint)\n", file);
		} else {
			/* one last check- does any other package own this file? */
			klpm_list_t *local, *local_pkgs;
			int found = 0;
			local_pkgs = _klpm_db_get_pkgcache(handle->db_local);
			for(local = local_pkgs; local && !found; local = local->next) {
				klpm_pkg_t *local_pkg = local->data;
				klpm_filelist_t *filelist;

				/* we duplicated the package when we put it in the removal list, so we
				 * so we can't use direct pointer comparison here. */
				if(oldpkg->name_hash == local_pkg->name_hash
						&& strcmp(oldpkg->name, local_pkg->name) == 0) {
					continue;
				}
				filelist = klpm_pkg_get_files(local_pkg);
				if(klpm_filelist_contains(filelist, fileobj->name)) {
					_klpm_log(handle, KUZPKG_LOG_DEBUG,
							"keeping directory %s (owned by %s)\n", file, local_pkg->name);
					found = 1;
				}
			}
			if(!found) {
				if(rmdir(file)) {
					_klpm_log(handle, KUZPKG_LOG_DEBUG,
							"directory removal of %s failed: %s\n", file, strerror(errno));
					return -1;
				} else {
					_klpm_log(handle, KUZPKG_LOG_DEBUG,
							"removed directory %s (no remaining owners)\n", file);
				}
			}
		}
	} else {
		/* if the file needs backup and has been modified, back it up to .pacsave */
		klpm_backup_t *backup = _klpm_needbackup(fileobj->name, oldpkg);
		if(backup) {
			if(nosave) {
				_klpm_log(handle, KUZPKG_LOG_DEBUG, "transaction is set to NOSAVE, not backing up '%s'\n", file);
			} else {
				char *filehash = klpm_compute_md5sum(file);
				int cmp = filehash ? strcmp(filehash, backup->hash) : 0;
				FREE(filehash);
				if(cmp != 0) {
					klpm_event_pacsave_created_t event = {
						.type = KUZPKG_EVENT_PACSAVE_CREATED,
						.oldpkg = oldpkg,
						.file = file
					};
					char *newpath;
					size_t len = strlen(file) + 8 + 1;
					MALLOC(newpath, len, RET_ERR(handle, KUZPKG_ERR_MEMORY, -1));
					shift_pacsave(handle, file);
					snprintf(newpath, len, "%s.pacsave", file);
					if(rename(file, newpath)) {
						_klpm_log(handle, KUZPKG_LOG_ERROR, _("could not rename %s to %s (%s)\n"),
								file, newpath, strerror(errno));
						klpm_logaction(handle, KUZPKG_CALLER_PREFIX,
								"error: could not rename %s to %s (%s)\n",
								file, newpath, strerror(errno));
						free(newpath);
						return -1;
					}
					EVENT(handle, &event);
					klpm_logaction(handle, KUZPKG_CALLER_PREFIX,
							"warning: %s saved as %s\n", file, newpath);
					free(newpath);
					return 0;
				}
			}
		}

		_klpm_log(handle, KUZPKG_LOG_DEBUG, "unlinking %s\n", file);

		if(unlink(file) == -1) {
			_klpm_log(handle, KUZPKG_LOG_ERROR, _("cannot remove %s (%s)\n"),
					file, strerror(errno));
			klpm_logaction(handle, KUZPKG_CALLER_PREFIX,
					"error: cannot remove %s (%s)\n", file, strerror(errno));
			return -1;
		}
	}
	return 0;
}

/**
 * @brief Check if a package file should be removed.
 *
 * @param handle the context handle
 * @param newpkg the package replacing the current owner of \a path
 * @param path file to be removed
 *
 * @return 1 if the file should be skipped, 0 if it should be removed
 */
static int should_skip_file(klpm_handle_t *handle,
		klpm_pkg_t *newpkg, const char *path)
{
	return _klpm_fnmatch_patterns(handle->noupgrade, path) == 0
		|| klpm_list_find_str(handle->trans->skip_remove, path)
		|| (newpkg && _klpm_needbackup(path, newpkg)
				&& klpm_filelist_contains(klpm_pkg_get_files(newpkg), path));
}

/**
 * @brief Remove a package's files, optionally skipping its replacement's
 * files.
 *
 * @param handle the context handle
 * @param oldpkg package to remove
 * @param newpkg package to replace \a oldpkg (optional)
 * @param targ_count current index within the transaction (1-based)
 * @param pkg_count the number of packages affected by the transaction
 *
 * @return 0 on success, -1 if klpm lacks permission to delete some of the
 * files, >0 the number of files klpm was unable to delete
 */
static int remove_package_files(klpm_handle_t *handle,
		klpm_pkg_t *oldpkg, klpm_pkg_t *newpkg,
		size_t targ_count, size_t pkg_count)
{
	klpm_filelist_t *filelist;
	size_t i;
	int err = 0;
	int nosave = handle->trans->flags & KUZPKG_TRANS_FLAG_NOSAVE;

	filelist = klpm_pkg_get_files(oldpkg);
	for(i = 0; i < filelist->count; i++) {
		klpm_file_t *file = filelist->files + i;
		if(!should_skip_file(handle, newpkg, file->name)
				&& !can_remove_file(handle, file)) {
			_klpm_log(handle, KUZPKG_LOG_DEBUG,
					"not removing package '%s', can't remove all files\n",
					oldpkg->name);
			RET_ERR(handle, KUZPKG_ERR_PKG_CANT_REMOVE, -1);
		}
	}

	_klpm_log(handle, KUZPKG_LOG_DEBUG, "removing %zu files\n", filelist->count);

	if(!newpkg) {
		/* init progress bar, but only on true remove transactions */
		PROGRESS(handle, KUZPKG_PROGRESS_REMOVE_START, oldpkg->name, 0,
				pkg_count, targ_count);
	}

	/* iterate through the list backwards, unlinking files */
	for(i = filelist->count; i > 0; i--) {
		klpm_file_t *file = filelist->files + i - 1;

		/* check the remove skip list before removing the file.
		 * see the big comment block in db_find_fileconflicts() for an
		 * explanation. */
		if(should_skip_file(handle, newpkg, file->name)) {
			_klpm_log(handle, KUZPKG_LOG_DEBUG,
					"%s is in skip_remove, skipping removal\n", file->name);
			continue;
		}

		if(unlink_file(handle, oldpkg, newpkg, file, nosave) < 0) {
			err++;
		}

		if(!newpkg) {
			/* update progress bar after each file */
			int percent = ((filelist->count - i) * 100) / filelist->count;
			PROGRESS(handle, KUZPKG_PROGRESS_REMOVE_START, oldpkg->name,
					percent, pkg_count, targ_count);
		}
	}

	if(!newpkg) {
		/* set progress to 100% after we finish unlinking files */
		PROGRESS(handle, KUZPKG_PROGRESS_REMOVE_START, oldpkg->name, 100,
				pkg_count, targ_count);
	}

	return err;
}

/**
 * @brief Remove a package from the filesystem.
 *
 * @param handle the context handle
 * @param oldpkg package to remove
 * @param newpkg package to replace \a oldpkg (optional)
 * @param targ_count current index within the transaction (1-based)
 * @param pkg_count the number of packages affected by the transaction
 *
 * @return 0
 */
int _klpm_remove_single_package(klpm_handle_t *handle,
		klpm_pkg_t *oldpkg, klpm_pkg_t *newpkg,
		size_t targ_count, size_t pkg_count)
{
	const char *pkgname = oldpkg->name;
	const char *pkgver = oldpkg->version;
	klpm_event_package_operation_t event = {
		.type = KUZPKG_EVENT_PACKAGE_OPERATION_START,
		.operation = KUZPKG_PACKAGE_REMOVE,
		.oldpkg = oldpkg,
		.newpkg = NULL
	};

	if(newpkg) {
		_klpm_log(handle, KUZPKG_LOG_DEBUG, "removing old package first (%s-%s)\n",
				pkgname, pkgver);
	} else {
		EVENT(handle, &event);
		_klpm_log(handle, KUZPKG_LOG_DEBUG, "removing package %s-%s\n",
				pkgname, pkgver);

		/* run the pre-remove scriptlet if it exists */
		if(klpm_pkg_has_scriptlet(oldpkg) &&
				!(handle->trans->flags & KUZPKG_TRANS_FLAG_NOSCRIPTLET)) {
			char *scriptlet = _klpm_local_db_pkgpath(handle->db_local,
					oldpkg, "install");
			_klpm_runscriptlet(handle, scriptlet, "pre_remove", pkgver, NULL, 0);
			free(scriptlet);
		}
	}

	if(!(handle->trans->flags & KUZPKG_TRANS_FLAG_DBONLY)) {
		/* TODO check returned errors if any */
		remove_package_files(handle, oldpkg, newpkg, targ_count, pkg_count);
	}

	if(!newpkg) {
		klpm_logaction(handle, KUZPKG_CALLER_PREFIX, "removed %s (%s)\n",
					oldpkg->name, oldpkg->version);
	}

	/* run the post-remove script if it exists */
	if(!newpkg && klpm_pkg_has_scriptlet(oldpkg) &&
			!(handle->trans->flags & KUZPKG_TRANS_FLAG_NOSCRIPTLET)) {
		char *scriptlet = _klpm_local_db_pkgpath(handle->db_local,
				oldpkg, "install");
		_klpm_runscriptlet(handle, scriptlet, "post_remove", pkgver, NULL, 0);
		free(scriptlet);
	}

	if(!newpkg) {
		event.type = KUZPKG_EVENT_PACKAGE_OPERATION_DONE;
		EVENT(handle, &event);
	}

	/* remove the package from the database */
	_klpm_log(handle, KUZPKG_LOG_DEBUG, "removing database entry '%s'\n", pkgname);
	if(_klpm_local_db_remove(handle->db_local, oldpkg) == -1) {
		_klpm_log(handle, KUZPKG_LOG_ERROR, _("could not remove database entry %s-%s\n"),
				pkgname, pkgver);
	}
	/* remove the package from the cache */
	if(_klpm_db_remove_pkgfromcache(handle->db_local, oldpkg) == -1) {
		_klpm_log(handle, KUZPKG_LOG_ERROR, _("could not remove entry '%s' from cache\n"),
				pkgname);
	}

	/* TODO: useful return values */
	return 0;
}

/**
 * @brief Remove packages in the current transaction.
 *
 * @param handle the context handle
 * @param run_ldconfig whether to run ld_config after removing the packages
 *
 * @return 0 on success, -1 if errors occurred while removing files
 */
int _klpm_remove_packages(klpm_handle_t *handle, int run_ldconfig)
{
	klpm_list_t *targ;
	size_t pkg_count, targ_count;
	klpm_trans_t *trans = handle->trans;
	int ret = 0;

	pkg_count = klpm_list_count(trans->remove);
	targ_count = 1;

	for(targ = trans->remove; targ; targ = targ->next) {
		klpm_pkg_t *pkg = targ->data;

		if(trans->state == STATE_INTERRUPTED) {
			return ret;
		}

		if(_klpm_remove_single_package(handle, pkg, NULL,
					targ_count, pkg_count) == -1) {
			handle->pm_errno = KUZPKG_ERR_TRANS_ABORT;
			/* running ldconfig at this point could possibly screw system */
			run_ldconfig = 0;
			ret = -1;
		}

		targ_count++;
	}

	if(run_ldconfig) {
		/* run ldconfig if it exists */
		_klpm_ldconfig(handle);
	}

	return ret;
}
