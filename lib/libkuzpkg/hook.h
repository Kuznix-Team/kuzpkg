/*
 *  hook.h
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

#ifndef KUZPKG_HOOK_H
#define KUZPKG_HOOK_H

#include "klpm.h"

#define KUZPKG_HOOK_SUFFIX ".hook"

int _klpm_hook_run(klpm_handle_t *handle, klpm_hook_when_t when);

#endif /* KUZPKG_HOOK_H */
