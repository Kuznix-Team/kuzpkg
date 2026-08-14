/*
 *  deps.c
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* libkuzpkg */
#include "deps.h"
#include "klpm_list.h"
#include "util.h"
#include "log.h"
#include "graph.h"
#include "package.h"
#include "db.h"
#include "handle.h"
#include "trans.h"

void SYMEXPORT klpm_dep_free(klpm_depend_t *dep)
{
	ASSERT(dep != NULL, return);
	FREE(dep->name);
	FREE(dep->version);
	FREE(dep->desc);
	FREE(dep);
}

static klpm_depmissing_t *depmiss_new(const char *target, klpm_depend_t *dep,
		const char *causingpkg)
{
	klpm_depmissing_t *miss;

	CALLOC(miss, 1, sizeof(klpm_depmissing_t), return NULL);

	STRDUP(miss->target, target, goto error);
	miss->depend = _klpm_dep_dup(dep);
	STRDUP(miss->causingpkg, causingpkg, goto error);

	return miss;

error:
	klpm_depmissing_free(miss);
	return NULL;
}

void SYMEXPORT klpm_depmissing_free(klpm_depmissing_t *miss)
{
	ASSERT(miss != NULL, return);
	klpm_dep_free(miss->depend);
	FREE(miss->target);
	FREE(miss->causingpkg);
	FREE(miss);
}

/** Check if pkg2 satisfies a dependency of pkg1 */
static int _klpm_pkg_depends_on(klpm_pkg_t *pkg1, klpm_pkg_t *pkg2)
{
	klpm_list_t *i;
	for(i = klpm_pkg_get_depends(pkg1); i; i = i->next) {
		if(_klpm_depcmp(pkg2, i->data)) {
			return 1;
		}
	}
	return 0;
}

static klpm_pkg_t *find_dep_satisfier(klpm_list_t *pkgs, klpm_depend_t *dep)
{
	klpm_list_t *i;

	for(i = pkgs; i; i = i->next) {
		klpm_pkg_t *pkg = i->data;
		if(_klpm_depcmp(pkg, dep)) {
			return pkg;
		}
	}
	return NULL;
}

/* Convert a list of klpm_pkg_t * to a graph structure,
 * with a edge for each dependency.
 * Returns a list of vertices (one vertex = one package)
 * (used by klpm_sortbydeps)
 */
static klpm_list_t *dep_graph_init(klpm_handle_t *handle,
		klpm_list_t *targets, klpm_list_t *ignore)
{
	klpm_list_t *i, *j;
	klpm_list_t *vertices = NULL;
	klpm_list_t *localpkgs = klpm_list_diff(
			klpm_db_get_pkgcache(handle->db_local), targets, _klpm_pkg_cmp);

	if(ignore) {
		klpm_list_t *oldlocal = localpkgs;
		localpkgs = klpm_list_diff(oldlocal, ignore, _klpm_pkg_cmp);
		klpm_list_free(oldlocal);
	}

	/* We create the vertices */
	for(i = targets; i; i = i->next) {
		klpm_graph_t *vertex = _klpm_graph_new();
		vertex->data = (void *)i->data;
		vertices = klpm_list_add(vertices, vertex);
	}

	/* We compute the edges */
	for(i = vertices; i; i = i->next) {
		klpm_graph_t *vertex_i = i->data;
		klpm_pkg_t *p_i = vertex_i->data;
		/* TODO this should be somehow combined with klpm_checkdeps */
		for(j = vertices; j; j = j->next) {
			klpm_graph_t *vertex_j = j->data;
			klpm_pkg_t *p_j = vertex_j->data;
			if(_klpm_pkg_depends_on(p_i, p_j)) {
				vertex_i->children =
					klpm_list_add(vertex_i->children, vertex_j);
			}
		}

		/* lazily add local packages to the dep graph so they don't
		 * get resolved unnecessarily */
		j = localpkgs;
		while(j) {
			klpm_list_t *next = j->next;
			if(_klpm_pkg_depends_on(p_i, j->data)) {
				klpm_graph_t *vertex_j = _klpm_graph_new();
				vertex_j->data = (void *)j->data;
				vertices = klpm_list_add(vertices, vertex_j);
				vertex_i->children = klpm_list_add(vertex_i->children, vertex_j);
				localpkgs = klpm_list_remove_item(localpkgs, j);
				free(j);
			}
			j = next;
		}

		vertex_i->iterator = vertex_i->children;
	}
	klpm_list_free(localpkgs);
	return vertices;
}

static void _klpm_warn_dep_cycle(klpm_handle_t *handle, klpm_list_t *targets,
		klpm_graph_t *ancestor, klpm_graph_t *vertex, int reverse)
{
	/* vertex depends on and is required by ancestor */
	if(!klpm_list_find_ptr(targets, vertex->data)) {
		/* child is not part of the transaction, not a problem */
		return;
	}

	/* find the nearest ancestor that's part of the transaction */
	while(ancestor) {
		if(klpm_list_find_ptr(targets, ancestor->data)) {
			break;
		}
		ancestor = ancestor->parent;
	}

	if(!ancestor || ancestor == vertex) {
		/* no transaction package in our ancestry or the package has
		 * a circular dependency with itself, not a problem */
	} else {
		klpm_pkg_t *ancestorpkg = ancestor->data;
		klpm_pkg_t *childpkg = vertex->data;
		_klpm_log(handle, KUZPKG_LOG_DEBUG, _("dependency cycle detected:\n"));
		if(reverse) {
			_klpm_log(handle, KUZPKG_LOG_DEBUG,
					_("%s will be removed after its %s dependency\n"),
					ancestorpkg->name, childpkg->name);
		} else {
			_klpm_log(handle, KUZPKG_LOG_DEBUG,
					_("%s will be installed before its %s dependency\n"),
					ancestorpkg->name, childpkg->name);
		}
	}
}

/* Re-order a list of target packages with respect to their dependencies.
 *
 * Example (reverse == 0):
 *   A depends on C
 *   B depends on A
 *   Target order is A,B,C,D
 *
 *   Should be re-ordered to C,A,B,D
 *
 * packages listed in ignore will not be used to detect indirect dependencies
 *
 * if reverse is > 0, the dependency order will be reversed.
 *
 * This function returns the new klpm_list_t* target list.
 *
 */
klpm_list_t *_klpm_sortbydeps(klpm_handle_t *handle,
		klpm_list_t *targets, klpm_list_t *ignore, int reverse)
{
	klpm_list_t *newtargs = NULL;
	klpm_list_t *vertices = NULL;
	klpm_list_t *i;
	klpm_graph_t *vertex;

	if(targets == NULL) {
		return NULL;
	}

	_klpm_log(handle, KUZPKG_LOG_DEBUG, "started sorting dependencies\n");

	vertices = dep_graph_init(handle, targets, ignore);

	i = vertices;
	vertex = vertices->data;
	while(i) {
		/* mark that we touched the vertex */
		vertex->state = KUZPKG_GRAPH_STATE_PROCESSING;
		int switched_to_child = 0;
		while(vertex->iterator && !switched_to_child) {
			klpm_graph_t *nextchild = vertex->iterator->data;
			vertex->iterator = vertex->iterator->next;
			if(nextchild->state == KUZPKG_GRAPH_STATE_UNPROCESSED) {
				switched_to_child = 1;
				nextchild->parent = vertex;
				vertex = nextchild;
			} else if(nextchild->state == KUZPKG_GRAPH_STATE_PROCESSING) {
				_klpm_warn_dep_cycle(handle, targets, vertex, nextchild, reverse);
			}
		}
		if(!switched_to_child) {
			if(klpm_list_find_ptr(targets, vertex->data)) {
				newtargs = klpm_list_add(newtargs, vertex->data);
			}
			/* mark that we've left this vertex */
			vertex->state = KUZPKG_GRAPH_STATE_PROCESSED;
			vertex = vertex->parent;
			if(!vertex) {
				/* top level vertex reached, move to the next unprocessed vertex */
				for(i = i->next; i; i = i->next) {
					vertex = i->data;
					if(vertex->state == KUZPKG_GRAPH_STATE_UNPROCESSED) {
						break;
					}
				}
			}
		}
	}

	_klpm_log(handle, KUZPKG_LOG_DEBUG, "sorting dependencies finished\n");

	if(reverse) {
		/* reverse the order */
		klpm_list_t *tmptargs = klpm_list_reverse(newtargs);
		/* free the old one */
		klpm_list_free(newtargs);
		newtargs = tmptargs;
	}

	klpm_list_free_inner(vertices, _klpm_graph_free);
	klpm_list_free(vertices);

	return newtargs;
}

static int no_dep_version(klpm_handle_t *handle)
{
	if(!handle->trans) {
		return 0;
	}
	return (handle->trans->flags & KUZPKG_TRANS_FLAG_NODEPVERSION);
}

klpm_pkg_t SYMEXPORT *klpm_find_satisfier(klpm_list_t *pkgs, const char *depstring)
{
	klpm_depend_t *dep = klpm_dep_from_string(depstring);
	if(!dep) {
		return NULL;
	}
	klpm_pkg_t *pkg = find_dep_satisfier(pkgs, dep);
	klpm_dep_free(dep);
	return pkg;
}

klpm_list_t SYMEXPORT *klpm_checkdeps(klpm_handle_t *handle,
		klpm_list_t *pkglist, klpm_list_t *rem, klpm_list_t *upgrade,
		int reversedeps)
{
	klpm_list_t *i, *j;
	klpm_list_t *dblist = NULL, *modified = NULL;
	klpm_list_t *baddeps = NULL;
	int nodepversion;

	CHECK_HANDLE(handle, return NULL);

	for(i = pkglist; i; i = i->next) {
		klpm_pkg_t *pkg = i->data;
		if(klpm_pkg_find(rem, pkg->name) || klpm_pkg_find(upgrade, pkg->name)) {
			modified = klpm_list_add(modified, pkg);
		} else {
			dblist = klpm_list_add(dblist, pkg);
		}
	}

	nodepversion = no_dep_version(handle);

	/* look for unsatisfied dependencies of the upgrade list */
	for(i = upgrade; i; i = i->next) {
		klpm_pkg_t *tp = i->data;
		_klpm_log(handle, KUZPKG_LOG_DEBUG, "checkdeps: package %s-%s\n",
				tp->name, tp->version);

		for(j = klpm_pkg_get_depends(tp); j; j = j->next) {
			klpm_depend_t *depend = j->data;
			klpm_depmod_t orig_mod = depend->mod;
			if(nodepversion) {
				depend->mod = KUZPKG_DEP_MOD_ANY;
			}
			/* 1. we check the upgrade list */
			/* 2. we check database for untouched satisfying packages */
			/* 3. we check the dependency ignore list */
			if(!find_dep_satisfier(upgrade, depend) &&
					!find_dep_satisfier(dblist, depend) &&
					!_klpm_depcmp_provides(depend, handle->assumeinstalled)) {
				/* Unsatisfied dependency in the upgrade list */
				klpm_depmissing_t *miss;
				char *missdepstring = klpm_dep_compute_string(depend);
				_klpm_log(handle, KUZPKG_LOG_DEBUG, "checkdeps: missing dependency '%s' for package '%s'\n",
						missdepstring, tp->name);
				free(missdepstring);
				miss = depmiss_new(tp->name, depend, NULL);
				baddeps = klpm_list_add(baddeps, miss);
			}
			depend->mod = orig_mod;
		}
	}

	if(reversedeps) {
		/* reversedeps handles the backwards dependencies, ie,
		 * the packages listed in the requiredby field. */
		for(i = dblist; i; i = i->next) {
			klpm_pkg_t *lp = i->data;
			for(j = klpm_pkg_get_depends(lp); j; j = j->next) {
				klpm_depend_t *depend = j->data;
				klpm_depmod_t orig_mod = depend->mod;
				if(nodepversion) {
					depend->mod = KUZPKG_DEP_MOD_ANY;
				}
				klpm_pkg_t *causingpkg = find_dep_satisfier(modified, depend);
				/* we won't break this depend, if it is already broken, we ignore it */
				/* 1. check upgrade list for satisfiers */
				/* 2. check dblist for satisfiers */
				/* 3. we check the dependency ignore list */
				if(causingpkg &&
						!find_dep_satisfier(upgrade, depend) &&
						!find_dep_satisfier(dblist, depend) &&
						!_klpm_depcmp_provides(depend, handle->assumeinstalled)) {
					klpm_depmissing_t *miss;
					char *missdepstring = klpm_dep_compute_string(depend);
					_klpm_log(handle, KUZPKG_LOG_DEBUG, "checkdeps: transaction would break '%s' dependency of '%s'\n",
							missdepstring, lp->name);
					free(missdepstring);
					miss = depmiss_new(lp->name, depend, causingpkg->name);
					baddeps = klpm_list_add(baddeps, miss);
				}
				depend->mod = orig_mod;
			}
		}
	}

	klpm_list_free(modified);
	klpm_list_free(dblist);

	return baddeps;
}

static int dep_vercmp(const char *version1, klpm_depmod_t mod,
		const char *version2)
{
	int equal = 0;

	if(mod == KUZPKG_DEP_MOD_ANY) {
		equal = 1;
	} else {
		int cmp = klpm_pkg_vercmp(version1, version2);
		switch(mod) {
			case KUZPKG_DEP_MOD_EQ: equal = (cmp == 0); break;
			case KUZPKG_DEP_MOD_GE: equal = (cmp >= 0); break;
			case KUZPKG_DEP_MOD_LE: equal = (cmp <= 0); break;
			case KUZPKG_DEP_MOD_LT: equal = (cmp < 0); break;
			case KUZPKG_DEP_MOD_GT: equal = (cmp > 0); break;
			default: equal = 1; break;
		}
	}
	return equal;
}

int _klpm_depcmp_literal(klpm_pkg_t *pkg, klpm_depend_t *dep)
{
	if(pkg->name_hash != dep->name_hash
			|| strcmp(pkg->name, dep->name) != 0) {
		/* skip more expensive checks */
		return 0;
	}
	return dep_vercmp(pkg->version, dep->mod, dep->version);
}

/**
 * @param dep dependency to check against the provision list
 * @param provisions provision list
 * @return 1 if provider is found, 0 otherwise
 */
int _klpm_depcmp_provides(klpm_depend_t *dep, klpm_list_t *provisions)
{
	int satisfy = 0;
	klpm_list_t *i;

	/* check provisions, name and version if available */
	for(i = provisions; i && !satisfy; i = i->next) {
		klpm_depend_t *provision = i->data;

		if(dep->mod == KUZPKG_DEP_MOD_ANY) {
			/* any version will satisfy the requirement */
			satisfy = (provision->name_hash == dep->name_hash
					&& strcmp(provision->name, dep->name) == 0);
		} else if(provision->mod == KUZPKG_DEP_MOD_EQ) {
			/* provision specifies a version, so try it out */
			satisfy = (provision->name_hash == dep->name_hash
					&& strcmp(provision->name, dep->name) == 0
					&& dep_vercmp(provision->version, dep->mod, dep->version));
		}
	}

	return satisfy;
}

int _klpm_depcmp(klpm_pkg_t *pkg, klpm_depend_t *dep)
{
	return _klpm_depcmp_literal(pkg, dep)
		|| _klpm_depcmp_provides(dep, klpm_pkg_get_provides(pkg));
}

klpm_depend_t SYMEXPORT *klpm_dep_from_string(const char *depstring)
{
	klpm_depend_t *depend;
	const char *ptr, *version, *desc;
	size_t deplen;

	if(depstring == NULL) {
		return NULL;
	}

	CALLOC(depend, 1, sizeof(klpm_depend_t), return NULL);

	/* Note the extra space in ": " to avoid matching the epoch */
	if((desc = strstr(depstring, ": ")) != NULL) {
		STRDUP(depend->desc, desc + 2, goto error);
		deplen = desc - depstring;
	} else {
		/* no description- point desc at NULL at end of string for later use */
		depend->desc = NULL;
		deplen = strlen(depstring);
		desc = depstring + deplen;
	}

	/* Find a version comparator if one exists. If it does, set the type and
	 * increment the ptr accordingly so we can copy the right strings. */
	if((ptr = memchr(depstring, '<', deplen))) {
		if(ptr[1] == '=') {
			depend->mod = KUZPKG_DEP_MOD_LE;
			version = ptr + 2;
		} else {
			depend->mod = KUZPKG_DEP_MOD_LT;
			version = ptr + 1;
		}
	} else if((ptr = memchr(depstring, '>', deplen))) {
		if(ptr[1] == '=') {
			depend->mod = KUZPKG_DEP_MOD_GE;
			version = ptr + 2;
		} else {
			depend->mod = KUZPKG_DEP_MOD_GT;
			version = ptr + 1;
		}
	} else if((ptr = memchr(depstring, '=', deplen))) {
		/* Note: we must do =,<,> checks after <=, >= checks */
		depend->mod = KUZPKG_DEP_MOD_EQ;
		version = ptr + 1;
	} else {
		/* no version specified, set ptr to end of string and version to NULL */
		ptr = depstring + deplen;
		depend->mod = KUZPKG_DEP_MOD_ANY;
		depend->version = NULL;
		version = NULL;
	}

	/* copy the right parts to the right places */
	STRNDUP(depend->name, depstring, ptr - depstring, goto error);
	depend->name_hash = _klpm_hash_sdbm(depend->name);
	if(version) {
		STRNDUP(depend->version, version, desc - version, goto error);
	}

	return depend;

error:
	klpm_dep_free(depend);
	return NULL;
}

klpm_depend_t *_klpm_dep_dup(const klpm_depend_t *dep)
{
	klpm_depend_t *newdep;
	CALLOC(newdep, 1, sizeof(klpm_depend_t), return NULL);

	STRDUP(newdep->name, dep->name, goto error);
	STRDUP(newdep->version, dep->version, goto error);
	STRDUP(newdep->desc, dep->desc, goto error);
	newdep->name_hash = dep->name_hash;
	newdep->mod = dep->mod;

	return newdep;

error:
	klpm_dep_free(newdep);
	return NULL;
}

/** Move package dependencies from one list to another
 * @param from list to scan for dependencies
 * @param to list to add dependencies to
 * @param pkg package whose dependencies are moved
 * @param explicit if 0, explicitly installed packages are not moved
 */
static void _klpm_select_depends(klpm_list_t **from, klpm_list_t **to,
		klpm_pkg_t *pkg, int explicit)
{
	klpm_list_t *i, *next;
	if(!klpm_pkg_get_depends(pkg)) {
		return;
	}
	for(i = *from; i; i = next) {
		klpm_pkg_t *deppkg = i->data;
		next = i->next;
		if((explicit || klpm_pkg_get_reason(deppkg) == KUZPKG_PKG_REASON_DEPEND)
				&& _klpm_pkg_depends_on(pkg, deppkg)) {
			*to = klpm_list_add(*to, deppkg);
			*from = klpm_list_remove_item(*from, i);
			free(i);
		}
	}
}

/**
 * @brief Adds unneeded dependencies to an existing list of packages.
 * By unneeded, we mean dependencies that are only required by packages in the
 * target list, so they can be safely removed.
 * If the input list was topo sorted, the output list will be topo sorted too.
 *
 * @param db package database to do dependency tracing in
 * @param *targs pointer to a list of packages
 * @param include_explicit if 0, explicitly installed packages are not included
 * @return 0 on success, -1 on errors
 */
int _klpm_recursedeps(klpm_db_t *db, klpm_list_t **targs, int include_explicit)
{
	klpm_list_t *i, *keep, *rem = NULL;

	if(db == NULL || targs == NULL) {
		return -1;
	}

	keep = klpm_list_copy(_klpm_db_get_pkgcache(db));
	for(i = *targs; i; i = i->next) {
		keep = klpm_list_remove(keep, i->data, _klpm_pkg_cmp, NULL);
	}

	/* recursively select all dependencies for removal */
	for(i = *targs; i; i = i->next) {
		_klpm_select_depends(&keep, &rem, i->data, include_explicit);
	}
	for(i = rem; i; i = i->next) {
		_klpm_select_depends(&keep, &rem, i->data, include_explicit);
	}

	/* recursively select any still needed packages to keep */
	for(i = keep; i && rem; i = i->next) {
		_klpm_select_depends(&rem, &keep, i->data, 1);
	}
	klpm_list_free(keep);

	/* copy selected packages into the target list */
	for(i = rem; i; i = i->next) {
		klpm_pkg_t *pkg = i->data, *copy = NULL;
		_klpm_log(db->handle, KUZPKG_LOG_DEBUG,
				"adding '%s' to the targets\n", pkg->name);
		if(_klpm_pkg_dup(pkg, &copy)) {
			/* we return memory on "non-fatal" error in _klpm_pkg_dup */
			_klpm_pkg_free(copy);
			klpm_list_free(rem);
			return -1;
		}
		*targs = klpm_list_add(*targs, copy);
	}
	klpm_list_free(rem);

	return 0;
}

/**
 * helper function for resolvedeps: search for dep satisfier in dbs
 *
 * @param handle the context handle
 * @param dep is the dependency to search for
 * @param dbs are the databases to search
 * @param excluding are the packages to exclude from the search
 * @param prompt if true, ask an klpm_question_install_ignorepkg_t to decide
 *        if ignored packages should be installed; if false, skip ignored
 *        packages.
 * @return the resolved package
 **/
static klpm_pkg_t *resolvedep(klpm_handle_t *handle, klpm_depend_t *dep,
		klpm_list_t *dbs, klpm_list_t *excluding, int prompt)
{
	klpm_list_t *i, *j;
	int ignored = 0;

	klpm_list_t *providers = NULL;
	int count;

	/* 1. literals */
	for(i = dbs; i; i = i->next) {
		klpm_pkg_t *pkg;
		klpm_db_t *db = i->data;

		if(!(db->usage & (KUZPKG_DB_USAGE_INSTALL|KUZPKG_DB_USAGE_UPGRADE))) {
			continue;
		}

		pkg = _klpm_db_get_pkgfromcache(db, dep->name);
		if(pkg && _klpm_depcmp_literal(pkg, dep)
				&& !klpm_pkg_find(excluding, pkg->name)) {
			if(klpm_pkg_should_ignore(handle, pkg)) {
				klpm_question_install_ignorepkg_t question = {
					.type = KUZPKG_QUESTION_INSTALL_IGNOREPKG,
					.install = 0,
					.pkg = pkg
				};
				if(prompt) {
					QUESTION(handle, &question);
				} else {
					_klpm_log(handle, KUZPKG_LOG_WARNING, _("ignoring package %s-%s\n"),
							pkg->name, pkg->version);
				}
				if(!question.install) {
					ignored = 1;
					continue;
				}
			}
			return pkg;
		}
	}
	/* 2. satisfiers (skip literals here) */
	for(i = dbs; i; i = i->next) {
		klpm_db_t *db = i->data;
		if(!(db->usage & (KUZPKG_DB_USAGE_INSTALL|KUZPKG_DB_USAGE_UPGRADE))) {
			continue;
		}
		for(j = _klpm_db_get_pkgcache(db); j; j = j->next) {
			klpm_pkg_t *pkg = j->data;
			if((pkg->name_hash != dep->name_hash || strcmp(pkg->name, dep->name) != 0)
					&& _klpm_depcmp_provides(dep, klpm_pkg_get_provides(pkg))
					&& !klpm_pkg_find(excluding, pkg->name)) {
				if(klpm_pkg_should_ignore(handle, pkg)) {
					klpm_question_install_ignorepkg_t question = {
						.type = KUZPKG_QUESTION_INSTALL_IGNOREPKG,
						.install = 0,
						.pkg = pkg
					};
					if(prompt) {
						QUESTION(handle, &question);
					} else {
						_klpm_log(handle, KUZPKG_LOG_WARNING, _("ignoring package %s-%s\n"),
								pkg->name, pkg->version);
					}
					if(!question.install) {
						ignored = 1;
						continue;
					}
				}
				_klpm_log(handle, KUZPKG_LOG_DEBUG, "provider found (%s provides %s)\n",
						pkg->name, dep->name);

				/* provide is already installed so return early instead of prompting later */
				if(_klpm_db_get_pkgfromcache(handle->db_local, pkg->name)) {
					klpm_list_free(providers);
					return pkg;
				}

				providers = klpm_list_add(providers, pkg);
				/* keep looking for other providers in the all dbs */
			}
		}
	}

	count = klpm_list_count(providers);
	if(count >= 1) {
		klpm_question_select_provider_t question = {
			.type = KUZPKG_QUESTION_SELECT_PROVIDER,
			/* default to first provider if there is no QUESTION callback */
			.use_index = 0,
			.providers = providers,
			.depend = dep
		};
		if(count > 1) {
			/* if there is more than one provider, we ask the user */
			QUESTION(handle, &question);
		}
		if(question.use_index >= 0 && question.use_index < count) {
			klpm_list_t *nth = klpm_list_nth(providers, question.use_index);
			klpm_pkg_t *pkg = nth->data;
			klpm_list_free(providers);
			return pkg;
		}
		klpm_list_free(providers);
		providers = NULL;
	}

	if(ignored) { /* resolvedeps will override these */
		handle->pm_errno = KUZPKG_ERR_PKG_IGNORED;
	} else {
		handle->pm_errno = KUZPKG_ERR_PKG_NOT_FOUND;
	}
	return NULL;
}

klpm_pkg_t SYMEXPORT *klpm_find_dbs_satisfier(klpm_handle_t *handle,
		klpm_list_t *dbs, const char *depstring)
{
	klpm_depend_t *dep;
	klpm_pkg_t *pkg;

	CHECK_HANDLE(handle, return NULL);
	ASSERT(dbs, RET_ERR(handle, KUZPKG_ERR_WRONG_ARGS, NULL));

	dep = klpm_dep_from_string(depstring);
	ASSERT(dep, return NULL);
	pkg = resolvedep(handle, dep, dbs, NULL, 1);
	klpm_dep_free(dep);
	return pkg;
}

/**
 * Computes resolvable dependencies for a given package and adds that package
 * and those resolvable dependencies to a list.
 *
 * @param handle the context handle
 * @param localpkgs is the list of local packages
 * @param pkg is the package to resolve
 * @param preferred packages to prefer when resolving
 * @param packages is a pointer to a list of packages which will be
 *        searched first for any dependency packages needed to complete the
 *        resolve, and to which will be added any [pkg] and all of its
 *        dependencies not already on the list
 * @param remove is the set of packages which will be removed in this
 *        transaction
 * @param data returns the dependency which could not be satisfied in the
 *        event of an error
 * @return 0 on success, with [pkg] and all of its dependencies not already on
 *         the [*packages] list added to that list, or -1 on failure due to an
 *         unresolvable dependency, in which case the [*packages] list will be
 *         unmodified by this function
 */
int _klpm_resolvedeps(klpm_handle_t *handle, klpm_list_t *localpkgs,
		klpm_pkg_t *pkg, klpm_list_t *preferred, klpm_list_t **packages,
		klpm_list_t *rem, klpm_list_t **data)
{
	int ret = 0;
	klpm_list_t *j;
	klpm_list_t *targ;
	klpm_list_t *deps = NULL;
	klpm_list_t *packages_copy;

	if(klpm_pkg_find(*packages, pkg->name) != NULL) {
		return 0;
	}

	/* Create a copy of the packages list, so that it can be restored
	   on error */
	packages_copy = klpm_list_copy(*packages);
	/* [pkg] has not already been resolved into the packages list, so put it
	   on that list */
	*packages = klpm_list_add(*packages, pkg);

	_klpm_log(handle, KUZPKG_LOG_DEBUG, "started resolving dependencies\n");
	targ = klpm_list_add(NULL, pkg);
	deps = klpm_checkdeps(handle, localpkgs, rem, targ, 0);
	klpm_list_free(targ);
	targ = NULL;

	for(j = deps; j; j = j->next) {
		klpm_depmissing_t *miss = j->data;
		klpm_depend_t *missdep = miss->depend;
		/* check if one of the packages in the [*packages] list already satisfies
		 * this dependency */
		if(find_dep_satisfier(*packages, missdep)) {
			klpm_depmissing_free(miss);
			continue;
		}
		/* check if one of the packages in the [preferred] list already satisfies
		 * this dependency */
		klpm_pkg_t *spkg = find_dep_satisfier(preferred, missdep);
		if(!spkg) {
			/* find a satisfier package in the given repositories */
			spkg = resolvedep(handle, missdep, handle->dbs_sync, *packages, 0);
		}
		if(spkg && _klpm_resolvedeps(handle, localpkgs, spkg, preferred, packages, rem, data) == 0) {
			_klpm_log(handle, KUZPKG_LOG_DEBUG,
					"pulling dependency %s (needed by %s)\n",
					spkg->name, pkg->name);
			klpm_depmissing_free(miss);
		} else if(resolvedep(handle, missdep, (targ = klpm_list_add(NULL, handle->db_local)), rem, 0)) {
			klpm_depmissing_free(miss);
		} else {
			handle->pm_errno = KUZPKG_ERR_UNSATISFIED_DEPS;
			char *missdepstring = klpm_dep_compute_string(missdep);
			_klpm_log(handle, KUZPKG_LOG_WARNING,
					_("cannot resolve \"%s\", a dependency of \"%s\"\n"),
					missdepstring, pkg->name);
			free(missdepstring);
			if(data) {
				*data = klpm_list_add(*data, miss);
			}
			ret = -1;
		}
		klpm_list_free(targ);
		targ = NULL;
	}
	klpm_list_free(deps);

	if(ret != 0) {
		klpm_list_free(*packages);
		*packages = packages_copy;
	} else {
		klpm_list_free(packages_copy);
	}
	_klpm_log(handle, KUZPKG_LOG_DEBUG, "finished resolving dependencies\n");
	return ret;
}

char SYMEXPORT *klpm_dep_compute_string(const klpm_depend_t *dep)
{
	const char *name, *opr, *ver, *desc_delim, *desc;
	char *str;
	size_t len;

	ASSERT(dep != NULL, return NULL);

	if(dep->name) {
		name = dep->name;
	} else {
		name = "";
	}

	switch(dep->mod) {
		case KUZPKG_DEP_MOD_ANY:
			opr = "";
			break;
		case KUZPKG_DEP_MOD_GE:
			opr = ">=";
			break;
		case KUZPKG_DEP_MOD_LE:
			opr = "<=";
			break;
		case KUZPKG_DEP_MOD_EQ:
			opr = "=";
			break;
		case KUZPKG_DEP_MOD_LT:
			opr = "<";
			break;
		case KUZPKG_DEP_MOD_GT:
			opr = ">";
			break;
		default:
			opr = "";
			break;
	}

	if(dep->mod != KUZPKG_DEP_MOD_ANY && dep->version) {
		ver = dep->version;
	} else {
		ver = "";
	}

	if(dep->desc) {
		desc_delim = ": ";
		desc = dep->desc;
	} else {
		desc_delim = "";
		desc = "";
	}

	/* we can always compute len and print the string like this because opr
	 * and ver will be empty when KUZPKG_DEP_MOD_ANY is the depend type. the
	 * reassignments above also ensure we do not do a strlen(NULL). */
	len = strlen(name) + strlen(opr) + strlen(ver)
		+ strlen(desc_delim) + strlen(desc) + 1;
	MALLOC(str, len, return NULL);
	snprintf(str, len, "%s%s%s%s%s", name, opr, ver, desc_delim, desc);

	return str;
}
