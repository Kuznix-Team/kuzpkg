/*
 *  deps.h
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
#ifndef KUZPKG_DEPS_H
#define KUZPKG_DEPS_H

#include "db.h"
#include "sync.h"
#include "package.h"
#include "klpm.h"

klpm_depend_t *_klpm_dep_dup(const klpm_depend_t *dep);
klpm_list_t *_klpm_sortbydeps(klpm_handle_t *handle,
		klpm_list_t *targets, klpm_list_t *ignore, int reverse);
int _klpm_recursedeps(klpm_db_t *db, klpm_list_t **targs, int include_explicit);
int _klpm_resolvedeps(klpm_handle_t *handle, klpm_list_t *localpkgs, klpm_pkg_t *pkg,
		klpm_list_t *preferred, klpm_list_t **packages, klpm_list_t *remove,
		klpm_list_t **data);
int _klpm_depcmp_literal(klpm_pkg_t *pkg, klpm_depend_t *dep);
int _klpm_depcmp_provides(klpm_depend_t *dep, klpm_list_t *provisions);
int _klpm_depcmp(klpm_pkg_t *pkg, klpm_depend_t *dep);

#endif /* KUZPKG_DEPS_H */
