/*
 *  add.c
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
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h> /* int64_t */

/* libarchive */
#include <archive.h>
#include <archive_entry.h>

/* libkuzpkg */
#include "add.h"
#include "klpm.h"
#include "klpm_list.h"
#include "handle.h"
#include "libarchive-compat.h"
#include "trans.h"
#include "util.h"
#include "log.h"
#include "backup.h"
#include "package.h"
#include "db.h"
#include "remove.h"
#include "handle.h"

int SYMEXPORT klpm_add_pkg(klpm_handle_t *handle, klpm_pkg_t *pkg)
{
	const char *pkgname, *pkgver;
	klpm_trans_t *trans;
	klpm_pkg_t *local;
	klpm_pkg_t *dup;

	/* Sanity checks */
	CHECK_HANDLE(handle, return -1);
	ASSERT(pkg != NULL, RET_ERR(handle, KUZPKG_ERR_WRONG_ARGS, -1));
	ASSERT(pkg->origin != KUZPKG_PKG_FROM_LOCALDB,
			RET_ERR(handle, KUZPKG_ERR_WRONG_ARGS, -1));
	ASSERT(handle == pkg->handle, RET_ERR(handle, KUZPKG_ERR_WRONG_ARGS, -1));
	trans = handle->trans;
	ASSERT(trans != NULL, RET_ERR(handle, KUZPKG_ERR_TRANS_NULL, -1));
	ASSERT(trans->state == STATE_INITIALIZED,
			RET_ERR(handle, KUZPKG_ERR_TRANS_NOT_INITIALIZED, -1));

	pkgname = pkg->name;
	pkgver = pkg->version;

	_klpm_log(handle, KUZPKG_LOG_DEBUG, "adding package '%s'\n", pkgname);

	if((dup = klpm_pkg_find(trans->add, pkgname))) {
		if(dup == pkg) {
			_klpm_log(handle, KUZPKG_LOG_DEBUG, "skipping duplicate target: %s\n", pkgname);
			return 0;
		}
		/* error for separate packages with the same name */
		RET_ERR(handle, KUZPKG_ERR_TRANS_DUP_TARGET, -1);
	}

	if((local = _klpm_db_get_pkgfromcache(handle->db_local, pkgname))) {
		const char *localpkgname = local->name;
		const char *localpkgver = local->version;
		int cmp = _klpm_pkg_compare_versions(pkg, local);

		if(cmp == 0) {
			if(trans->flags & KUZPKG_TRANS_FLAG_NEEDED) {
				/* with the NEEDED flag, packages up to date are not reinstalled */
				_klpm_log(handle, KUZPKG_LOG_WARNING, _("%s-%s is up to date -- skipping\n"),
						localpkgname, localpkgver);
				return 0;
			} else if(!(trans->flags & KUZPKG_TRANS_FLAG_DOWNLOADONLY)) {
				_klpm_log(handle, KUZPKG_LOG_WARNING, _("%s-%s is up to date -- reinstalling\n"),
						localpkgname, localpkgver);
			}
		} else if(cmp < 0 && !(trans->flags & KUZPKG_TRANS_FLAG_DOWNLOADONLY)) {
			/* local version is newer */
			_klpm_log(handle, KUZPKG_LOG_WARNING, _("downgrading package %s (%s => %s)\n"),
					localpkgname, localpkgver, pkgver);
		}
	}

	/* add the package to the transaction */
	pkg->reason = KUZPKG_PKG_REASON_EXPLICIT;
	_klpm_log(handle, KUZPKG_LOG_DEBUG, "adding package %s-%s to the transaction add list\n",
						pkgname, pkgver);
	trans->add = klpm_list_add(trans->add, pkg);

	return 0;
}

static int perform_extraction(klpm_handle_t *handle, struct archive *archive,
		struct archive_entry *entry, const char *filename)
{
	int ret;
	struct archive *archive_writer;
	const int archive_flags = ARCHIVE_EXTRACT_OWNER |
	                          ARCHIVE_EXTRACT_PERM |
	                          ARCHIVE_EXTRACT_TIME |
	                          ARCHIVE_EXTRACT_UNLINK |
	                          ARCHIVE_EXTRACT_XATTR |
	                          ARCHIVE_EXTRACT_SECURE_SYMLINKS;

	archive_entry_set_pathname(entry, filename);

	archive_writer = archive_write_disk_new();
	if (archive_writer == NULL) {
		_klpm_log(handle, KUZPKG_LOG_ERROR, _("cannot allocate disk archive object"));
		klpm_logaction(handle, KUZPKG_CALLER_PREFIX,
				"error: cannot allocate disk archive object");
		return 1;
	}

	archive_write_disk_set_options(archive_writer, archive_flags);

	ret = archive_read_extract2(archive, entry, archive_writer);

	archive_write_free(archive_writer);

	if(ret == ARCHIVE_WARN && archive_errno(archive) != ENOSPC) {
		/* operation succeeded but a "non-critical" error was encountered */
		_klpm_log(handle, KUZPKG_LOG_WARNING, _("warning given when extracting %s (%s)\n"),
				filename, archive_error_string(archive));
	} else if(ret != ARCHIVE_OK) {
		_klpm_log(handle, KUZPKG_LOG_ERROR, _("could not extract %s (%s)\n"),
				filename, archive_error_string(archive));
		klpm_logaction(handle, KUZPKG_CALLER_PREFIX,
				"error: could not extract %s (%s)\n",
				filename, archive_error_string(archive));
		return 1;
	}
	return 0;
}

static int try_rename(klpm_handle_t *handle, const char *src, const char *dest)
{
	if(rename(src, dest)) {
		_klpm_log(handle, KUZPKG_LOG_ERROR, _("could not rename %s to %s (%s)\n"),
				src, dest, strerror(errno));
		klpm_logaction(handle, KUZPKG_CALLER_PREFIX,
				"error: could not rename %s to %s (%s)\n", src, dest, strerror(errno));
		return 1;
	}
	return 0;
}

static int extract_db_file(klpm_handle_t *handle, struct archive *archive,
		struct archive_entry *entry, klpm_pkg_t *newpkg, const char *entryname)
{
	char filename[PATH_MAX]; /* the actual file we're extracting */
	const char *dbfile = NULL;
	if(strcmp(entryname, ".INSTALL") == 0) {
		dbfile = "install";
	} else if(strcmp(entryname, ".CHANGELOG") == 0) {
		dbfile = "changelog";
	} else if(strcmp(entryname, ".MTREE") == 0) {
		dbfile = "mtree";
	} else if(*entryname == '.') {
		/* reserve all files starting with '.' for future possibilities */
		_klpm_log(handle, KUZPKG_LOG_DEBUG, "skipping extraction of '%s'\n", entryname);
		archive_read_data_skip(archive);
		return 0;
	}
	archive_entry_set_perm(entry, 0644);
	snprintf(filename, PATH_MAX, "%s%s-%s/%s",
			_klpm_db_path(handle->db_local), newpkg->name, newpkg->version, dbfile);
	return perform_extraction(handle, archive, entry, filename);
}

static int extract_single_file(klpm_handle_t *handle, struct archive *archive,
		struct archive_entry *entry, klpm_pkg_t *newpkg, klpm_pkg_t *oldpkg)
{
	const char *entryname = archive_entry_pathname(entry);
	mode_t entrymode = archive_entry_mode(entry);
	klpm_backup_t *backup = _klpm_needbackup(entryname, newpkg);
	char filename[PATH_MAX]; /* the actual file we're extracting */
	int needbackup = 0, notouch = 0;
	const char *hash_orig = NULL;
	int isnewfile = 0, errors = 0;
	struct stat lsbuf;
	size_t filename_len;

	if(*entryname == '.') {
		return extract_db_file(handle, archive, entry, newpkg, entryname);
	}

	if (!klpm_filelist_contains(&newpkg->files, entryname)) {
		_klpm_log(handle, KUZPKG_LOG_WARNING,
				_("file not found in file list for package %s. skipping extraction of %s\n"),
				newpkg->name, entryname);
		return 0;
	}

	/* build the new entryname relative to handle->root */
	filename_len = snprintf(filename, PATH_MAX, "%s%s", handle->root, entryname);
	if(filename_len >= PATH_MAX) {
		_klpm_log(handle, KUZPKG_LOG_ERROR,
				_("unable to extract %s%s: path too long"), handle->root, entryname);
		return 1;
	}

	/* if a file is in NoExtract then we never extract it */
	if(_klpm_fnmatch_patterns(handle->noextract, entryname) == 0) {
		_klpm_log(handle, KUZPKG_LOG_DEBUG, "%s is in NoExtract,"
				" skipping extraction of %s\n",
				entryname, filename);
		archive_read_data_skip(archive);
		return 0;
	}

	/* Check for file existence. This is one of the more crucial parts
	 * to get 'right'. Here are the possibilities, with the filesystem
	 * on the left and the package on the top:
	 * (F=file, N=node, S=symlink, D=dir)
	 *               |  F/N  |   D
	 *  non-existent |   1   |   2
	 *  F/N          |   3   |   4
	 *  D            |   5   |   6
	 *
	 *  1,2- extract, no magic necessary. lstat (llstat) will fail here.
	 *  3,4- conflict checks should have caught this. either overwrite
	 *      or backup the file.
	 *  5- file replacing directory- don't allow it.
	 *  6- skip extraction, dir already exists.
	 */

	isnewfile = llstat(filename, &lsbuf) != 0;
	if(isnewfile) {
		/* cases 1,2: file doesn't exist, skip all backup checks */
	} else if(S_ISDIR(lsbuf.st_mode) && S_ISDIR(entrymode)) {
#if 0
		uid_t entryuid = archive_entry_uid(entry);
		gid_t entrygid = archive_entry_gid(entry);
#endif

		/* case 6: existing dir, ignore it */
		if(lsbuf.st_mode != entrymode) {
			/* if filesystem perms are different than pkg perms, warn user */
			mode_t mask = 07777;
			_klpm_log(handle, KUZPKG_LOG_WARNING, _("directory permissions differ on %s\n"
					"filesystem: %o  package: %o\n"), filename, lsbuf.st_mode & mask,
					entrymode & mask);
			klpm_logaction(handle, KUZPKG_CALLER_PREFIX,
					"warning: directory permissions differ on %s, "
					"filesystem: %o  package: %o\n", filename, lsbuf.st_mode & mask,
					entrymode & mask);
		}

#if 0
		/* Disable this warning until our user management in packages has improved.
		   Currently many packages have to create users in post_install and chown the
		   directories. These all resulted in "false-positive" warnings. */

		if((entryuid != lsbuf.st_uid) || (entrygid != lsbuf.st_gid)) {
			_klpm_log(handle, KUZPKG_LOG_WARNING, _("directory ownership differs on %s\n"
					"filesystem: %u:%u  package: %u:%u\n"), filename,
					lsbuf.st_uid, lsbuf.st_gid, entryuid, entrygid);
			klpm_logaction(handle, KUZPKG_CALLER_PREFIX,
					"warning: directory ownership differs on %s, "
					"filesystem: %u:%u  package: %u:%u\n", filename,
					lsbuf.st_uid, lsbuf.st_gid, entryuid, entrygid);
		}
#endif

		_klpm_log(handle, KUZPKG_LOG_DEBUG, "extract: skipping dir extraction of %s\n",
				filename);
		archive_read_data_skip(archive);
		return 0;
	} else if(S_ISDIR(lsbuf.st_mode)) {
		/* case 5: trying to overwrite dir with file, don't allow it */
		_klpm_log(handle, KUZPKG_LOG_ERROR, _("extract: not overwriting dir with file %s\n"),
				filename);
		archive_read_data_skip(archive);
		return 1;
	} else if(S_ISDIR(entrymode)) {
		/* case 4: trying to overwrite file with dir */
		_klpm_log(handle, KUZPKG_LOG_DEBUG, "extract: overwriting file with dir %s\n",
				filename);
	} else {
		/* case 3: trying to overwrite file with file */
		/* if file is in NoUpgrade, don't touch it */
		if(_klpm_fnmatch_patterns(handle->noupgrade, entryname) == 0) {
			notouch = 1;
		} else {
			klpm_backup_t *oldbackup;
			if(oldpkg && (oldbackup = _klpm_needbackup(entryname, oldpkg))) {
				hash_orig = oldbackup->hash;
				needbackup = 1;
			} else if(backup) {
				/* allow adding backup files retroactively */
				needbackup = 1;
			}
		}
	}

	if(notouch || needbackup) {
		if(filename_len + strlen(".pacnew") >= PATH_MAX) {
			_klpm_log(handle, KUZPKG_LOG_ERROR,
					_("unable to extract %s.pacnew: path too long"), filename);
			return 1;
		}
		strcpy(filename + filename_len, ".pacnew");
		isnewfile = (llstat(filename, &lsbuf) != 0 && errno == ENOENT);
	}

	_klpm_log(handle, KUZPKG_LOG_DEBUG, "extracting %s\n", filename);
	if(perform_extraction(handle, archive, entry, filename)) {
		errors++;
		return errors;
	}

	if(backup) {
		FREE(backup->hash);
		backup->hash = klpm_compute_md5sum(filename);
	}

	if(notouch) {
		klpm_event_pacnew_created_t event = {
			.type = KUZPKG_EVENT_PACNEW_CREATED,
			.from_noupgrade = 1,
			.oldpkg = oldpkg,
			.newpkg = newpkg,
			.file = filename
		};
		/* "remove" the .pacnew suffix */
		filename[filename_len] = '\0';
		EVENT(handle, &event);
		klpm_logaction(handle, KUZPKG_CALLER_PREFIX,
				"warning: %s installed as %s.pacnew\n", filename, filename);
	} else if(needbackup) {
		char *hash_local = NULL, *hash_pkg = NULL;
		char origfile[PATH_MAX] = "";

		strncat(origfile, filename, filename_len);

		hash_local = klpm_compute_md5sum(origfile);
		hash_pkg = backup ? backup->hash : klpm_compute_md5sum(filename);

		_klpm_log(handle, KUZPKG_LOG_DEBUG, "checking hashes for %s\n", origfile);
		_klpm_log(handle, KUZPKG_LOG_DEBUG, "current:  %s\n", hash_local);
		_klpm_log(handle, KUZPKG_LOG_DEBUG, "new:      %s\n", hash_pkg);
		_klpm_log(handle, KUZPKG_LOG_DEBUG, "original: %s\n", hash_orig);

		if(hash_local && hash_pkg && strcmp(hash_local, hash_pkg) == 0) {
			/* local and new files are the same, updating anyway to get
			 * correct timestamps */
			_klpm_log(handle, KUZPKG_LOG_DEBUG, "action: installing new file: %s\n",
					origfile);
			if(try_rename(handle, filename, origfile)) {
				errors++;
			}
		} else if(hash_orig && hash_pkg && strcmp(hash_orig, hash_pkg) == 0) {
			/* original and new files are the same, leave the local version alone,
			 * including any user changes */
			_klpm_log(handle, KUZPKG_LOG_DEBUG,
					"action: leaving existing file in place\n");
			if(isnewfile) {
				unlink(filename);
			}
		} else if(hash_orig && hash_local && strcmp(hash_orig, hash_local) == 0) {
			/* installed file has NOT been changed by user,
			 * update to the new version */
			_klpm_log(handle, KUZPKG_LOG_DEBUG, "action: installing new file: %s\n",
					origfile);
			if(try_rename(handle, filename, origfile)) {
				errors++;
			}
		} else {
			/* none of the three files matched another,  leave the unpacked
			 * file alongside the local file */
			klpm_event_pacnew_created_t event = {
				.type = KUZPKG_EVENT_PACNEW_CREATED,
				.from_noupgrade = 0,
				.oldpkg = oldpkg,
				.newpkg = newpkg,
				.file = origfile
			};
			_klpm_log(handle, KUZPKG_LOG_DEBUG,
					"action: keeping current file and installing"
					" new one with .pacnew ending\n");
			EVENT(handle, &event);
			klpm_logaction(handle, KUZPKG_CALLER_PREFIX,
					"warning: %s installed as %s\n", origfile, filename);
		}

		free(hash_local);
		if(!backup) {
			free(hash_pkg);
		}
	}
	return errors;
}

static time_t get_install_time(void)
{
	time_t now;
	char *source_date_epoch;
	unsigned long long sde;
	char *endptr;

	source_date_epoch = getenv("SOURCE_DATE_EPOCH");
	if (source_date_epoch) {
		errno = 0;
		sde = strtoull(source_date_epoch, &endptr, 10);
		if (source_date_epoch == endptr || *endptr != '\0' || errno != 0) {
			now = time(NULL);
		} else {
			now = sde;
		}
	} else {
		now = time(NULL);
	}

	return now;
}

static int commit_single_pkg(klpm_handle_t *handle, klpm_pkg_t *newpkg,
		size_t pkg_current, size_t pkg_count)
{
	int ret = 0, errors = 0;
	int is_upgrade = 0;
	klpm_pkg_t *oldpkg = NULL;
	klpm_db_t *db = handle->db_local;
	klpm_trans_t *trans = handle->trans;
	klpm_progress_t progress = KUZPKG_PROGRESS_ADD_START;
	klpm_event_package_operation_t event;
	const char *log_msg = "adding";
	const char *pkgfile;
	struct archive *archive;
	struct archive_entry *entry;
	int fd, cwdfd;
	struct stat buf;

	ASSERT(trans != NULL, return -1);

	/* see if this is an upgrade. if so, remove the old package first */
	if(_klpm_db_get_pkgfromcache(db, newpkg->name) && (oldpkg = newpkg->oldpkg)) {
		int cmp = _klpm_pkg_compare_versions(newpkg, oldpkg);
		if(cmp < 0) {
			log_msg = "downgrading";
			progress = KUZPKG_PROGRESS_DOWNGRADE_START;
			event.operation = KUZPKG_PACKAGE_DOWNGRADE;
		} else if(cmp == 0) {
			log_msg = "reinstalling";
			progress = KUZPKG_PROGRESS_REINSTALL_START;
			event.operation = KUZPKG_PACKAGE_REINSTALL;
		} else {
			log_msg = "upgrading";
			progress = KUZPKG_PROGRESS_UPGRADE_START;
			event.operation = KUZPKG_PACKAGE_UPGRADE;
		}
		is_upgrade = 1;

		/* copy over the install reason */
		newpkg->reason = klpm_pkg_get_reason(oldpkg);
	} else {
		event.operation = KUZPKG_PACKAGE_INSTALL;
	}

	event.type = KUZPKG_EVENT_PACKAGE_OPERATION_START;
	event.oldpkg = oldpkg;
	event.newpkg = newpkg;
	EVENT(handle, &event);

	pkgfile = newpkg->origin_data.file;

	_klpm_log(handle, KUZPKG_LOG_DEBUG, "%s package %s-%s\n",
			log_msg, newpkg->name, newpkg->version);
		/* pre_install/pre_upgrade scriptlet */
	if(klpm_pkg_has_scriptlet(newpkg) &&
			!(trans->flags & KUZPKG_TRANS_FLAG_NOSCRIPTLET)) {
		const char *scriptlet_name = is_upgrade ? "pre_upgrade" : "pre_install";

		_klpm_runscriptlet(handle, pkgfile, scriptlet_name,
				newpkg->version, oldpkg ? oldpkg->version : NULL, 1);
	}

	/* we override any pre-set reason if we have alldeps or allexplicit set */
	if(trans->flags & KUZPKG_TRANS_FLAG_ALLDEPS) {
		newpkg->reason = KUZPKG_PKG_REASON_DEPEND;
	} else if(trans->flags & KUZPKG_TRANS_FLAG_ALLEXPLICIT) {
		newpkg->reason = KUZPKG_PKG_REASON_EXPLICIT;
	}

	if(oldpkg) {
		/* set up fake remove transaction */
		if(_klpm_remove_single_package(handle, oldpkg, newpkg, 0, 0) == -1) {
			handle->pm_errno = KUZPKG_ERR_TRANS_ABORT;
			return -1;
		}
	}

	/* prepare directory for database entries so permissions are correct after
	   changelog/install script installation */
	if(_klpm_local_db_prepare(db, newpkg)) {
		klpm_logaction(handle, KUZPKG_CALLER_PREFIX,
				"error: could not create database entry %s-%s\n",
				newpkg->name, newpkg->version);
		handle->pm_errno = KUZPKG_ERR_DB_WRITE;
		return -1;
	}

	fd = _klpm_open_archive(db->handle, pkgfile, &buf,
			&archive, KUZPKG_ERR_PKG_OPEN);
	if(fd < 0) {
		return -1;
	}

	/* save the cwd so we can restore it later */
	OPEN(cwdfd, ".", O_RDONLY | O_CLOEXEC);
	if(cwdfd < 0) {
		_klpm_log(handle, KUZPKG_LOG_ERROR, _("could not get current working directory\n"));
	}

	/* libarchive requires this for extracting hard links */
	if(chdir(handle->root) != 0) {
		_klpm_log(handle, KUZPKG_LOG_ERROR, _("could not change directory to %s (%s)\n"),
				handle->root, strerror(errno));
		_klpm_archive_read_free(archive);
		if(cwdfd >= 0) {
			close(cwdfd);
		}
		close(fd);
		return -1;
	}

	if(trans->flags & KUZPKG_TRANS_FLAG_DBONLY) {
		_klpm_log(handle, KUZPKG_LOG_DEBUG, "extracting db files\n");
		while(archive_read_next_header(archive, &entry) == ARCHIVE_OK) {
			const char *entryname = archive_entry_pathname(entry);
			if(entryname[0] == '.') {
				errors += extract_db_file(handle, archive, entry, newpkg, entryname);
			} else {
				archive_read_data_skip(archive);
			}
		}
	} else {
		_klpm_log(handle, KUZPKG_LOG_DEBUG, "extracting files\n");

		/* call PROGRESS once with 0 percent, as we sort-of skip that here */
		PROGRESS(handle, progress, newpkg->name, 0, pkg_count, pkg_current);

		while(archive_read_next_header(archive, &entry) == ARCHIVE_OK) {
			int percent;

			if(newpkg->size != 0) {
				/* Using compressed size for calculations here, as newpkg->isize is not
				 * exact when it comes to comparing to the ACTUAL uncompressed size
				 * (missing metadata sizes) */
				int64_t pos = _klpm_archive_compressed_ftell(archive);
				percent = (pos * 100) / newpkg->size;
				if(percent >= 100) {
					percent = 100;
				}
			} else {
				percent = 0;
			}

			PROGRESS(handle, progress, newpkg->name, percent, pkg_count, pkg_current);

			/* extract the next file from the archive */
			errors += extract_single_file(handle, archive, entry, newpkg, oldpkg);
		}
	}

	_klpm_archive_read_free(archive);
	close(fd);

	/* restore the old cwd if we have it */
	if(cwdfd >= 0) {
		if(fchdir(cwdfd) != 0) {
			_klpm_log(handle, KUZPKG_LOG_ERROR,
					_("could not restore working directory (%s)\n"), strerror(errno));
		}
		close(cwdfd);
	}

	if(errors) {
		ret = -1;
		if(is_upgrade) {
			_klpm_log(handle, KUZPKG_LOG_ERROR, _("problem occurred while upgrading %s\n"),
					newpkg->name);
			klpm_logaction(handle, KUZPKG_CALLER_PREFIX,
					"error: problem occurred while upgrading %s\n",
					newpkg->name);
		} else {
			_klpm_log(handle, KUZPKG_LOG_ERROR, _("problem occurred while installing %s\n"),
					newpkg->name);
			klpm_logaction(handle, KUZPKG_CALLER_PREFIX,
					"error: problem occurred while installing %s\n",
					newpkg->name);
		}
	}

	/* make an install date (in UTC) */
	newpkg->installdate = get_install_time();

	_klpm_log(handle, KUZPKG_LOG_DEBUG, "updating database\n");
	_klpm_log(handle, KUZPKG_LOG_DEBUG, "adding database entry '%s'\n", newpkg->name);

	if(_klpm_local_db_write(db, newpkg, INFRQ_ALL)) {
		_klpm_log(handle, KUZPKG_LOG_ERROR, _("could not update database entry %s-%s\n"),
				newpkg->name, newpkg->version);
		klpm_logaction(handle, KUZPKG_CALLER_PREFIX,
				"error: could not update database entry %s-%s\n",
				newpkg->name, newpkg->version);
		handle->pm_errno = KUZPKG_ERR_DB_WRITE;
		return -1;
	}

	if(_klpm_db_add_pkgincache(db, newpkg) == -1) {
		_klpm_log(handle, KUZPKG_LOG_ERROR, _("could not add entry '%s' in cache\n"),
				newpkg->name);
	}

	PROGRESS(handle, progress, newpkg->name, 100, pkg_count, pkg_current);

	switch(event.operation) {
		case KUZPKG_PACKAGE_INSTALL:
			klpm_logaction(handle, KUZPKG_CALLER_PREFIX, "installed %s (%s)\n",
					newpkg->name, newpkg->version);
			break;
		case KUZPKG_PACKAGE_DOWNGRADE:
			klpm_logaction(handle, KUZPKG_CALLER_PREFIX, "downgraded %s (%s -> %s)\n",
					newpkg->name, oldpkg->version, newpkg->version);
			break;
		case KUZPKG_PACKAGE_REINSTALL:
			klpm_logaction(handle, KUZPKG_CALLER_PREFIX, "reinstalled %s (%s)\n",
					newpkg->name, newpkg->version);
			break;
		case KUZPKG_PACKAGE_UPGRADE:
			klpm_logaction(handle, KUZPKG_CALLER_PREFIX, "upgraded %s (%s -> %s)\n",
					newpkg->name, oldpkg->version, newpkg->version);
			break;
		default:
			/* we should never reach here */
			break;
	}

	/* run the post-install script if it exists */
	if(klpm_pkg_has_scriptlet(newpkg)
			&& !(trans->flags & KUZPKG_TRANS_FLAG_NOSCRIPTLET)) {
		char *scriptlet = _klpm_local_db_pkgpath(db, newpkg, "install");
		const char *scriptlet_name = is_upgrade ? "post_upgrade" : "post_install";

		_klpm_runscriptlet(handle, scriptlet, scriptlet_name,
				newpkg->version, oldpkg ? oldpkg->version : NULL, 0);
		free(scriptlet);
	}

	event.type = KUZPKG_EVENT_PACKAGE_OPERATION_DONE;
	EVENT(handle, &event);

	return ret;
}

int _klpm_upgrade_packages(klpm_handle_t *handle)
{
	size_t pkg_count, pkg_current;
	int skip_ldconfig = 0, ret = 0;
	klpm_list_t *targ;
	klpm_trans_t *trans = handle->trans;

	if(trans->add == NULL) {
		return 0;
	}

	pkg_count = klpm_list_count(trans->add);
	pkg_current = 1;

	/* loop through our package list adding/upgrading one at a time */
	for(targ = trans->add; targ; targ = targ->next) {
		klpm_pkg_t *newpkg = targ->data;

		if(handle->trans->state == STATE_INTERRUPTED) {
			return ret;
		}

		if(commit_single_pkg(handle, newpkg, pkg_current, pkg_count)) {
			/* something screwed up on the commit, abort the trans */
			trans->state = STATE_INTERRUPTED;
			handle->pm_errno = KUZPKG_ERR_TRANS_ABORT;
			/* running ldconfig at this point could possibly screw system */
			skip_ldconfig = 1;
			ret = -1;
		}

		pkg_current++;
	}

	if(!skip_ldconfig) {
		/* run ldconfig if it exists */
		_klpm_ldconfig(handle);
	}

	return ret;
}
