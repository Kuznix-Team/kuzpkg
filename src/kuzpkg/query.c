/*
 *  query.c
 *
 *  Copyright (C) 2026 Kuznix
 *  Copyright (c) 2002-2006 by Judd Vinet <jvinet@zeroflux.org>
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
#include <stdint.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include <klpm.h>
#include <klpm_list.h>

/* kuzpkg */
#include "kuzpkg.h"
#include "package.h"
#include "check.h"
#include "conf.h"
#include "util.h"

#define LOCAL_PREFIX "local/"

/* check if filename exists in PATH */
static int search_path(char **filename, struct stat *bufptr)
{
	char *envpath, *envpathsplit, *path, *fullname;
	size_t flen;

	if((envpath = getenv("PATH")) == NULL) {
		return -1;
	}
	if((envpath = envpathsplit = strdup(envpath)) == NULL) {
		return -1;
	}

	flen = strlen(*filename);

	while((path = strsep(&envpathsplit, ":")) != NULL) {
		size_t plen = strlen(path);

		/* strip the trailing slash if one exists */
		while(path[plen - 1] == '/') {
			path[--plen] = '\0';
		}

		fullname = malloc(plen + flen + 2);
		if(!fullname) {
			free(envpath);
			return -1;
		}
		snprintf(fullname, plen + flen + 2, "%s/%s", path, *filename);

		if(lstat(fullname, bufptr) == 0) {
			free(*filename);
			*filename = fullname;
			free(envpath);
			return 0;
		}
		free(fullname);
	}
	free(envpath);
	return -1;
}

static void print_query_fileowner(const char *filename, klpm_pkg_t *info)
{
	if(!config->quiet) {
		const colstr_t *colstr = &config->colstr;
		printf(_("%s is owned by %s%s %s%s%s\n"), filename, colstr->title,
				klpm_pkg_get_name(info), colstr->version, klpm_pkg_get_version(info),
				colstr->nocolor);
	} else {
		printf("%s\n", klpm_pkg_get_name(info));
	}
}

/** Resolve the canonicalized absolute path of a symlink.
 * @param path path to resolve
 * @param resolved_path destination for the resolved path, will be malloc'd if
 * NULL
 * @return the resolved path
 */
static char *lrealpath(const char *path, char *resolved_path)
{
	const char *bname = mbasename(path);
	char *rpath = NULL, *dname = NULL;
	int success = 0;

	if(strcmp(bname, ".") == 0 || strcmp(bname, "..") == 0) {
		/* the entire path needs to be resolved */
		return realpath(path, resolved_path);
	}

	if(!(dname = mdirname(path))) {
		goto cleanup;
	}
	if(!(rpath = realpath(dname, NULL))) {
		goto cleanup;
	}
	if(!resolved_path) {
		if(!(resolved_path = malloc(strlen(rpath) + strlen(bname) + 2))) {
			goto cleanup;
		}
	}

	strcpy(resolved_path, rpath);
	if(resolved_path[strlen(resolved_path) - 1] != '/') {
		strcat(resolved_path, "/");
	}
	strcat(resolved_path, bname);
	success = 1;

cleanup:
	free(dname);
	free(rpath);

	return (success ? resolved_path : NULL);
}

static int query_fileowner(klpm_list_t *targets)
{
	int ret = 0;
	const char *root = klpm_option_get_root(config->handle);
	size_t rootlen = strlen(root);
	klpm_list_t *t;
	klpm_db_t *db_local;
	klpm_list_t *packages;

	/* This code is here for safety only */
	if(targets == NULL) {
		pm_printf(KUZPKG_LOG_ERROR, _("no file was specified for --owns\n"));
		return 1;
	}

	db_local = klpm_get_localdb(config->handle);
	packages = klpm_db_get_pkgcache(db_local);

	for(t = targets; t; t = klpm_list_next(t)) {
		char *filename = NULL;
		char rpath[PATH_MAX], *rel_path;
		struct stat buf;
		klpm_list_t *i;
		size_t len;
		unsigned int found = 0;
		int is_dir = 0, is_missing = 0;

		if((filename = strdup(t->data)) == NULL) {
			goto targcleanup;
		}

		if(strcmp(filename, "") == 0) {
			pm_printf(KUZPKG_LOG_ERROR, _("empty string passed to file owner query\n"));
			goto targcleanup;
		}

		/* trailing '/' causes lstat to dereference directory symlinks */
		len = strlen(filename) - 1;
		while(len > 0 && filename[len] == '/') {
			filename[len--] = '\0';
			/* If a non-dir file exists, S_ISDIR will correct this later. */
			is_dir = 1;
		}

		if(lstat(filename, &buf) == -1) {
			is_missing = 1;
			/* if it is not a path but a program name, then check in PATH */
			if ((strchr(filename, '/') == NULL) && (search_path(&filename, &buf) == 0)) {
				is_missing = 0;
			}
		}

		if(!lrealpath(filename, rpath)) {
			/* Can't canonicalize path, try to proceed anyway */
			strcpy(rpath, filename);
		}

		if(strncmp(rpath, root, rootlen) != 0) {
			/* file is outside root, we know nothing can own it */
			pm_printf(KUZPKG_LOG_ERROR, _("No package owns %s\n"), filename);
			goto targcleanup;
		}

		rel_path = rpath + rootlen;

		if((is_missing && is_dir) || (!is_missing && (is_dir = S_ISDIR(buf.st_mode)))) {
			size_t rlen = strlen(rpath);
			if(rlen + 2 >= PATH_MAX) {
					pm_printf(KUZPKG_LOG_ERROR, _("path too long: %s/\n"), rpath);
					goto targcleanup;
			}
			strcat(rpath + rlen, "/");
		}

		for(i = packages; i && (!found || is_dir); i = klpm_list_next(i)) {
			if(klpm_filelist_contains(klpm_pkg_get_files(i->data), rel_path)) {
				print_query_fileowner(rpath, i->data);
				found = 1;
			}
		}
		if(!found) {
			pm_printf(KUZPKG_LOG_ERROR, _("No package owns %s\n"), filename);
		}

targcleanup:
		if(!found) {
			ret++;
		}
		free(filename);
	}

	return ret;
}

/* search the local database for a matching package */
static int query_search(klpm_list_t *targets)
{
	klpm_db_t *db_local = klpm_get_localdb(config->handle);
	int ret = dump_pkg_search(db_local, targets, 0);
	if(ret == -1) {
		klpm_errno_t err = klpm_errno(config->handle);
		pm_printf(KUZPKG_LOG_ERROR, "search failed: %s\n", klpm_strerror(err));
		return 1;
	}

	return ret;

}

static unsigned short pkg_get_locality(klpm_pkg_t *pkg)
{
	const char *pkgname = klpm_pkg_get_name(pkg);
	klpm_list_t *j;
	klpm_list_t *sync_dbs = klpm_get_syncdbs(config->handle);

	for(j = sync_dbs; j; j = klpm_list_next(j)) {
		if(klpm_db_get_pkg(j->data, pkgname)) {
			return PKG_LOCALITY_NATIVE;
		}
	}
	return PKG_LOCALITY_FOREIGN;
}

static int is_unrequired(klpm_pkg_t *pkg, unsigned short level)
{
	klpm_list_t *requiredby = klpm_pkg_compute_requiredby(pkg);
	if(requiredby == NULL) {
		if(level == 1) {
			requiredby = klpm_pkg_compute_optionalfor(pkg);
		}
		if(requiredby == NULL) {
			return 1;
		}
	}
	FREELIST(requiredby);
	return 0;
}

static int filter(klpm_pkg_t *pkg)
{
	/* check if this package was explicitly installed */
	if(config->op_q_explicit &&
			klpm_pkg_get_reason(pkg) != KUZPKG_PKG_REASON_EXPLICIT) {
		return 0;
	}
	/* check if this package was installed as a dependency */
	if(config->op_q_deps &&
			klpm_pkg_get_reason(pkg) != KUZPKG_PKG_REASON_DEPEND) {
		return 0;
	}
	/* check if this pkg is or isn't in a sync DB */
	if(config->op_q_locality && config->op_q_locality != pkg_get_locality(pkg)) {
		return 0;
	}
	/* check if this pkg is unrequired */
	if(config->op_q_unrequired && !is_unrequired(pkg, config->op_q_unrequired)) {
		return 0;
	}
	/* check if this pkg is outdated */
	if(config->op_q_upgrade && (klpm_sync_get_new_version(pkg,
					klpm_get_syncdbs(config->handle)) == NULL)) {
		return 0;
	}
	return 1;
}

static int display(klpm_pkg_t *pkg)
{
	int ret = 0;

	if(config->op_q_info) {
		dump_pkg_full(pkg, config->op_q_info > 1);
	}
	if(config->op_q_list) {
		dump_pkg_files(pkg, config->quiet);
	}
	if(config->op_q_changelog) {
		dump_pkg_changelog(pkg);
	}
	if(config->op_q_check) {
		if(config->op_q_check == 1) {
			ret = check_pkg_fast(pkg);
		} else {
			ret = check_pkg_full(pkg);
		}
	}
	if(!config->op_q_info && !config->op_q_list
			&& !config->op_q_changelog && !config->op_q_check) {
		if(!config->quiet) {
			const colstr_t *colstr = &config->colstr;
			printf("%s%s %s%s%s", colstr->title, klpm_pkg_get_name(pkg),
					colstr->version, klpm_pkg_get_version(pkg), colstr->nocolor);

			if(config->op_q_upgrade) {
				int usage;
				klpm_pkg_t *newpkg = klpm_sync_get_new_version(pkg, klpm_get_syncdbs(config->handle));
				klpm_db_t *db = klpm_pkg_get_db(newpkg);
				klpm_db_get_usage(db, &usage);
				
				printf(" -> %s%s%s", colstr->version, klpm_pkg_get_version(newpkg), colstr->nocolor);

				if(klpm_pkg_should_ignore(config->handle, pkg) || !(usage & KUZPKG_DB_USAGE_UPGRADE)) {
					printf(" %s", _("[ignored]"));
				}
			}

			printf("\n");
		} else {
			printf("%s\n", klpm_pkg_get_name(pkg));
		}
	}
	return ret;
}

static int query_group(klpm_list_t *targets)
{
	klpm_list_t *i, *j;
	const char *grpname = NULL;
	int ret = 0;
	klpm_db_t *db_local = klpm_get_localdb(config->handle);

	if(targets == NULL) {
		for(j = klpm_db_get_groupcache(db_local); j; j = klpm_list_next(j)) {
			klpm_group_t *grp = j->data;
			const klpm_list_t *p;

			for(p = grp->packages; p; p = klpm_list_next(p)) {
				klpm_pkg_t *pkg = p->data;
				if(!filter(pkg)) {
					continue;
				}
				printf("%s %s\n", grp->name, klpm_pkg_get_name(pkg));
			}
		}
	} else {
		for(i = targets; i; i = klpm_list_next(i)) {
			klpm_group_t *grp;
			grpname = i->data;
			grp = klpm_db_get_group(db_local, grpname);
			if(grp) {
				const klpm_list_t *p;
				for(p = grp->packages; p; p = klpm_list_next(p)) {
					if(!filter(p->data)) {
						continue;
					}
					if(!config->quiet) {
						printf("%s %s\n", grpname,
								klpm_pkg_get_name(p->data));
					} else {
						printf("%s\n", klpm_pkg_get_name(p->data));
					}
				}
			} else {
				pm_printf(KUZPKG_LOG_ERROR, _("group '%s' was not found\n"), grpname);
				ret++;
			}
		}
	}
	return ret;
}

int kuzpkg_query(klpm_list_t *targets)
{
	int ret = 0;
	int match = 0;
	klpm_list_t *i;
	klpm_pkg_t *pkg = NULL;
	klpm_db_t *db_local;

	/* First: operations that do not require targets */

	/* search for a package */
	if(config->op_q_search) {
		ret = query_search(targets);
		return ret;
	}

	/* looking for groups */
	if(config->group) {
		ret = query_group(targets);
		return ret;
	}

	if(config->op_q_locality || config->op_q_upgrade) {
		if(check_syncdbs(1, 1)) {
			return 1;
		}
	}

	db_local = klpm_get_localdb(config->handle);

	/* operations on all packages in the local DB
	 * valid: no-op (plain -Q), list, info, check
	 * invalid: isfile, owns */
	if(targets == NULL) {
		if(config->op_q_isfile || config->op_q_owns) {
			pm_printf(KUZPKG_LOG_ERROR, _("no targets specified (use -h for help)\n"));
			return 1;
		}

		for(i = klpm_db_get_pkgcache(db_local); i; i = klpm_list_next(i)) {
			pkg = i->data;
			if(filter(pkg)) {
				int value = display(pkg);
				if(value != 0) {
					ret = 1;
				}
				match = 1;
			}
		}
		if(!match) {
			ret = 1;
		}
		return ret;
	}

	/* Second: operations that require target(s) */

	/* determine the owner of a file */
	if(config->op_q_owns) {
		ret = query_fileowner(targets);
		return ret;
	}

	/* operations on named packages in the local DB
	 * valid: no-op (plain -Q), list, info, check */
	for(i = targets; i; i = klpm_list_next(i)) {
		const char *strname = i->data;

		if(config->op_q_isfile) {
			klpm_pkg_load(config->handle, strname, 1, 0, &pkg);

			if(pkg == NULL) {
				pm_printf(KUZPKG_LOG_ERROR,
						_("could not load package '%s': %s\n"), strname,
						klpm_strerror(klpm_errno(config->handle)));
			}
		} else {
			/* strip leading part of "local/pkgname" */
			if(strncmp(strname, LOCAL_PREFIX, strlen(LOCAL_PREFIX)) == 0) {
				strname += strlen(LOCAL_PREFIX);
			}

			pkg = klpm_db_get_pkg(db_local, strname);
			if(pkg == NULL) {
				pkg = klpm_find_satisfier(klpm_db_get_pkgcache(db_local), strname);
			}

			if(pkg == NULL) {
				pm_printf(KUZPKG_LOG_ERROR,
						_("package '%s' was not found\n"), strname);
				if(!config->op_q_isfile && access(strname, R_OK) == 0) {
					pm_printf(KUZPKG_LOG_WARNING,
							_("'%s' is a file, you might want to use %s.\n"),
							strname, "-p/--file");
				}
			}
		}

		if(pkg == NULL) {
			ret = 1;
			continue;
		}

		if(filter(pkg)) {
			int value = display(pkg);
			if(value != 0) {
				ret = 1;
			}
			match = 1;
		}

		if(config->op_q_isfile) {
			klpm_pkg_free(pkg);
			pkg = NULL;
		}
	}

	if(!match) {
		ret = 1;
	}

	return ret;
}
