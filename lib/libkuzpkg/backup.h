/*
 *  backup.h
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
#ifndef KUZPKG_BACKUP_H
#define KUZPKG_BACKUP_H

#include "klpm_list.h"
#include "klpm.h"

int _klpm_split_backup(const char *string, klpm_backup_t **backup);
klpm_backup_t *_klpm_needbackup(const char *file, klpm_pkg_t *pkg);
void _klpm_backup_free(klpm_backup_t *backup);
klpm_backup_t *_klpm_backup_dup(const klpm_backup_t *backup);

#endif /* KUZPKG_BACKUP_H */
