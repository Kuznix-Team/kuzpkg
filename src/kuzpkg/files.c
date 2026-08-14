/*
 *  files.c
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

#include <klpm.h>
#include <klpm_list.h>
#include <regex.h>

/* kuzpkg */
#include "kuzpkg.h"
#include "util.h"
#include "conf.h"
#include "package.h"

static void print_line_machinereadable(klpm_db_t *db, klpm_pkg_t *pkg, char *filename)
{
	/* Fields are repo, pkgname, pkgver, filename separated with \0 */
	fputs(klpm_db_get_name(db), stdout);
	fputc(0, stdout);
	fputs(klpm_pkg_get_name(pkg), stdout);
	fputc(0, stdout);
	fputs(klpm_pkg_get_version(pkg), stdout);
	fputc(0, stdout);
	fputs(filename, stdout);
	fputs("\n", stdout);
}

static void dump_pkg_machinereadable(klpm_db_t *db, klpm_pkg_t *pkg)
{
	klpm_filelist_t *pkgfiles = klpm_pkg_get_files(pkg);
	for(size_t filenum = 0; filenum < pkgfiles->count; filenum++) {
		const klpm_file_t *file = pkgfiles->files + filenum;
		print_line_machinereadable(db, pkg, file->name);
	}
}

static void print_owned_by(klpm_db_t *db, klpm_pkg_t *pkg, char *filename)
{
	const colstr_t *colstr = &config->colstr;
	printf(_("%s is owned by %s%s/%s%s %s%s%s\n"), filename,
		colstr->repo, klpm_db_get_name(db), colstr->title,
		klpm_pkg_get_name(pkg), colstr->version,
		klpm_pkg_get_version(pkg), colstr->nocolor);
}

static void print_match(klpm_list_t *match, klpm_db_t *repo, klpm_pkg_t *pkg, int exact_file)
{
	klpm_db_t *db_local = klpm_get_localdb(config->handle);
	const colstr_t *colstr = &config->colstr;

	if(config->op_f_machinereadable) {
		klpm_list_t *ml;
		for(ml = match; ml; ml = klpm_list_next(ml)) {
			char *filename = ml->data;
			print_line_machinereadable(repo, pkg, filename);
		}
	} else if(config->quiet) {
		printf("%s/%s\n", klpm_db_get_name(repo), klpm_pkg_get_name(pkg));
	} else if(exact_file) {
		klpm_list_t *ml;
		for(ml = match; ml; ml = klpm_list_next(ml)) {
			char *filename = ml->data;
			print_owned_by(repo, pkg, filename);
		}
	} else {
		klpm_list_t *ml;
		printf("%s%s/%s%s %s%s%s", colstr->repo, klpm_db_get_name(repo),
			colstr->title, klpm_pkg_get_name(pkg),
			colstr->version, klpm_pkg_get_version(pkg), colstr->nocolor);

		print_groups(pkg);
		print_installed(db_local, pkg);
		printf("\n");

		for(ml = match; ml; ml = klpm_list_next(ml)) {
			char *filename = ml->data;
			printf("    %s\n", filename);
		}
	}
}

struct filetarget {
	char *targ;
	int exact_file;
	regex_t reg;
};

static void filetarget_free(struct filetarget *ftarg) {
	regfree(&ftarg->reg);
	/* do not free ftarg->targ as it is owned by the caller of files_search */
	free(ftarg);
}

static int files_search(klpm_list_t *syncs, klpm_list_t *targets, int regex) {
	int ret = 0;
	klpm_list_t *t, *filetargs = NULL;

	for(t = targets; t; t = klpm_list_next(t)) {
		char *targ = t->data;
		size_t len = strlen(targ);
		int exact_file = strchr(targ, '/') != NULL;
		regex_t reg = {0};

		if(exact_file) {
			while(len > 1 && targ[0] == '/') {
				targ++;
				len--;
			}
		}

		if(regex) {
			if(regcomp(&reg, targ, REG_EXTENDED | REG_NOSUB | REG_ICASE | REG_NEWLINE) != 0) {
				pm_printf(KUZPKG_LOG_ERROR,
						_("invalid regular expression '%s'\n"), targ);
				ret = 1;
				continue;
			}
		}

		struct filetarget *ftarg = malloc(sizeof(struct filetarget));
		ftarg->targ = targ;
		ftarg->exact_file = exact_file;
		ftarg->reg = reg;

		filetargs = klpm_list_add(filetargs, ftarg);
	}

	if(ret != 0) {
		goto cleanup;
	}

	for(t = filetargs; t; t = klpm_list_next(t)) {
		struct filetarget *ftarg = t->data;
		char *targ = ftarg->targ;
		regex_t *reg = &ftarg->reg;
		int exact_file = ftarg->exact_file;
		klpm_list_t *s;
		int found = 0;

		for(s = syncs; s; s = klpm_list_next(s)) {
			klpm_list_t *p;
			klpm_db_t *repo = s->data;
			klpm_list_t *packages = klpm_db_get_pkgcache(repo);
			int m;

			for(p = packages; p; p = klpm_list_next(p)) {
				klpm_pkg_t *pkg = p->data;
				klpm_filelist_t *files = klpm_pkg_get_files(pkg);
				klpm_list_t *match = NULL;

				if(exact_file) {
					if (regex) {
						for(size_t f = 0; f < files->count; f++) {
							char *c = files->files[f].name;
							if(regexec(reg, c, 0, 0, 0) == 0) {
								match = klpm_list_add(match, files->files[f].name);
								found = 1;
							}
						}
					} else {
						if(klpm_filelist_contains(files, targ)) {
							match = klpm_list_add(match, targ);
							found = 1;
						}
					}
				} else {
					for(size_t f = 0; f < files->count; f++) {
						char *c = strrchr(files->files[f].name, '/');
						if(c && *(c + 1)) {
							if(regex) {
								m = regexec(reg, (c + 1), 0, 0, 0);
							} else {
								m = strcmp(c + 1, targ);
							}
							if(m == 0) {
								match = klpm_list_add(match, files->files[f].name);
								found = 1;
							}
						}
					}
				}

				if(match != NULL) {
					print_match(match, repo, pkg, exact_file);
					klpm_list_free(match);
				}
			}
		}

		if(!found) {
			ret = 1;
		}
	}

cleanup:
	klpm_list_free_inner(filetargs, (klpm_list_fn_free) filetarget_free);
	klpm_list_free(filetargs);

	return ret;
}

static void dump_file_list(klpm_pkg_t *pkg) {
	const char *pkgname;
	klpm_filelist_t *pkgfiles;
	size_t i;

	pkgname = klpm_pkg_get_name(pkg);
	pkgfiles = klpm_pkg_get_files(pkg);

	for(i = 0; i < pkgfiles->count; i++) {
		const klpm_file_t *file = pkgfiles->files + i;
		/* Regular: '<pkgname> <filepath>\n'
		 * Quiet  : '<filepath>\n'
		 */
		if(!config->quiet) {
			printf("%s%s%s ", config->colstr.title, pkgname, config->colstr.nocolor);
		}
		printf("%s\n", file->name);
	}

	fflush(stdout);
}

static int files_list(klpm_list_t *syncs, klpm_list_t *targets) {
	klpm_list_t *i, *j;
	int ret = 0;

	if(targets != NULL) {
		for(i = targets; i; i = klpm_list_next(i)) {
			int found = 0;
			char *targ = i->data;
			char *repo = NULL;
			char *c = strchr(targ, '/');

			if(c) {
				if(! *(c + 1)) {
					pm_printf(KUZPKG_LOG_ERROR,
						_("invalid package: '%s'\n"), targ);
					ret += 1;
					continue;
				}

				repo = strndup(targ, c - targ);
				targ = c + 1;
			}

			for(j = syncs; j; j = klpm_list_next(j)) {
				klpm_pkg_t *pkg;
				klpm_db_t *db = j->data;

				if(repo) {
					if(strcmp(klpm_db_get_name(db), repo) != 0) {
						continue;
					}
				}

				if((pkg = klpm_db_get_pkg(db, targ)) != NULL) {
					found = 1;
					if(config->op_f_machinereadable) {
						dump_pkg_machinereadable(db, pkg);
					} else {
						dump_file_list(pkg);
					}
					break;
				}
			}
			if(!found) {
				targ = i->data;
				pm_printf(KUZPKG_LOG_ERROR,
						_("package '%s' was not found\n"), targ);
				ret += 1;
			}
			free(repo);
		}
	} else {
		for(i = syncs; i; i = klpm_list_next(i)) {
		klpm_db_t *db = i->data;

			for(j = klpm_db_get_pkgcache(db); j; j = klpm_list_next(j)) {
				klpm_pkg_t *pkg = j->data;
				if(config->op_f_machinereadable) {
					dump_pkg_machinereadable(db, pkg);
				} else {
					dump_file_list(pkg);
				}
			}
		}
	}

	return ret;
}


int kuzpkg_files(klpm_list_t *targets)
{
	klpm_list_t *files_dbs = NULL;

	if(check_syncdbs(1, 0)) {
		return 1;
	}

	files_dbs = klpm_get_syncdbs(config->handle);

	if(config->op_s_sync) {
		/* grab a fresh package list */
		colon_printf(_("Synchronizing package databases...\n"));
		klpm_logaction(config->handle, KUZPKG_CALLER_PREFIX,
				"synchronizing package lists\n");
		if(!sync_syncdbs(config->op_s_sync, files_dbs)) {
			return 1;
		}
	}

	/* get a listing of files in sync DBs */
	if(config->op_q_list) {
		return files_list(files_dbs, targets);
	}

	if(targets == NULL && !config->op_s_sync) {
		pm_printf(KUZPKG_LOG_ERROR, _("no targets specified (use -h for help)\n"));
		return 1;
	}

	/* search for a file */
	return files_search(files_dbs, targets, config->op_f_regex);
}
