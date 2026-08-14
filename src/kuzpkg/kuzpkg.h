/*
 *  kuzpkg.h
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
#ifndef PM_KUZPKG_H
#define PM_KUZPKG_H

#include <klpm_list.h>

#define KUZPKG_CALLER_PREFIX "KUZPKG"

/* database.c */
int kuzpkg_database(klpm_list_t *targets);
/* deptest.c */
int kuzpkg_deptest(klpm_list_t *targets);
/* files.c */
int kuzpkg_files(klpm_list_t *files);
/* query.c */
int kuzpkg_query(klpm_list_t *targets);
/* remove.c */
int kuzpkg_remove(klpm_list_t *targets);
/* sync.c */
int kuzpkg_sync(klpm_list_t *targets);
int sync_prepare_execute(void);
/* upgrade.c */
int kuzpkg_upgrade(klpm_list_t *targets);

#endif /* PM_KUZPKG_H */
