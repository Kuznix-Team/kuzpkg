/*
 *  pkghash.h
 *
 *  Copyright (C) 2026 Kuznix
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

#ifndef KUZPKG_PKGHASH_H
#define KUZPKG_PKGHASH_H

#include <stdlib.h>

#include "klpm.h"
#include "klpm_list.h"


/**
 * @brief A hash table for holding klpm_pkg_t objects.
 *
 * A combination of a hash table and a list, allowing for fast look-up
 * by package name but also iteration over the packages.
 */
struct _klpm_pkghash_t {
	/** data held by the hash table */
	klpm_list_t **hash_table;
	/** head node of the hash table data in normal list format */
	klpm_list_t *list;
	/** number of buckets in hash table */
	unsigned int buckets;
	/** number of entries in hash table */
	unsigned int entries;
	/** max number of entries before a resize is needed */
	unsigned int limit;
};

typedef struct _klpm_pkghash_t klpm_pkghash_t;

klpm_pkghash_t *_klpm_pkghash_create(unsigned int size);

klpm_pkghash_t *_klpm_pkghash_add(klpm_pkghash_t **hash, klpm_pkg_t *pkg);
klpm_pkghash_t *_klpm_pkghash_add_sorted(klpm_pkghash_t **hash, klpm_pkg_t *pkg);
klpm_pkghash_t *_klpm_pkghash_remove(klpm_pkghash_t *hash, klpm_pkg_t *pkg, klpm_pkg_t **data);

void _klpm_pkghash_free(klpm_pkghash_t *hash);

klpm_pkg_t *_klpm_pkghash_find(klpm_pkghash_t *hash, const char *name);

#endif /* KUZPKG_PKGHASH_H */
