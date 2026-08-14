/*
 *  signing.h
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
#ifndef KUZPKG_SIGNING_H
#define KUZPKG_SIGNING_H

#include "klpm.h"

char *_klpm_sigpath(klpm_handle_t *handle, const char *path);
int _klpm_gpgme_checksig(klpm_handle_t *handle, const char *path,
		const char *base64_sig, klpm_siglist_t *result);

int _klpm_check_pgp_helper(klpm_handle_t *handle, const char *path,
		const char *base64_sig, int optional, int marginal, int unknown,
		klpm_siglist_t **sigdata);
int _klpm_process_siglist(klpm_handle_t *handle, const char *identifier,
		klpm_siglist_t *siglist, int optional, int marginal, int unknown);

int _klpm_key_in_keychain(klpm_handle_t *handle, const char *fpr);
int _klpm_key_import(klpm_handle_t *handle, const char *uid, const char *fpr);

#endif /* KUZPKG_SIGNING_H */
