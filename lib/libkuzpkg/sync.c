/*
 *  sync.c
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

#include <sys/types.h> /* off_t */
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h> /* intmax_t */
#include <unistd.h>
#include <limits.h>

/* libkuzpkg */
#include "sync.h"
#include "klpm_list.h"
#include "log.h"
#include "package.h"
#include "db.h"
#include "deps.h"
#include "conflict.h"
#include "trans.h"
#include "add.h"
#include "util.h"
#include "handle.h"
#include "klpm.h"
#include "dload.h"
#include "remove.h"
#include "diskspace.h"
#include "signing.h"

struct keyinfo_t {
       char* uid;
       char* keyid;
};

klpm_pkg_t SYMEXPORT *klpm_sync_get_new_version(klpm_pkg_t *pkg, klpm_list_t *dbs_sync)
{
	klpm_list_t *i;
	klpm_pkg_t *spkg = NULL;

	ASSERT(pkg != NULL, return NULL);
	pkg->handle->pm_errno = KUZPKG_ERR_OK;

	for(i = dbs_sync; !spkg && i; i = i->next) {
		klpm_db_t *db = i->data;
		spkg = _klpm_db_get_pkgfromcache(db, pkg->name);
	}

	if(spkg == NULL) {
		_klpm_log(pkg->handle, KUZPKG_LOG_DEBUG, "'%s' not found in sync db => no upgrade\n",
				pkg->name);
		return NULL;
	}

	/* compare versions and see if spkg is an upgrade */
	if(_klpm_pkg_compare_versions(spkg, pkg) > 0) {
		_klpm_log(pkg->handle, KUZPKG_LOG_DEBUG, "new version of '%s' found (%s => %s)\n",
					pkg->name, pkg->version, spkg->version);
		return spkg;
	}
	/* spkg is not an upgrade */
	return NULL;
}

static int check_literal(klpm_handle_t *handle, klpm_pkg_t *lpkg,
		klpm_pkg_t *spkg, int enable_downgrade)
{
	/* 1. literal was found in sdb */
	int cmp = _klpm_pkg_compare_versions(spkg, lpkg);
	if(cmp > 0) {
		_klpm_log(handle, KUZPKG_LOG_DEBUG, "new version of '%s' found (%s => %s)\n",
				lpkg->name, lpkg->version, spkg->version);
		/* check IgnorePkg/IgnoreGroup */
		if(klpm_pkg_should_ignore(handle, spkg)
				|| klpm_pkg_should_ignore(handle, lpkg)) {
			_klpm_log(handle, KUZPKG_LOG_WARNING, _("%s: ignoring package upgrade (%s => %s)\n"),
					lpkg->name, lpkg->version, spkg->version);
		} else {
			_klpm_log(handle, KUZPKG_LOG_DEBUG, "adding package %s-%s to the transaction targets\n",
					spkg->name, spkg->version);
			return 1;
		}
	} else if(cmp < 0) {
		if(enable_downgrade) {
			/* check IgnorePkg/IgnoreGroup */
			if(klpm_pkg_should_ignore(handle, spkg)
					|| klpm_pkg_should_ignore(handle, lpkg)) {
				_klpm_log(handle, KUZPKG_LOG_WARNING, _("%s: ignoring package downgrade (%s => %s)\n"),
						lpkg->name, lpkg->version, spkg->version);
			} else {
				_klpm_log(handle, KUZPKG_LOG_WARNING, _("%s: downgrading from version %s to version %s\n"),
						lpkg->name, lpkg->version, spkg->version);
				return 1;
			}
		} else {
			klpm_db_t *sdb = klpm_pkg_get_db(spkg);
			_klpm_log(handle, KUZPKG_LOG_WARNING, _("%s: local (%s) is newer than %s (%s)\n"),
					lpkg->name, lpkg->version, sdb->treename, spkg->version);
		}
	}
	return 0;
}

static klpm_list_t *check_replacers(klpm_handle_t *handle, klpm_pkg_t *lpkg,
		klpm_db_t *sdb)
{
	/* 2. search for replacers in sdb */
	klpm_list_t *replacers = NULL;
	klpm_list_t *k;
	_klpm_log(handle, KUZPKG_LOG_DEBUG,
			"searching for replacements for %s in %s\n",
			lpkg->name, sdb->treename);
	for(k = _klpm_db_get_pkgcache(sdb); k; k = k->next) {
		int found = 0;
		klpm_pkg_t *spkg = k->data;
		klpm_list_t *l;
		for(l = klpm_pkg_get_replaces(spkg); l; l = l->next) {
			klpm_depend_t *replace = l->data;
			/* we only want to consider literal matches at this point. */
			if(_klpm_depcmp_literal(lpkg, replace)) {
				found = 1;
				break;
			}
		}
		if(found) {
			klpm_question_replace_t question = {
				.type = KUZPKG_QUESTION_REPLACE_PKG,
				.replace = 0,
				.oldpkg = lpkg,
				.newpkg = spkg,
				.newdb = sdb
			};
			klpm_pkg_t *tpkg;
			/* check IgnorePkg/IgnoreGroup */
			if(klpm_pkg_should_ignore(handle, spkg)
					|| klpm_pkg_should_ignore(handle, lpkg)) {
				_klpm_log(handle, KUZPKG_LOG_WARNING,
						_("ignoring package replacement (%s-%s => %s-%s)\n"),
						lpkg->name, lpkg->version, spkg->name, spkg->version);
				continue;
			}

			QUESTION(handle, &question);
			if(!question.replace) {
				continue;
			}

			/* If spkg is already in the target list, we append lpkg to spkg's
			 * removes list */
			tpkg = klpm_pkg_find(handle->trans->add, spkg->name);
			if(tpkg) {
				/* sanity check, multiple repos can contain spkg->name */
				if(tpkg->origin_data.db != sdb) {
					_klpm_log(handle, KUZPKG_LOG_WARNING, _("cannot replace %s by %s\n"),
							lpkg->name, spkg->name);
					continue;
				}
				_klpm_log(handle, KUZPKG_LOG_DEBUG, "appending %s to the removes list of %s\n",
						lpkg->name, tpkg->name);
				tpkg->removes = klpm_list_add(tpkg->removes, lpkg);
				/* check the to-be-replaced package's reason field */
				if(klpm_pkg_get_reason(lpkg) == KUZPKG_PKG_REASON_EXPLICIT) {
					tpkg->reason = KUZPKG_PKG_REASON_EXPLICIT;
				}
			} else {
				/* add spkg to the target list */
				/* copy over reason */
				spkg->reason = klpm_pkg_get_reason(lpkg);
				spkg->removes = klpm_list_add(NULL, lpkg);
				_klpm_log(handle, KUZPKG_LOG_DEBUG,
						"adding package %s-%s to the transaction targets\n",
						spkg->name, spkg->version);
				replacers = klpm_list_add(replacers, spkg);
			}
		}
	}
	return replacers;
}

int SYMEXPORT klpm_sync_sysupgrade(klpm_handle_t *handle, int enable_downgrade)
{
	klpm_list_t *i, *j;
	klpm_trans_t *trans;

	CHECK_HANDLE(handle, return -1);
	trans = handle->trans;
	ASSERT(trans != NULL, RET_ERR(handle, KUZPKG_ERR_TRANS_NULL, -1));
	ASSERT(trans->state == STATE_INITIALIZED, RET_ERR(handle, KUZPKG_ERR_TRANS_NOT_INITIALIZED, -1));

	_klpm_log(handle, KUZPKG_LOG_DEBUG, "checking for package upgrades\n");
	for(i = _klpm_db_get_pkgcache(handle->db_local); i; i = i->next) {
		klpm_pkg_t *lpkg = i->data;

		if(klpm_pkg_find(trans->remove, lpkg->name)) {
			_klpm_log(handle, KUZPKG_LOG_DEBUG, "%s is marked for removal -- skipping\n", lpkg->name);
			continue;
		}

		if(klpm_pkg_find(trans->add, lpkg->name)) {
			_klpm_log(handle, KUZPKG_LOG_DEBUG, "%s is already in the target list -- skipping\n", lpkg->name);
			continue;
		}

		/* Search for replacers then literal (if no replacer) in each sync database. */
		for(j = handle->dbs_sync; j; j = j->next) {
			klpm_db_t *sdb = j->data;
			klpm_list_t *replacers;

			if(!(sdb->usage & KUZPKG_DB_USAGE_UPGRADE)) {
				continue;
			}

			/* Check sdb */
			replacers = check_replacers(handle, lpkg, sdb);
			if(replacers) {
				trans->add = klpm_list_join(trans->add, replacers);
				/* jump to next local package */
				break;
			} else {
				klpm_pkg_t *spkg = _klpm_db_get_pkgfromcache(sdb, lpkg->name);
				if(spkg) {
					if(check_literal(handle, lpkg, spkg, enable_downgrade)) {
						trans->add = klpm_list_add(trans->add, spkg);
					}
					/* jump to next local package */
					break;
				}
			}
		}
	}

	return 0;
}

klpm_list_t SYMEXPORT *klpm_find_group_pkgs(klpm_list_t *dbs,
		const char *name)
{
	klpm_list_t *i, *j, *pkgs = NULL, *ignorelist = NULL;

	for(i = dbs; i; i = i->next) {
		klpm_db_t *db = i->data;
		klpm_group_t *grp = klpm_db_get_group(db, name);

		if(!grp) {
			continue;
		}

		for(j = grp->packages; j; j = j->next) {
			klpm_pkg_t *pkg = j->data;
			klpm_trans_t *trans = db->handle->trans;

			if(klpm_pkg_find(ignorelist, pkg->name)) {
				continue;
			}
			if(trans != NULL && trans->flags & KUZPKG_TRANS_FLAG_NEEDED) {
				klpm_pkg_t *local = _klpm_db_get_pkgfromcache(db->handle->db_local, pkg->name);
				if(local && _klpm_pkg_compare_versions(pkg, local) == 0) {
					/* with the NEEDED flag, packages up to date are not reinstalled */
					_klpm_log(db->handle, KUZPKG_LOG_WARNING, _("%s-%s is up to date -- skipping\n"),
							local->name, local->version);
					ignorelist = klpm_list_add(ignorelist, pkg);
					continue;
				}
			}
			if(klpm_pkg_should_ignore(db->handle, pkg)) {
				klpm_question_install_ignorepkg_t question = {
					.type = KUZPKG_QUESTION_INSTALL_IGNOREPKG,
					.install = 0,
					.pkg = pkg
				};
				ignorelist = klpm_list_add(ignorelist, pkg);
				QUESTION(db->handle, &question);
				if(!question.install) {
					continue;
				}
			}
			if(!klpm_pkg_find(pkgs, pkg->name)) {
				pkgs = klpm_list_add(pkgs, pkg);
			}
		}
	}
	klpm_list_free(ignorelist);
	return pkgs;
}

/** Compute the size of the files that will be downloaded to install a
 * package.
 * @param newpkg the new package to upgrade to
 */
static int compute_download_size(klpm_pkg_t *newpkg)
{
	const char *fname;
	char *fpath, *fnamepart = NULL;
	off_t size = 0;
	klpm_handle_t *handle = newpkg->handle;
	int ret = 0;
	size_t fnamepartlen = 0;

	if(newpkg->origin != KUZPKG_PKG_FROM_SYNCDB) {
		newpkg->infolevel |= INFRQ_DSIZE;
		newpkg->download_size = 0;
		return 0;
	}

	ASSERT(newpkg->filename != NULL, RET_ERR(handle, KUZPKG_ERR_PKG_INVALID_NAME, -1));
	fname = newpkg->filename;
	fpath = _klpm_filecache_find(handle, fname);

	/* downloaded file exists, so there's nothing to grab */
	if(fpath) {
		size = 0;
		goto finish;
	}

	fnamepartlen = strlen(fname) + 6;
	CALLOC(fnamepart, fnamepartlen, sizeof(char), return -1);
	snprintf(fnamepart, fnamepartlen, "%s.part", fname);
	fpath = _klpm_filecache_find(handle, fnamepart);
	if(fpath) {
		struct stat st;
		if(stat(fpath, &st) == 0) {
			/* subtract the size of the .part file */
			_klpm_log(handle, KUZPKG_LOG_DEBUG, "using (package - .part) size\n");
			size = newpkg->size - st.st_size;
			size = size < 0 ? 0 : size;
		}

		/* tell the caller that we have a partial */
		ret = 1;
	} else {
		size = newpkg->size;
	}

finish:
	_klpm_log(handle, KUZPKG_LOG_DEBUG, "setting download size %jd for pkg %s\n",
			(intmax_t)size, newpkg->name);

	newpkg->infolevel |= INFRQ_DSIZE;
	newpkg->download_size = size;

	FREE(fpath);
	FREE(fnamepart);

	return ret;
}

int _klpm_sync_prepare(klpm_handle_t *handle, klpm_list_t **data)
{
	klpm_list_t *i, *j;
	klpm_list_t *deps = NULL;
	klpm_list_t *unresolvable = NULL;
	int from_sync = 0;
	int ret = 0;
	klpm_trans_t *trans = handle->trans;
	klpm_event_t event;

	if(data) {
		*data = NULL;
	}

	for(i = trans->add; i; i = i->next) {
		klpm_pkg_t *spkg = i->data;
		if (spkg->origin == KUZPKG_PKG_FROM_SYNCDB){
			from_sync = 1;
			break;
		}
	}

	/* ensure all sync database are valid if we will be using them */
	for(i = handle->dbs_sync; i; i = i->next) {
		const klpm_db_t *db = i->data;
		if(db->status & DB_STATUS_INVALID) {
			RET_ERR(handle, KUZPKG_ERR_DB_INVALID, -1);
		}
		/* missing databases are not allowed if we have sync targets */
		if(from_sync && db->status & DB_STATUS_MISSING) {
			RET_ERR(handle, KUZPKG_ERR_DB_NOT_FOUND, -1);
		}
	}

	if(!(trans->flags & KUZPKG_TRANS_FLAG_NODEPS)) {
		klpm_list_t *resolved = NULL;
		klpm_list_t *remove = klpm_list_copy(trans->remove);
		klpm_list_t *localpkgs;

		/* Build up list by repeatedly resolving each transaction package */
		/* Resolve targets dependencies */
		event.type = KUZPKG_EVENT_RESOLVEDEPS_START;
		EVENT(handle, &event);
		_klpm_log(handle, KUZPKG_LOG_DEBUG, "resolving target's dependencies\n");

		/* build remove list for resolvedeps */
		for(i = trans->add; i; i = i->next) {
			klpm_pkg_t *spkg = i->data;
			for(j = spkg->removes; j; j = j->next) {
				remove = klpm_list_add(remove, j->data);
			}
		}

		/* Compute the fake local database for resolvedeps (partial fix for the
		 * phonon/qt issue) */
		localpkgs = klpm_list_diff(_klpm_db_get_pkgcache(handle->db_local),
				trans->add, _klpm_pkg_cmp);

		/* Resolve packages in the transaction one at a time, in addition
		   building up a list of packages which could not be resolved. */
		for(i = trans->add; i; i = i->next) {
			klpm_pkg_t *pkg = i->data;
			if(_klpm_resolvedeps(handle, localpkgs, pkg, trans->add,
						&resolved, remove, data) == -1) {
				unresolvable = klpm_list_add(unresolvable, pkg);
			}
			/* Else, [resolved] now additionally contains [pkg] and all of its
			   dependencies not already on the list */
		}
		klpm_list_free(localpkgs);
		klpm_list_free(remove);

		/* If there were unresolvable top-level packages, prompt the user to
		   see if they'd like to ignore them rather than failing the sync */
		if(unresolvable != NULL) {
			klpm_question_remove_pkgs_t question = {
				.type = KUZPKG_QUESTION_REMOVE_PKGS,
				.skip = 0,
				.packages = unresolvable
			};
			QUESTION(handle, &question);
			if(question.skip) {
				/* User wants to remove the unresolvable packages from the
				   transaction. The packages will be removed from the actual
				   transaction when the transaction packages are replaced with a
				   dependency-reordered list below */
				handle->pm_errno = KUZPKG_ERR_OK;
				if(data) {
					klpm_list_free_inner(*data,
							(klpm_list_fn_free)klpm_depmissing_free);
					klpm_list_free(*data);
					*data = NULL;
				}
			} else {
				/* pm_errno was set by resolvedeps, callback may have overwrote it */
				klpm_list_free(resolved);
				klpm_list_free(unresolvable);
				ret = -1;
				GOTO_ERR(handle, KUZPKG_ERR_UNSATISFIED_DEPS, cleanup);
			}
		}

		/* Ensure two packages don't have the same filename */
		for(i = resolved; i; i = i->next) {
			klpm_pkg_t *pkg1 = i->data;
			for(j = i->next; j; j = j->next) {
				klpm_pkg_t *pkg2 = j->data;
				if(strcmp(pkg1->filename, pkg2->filename) == 0) {
					ret = -1;
					handle->pm_errno = KUZPKG_ERR_TRANS_DUP_FILENAME;
					_klpm_log(handle, KUZPKG_LOG_ERROR, _("packages %s and %s have the same filename: %s\n"),
						pkg1->name, pkg2->name, pkg1->filename);
				}
			}
		}

		if(ret != 0) {
			klpm_list_free(resolved);
			goto cleanup;
		}

		/* Set DEPEND reason for pulled packages */
		for(i = resolved; i; i = i->next) {
			klpm_pkg_t *pkg = i->data;
			if(!klpm_pkg_find(trans->add, pkg->name)) {
				pkg->reason = KUZPKG_PKG_REASON_DEPEND;
			}
		}

		/* Unresolvable packages will be removed from the target list; set these
		 * aside in the transaction as a list we won't operate on. If we free them
		 * before the end of the transaction, we may kill pointers the frontend
		 * holds to package objects. */
		trans->unresolvable = unresolvable;

		klpm_list_free(trans->add);
		trans->add = resolved;

		event.type = KUZPKG_EVENT_RESOLVEDEPS_DONE;
		EVENT(handle, &event);
	}

	if(!(trans->flags & KUZPKG_TRANS_FLAG_NOCONFLICTS)) {
		/* check for inter-conflicts and whatnot */
		event.type = KUZPKG_EVENT_INTERCONFLICTS_START;
		EVENT(handle, &event);

		_klpm_log(handle, KUZPKG_LOG_DEBUG, "looking for conflicts\n");

		/* 1. check for conflicts in the target list */
		_klpm_log(handle, KUZPKG_LOG_DEBUG, "check targets vs targets\n");
		deps = _klpm_innerconflicts(handle, trans->add);

		for(i = deps; i; i = i->next) {
			klpm_conflict_t *conflict = i->data;
			const char *name1 = conflict->package1->name;
			const char *name2 = conflict->package2->name;
			klpm_pkg_t *rsync, *sync, *sync1, *sync2;

			/* have we already removed one of the conflicting targets? */
			sync1 = klpm_pkg_find(trans->add, name1);
			sync2 = klpm_pkg_find(trans->add, name2);
			if(!sync1 || !sync2) {
				continue;
			}

			_klpm_log(handle, KUZPKG_LOG_DEBUG, "conflicting packages in the sync list: '%s' <-> '%s'\n",
					name1, name2);

			/* if sync1 provides sync2, we remove sync2 from the targets, and vice versa */
			klpm_depend_t *dep1 = klpm_dep_from_string(name1);
			klpm_depend_t *dep2 = klpm_dep_from_string(name2);
			if(_klpm_depcmp(sync1, dep2)) {
				rsync = sync2;
				sync = sync1;
			} else if(_klpm_depcmp(sync2, dep1)) {
				rsync = sync1;
				sync = sync2;
			} else {
				_klpm_log(handle, KUZPKG_LOG_ERROR, _("unresolvable package conflicts detected\n"));
				handle->pm_errno = KUZPKG_ERR_CONFLICTING_DEPS;
				ret = -1;
				if(data) {
					klpm_conflict_t *newconflict = _klpm_conflict_dup(conflict);
					if(newconflict) {
						*data = klpm_list_add(*data, newconflict);
					}
				}
				klpm_list_free_inner(deps, (klpm_list_fn_free)klpm_conflict_free);
				klpm_list_free(deps);
				klpm_dep_free(dep1);
				klpm_dep_free(dep2);
				goto cleanup;
			}
			klpm_dep_free(dep1);
			klpm_dep_free(dep2);

			/* Prints warning */
			_klpm_log(handle, KUZPKG_LOG_WARNING,
					_("removing '%s-%s' from target list because it conflicts with '%s-%s'\n"),
					rsync->name, rsync->version, sync->name, sync->version);
			trans->add = klpm_list_remove(trans->add, rsync, _klpm_pkg_cmp, NULL);
			/* rsync is not a transaction target anymore */
			trans->unresolvable = klpm_list_add(trans->unresolvable, rsync);
		}

		klpm_list_free_inner(deps, (klpm_list_fn_free)klpm_conflict_free);
		klpm_list_free(deps);
		deps = NULL;

		/* 2. we check for target vs db conflicts (and resolve)*/
		_klpm_log(handle, KUZPKG_LOG_DEBUG, "check targets vs db and db vs targets\n");
		deps = _klpm_outerconflicts(handle->db_local, trans->add);

		for(i = deps; i; i = i->next) {
			klpm_question_conflict_t question = {
				.type = KUZPKG_QUESTION_CONFLICT_PKG,
				.remove = 0,
				.conflict = i->data
			};
			klpm_conflict_t *conflict = i->data;
			const char *name1 = conflict->package1->name;
			const char *name2 = conflict->package2->name;
			int found = 0;

			/* if name2 (the local package) is not elected for removal,
			   we ask the user */
			if(klpm_pkg_find(trans->remove, name2)) {
				found = 1;
			}
			for(j = trans->add; j && !found; j = j->next) {
				klpm_pkg_t *spkg = j->data;
				if(klpm_pkg_find(spkg->removes, name2)) {
					found = 1;
				}
			}
			if(found) {
				continue;
			}

			_klpm_log(handle, KUZPKG_LOG_DEBUG, "package '%s-%s' conflicts with '%s-%s'\n",
					name1, conflict->package1->version, name2,conflict->package2->version);

			QUESTION(handle, &question);
			if(question.remove) {
				/* append to the removes list */
				klpm_pkg_t *sync = klpm_pkg_find(trans->add, name1);
				klpm_pkg_t *local = _klpm_db_get_pkgfromcache(handle->db_local, name2);
				_klpm_log(handle, KUZPKG_LOG_DEBUG, "electing '%s' for removal\n", name2);
				sync->removes = klpm_list_add(sync->removes, local);
			} else { /* abort */
				_klpm_log(handle, KUZPKG_LOG_ERROR, _("unresolvable package conflicts detected\n"));
				handle->pm_errno = KUZPKG_ERR_CONFLICTING_DEPS;
				ret = -1;
				if(data) {
					klpm_conflict_t *newconflict = _klpm_conflict_dup(conflict);
					if(newconflict) {
						*data = klpm_list_add(*data, newconflict);
					}
				}
				klpm_list_free_inner(deps, (klpm_list_fn_free)klpm_conflict_free);
				klpm_list_free(deps);
				goto cleanup;
			}
		}
		event.type = KUZPKG_EVENT_INTERCONFLICTS_DONE;
		EVENT(handle, &event);
		klpm_list_free_inner(deps, (klpm_list_fn_free)klpm_conflict_free);
		klpm_list_free(deps);
	}

	/* Build trans->remove list */
	for(i = trans->add; i; i = i->next) {
		klpm_pkg_t *spkg = i->data;
		for(j = spkg->removes; j; j = j->next) {
			klpm_pkg_t *rpkg = j->data;
			if(!klpm_pkg_find(trans->remove, rpkg->name)) {
				klpm_pkg_t *copy;
				_klpm_log(handle, KUZPKG_LOG_DEBUG, "adding '%s' to remove list\n", rpkg->name);
				if(_klpm_pkg_dup(rpkg, &copy) == -1) {
					return -1;
				}
				trans->remove = klpm_list_add(trans->remove, copy);
			}
		}
	}

	if(!(trans->flags & KUZPKG_TRANS_FLAG_NODEPS)) {
		_klpm_log(handle, KUZPKG_LOG_DEBUG, "checking dependencies\n");
		deps = klpm_checkdeps(handle, _klpm_db_get_pkgcache(handle->db_local),
				trans->remove, trans->add, 1);
		if(deps) {
			handle->pm_errno = KUZPKG_ERR_UNSATISFIED_DEPS;
			ret = -1;
			if(data) {
				*data = deps;
			} else {
				klpm_list_free_inner(deps,
						(klpm_list_fn_free)klpm_depmissing_free);
				klpm_list_free(deps);
			}
			goto cleanup;
		}
	}
	for(i = trans->add; i; i = i->next) {
		/* update download size field */
		klpm_pkg_t *spkg = i->data;
		klpm_pkg_t *lpkg = klpm_db_get_pkg(handle->db_local, spkg->name);
		if(compute_download_size(spkg) < 0) {
			ret = -1;
			goto cleanup;
		}
		if(lpkg && _klpm_pkg_dup(lpkg, &spkg->oldpkg) != 0) {
			ret = -1;
			goto cleanup;
		}
	}

cleanup:
	return ret;
}

off_t SYMEXPORT klpm_pkg_download_size(klpm_pkg_t *newpkg)
{
	if(!(newpkg->infolevel & INFRQ_DSIZE)) {
		compute_download_size(newpkg);
	}
	return newpkg->download_size;
}

/**
 * Prompts to delete the file now that we know it is invalid.
 * @param handle the context handle
 * @param filename the absolute path of the file to test
 * @param reason an error code indicating the reason for package invalidity
 *
 * @return 1 if file was removed, 0 otherwise
 */
static int prompt_to_delete(klpm_handle_t *handle, const char *filepath,
		klpm_errno_t reason)
{
	klpm_question_corrupted_t question = {
		.type = KUZPKG_QUESTION_CORRUPTED_PKG,
		.remove = 0,
		.filepath = filepath,
		.reason = reason
	};
	QUESTION(handle, &question);
	if(question.remove) {
		char *sig_filepath;

		unlink(filepath);

		sig_filepath = _klpm_sigpath(handle, filepath);
		unlink(sig_filepath);
		FREE(sig_filepath);
	}
	return question.remove;
}

static int find_dl_candidates(klpm_handle_t *handle, klpm_list_t **files)
{
	for(klpm_list_t *i = handle->trans->add; i; i = i->next) {
		klpm_pkg_t *spkg = i->data;

		if(spkg->origin != KUZPKG_PKG_FROM_FILE) {
			klpm_db_t *repo = spkg->origin_data.db;
			bool need_download;
			int siglevel = klpm_db_get_siglevel(klpm_pkg_get_db(spkg));

			if(!repo->servers) {
				handle->pm_errno = KUZPKG_ERR_SERVER_NONE;
				_klpm_log(handle, KUZPKG_LOG_ERROR, "%s: %s\n",
						klpm_strerror(handle->pm_errno), repo->treename);
				return -1;
			}

			ASSERT(spkg->filename != NULL, RET_ERR(handle, KUZPKG_ERR_PKG_INVALID_NAME, -1));

			need_download = spkg->download_size != 0 || !_klpm_filecache_exists(handle, spkg->filename);
			/* even if the package file in the cache we need to check for
			 * accompanion *.sig file as well.
			 * If *.sig is not cached then force download the package + its signature file.
			 */
			if(!need_download && (siglevel & KUZPKG_SIG_PACKAGE)) {
				char *sig_filename = NULL;
				int len = strlen(spkg->filename) + 5;

				MALLOC(sig_filename, len, RET_ERR(handle, KUZPKG_ERR_MEMORY, -1));
				snprintf(sig_filename, len, "%s.sig", spkg->filename);

				need_download = !_klpm_filecache_exists(handle, sig_filename);

				FREE(sig_filename);
			}

			if(need_download) {
				*files = klpm_list_add(*files, spkg);
			}
		}
	}

	return 0;
}


static int download_files(klpm_handle_t *handle)
{
	const char *cachedir;
	char * temporary_cachedir = NULL;
	klpm_list_t *i, *files = NULL;
	int ret = 0;
	klpm_event_t event;
	klpm_list_t *payloads = NULL;

	cachedir = _klpm_filecache_setup(handle);
	temporary_cachedir = _klpm_download_dir_setup(handle, cachedir);
	if(temporary_cachedir == NULL) {
		ret = -1;
		goto finish;
	}
	handle->trans->state = STATE_DOWNLOADING;

	ret = find_dl_candidates(handle, &files);
	if(ret != 0) {
		goto finish;
	}

	if(files) {
		/* check for necessary disk space for download */
		if(handle->checkspace) {
			off_t *file_sizes;
			size_t idx, num_files;

			_klpm_log(handle, KUZPKG_LOG_DEBUG, "checking available disk space for download\n");

			num_files = klpm_list_count(files);
			CALLOC(file_sizes, num_files, sizeof(off_t), goto finish);

			for(i = files, idx = 0; i; i = i->next, idx++) {
				const klpm_pkg_t *pkg = i->data;
				file_sizes[idx] = pkg->download_size;
			}

			ret = _klpm_check_downloadspace(handle, temporary_cachedir, num_files, file_sizes);
			free(file_sizes);

			if(ret != 0) {
				goto finish;
			}
		}

		event.type = KUZPKG_EVENT_PKG_RETRIEVE_START;
		event.pkg_retrieve.total_size = 0;
		event.pkg_retrieve.num = 0;

		/* sum up the number of packages to download and its total size */
		for(i = files; i; i = i->next) {
			klpm_pkg_t *spkg = i->data;
			event.pkg_retrieve.total_size += spkg->download_size;
			event.pkg_retrieve.num++;
		}

		EVENT(handle, &event);
		for(i = files; i; i = i->next) {
			klpm_pkg_t *pkg = i->data;
			int siglevel = klpm_db_get_siglevel(klpm_pkg_get_db(pkg));
			struct dload_payload *payload = NULL;

			CALLOC(payload, 1, sizeof(*payload), GOTO_ERR(handle, KUZPKG_ERR_MEMORY, finish));
			STRDUP(payload->remote_name, pkg->filename, FREE(payload); GOTO_ERR(handle, KUZPKG_ERR_MEMORY, finish));
			STRDUP(payload->filepath, pkg->filename,
				_klpm_dload_payload_reset(payload); FREE(payload);
				GOTO_ERR(handle, KUZPKG_ERR_MEMORY, finish));
			payload->destfile_name = _klpm_get_fullpath(temporary_cachedir, payload->remote_name, "");
			payload->tempfile_name = _klpm_get_fullpath(temporary_cachedir, payload->remote_name, ".part");
			if(!payload->destfile_name || !payload->tempfile_name) {
				_klpm_dload_payload_reset(payload);
				FREE(payload);
				GOTO_ERR(handle, KUZPKG_ERR_MEMORY, finish);
			}
			payload->max_size = pkg->size;
			payload->cache_servers = pkg->origin_data.db->cache_servers;
			payload->servers = pkg->origin_data.db->servers;
			payload->handle = handle;
			payload->allow_resume = 1;
			payload->download_signature = (siglevel & KUZPKG_SIG_PACKAGE);
			payload->signature_optional = (siglevel & KUZPKG_SIG_PACKAGE_OPTIONAL);

			payloads = klpm_list_add(payloads, payload);
		}

		ret = _klpm_download(handle, payloads, cachedir, temporary_cachedir);
		if(ret == -1) {
			event.type = KUZPKG_EVENT_PKG_RETRIEVE_FAILED;
			EVENT(handle, &event);
			_klpm_log(handle, KUZPKG_LOG_WARNING, _("failed to retrieve some files\n"));
			goto finish;
		}
		event.type = KUZPKG_EVENT_PKG_RETRIEVE_DONE;
		EVENT(handle, &event);
	}

finish:
	if(payloads) {
		klpm_list_free_inner(payloads, (klpm_list_fn_free)_klpm_dload_payload_reset);
		FREELIST(payloads);
	}

	if(files) {
		klpm_list_free(files);
	}

	for(i = handle->trans->add; i; i = i->next) {
		klpm_pkg_t *pkg = i->data;
		pkg->infolevel &= ~INFRQ_DSIZE;
		pkg->download_size = 0;
	}
	FREE(temporary_cachedir);

	return ret;
}

#ifdef HAVE_LIBGPGME

static int key_cmp(const void *k1, const void *k2) {
	const struct keyinfo_t *key1 = k1;
	const char *key2 = k2;

	return strcmp(key1->keyid, key2);
}

static int check_keyring(klpm_handle_t *handle)
{
	size_t current = 0, numtargs;
	klpm_list_t *i, *errors = NULL;
	klpm_event_t event;
	struct keyinfo_t *keyinfo;

	event.type = KUZPKG_EVENT_KEYRING_START;
	EVENT(handle, &event);

	numtargs = klpm_list_count(handle->trans->add);

	for(i = handle->trans->add; i; i = i->next, current++) {
		klpm_pkg_t *pkg = i->data;
		int level;

		int percent = (current * 100) / numtargs;
		PROGRESS(handle, KUZPKG_PROGRESS_KEYRING_START, "", percent,
				numtargs, current);

		if(pkg->origin == KUZPKG_PKG_FROM_FILE) {
			continue; /* pkg_load() has been already called, this package is valid */
		}

		level = klpm_db_get_siglevel(klpm_pkg_get_db(pkg));
		if((level & KUZPKG_SIG_PACKAGE)) {
			unsigned char *sig = NULL;
			size_t sig_len;
			int ret = klpm_pkg_get_sig(pkg, &sig, &sig_len);
			if(ret == 0) {
				klpm_list_t *keys = NULL;
				if(klpm_extract_keyid(handle, pkg->name, sig,
							sig_len, &keys) == 0) {
					klpm_list_t *k;
					for(k = keys; k; k = k->next) {
						char *key = k->data;
						_klpm_log(handle, KUZPKG_LOG_DEBUG, "found signature key: %s\n", key);
						if(!klpm_list_find(errors, key, key_cmp) &&
								_klpm_key_in_keychain(handle, key) == 0) {
							keyinfo = malloc(sizeof(struct keyinfo_t));
							if(!keyinfo) {
								break;
							}
							keyinfo->uid = strdup(pkg->packager);
							keyinfo->keyid = strdup(key);
							errors = klpm_list_add(errors, keyinfo);
						}
					}
					FREELIST(keys);
				}
			}
			free(sig);
		}
	}

	PROGRESS(handle, KUZPKG_PROGRESS_KEYRING_START, "", 100,
			numtargs, current);
	event.type = KUZPKG_EVENT_KEYRING_DONE;
	EVENT(handle, &event);

	if(errors) {
		event.type = KUZPKG_EVENT_KEY_DOWNLOAD_START;
		EVENT(handle, &event);
		int fail = 0;
		klpm_list_t *k;
		for(k = errors; k; k = k->next) {
			keyinfo = k->data;
			if(_klpm_key_import(handle, keyinfo->uid, keyinfo->keyid) == -1) {
				fail = 1;
			}
			free(keyinfo->uid);
			free(keyinfo->keyid);
			free(keyinfo);
		}
		klpm_list_free(errors);
		event.type = KUZPKG_EVENT_KEY_DOWNLOAD_DONE;
		EVENT(handle, &event);
		if(fail) {
			_klpm_log(handle, KUZPKG_LOG_ERROR, _("required key missing from keyring\n"));
			return -1;
		}
	}

	return 0;
}
#endif /* HAVE_LIBGPGME */

static int check_validity(klpm_handle_t *handle,
		size_t total, uint64_t total_bytes)
{
	struct validity {
		klpm_pkg_t *pkg;
		char *path;
		klpm_siglist_t *siglist;
		int siglevel;
		int validation;
		klpm_errno_t error;
	};
	size_t current = 0;
	uint64_t current_bytes = 0;
	klpm_list_t *i, *errors = NULL;
	klpm_event_t event;

	/* Check integrity of packages */
	event.type = KUZPKG_EVENT_INTEGRITY_START;
	EVENT(handle, &event);

	for(i = handle->trans->add; i; i = i->next, current++) {
		struct validity v = { i->data, NULL, NULL, 0, 0, 0 };
		int percent = (int)(((double)current_bytes / total_bytes) * 100);

		PROGRESS(handle, KUZPKG_PROGRESS_INTEGRITY_START, "", percent,
				total, current);
		if(v.pkg->origin == KUZPKG_PKG_FROM_FILE) {
			continue; /* pkg_load() has been already called, this package is valid */
		}

		current_bytes += v.pkg->size;
		v.path = _klpm_filecache_find(handle, v.pkg->filename);

		if(!v.path) {
			_klpm_log(handle, KUZPKG_LOG_ERROR,
					_("%s: could not find package in cache\n"), v.pkg->name);
			RET_ERR(handle, KUZPKG_ERR_PKG_NOT_FOUND, -1);
		}

		v.siglevel = klpm_db_get_siglevel(klpm_pkg_get_db(v.pkg));

		if(_klpm_pkg_validate_internal(handle, v.path, v.pkg,
					v.siglevel, &v.siglist, &v.validation) == -1) {
			struct validity *invalid;
			v.error = handle->pm_errno;
			MALLOC(invalid, sizeof(struct validity), return -1);
			memcpy(invalid, &v, sizeof(struct validity));
			errors = klpm_list_add(errors, invalid);
		} else {
			klpm_siglist_cleanup(v.siglist);
			free(v.siglist);
			free(v.path);
			v.pkg->validation = v.validation;
		}
	}

	PROGRESS(handle, KUZPKG_PROGRESS_INTEGRITY_START, "", 100,
			total, current);
	event.type = KUZPKG_EVENT_INTEGRITY_DONE;
	EVENT(handle, &event);

	if(errors) {
		for(i = errors; i; i = i->next) {
			struct validity *v = i->data;
			switch(v->error) {
				case KUZPKG_ERR_PKG_MISSING_SIG:
					_klpm_log(handle, KUZPKG_LOG_ERROR,
							_("%s: missing required signature\n"), v->pkg->name);
					break;
				case KUZPKG_ERR_PKG_INVALID_SIG:
					_klpm_process_siglist(handle, v->pkg->name, v->siglist,
							v->siglevel & KUZPKG_SIG_PACKAGE_OPTIONAL,
							v->siglevel & KUZPKG_SIG_PACKAGE_MARGINAL_OK,
							v->siglevel & KUZPKG_SIG_PACKAGE_UNKNOWN_OK);
					__attribute__((fallthrough));
				case KUZPKG_ERR_PKG_INVALID_CHECKSUM:
					prompt_to_delete(handle, v->path, v->error);
					break;
				case KUZPKG_ERR_PKG_NOT_FOUND:
				case KUZPKG_ERR_BADPERMS:
				case KUZPKG_ERR_PKG_OPEN:
					_klpm_log(handle, KUZPKG_LOG_ERROR, _("failed to read file %s: %s\n"), v->path, klpm_strerror(v->error));
					break;
				default:
					/* ignore */
					break;
			}
			klpm_siglist_cleanup(v->siglist);
			free(v->siglist);
			free(v->path);
			free(v);
		}
		klpm_list_free(errors);

		if(handle->pm_errno == KUZPKG_ERR_OK) {
			RET_ERR(handle, KUZPKG_ERR_PKG_INVALID, -1);
		}
		return -1;
	}

	return 0;
}

static int dep_not_equal(const klpm_depend_t *left, const klpm_depend_t *right)
{
	return left->name_hash != right->name_hash
		|| strcmp(left->name, right->name) != 0
		|| left->mod != right->mod
		|| (left->version == NULL) != (right->version == NULL)
		|| ((left->version && right->version) && strcmp(left->version, right->version) != 0);
}

static int check_pkg_field_matches_db(klpm_handle_t *handle, const char *field,
		klpm_list_t *left, klpm_list_t *right, klpm_list_fn_cmp cmp)
{
	switch(klpm_list_cmp_unsorted(left, right, cmp)) {
		case 0:
			_klpm_log(handle, KUZPKG_LOG_DEBUG,
					"internal package %s mismatch\n", field);
			return 1;
		case 1:
			return 0;
		default:
			RET_ERR(handle, KUZPKG_ERR_MEMORY, -1);
	}
}

static int check_pkg_matches_db(klpm_pkg_t *spkg, klpm_pkg_t *pkgfile)
{
	klpm_handle_t *handle = spkg->handle;
	int error = 0;

#define CHECK_FIELD(STR, FIELD, CMP) do { \
	int ok = check_pkg_field_matches_db(handle, STR, spkg->FIELD, pkgfile->FIELD, (klpm_list_fn_cmp)CMP); \
	if(ok == -1) { \
		return 1; \
	} else if(ok != 0) { \
		error = 1; \
	} \
} while(0)

	if(strcmp(spkg->name, pkgfile->name) != 0) {
		_klpm_log(handle, KUZPKG_LOG_DEBUG,
				"internal package name mismatch, expected: '%s', actual: '%s'\n",
				spkg->name, pkgfile->name);
		error = 1;
	}
	if(strcmp(spkg->version, pkgfile->version) != 0) {
		_klpm_log(handle, KUZPKG_LOG_DEBUG,
				"internal package version mismatch, expected: '%s', actual: '%s'\n",
				spkg->version, pkgfile->version);
		error = 1;
	}
	if(spkg->isize != pkgfile->isize) {
		_klpm_log(handle, KUZPKG_LOG_DEBUG,
				"internal package install size mismatch, expected: '%ld', actual: '%ld'\n",
				spkg->isize, pkgfile->isize);
		error = 1;
	}

	CHECK_FIELD("depends", depends, dep_not_equal);
	CHECK_FIELD("conflicts", conflicts, dep_not_equal);
	CHECK_FIELD("replaces", replaces, dep_not_equal);
	CHECK_FIELD("provides", provides, dep_not_equal);
	CHECK_FIELD("groups", groups, strcmp);

#undef CHECK_FIELD

	return error;
}


static int load_packages(klpm_handle_t *handle, klpm_list_t **data,
		size_t total, size_t total_bytes)
{
	size_t current = 0, current_bytes = 0;
	int errors = 0;
	klpm_list_t *i, *delete = NULL;
	klpm_event_t event;

	/* load packages from disk now that they are known-valid */
	event.type = KUZPKG_EVENT_LOAD_START;
	EVENT(handle, &event);

	for(i = handle->trans->add; i; i = i->next, current++) {
		int error = 0;
		klpm_pkg_t *spkg = i->data;
		char *filepath;
		int percent = (int)(((double)current_bytes / total_bytes) * 100);

		PROGRESS(handle, KUZPKG_PROGRESS_LOAD_START, "", percent,
				total, current);
		if(spkg->origin == KUZPKG_PKG_FROM_FILE) {
			continue; /* pkg_load() has been already called, this package is valid */
		}

		current_bytes += spkg->size;
		filepath = _klpm_filecache_find(handle, spkg->filename);

		if(!filepath) {
			FREELIST(delete);
			_klpm_log(handle, KUZPKG_LOG_ERROR,
					_("%s: could not find package in cache\n"), spkg->name);
			RET_ERR(handle, KUZPKG_ERR_PKG_NOT_FOUND, -1);
		}

		/* load the package file and replace pkgcache entry with it in the target list */
		/* TODO: klpm_pkg_get_db() will not work on this target anymore */
		_klpm_log(handle, KUZPKG_LOG_DEBUG,
				"replacing pkgcache entry with package file for target %s\n",
				spkg->name);
		klpm_pkg_t *pkgfile =_klpm_pkg_load_internal(handle, filepath, 1);
		if(!pkgfile) {
			_klpm_log(handle, KUZPKG_LOG_DEBUG, "failed to load pkgfile internal\n");
			error = 1;
		} else {
			error |= check_pkg_matches_db(spkg, pkgfile);
		}
		if(error != 0) {
			errors++;
			*data = klpm_list_add(*data, strdup(spkg->filename));
			delete = klpm_list_add(delete, filepath);
			_klpm_pkg_free(pkgfile);
			continue;
		}
		free(filepath);
		/* copy over the install reason */
		pkgfile->reason = spkg->reason;
		/* copy over validation method */
		pkgfile->validation = spkg->validation;
		/* transfer oldpkg */
		pkgfile->oldpkg = spkg->oldpkg;
		spkg->oldpkg = NULL;
		i->data = pkgfile;
		/* spkg has been removed from the target list, so we can free the
		 * sync-specific fields */
		_klpm_pkg_free_trans(spkg);
	}

	PROGRESS(handle, KUZPKG_PROGRESS_LOAD_START, "", 100,
			total, current);
	event.type = KUZPKG_EVENT_LOAD_DONE;
	EVENT(handle, &event);

	if(errors) {
		for(i = delete; i; i = i->next) {
			prompt_to_delete(handle, i->data, KUZPKG_ERR_PKG_INVALID);
		}
		FREELIST(delete);

		if(handle->pm_errno == KUZPKG_ERR_OK) {
			RET_ERR(handle, KUZPKG_ERR_PKG_INVALID, -1);
		}
		return -1;
	}

	return 0;
}

int _klpm_sync_load(klpm_handle_t *handle, klpm_list_t **data)
{
	klpm_list_t *i;
	size_t total = 0;
	uint64_t total_bytes = 0;
	klpm_trans_t *trans = handle->trans;

	if(download_files(handle) == -1) {
		return -1;
	}

#ifdef HAVE_LIBGPGME
	/* make sure all required signatures are in keyring */
	if(check_keyring(handle)) {
		return -1;
	}
#endif

	/* get the total size of all packages so we can adjust the progress bar more
	 * realistically if there are small and huge packages involved */
	for(i = trans->add; i; i = i->next) {
		klpm_pkg_t *spkg = i->data;
		if(spkg->origin != KUZPKG_PKG_FROM_FILE) {
			total_bytes += spkg->size;
		}
		total++;
	}
	/* this can only happen maliciously */
	total_bytes = total_bytes ? total_bytes : 1;

	if(check_validity(handle, total, total_bytes) != 0) {
		return -1;
	}

	if(trans->flags & KUZPKG_TRANS_FLAG_DOWNLOADONLY) {
		return 0;
	}

	if(load_packages(handle, data, total, total_bytes)) {
		return -1;
	}

	return 0;
}

int _klpm_sync_check(klpm_handle_t *handle, klpm_list_t **data)
{
	klpm_trans_t *trans = handle->trans;
	klpm_event_t event;

	/* fileconflict check */
	if(!(trans->flags & KUZPKG_TRANS_FLAG_DBONLY)) {
		event.type = KUZPKG_EVENT_FILECONFLICTS_START;
		EVENT(handle, &event);

		_klpm_log(handle, KUZPKG_LOG_DEBUG, "looking for file conflicts\n");
		klpm_list_t *conflict = _klpm_db_find_fileconflicts(handle,
				trans->add, trans->remove);
		if(conflict) {
			if(data) {
				*data = conflict;
			} else {
				klpm_list_free_inner(conflict,
						(klpm_list_fn_free)klpm_fileconflict_free);
				klpm_list_free(conflict);
			}
			RET_ERR(handle, KUZPKG_ERR_FILE_CONFLICTS, -1);
		}

		event.type = KUZPKG_EVENT_FILECONFLICTS_DONE;
		EVENT(handle, &event);
	}

	/* check available disk space */
	if(handle->checkspace && !(trans->flags & KUZPKG_TRANS_FLAG_DBONLY)) {
		event.type = KUZPKG_EVENT_DISKSPACE_START;
		EVENT(handle, &event);

		_klpm_log(handle, KUZPKG_LOG_DEBUG, "checking available disk space\n");
		if(_klpm_check_diskspace(handle) == -1) {
			_klpm_log(handle, KUZPKG_LOG_ERROR, _("not enough free disk space\n"));
			return -1;
		}

		event.type = KUZPKG_EVENT_DISKSPACE_DONE;
		EVENT(handle, &event);
	}

	return 0;
}

int _klpm_sync_commit(klpm_handle_t *handle)
{
	klpm_trans_t *trans = handle->trans;

	/* remove conflicting and to-be-replaced packages */
	if(trans->remove) {
		_klpm_log(handle, KUZPKG_LOG_DEBUG,
				"removing conflicting and to-be-replaced packages\n");
		/* we want the frontend to be aware of commit details */
		if(_klpm_remove_packages(handle, 0) == -1) {
			_klpm_log(handle, KUZPKG_LOG_ERROR,
					_("could not commit removal transaction\n"));
			return -1;
		}
	}

	/* install targets */
	_klpm_log(handle, KUZPKG_LOG_DEBUG, "installing packages\n");
	if(_klpm_upgrade_packages(handle) == -1) {
		_klpm_log(handle, KUZPKG_LOG_ERROR, _("could not commit transaction\n"));
		return -1;
	}

	return 0;
}
