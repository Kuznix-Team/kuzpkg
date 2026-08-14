/*
 *  group.c
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
#include <string.h>

/* libkuzpkg */
#include "group.h"
#include "klpm_list.h"
#include "util.h"
#include "log.h"
#include "klpm.h"

klpm_group_t *_klpm_group_new(const char *name)
{
	klpm_group_t *grp;

	CALLOC(grp, 1, sizeof(klpm_group_t), return NULL);
	STRDUP(grp->name, name, free(grp); return NULL);

	return grp;
}

void _klpm_group_free(klpm_group_t *grp)
{
	if(grp == NULL) {
		return;
	}

	FREE(grp->name);
	/* do NOT free the contents of the list, just the nodes */
	klpm_list_free(grp->packages);
	FREE(grp);
}
