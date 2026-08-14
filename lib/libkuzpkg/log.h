/*
 *  log.h
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
#ifndef KUZPKG_LOG_H
#define KUZPKG_LOG_H

#include "klpm.h"

#define KUZPKG_CALLER_PREFIX "KUZPKG"

void _klpm_log(klpm_handle_t *handle, klpm_loglevel_t flag,
		const char *fmt, ...) __attribute__((format(printf,3,4)));

#endif /* KUZPKG_LOG_H */
