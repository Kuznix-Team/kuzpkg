/*
 *  handle.h
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
#ifndef KUZPKG_HANDLE_H
#define KUZPKG_HANDLE_H

#include <stdio.h>
#include <sys/types.h>
#include <regex.h>
#include <unistd.h>

#include "klpm_list.h"
#include "klpm.h"
#include "trans.h"

#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#endif

#define EVENT(h, e) \
do { \
	if((h)->eventcb) { \
		(h)->eventcb((h)->eventcb_ctx, (klpm_event_t *) (e)); \
	} \
} while(0)
#define QUESTION(h, q) \
do { \
	if((h)->questioncb) { \
		(h)->questioncb((h)->questioncb_ctx, (klpm_question_t *) (q)); \
	} \
} while(0)
#define PROGRESS(h, e, p, per, n, r) \
do { \
	if((h)->progresscb) { \
		(h)->progresscb((h)->progresscb_ctx, e, p, per, n, r); \
	} \
} while(0)

struct _klpm_handle_t {
	/* internal usage */
	klpm_db_t *db_local;    /* local db pointer */
	klpm_list_t *dbs_sync;  /* List of (klpm_db_t *) */
	FILE *logstream;        /* log file stream pointer */
	klpm_trans_t *trans;
	uid_t user;

#ifdef HAVE_LIBCURL
	/* libcurl handle */
	CURLM *curlm;
	klpm_list_t *server_errors;
#endif

	unsigned short disable_dl_timeout;
	unsigned short disable_sandbox_filesystem;
	unsigned short disable_sandbox_syscalls;
	unsigned int parallel_downloads; /* number of download streams */

#ifdef HAVE_LIBGPGME
	klpm_list_t *known_keys;  /* keys verified to be in our keychain */
#endif

	/* callback functions */
	klpm_cb_log logcb;          /* Log callback function */
	void *logcb_ctx;
	klpm_cb_download dlcb;      /* Download callback function */
	void *dlcb_ctx;
	klpm_cb_fetch fetchcb;      /* Download file callback function */
	void *fetchcb_ctx;
	klpm_cb_event eventcb;
	void *eventcb_ctx;
	klpm_cb_question questioncb;
	void *questioncb_ctx;
	klpm_cb_progress progresscb;
	void *progresscb_ctx;

	/* filesystem paths */
	char *root;              /* Root path, default '/' */
	char *dbpath;            /* Base path to kuzpkg's DBs */
	char *logfile;           /* Name of the log file */
	char *lockfile;          /* Name of the lock file */
	char *gpgdir;            /* Directory where GnuPG files are stored */
	char *sandboxuser;       /* User to switch to for sensitive operations */
	klpm_list_t *cachedirs;  /* Paths to kuzpkg cache directories */
	klpm_list_t *hookdirs;   /* Paths to hook directories */
	klpm_list_t *overwrite_files; /* Paths that may be overwritten */

	/* package lists */
	klpm_list_t *noupgrade;   /* List of packages NOT to be upgraded */
	klpm_list_t *noextract;   /* List of files NOT to extract */
	klpm_list_t *ignorepkg;   /* List of packages to ignore */
	klpm_list_t *ignoregroup; /* List of groups to ignore */
	klpm_list_t *assumeinstalled;   /* List of virtual packages used to satisfy dependencies */

	/* options */
	klpm_list_t *architectures; /* Architectures of packages we should allow */
	int usesyslog;           /* Use syslog instead of logfile? */ /* TODO move to frontend */
	int checkspace;          /* Check disk space before installing */
	char *dbext;             /* Sync DB extension */
	int siglevel;            /* Default signature verification level */
	int localfilesiglevel;   /* Signature verification level for local file
	                                       upgrade operations */
	int remotefilesiglevel;  /* Signature verification level for remote file
	                                       upgrade operations */

	/* error code */
	klpm_errno_t pm_errno;

	/* lock file descriptor */
	int lockfd;
};

klpm_handle_t *_klpm_handle_new(void);
void _klpm_handle_free(klpm_handle_t *handle);

int _klpm_handle_lock(klpm_handle_t *handle);
int _klpm_handle_unlock(klpm_handle_t *handle);

klpm_errno_t _klpm_set_directory_option(const char *value,
		char **storage, int must_exist);

#endif /* KUZPKG_HANDLE_H */
