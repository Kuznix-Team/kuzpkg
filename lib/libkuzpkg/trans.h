/*
 *  trans.h
 *
 *  Copyright (C) 2026 Kuznix
 *  Copyright (c) 2002-2006 by Judd Vinet <jvinet@zeroflux.org>
 *  Copyright (c) 2005 by Aurelien Foret <orelien@chez.com>
 *  Copyright (c) 2005 by Christian Hamar <krics@linuxforum.hu>
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
#ifndef KUZPKG_TRANS_H
#define KUZPKG_TRANS_H

#include "klpm.h"

typedef enum _klpm_transstate_t {
	STATE_IDLE = 0,
	STATE_INITIALIZED,
	STATE_PREPARED,
	STATE_DOWNLOADING,
	STATE_COMMITTING,
	STATE_COMMITTED,
	STATE_INTERRUPTED
} klpm_transstate_t;

/* Transaction */
typedef struct _klpm_trans_t {
	/* bitfield of klpm_transflag_t flags */
	int flags;
	klpm_transstate_t state;
	klpm_list_t *unresolvable;  /* list of (klpm_pkg_t *) */
	klpm_list_t *add;           /* list of (klpm_pkg_t *) */
	klpm_list_t *remove;        /* list of (klpm_pkg_t *) */
	klpm_list_t *skip_remove;   /* list of (char *) */
} klpm_trans_t;

void _klpm_trans_free(klpm_trans_t *trans);
/* flags is a bitfield of klpm_transflag_t flags */
int _klpm_trans_init(klpm_trans_t *trans, int flags);
int _klpm_runscriptlet(klpm_handle_t *handle, const char *filepath,
		const char *script, const char *ver, const char *oldver, int is_archive);

#endif /* KUZPKG_TRANS_H */
