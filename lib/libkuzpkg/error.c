/*
 *  error.c
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

#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#endif

/* libkuzpkg */
#include "util.h"
#include "klpm.h"
#include "handle.h"

klpm_errno_t SYMEXPORT klpm_errno(klpm_handle_t *handle)
{
	return handle->pm_errno;
}

const char SYMEXPORT *klpm_strerror(klpm_errno_t err)
{
	switch(err) {
		/* System */
		case KUZPKG_ERR_MEMORY:
			return _("out of memory!");
		case KUZPKG_ERR_SYSTEM:
			return _("unexpected system error");
		case KUZPKG_ERR_BADPERMS:
			return _("permission denied");
		case KUZPKG_ERR_NOT_A_FILE:
			return _("could not find or read file");
		case KUZPKG_ERR_NOT_A_DIR:
			return _("could not find or read directory");
		case KUZPKG_ERR_WRONG_ARGS:
			return _("wrong or NULL argument passed");
		case KUZPKG_ERR_DISK_SPACE:
			return _("not enough free disk space");
		/* Interface */
		case KUZPKG_ERR_HANDLE_NULL:
			return _("library not initialized");
		case KUZPKG_ERR_HANDLE_NOT_NULL:
			return _("library already initialized");
		case KUZPKG_ERR_HANDLE_LOCK:
			return _("unable to lock database");
		/* Databases */
		case KUZPKG_ERR_DB_OPEN:
			return _("could not open database");
		case KUZPKG_ERR_DB_CREATE:
			return _("could not create database");
		case KUZPKG_ERR_DB_NULL:
			return _("database not initialized");
		case KUZPKG_ERR_DB_NOT_NULL:
			return _("database already registered");
		case KUZPKG_ERR_DB_NOT_FOUND:
			return _("could not find database");
		case KUZPKG_ERR_DB_INVALID:
			return _("invalid or corrupted database");
		case KUZPKG_ERR_DB_INVALID_SIG:
			return _("invalid or corrupted database (PGP signature)");
		case KUZPKG_ERR_DB_VERSION:
			return _("database is incorrect version");
		case KUZPKG_ERR_DB_WRITE:
			return _("could not update database");
		case KUZPKG_ERR_DB_REMOVE:
			return _("could not remove database entry");
		/* Servers */
		case KUZPKG_ERR_SERVER_BAD_URL:
			return _("invalid url for server");
		case KUZPKG_ERR_SERVER_NONE:
			return _("no servers configured for repository");
		/* Transactions */
		case KUZPKG_ERR_TRANS_NOT_NULL:
			return _("transaction already initialized");
		case KUZPKG_ERR_TRANS_NULL:
			return _("transaction not initialized");
		case KUZPKG_ERR_TRANS_DUP_TARGET:
			return _("duplicate target");
		case KUZPKG_ERR_TRANS_DUP_FILENAME:
			return _("duplicate filename");
		case KUZPKG_ERR_TRANS_NOT_INITIALIZED:
			return _("transaction not initialized");
		case KUZPKG_ERR_TRANS_NOT_PREPARED:
			return _("transaction not prepared");
		case KUZPKG_ERR_TRANS_ABORT:
			return _("transaction aborted");
		case KUZPKG_ERR_TRANS_TYPE:
			return _("operation not compatible with the transaction type");
		case KUZPKG_ERR_TRANS_NOT_LOCKED:
			return _("transaction commit attempt when database is not locked");
		case KUZPKG_ERR_TRANS_HOOK_FAILED:
			return _("failed to run transaction hooks");
		/* Packages */
		case KUZPKG_ERR_PKG_NOT_FOUND:
			return _("could not find or read package");
		case KUZPKG_ERR_PKG_IGNORED:
			return _("operation cancelled due to ignorepkg");
		case KUZPKG_ERR_PKG_INVALID:
			return _("invalid or corrupted package");
		case KUZPKG_ERR_PKG_INVALID_CHECKSUM:
			return _("invalid or corrupted package (checksum)");
		case KUZPKG_ERR_PKG_INVALID_SIG:
			return _("invalid or corrupted package (PGP signature)");
		case KUZPKG_ERR_PKG_MISSING_SIG:
			return _("package missing required signature");
		case KUZPKG_ERR_PKG_OPEN:
			return _("cannot open package file");
		case KUZPKG_ERR_PKG_CANT_REMOVE:
			return _("cannot remove all files for package");
		case KUZPKG_ERR_PKG_INVALID_NAME:
			return _("package filename is not valid");
		case KUZPKG_ERR_PKG_INVALID_ARCH:
			return _("package architecture is not valid");
		/* Signatures */
		case KUZPKG_ERR_SIG_MISSING:
			return _("missing PGP signature");
		case KUZPKG_ERR_SIG_INVALID:
			return _("invalid PGP signature");
		/* Dependencies */
		case KUZPKG_ERR_UNSATISFIED_DEPS:
			return _("could not satisfy dependencies");
		case KUZPKG_ERR_CONFLICTING_DEPS:
			return _("conflicting dependencies");
		case KUZPKG_ERR_FILE_CONFLICTS:
			return _("conflicting files");
		/* Miscellaneous */
		case KUZPKG_ERR_RETRIEVE_PREPARE:
			return _("failed to initialize download");
		case KUZPKG_ERR_RETRIEVE:
			return _("failed to retrieve some files");
		case KUZPKG_ERR_INVALID_REGEX:
			return _("invalid regular expression");
		/* Errors from external libraries- our own wrapper error */
		case KUZPKG_ERR_LIBARCHIVE:
			/* it would be nice to use archive_error_string() here, but that
			 * requires the archive struct, so we can't. Just use a generic
			 * error string instead. */
			return _("libarchive error");
		case KUZPKG_ERR_LIBCURL:
			return _("download library error");
		case KUZPKG_ERR_GPGME:
			return _("gpgme error");
		case KUZPKG_ERR_EXTERNAL_DOWNLOAD:
			return _("error invoking external downloader");
		/* Missing compile-time features */
		case KUZPKG_ERR_MISSING_CAPABILITY_SIGNATURES:
				return _("compiled without signature support");
		/* Unknown error! */
		default:
			return _("unexpected error");
	}
}
