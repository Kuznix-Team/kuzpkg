/*
 *  package.c
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
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <time.h>
#include <wchar.h>

#include <klpm.h>
#include <klpm_list.h>

/* kuzpkg */
#include "package.h"
#include "util.h"
#include "conf.h"

#define CLBUF_SIZE 4096

/* The term "title" refers to the first field of each line in the package
 * information displayed by kuzpkg. Titles are stored in the `titles` array and
 * referenced by the following indices.
 */
enum {
	T_ARCHITECTURE = 0,
	T_BACKUP_FILES,
	T_BUILD_DATE,
	T_COMPRESSED_SIZE,
	T_CONFLICTS_WITH,
	T_DEPENDS_ON,
	T_DESCRIPTION,
	T_DOWNLOAD_SIZE,
	T_GROUPS,
	T_INSTALL_DATE,
	T_INSTALL_REASON,
	T_INSTALL_SCRIPT,
	T_INSTALLED_SIZE,
	T_LICENSES,
	T_NAME,
	T_OPTIONAL_DEPS,
	T_OPTIONAL_FOR,
	T_PACKAGER,
	T_PROVIDES,
	T_REPLACES,
	T_REPOSITORY,
	T_REQUIRED_BY,
	T_SHA_256_SUM,
	T_SIGNATURES,
	T_URL,
	T_VALIDATED_BY,
	T_VERSION,
	/* the following is a sentinel and should remain in last position */
	_T_MAX
};

/* As of 2015/10/20, the longest title (all locales considered) was less than 30
 * characters long. We set the title maximum length to 50 to allow for some
 * potential growth.
 */
#define TITLE_MAXLEN 50

static char titles[_T_MAX][TITLE_MAXLEN * sizeof(wchar_t)];

/** Build the `titles` array of localized titles and pad them with spaces so
 * that they align with the longest title. Storage for strings is stack
 * allocated and naively truncated to TITLE_MAXLEN characters.
 */
static void make_aligned_titles(void)
{
	unsigned int i;
	size_t maxlen = 0;
	int maxcol = 0;
	static const wchar_t title_suffix[] = L" :";
	wchar_t wbuf[ARRAYSIZE(titles)][TITLE_MAXLEN + ARRAYSIZE(title_suffix)] = {{ 0 }};
	size_t wlen[ARRAYSIZE(wbuf)];
	int wcol[ARRAYSIZE(wbuf)];
	char *buf[ARRAYSIZE(wbuf)];
	buf[T_ARCHITECTURE] = _("Architecture");
	buf[T_BACKUP_FILES] = _("Backup Files");
	buf[T_BUILD_DATE] = _("Build Date");
	buf[T_COMPRESSED_SIZE] = _("Compressed Size");
	buf[T_CONFLICTS_WITH] = _("Conflicts With");
	buf[T_DEPENDS_ON] = _("Depends On");
	buf[T_DESCRIPTION] = _("Description");
	buf[T_DOWNLOAD_SIZE] = _("Download Size");
	buf[T_GROUPS] = _("Groups");
	buf[T_INSTALL_DATE] = _("Install Date");
	buf[T_INSTALL_REASON] = _("Install Reason");
	buf[T_INSTALL_SCRIPT] = _("Install Script");
	buf[T_INSTALLED_SIZE] = _("Installed Size");
	buf[T_LICENSES] = _("Licenses");
	buf[T_NAME] = _("Name");
	buf[T_OPTIONAL_DEPS] = _("Optional Deps");
	buf[T_OPTIONAL_FOR] = _("Optional For");
	buf[T_PACKAGER] = _("Packager");
	buf[T_PROVIDES] = _("Provides");
	buf[T_REPLACES] = _("Replaces");
	buf[T_REPOSITORY] = _("Repository");
	buf[T_REQUIRED_BY] = _("Required By");
	buf[T_SHA_256_SUM] = _("SHA-256 Sum");
	buf[T_SIGNATURES] = _("Signatures");
	buf[T_URL] = _("URL");
	buf[T_VALIDATED_BY] = _("Validated By");
	buf[T_VERSION] = _("Version");

	for(i = 0; i < ARRAYSIZE(wbuf); i++) {
		wlen[i] = mbstowcs(wbuf[i], buf[i], strlen(buf[i]) + 1);
		wcol[i] = wcswidth(wbuf[i], wlen[i]);
		if(wcol[i] > maxcol) {
			maxcol = wcol[i];
		}
		if(wlen[i] > maxlen) {
			maxlen = wlen[i];
		}
	}

	for(i = 0; i < ARRAYSIZE(wbuf); i++) {
		size_t padlen = maxcol - wcol[i];
		wmemset(wbuf[i] + wlen[i], L' ', padlen);
		wmemcpy(wbuf[i] + wlen[i] + padlen, title_suffix, ARRAYSIZE(title_suffix));
		wcstombs(titles[i], wbuf[i], sizeof(titles[i]));
	}
}

/** Turn a depends list into a text list.
 * @param deps a list with items of type klpm_depend_t
 */
static void deplist_display(const char *title,
		klpm_list_t *deps, unsigned short cols)
{
	klpm_list_t *i, *text = NULL;
	for(i = deps; i; i = klpm_list_next(i)) {
		klpm_depend_t *dep = i->data;
		text = klpm_list_add(text, klpm_dep_compute_string(dep));
	}
	list_display(title, text, cols);
	FREELIST(text);
}

/** Turn a optdepends list into a text list.
 * @param optdeps a list with items of type klpm_depend_t
 */
static void optdeplist_display(klpm_pkg_t *pkg, unsigned short cols)
{
	klpm_list_t *i, *text = NULL;
	klpm_db_t *localdb = klpm_get_localdb(config->handle);
	for(i = klpm_pkg_get_optdepends(pkg); i; i = klpm_list_next(i)) {
		klpm_depend_t *optdep = i->data;
		char *depstring = klpm_dep_compute_string(optdep);
		if(klpm_pkg_get_origin(pkg) == KUZPKG_PKG_FROM_LOCALDB) {
			if(klpm_find_satisfier(klpm_db_get_pkgcache(localdb), depstring)) {
				const char *installed = _(" [installed]");
				depstring = realloc(depstring, strlen(depstring) + strlen(installed) + 1);
				strcpy(depstring + strlen(depstring), installed);
			}
		}
		text = klpm_list_add(text, depstring);
	}
	list_display_linebreak(titles[T_OPTIONAL_DEPS], text, cols);
	FREELIST(text);
}

/**
 * Display the details of a package.
 * Extra information entails 'required by' info for sync packages and backup
 * files info for local packages.
 * @param pkg package to display information for
 * @param extra should we show extra information
 */
void dump_pkg_full(klpm_pkg_t *pkg, int extra)
{
	unsigned short cols;
	time_t bdate, idate;
	klpm_pkgfrom_t from;
	double size;
	char bdatestr[50] = "", idatestr[50] = "";
	const char *label, *reason;
	klpm_list_t *validation = NULL, *requiredby = NULL, *optionalfor = NULL;

	/* make aligned titles once only */
	static int need_alignment = 1;
	if(need_alignment) {
		need_alignment = 0;
		make_aligned_titles();
	}

	from = klpm_pkg_get_origin(pkg);

	/* set variables here, do all output below */
	bdate = (time_t)klpm_pkg_get_builddate(pkg);
	if(bdate != -1) {
		strftime(bdatestr, 50, "%c", localtime(&bdate));
	}
	idate = (time_t)klpm_pkg_get_installdate(pkg);
	if(idate != -1) {
		strftime(idatestr, 50, "%c", localtime(&idate));
	}

	switch(klpm_pkg_get_reason(pkg)) {
		case KUZPKG_PKG_REASON_EXPLICIT:
			reason = _("Explicitly installed");
			break;
		case KUZPKG_PKG_REASON_DEPEND:
			reason = _("Installed as a dependency for another package");
			break;
		default:
			reason = _("Unknown");
			break;
	}

	int v = klpm_pkg_get_validation(pkg);
	if(v) {
		if(v & KUZPKG_PKG_VALIDATION_NONE) {
			validation = klpm_list_add(validation, _("None"));
		} else {
			if(v & KUZPKG_PKG_VALIDATION_SHA256SUM) {
				validation = klpm_list_add(validation, _("SHA-256 Sum"));
			}
			if(v & KUZPKG_PKG_VALIDATION_SIGNATURE) {
				validation = klpm_list_add(validation, _("Signature"));
			}
		}
	} else {
		validation = klpm_list_add(validation, _("Unknown"));
	}

	if(extra || from == KUZPKG_PKG_FROM_LOCALDB) {
		/* compute this here so we don't get a pause in the middle of output */
		requiredby = klpm_pkg_compute_requiredby(pkg);
		optionalfor = klpm_pkg_compute_optionalfor(pkg);
	}

	cols = getcols();

	/* actual output */
	if(from == KUZPKG_PKG_FROM_SYNCDB) {
		string_display(titles[T_REPOSITORY],
				klpm_db_get_name(klpm_pkg_get_db(pkg)), cols);
	}
	string_display(titles[T_NAME], klpm_pkg_get_name(pkg), cols);
	string_display(titles[T_VERSION], klpm_pkg_get_version(pkg), cols);
	string_display(titles[T_DESCRIPTION], klpm_pkg_get_desc(pkg), cols);
	string_display(titles[T_ARCHITECTURE], klpm_pkg_get_arch(pkg), cols);
	string_display(titles[T_URL], klpm_pkg_get_url(pkg), cols);
	list_display(titles[T_LICENSES], klpm_pkg_get_licenses(pkg), cols);
	list_display(titles[T_GROUPS], klpm_pkg_get_groups(pkg), cols);
	deplist_display(titles[T_PROVIDES], klpm_pkg_get_provides(pkg), cols);
	deplist_display(titles[T_DEPENDS_ON], klpm_pkg_get_depends(pkg), cols);
	optdeplist_display(pkg, cols);

	if(extra || from == KUZPKG_PKG_FROM_LOCALDB) {
		list_display(titles[T_REQUIRED_BY], requiredby, cols);
		list_display(titles[T_OPTIONAL_FOR], optionalfor, cols);
	}
	deplist_display(titles[T_CONFLICTS_WITH], klpm_pkg_get_conflicts(pkg), cols);
	deplist_display(titles[T_REPLACES], klpm_pkg_get_replaces(pkg), cols);

	size = humanize_size(klpm_pkg_get_size(pkg), '\0', 2, &label);
	if(from == KUZPKG_PKG_FROM_SYNCDB) {
		printf("%s%s%s %.2f %s\n", config->colstr.title, titles[T_DOWNLOAD_SIZE],
			config->colstr.nocolor, size, label);
	} else if(from == KUZPKG_PKG_FROM_FILE) {
		printf("%s%s%s %.2f %s\n", config->colstr.title, titles[T_COMPRESSED_SIZE],
			config->colstr.nocolor, size, label);
	} else {
		/* autodetect size for "Installed Size" */
		label = "\0";
	}

	size = humanize_size(klpm_pkg_get_isize(pkg), label[0], 2, &label);
	printf("%s%s%s %.2f %s\n", config->colstr.title, titles[T_INSTALLED_SIZE],
			config->colstr.nocolor, size, label);

	string_display(titles[T_PACKAGER], klpm_pkg_get_packager(pkg), cols);
	string_display(titles[T_BUILD_DATE], bdatestr, cols);
	if(from == KUZPKG_PKG_FROM_LOCALDB) {
		string_display(titles[T_INSTALL_DATE], idatestr, cols);
		string_display(titles[T_INSTALL_REASON], reason, cols);
	}
	if(from == KUZPKG_PKG_FROM_FILE || from == KUZPKG_PKG_FROM_LOCALDB) {
		string_display(titles[T_INSTALL_SCRIPT],
				klpm_pkg_has_scriptlet(pkg) ? _("Yes") : _("No"), cols);
	}

	if(from == KUZPKG_PKG_FROM_SYNCDB && extra) {
		const char *base64_sig = klpm_pkg_get_base64_sig(pkg);
		klpm_list_t *keys = NULL;
		if(base64_sig) {
			unsigned char *decoded_sigdata = NULL;
			size_t data_len;
			klpm_decode_signature(base64_sig, &decoded_sigdata, &data_len);
			klpm_extract_keyid(config->handle, klpm_pkg_get_name(pkg),
					decoded_sigdata, data_len, &keys);
			free(decoded_sigdata);
		} else {
			keys = klpm_list_add(keys, _("None"));
		}

		string_display(titles[T_SHA_256_SUM], klpm_pkg_get_sha256sum(pkg), cols);
		list_display(titles[T_SIGNATURES], keys, cols);

		if(base64_sig) {
			FREELIST(keys);
		}
	} else {
		list_display(titles[T_VALIDATED_BY], validation, cols);
	}

	if(from == KUZPKG_PKG_FROM_FILE) {
		klpm_siglist_t siglist;
		int err = klpm_pkg_check_pgp_signature(pkg, &siglist);
		if(err && klpm_errno(config->handle) == KUZPKG_ERR_SIG_MISSING) {
			string_display(titles[T_SIGNATURES], _("None"), cols);
		} else if(err) {
			string_display(titles[T_SIGNATURES],
					klpm_strerror(klpm_errno(config->handle)), cols);
		} else {
			signature_display(titles[T_SIGNATURES], &siglist, cols);
		}
		klpm_siglist_cleanup(&siglist);
	}

	/* Print additional package info if info flag passed more than once */
	if(from == KUZPKG_PKG_FROM_LOCALDB && extra) {
		dump_pkg_backups(pkg, cols);
	}

	if(extra) {
		klpm_list_t *text = NULL, *pdata = klpm_pkg_get_xdata(pkg);
		while(pdata) {
			klpm_pkg_xdata_t *pd = pdata->data;
			char *formatted = NULL;
			pm_asprintf(&formatted, "%s=%s", pd->name, pd->value);
			text = klpm_list_add(text, formatted);
			pdata = pdata->next;
		}
		list_display_linebreak("Extended Data   :", text, cols);
		FREELIST(text);
	}

	/* final newline to separate packages */
	printf("\n");

	FREELIST(requiredby);
	FREELIST(optionalfor);
	klpm_list_free(validation);
}

static const char *get_backup_file_status(const char *root,
		const klpm_backup_t *backup)
{
	char path[PATH_MAX];
	const char *ret;

	snprintf(path, PATH_MAX, "%s%s", root, backup->name);

	/* if we find the file, calculate checksums, otherwise it is missing */
	if(access(path, R_OK) == 0) {
		char *md5sum = klpm_compute_md5sum(path);

		if(md5sum == NULL) {
			pm_printf(KUZPKG_LOG_ERROR,
					_("could not calculate checksums for %s\n"), path);
			return NULL;
		}

		/* if checksums don't match, file has been modified */
		if(strcmp(md5sum, backup->hash) != 0) {
			ret = "[modified]";
		} else {
			ret = "[unmodified]";
		}
		free(md5sum);
	} else {
		switch(errno) {
			case EACCES:
				ret = "[unreadable]";
				break;
			case ENOENT:
				ret = "[missing]";
				break;
			default:
				ret = "[unknown]";
		}
	}
	return ret;
}

/* Display list of backup files and their modification states
 */
void dump_pkg_backups(klpm_pkg_t *pkg, unsigned short cols)
{
	klpm_list_t *i, *text = NULL;
	const char *root = klpm_option_get_root(config->handle);
	/* package has backup files, so print them */
	for(i = klpm_pkg_get_backup(pkg); i; i = klpm_list_next(i)) {
		const klpm_backup_t *backup = i->data;
		const char *value;
		char *line;
		size_t needed;
		if(!backup->hash) {
			continue;
		}
		value = get_backup_file_status(root, backup);
		needed = strlen(root) + strlen(backup->name) + 1 + strlen(value) + 1;
		line = malloc(needed);
		if(!line) {
			goto cleanup;
		}
		snprintf(line, needed, "%s%s %s", root, backup->name, value);
		text = klpm_list_add(text, line);
	}

	list_display_linebreak(titles[T_BACKUP_FILES], text, cols);

cleanup:
	FREELIST(text);
}

/* List all files contained in a package
 */
void dump_pkg_files(klpm_pkg_t *pkg, int quiet)
{
	const char *pkgname, *root;
	klpm_filelist_t *pkgfiles;
	size_t i;

	pkgname = klpm_pkg_get_name(pkg);
	pkgfiles = klpm_pkg_get_files(pkg);
	root = klpm_option_get_root(config->handle);

	for(i = 0; i < pkgfiles->count; i++) {
		const klpm_file_t *file = pkgfiles->files + i;
		/* Regular: '<pkgname> <root><filepath>\n'
		 * Quiet  : '<root><filepath>\n'
		 */
		if(!quiet) {
			printf("%s%s%s ", config->colstr.title, pkgname, config->colstr.nocolor);
		}
		printf("%s%s\n", root, file->name);
	}

	fflush(stdout);
}

/* Display the changelog of a package
 */
void dump_pkg_changelog(klpm_pkg_t *pkg)
{
	void *fp = NULL;

	if((fp = klpm_pkg_changelog_open(pkg)) == NULL) {
		pm_printf(KUZPKG_LOG_ERROR, _("no changelog available for '%s'.\n"),
				klpm_pkg_get_name(pkg));
		return;
	} else {
		fprintf(stdout, _("Changelog for %s:\n"), klpm_pkg_get_name(pkg));
		/* allocate a buffer to get the changelog back in chunks */
		char buf[CLBUF_SIZE];
		size_t ret = 0;
		while((ret = klpm_pkg_changelog_read(buf, CLBUF_SIZE, pkg, fp))) {
			fwrite(buf, 1, ret, stdout);
		}
		klpm_pkg_changelog_close(pkg, fp);
		putchar('\n');
	}
}

void print_installed(klpm_db_t *db_local, klpm_pkg_t *pkg)
{
	const char *pkgname = klpm_pkg_get_name(pkg);
	const char *pkgver = klpm_pkg_get_version(pkg);
	klpm_pkg_t *lpkg = klpm_db_get_pkg(db_local, pkgname);
	if(lpkg) {
		const char *lpkgver = klpm_pkg_get_version(lpkg);
		const colstr_t *colstr = &config->colstr;
		if(strcmp(lpkgver, pkgver) == 0) {
			printf(" %s[%s]%s", colstr->meta, _("installed"), colstr->nocolor);
		} else {
			printf(" %s[%s: %s]%s", colstr->meta, _("installed"),
					lpkgver, colstr->nocolor);
		}
	}
}

void print_groups(klpm_pkg_t *pkg)
{
	klpm_list_t *grp;
	if((grp = klpm_pkg_get_groups(pkg)) != NULL) {
		const colstr_t *colstr = &config->colstr;
		klpm_list_t *k;
		printf(" %s(", colstr->groups);
		for(k = grp; k; k = klpm_list_next(k)) {
			const char *group = k->data;
			fputs(group, stdout);
			if(klpm_list_next(k)) {
				/* only print a spacer if there are more groups */
				putchar(' ');
			}
		}
		printf(")%s", colstr->nocolor);
	}
}

/**
 * Display the details of a search.
 * @param db the database we're searching
 * @param targets the targets we're searching for
 * @param show_status show if the package is also in the local db
 * @return -1 on error, 0 if there were matches, 1 if there were not
 */
int dump_pkg_search(klpm_db_t *db, klpm_list_t *targets, int show_status)
{
	int freelist = 0;
	klpm_db_t *db_local;
	klpm_list_t *i, *searchlist = NULL;
	unsigned short cols;
	const colstr_t *colstr = &config->colstr;

	if(show_status) {
		db_local = klpm_get_localdb(config->handle);
	}

	/* if we have a targets list, search for packages matching it */
	if(targets) {
		if(klpm_db_search(db, targets, &searchlist) != 0) {
			return -1;
		}
		freelist = 1;
	} else {
		searchlist = klpm_db_get_pkgcache(db);
		freelist = 0;
	}
	if(searchlist == NULL) {
		return 1;
	}

	cols = getcols();
	for(i = searchlist; i; i = klpm_list_next(i)) {
		klpm_pkg_t *pkg = i->data;

		if(config->quiet) {
			fputs(klpm_pkg_get_name(pkg), stdout);
		} else {
			printf("%s%s/%s%s %s%s%s", colstr->repo, klpm_db_get_name(db),
					colstr->title, klpm_pkg_get_name(pkg),
					colstr->version, klpm_pkg_get_version(pkg), colstr->nocolor);

			print_groups(pkg);
			if(show_status) {
				print_installed(db_local, pkg);
			}

			/* we need a newline and initial indent first */
			fputs("\n    ", stdout);
			indentprint(klpm_pkg_get_desc(pkg), 4, cols);
		}
		fputc('\n', stdout);
	}

	/* we only want to free if the list was a search list */
	if(freelist) {
		klpm_list_free(searchlist);
	}

	return 0;
}
