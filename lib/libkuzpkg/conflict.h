/*
 *  conflict.h
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
#ifndef KUZPKG_CONFLICT_H
#define KUZPKG_CONFLICT_H

#include "klpm.h"
#include "db.h"
#include "package.h"

klpm_conflict_t *_klpm_conflict_dup(const klpm_conflict_t *conflict);
klpm_list_t *_klpm_innerconflicts(klpm_handle_t *handle, klpm_list_t *packages);
klpm_list_t *_klpm_outerconflicts(klpm_db_t *db, klpm_list_t *packages);
klpm_list_t *_klpm_db_find_fileconflicts(klpm_handle_t *handle,
		klpm_list_t *upgrade, klpm_list_t *remove);

#endif /* KUZPKG_CONFLICT_H */
