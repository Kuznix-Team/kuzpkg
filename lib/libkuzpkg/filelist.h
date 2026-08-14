/*
 *  filelist.h
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
#ifndef KUZPKG_FILELIST_H
#define KUZPKG_FILELIST_H

#include "klpm.h"

klpm_list_t *_klpm_filelist_difference(klpm_filelist_t *filesA,
		klpm_filelist_t *filesB);

klpm_list_t *_klpm_filelist_intersection(klpm_filelist_t *filesA,
		klpm_filelist_t *filesB);

void _klpm_filelist_sort(klpm_filelist_t *filelist);

#endif /* KUZPKG_FILELIST_H */
