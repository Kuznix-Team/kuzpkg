/*
 *  util.h
 *
 *  Copyright (C) 2026 Kuznix
 *  Copyright (c) 2002-2006 by Judd Vinet <jvinet@zeroflux.org>
 *  Copyright (c) 2005 by Aurelien Foret <orelien@chez.com>
 *  Copyright (c) 2005 by Christian Hamar <krics@linuxforum.hu>
 *  Copyright (c) 2006 by David Kimpe <dnaku@frugalware.org>
 *  Copyright (c) 2005, 2006 by Miklos Vajna <vmiklos@frugalware.org>
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
#ifndef KUZPKG_UTIL_H
#define KUZPKG_UTIL_H

#include "klpm_list.h"
#include "klpm.h"
#include "package.h" /* klpm_pkg_t */
#include "handle.h" /* klpm_handle_t */
#include "util-common.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h> /* size_t */
#include <sys/types.h>
#include <math.h> /* fabs */
#include <float.h> /* DBL_EPSILON */
#include <fcntl.h> /* open, close */

#include <archive.h> /* struct archive */

#ifdef ENABLE_NLS
#include <libintl.h> /* here so it doesn't need to be included elsewhere */
/* define _() as shortcut for gettext() */
#define _(str) dgettext ("libkuzpkg", str)
#else
#define _(s) (char *)s
#endif

void _klpm_alloc_fail(size_t size);

#define MALLOC(p, s, action) do { p = malloc(s); if(p == NULL) { _klpm_alloc_fail(s); action; } } while(0)
#define CALLOC(p, l, s, action) do { p = calloc(l, s); if(p == NULL) { _klpm_alloc_fail(l * s); action; } } while(0)
#define REALLOC(p, s, action) do { void* np = realloc(p, s); if(np == NULL) { _klpm_alloc_fail(s); action; } else { p = np; } } while(0)
/* This strdup macro is NULL safe- copying NULL will yield NULL */
#define STRDUP(r, s, action) do { if(s != NULL) { r = strdup(s); if(r == NULL) { _klpm_alloc_fail(strlen(s)); action; } } else { r = NULL; } } while(0)
#define STRNDUP(r, s, l, action) do { if(s != NULL) { r = strndup(s, l); if(r == NULL) { _klpm_alloc_fail(l); action; } } else { r = NULL; } } while(0)

#define FREE(p) do { free(p); p = NULL; } while(0)

#define ASSERT(cond, action) do { if(!(cond)) { action; } } while(0)

#define RET_ERR_VOID(handle, err) do { \
	_klpm_log(handle, KUZPKG_LOG_DEBUG, "returning error %d from %s (%s: %d) : %s\n", err, __func__, __FILE__, __LINE__, klpm_strerror(err)); \
	(handle)->pm_errno = (err); \
	return; } while(0)

#define RET_ERR(handle, err, ret) do { \
	_klpm_log(handle, KUZPKG_LOG_DEBUG, "returning error %d from %s (%s: %d) : %s\n", err, __func__, __FILE__, __LINE__, klpm_strerror(err)); \
	(handle)->pm_errno = (err); \
	return (ret); } while(0)

#define GOTO_ERR(handle, err, label) do { \
	_klpm_log(handle, KUZPKG_LOG_DEBUG, "got error %d at %s (%s: %d) : %s\n", err, __func__, __FILE__, __LINE__, klpm_strerror(err)); \
	(handle)->pm_errno = (err); \
	goto label; } while(0)

#define RET_ERR_ASYNC_SAFE(handle, err, ret) do { \
	(handle)->pm_errno = (err); \
	return (ret); } while(0)

#define CHECK_HANDLE(handle, action) do { if(!(handle)) { action; } (handle)->pm_errno = KUZPKG_ERR_OK; } while(0)

/** Standard buffer size used throughout the library. */
#ifdef BUFSIZ
#define KUZPKG_BUFFER_SIZE BUFSIZ
#else
#define KUZPKG_BUFFER_SIZE 8192
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif

#define OPEN(fd, path, flags) do { fd = open(path, flags | O_BINARY); } while(fd == -1 && errno == EINTR)

/**
 * Used as a buffer/state holder for _klpm_archive_fgets().
 */
struct archive_read_buffer {
	char *line;
	char *line_offset;
	size_t line_size;
	size_t max_line_size;
	size_t real_line_size;

	char *block;
	char *block_offset;
	size_t block_size;

	int ret;
};

int _klpm_makepath(const char *path);
int _klpm_makepath_mode(const char *path, mode_t mode);
int _klpm_copyfile(const char *src, const char *dest);
char *_klpm_get_fullpath(const char *path, const char *filename, const char *suffix);
size_t _klpm_strip_newline(char *str, size_t len);

int _klpm_open_archive(klpm_handle_t *handle, const char *path,
		struct stat *buf, struct archive **archive, klpm_errno_t error);
int _klpm_unpack_single(klpm_handle_t *handle, const char *archive,
		const char *prefix, const char *filename);
int _klpm_unpack(klpm_handle_t *handle, const char *archive, const char *prefix,
		klpm_list_t *list, int breakfirst);

ssize_t _klpm_files_in_directory(klpm_handle_t *handle, const char *path, int full_count);

typedef ssize_t (*_klpm_cb_io)(void *buf, ssize_t len, void *ctx);

void _klpm_reset_signals(void);
int _klpm_run_chroot(klpm_handle_t *handle, const char *cmd, char *const argv[],
		_klpm_cb_io in_cb, void *in_ctx);
int _klpm_ldconfig(klpm_handle_t *handle);
int _klpm_str_cmp(const void *s1, const void *s2);
char *_klpm_filecache_find(klpm_handle_t *handle, const char *filename);
/* Checks whether a file exists in cache */
int _klpm_filecache_exists(klpm_handle_t *handle, const char *filename);
const char *_klpm_filecache_setup(klpm_handle_t *handle);
char *_klpm_download_dir_setup(klpm_handle_t *handle, const char *dir);
void _klpm_remove_temporary_download_dir(const char *dir);

/* Unlike many uses of klpm_pkgvalidation_t, _klpm_test_checksum expects
 * an enum value rather than a bitfield. */
int _klpm_test_checksum(const char *filepath, const char *expected, klpm_pkgvalidation_t type);
int _klpm_archive_fgets(struct archive *a, struct archive_read_buffer *b);
int _klpm_splitname(const char *target, char **name, char **version,
		unsigned long *name_hash);
unsigned long _klpm_hash_sdbm(const char *str);
off_t _klpm_strtoofft(const char *line);
klpm_time_t _klpm_parsedate(const char *line);
int _klpm_raw_cmp(const char *first, const char *second);
int _klpm_raw_ncmp(const char *first, const char *second, size_t max);
int _klpm_access(klpm_handle_t *handle, const char *dir, const char *file, int amode);
int _klpm_fnmatch_patterns(klpm_list_t *patterns, const char *string);
int _klpm_fnmatch(const void *pattern, const void *string);
void *_klpm_realloc(void **data, size_t *current, const size_t required);
void *_klpm_greedy_grow(void **data, size_t *current, const size_t required);
klpm_errno_t _klpm_read_file(const char *filepath, unsigned char **data, size_t *data_len);

#ifndef HAVE_STRSEP
char *strsep(char **, const char *);
#endif

/* check exported library symbols with: nm -C -D <lib> */
#define SYMEXPORT __attribute__((visibility("default")))

#define UNUSED __attribute__((unused))

#endif /* KUZPKG_UTIL_H */
