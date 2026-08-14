/*
 *  sync.h
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
#ifndef KUZPKG_SYNC_H
#define KUZPKG_SYNC_H

#include "klpm.h"

int _klpm_sync_prepare(klpm_handle_t *handle, klpm_list_t **data);
int _klpm_sync_load(klpm_handle_t *handle, klpm_list_t **data);
int _klpm_sync_check(klpm_handle_t *handle, klpm_list_t **data);
int _klpm_sync_commit(klpm_handle_t *handle);

#endif /* KUZPKG_SYNC_H */
