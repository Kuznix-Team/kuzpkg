/*
 *  util.c
 *
 *  Copyright (C) 2026 Kuznix
 *  Copyright (c) 2002-2006 by Judd Vinet <jvinet@zeroflux.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License; either version 2
 *  of the License, or (at your option) any later version.
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
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>

/* libarchive */
#include <archive.h>
#include <archive_entry.h>

#ifdef HAVE_LIBSSL
#include <openssl/evp.h>
#endif

#ifdef HAVE_LIBNETTLE
#include <nettle/md5.h>
#include <nettle/sha2.h>
#endif

/* libkuzpkg */
#include "util.h"
#include "log.h"
#include "libarchive-compat.h"
#include "klpm.h"
#include "klpm_list.h"
#include "handle.h"
#include "trans.h"
#include "sandbox.h"

#ifndef HAVE_STRSEP
/**
 * Extracts tokens from a string.
 * Replaces strsep which is not portable (missing on Solaris).
 *
 * Copyright (c) 2001 by François Gouget
 * <fgouget_at_codeweavers.com>
 */
char *strsep(char **str, const char *delims)
{
	char *token;

	if(*str == NULL) {
		return NULL;
	}

	token = *str;
	while(**str != '\0') {
		if(strchr(delims, **str) != NULL) {
			**str = '\0';
			(*str)++;
			return token;
		}
		(*str)++;
	}

	*str = NULL;
	return token;
}
#endif

int _klpm_makepath(const char *path)
{
	return _klpm_makepath_mode(path, 0755);
}

/**
 * Creates a directory, including parents if needed, similar to mkdir -p.
 */
int _klpm_makepath_mode(const char *path, mode_t mode)
{
	char *ptr, *str;
	mode_t oldmask;
	int ret = 0;

	STRDUP(str, path, return 1);

	oldmask = umask(0000);

	for(ptr = str; *ptr; ptr++) {
		if(*ptr != '/' || ptr == str || ptr[-1] == '/') {
			continue;
		}

		*ptr = '\0';

		if(mkdir(str, mode) < 0 && errno != EEXIST) {
			ret = 1;
			goto done;
		}

		*ptr = '/';
	}

	if(mkdir(str, mode) < 0 && errno != EEXIST) {
		ret = 1;
	}

done:
	umask(oldmask);
	free(str);
	return ret;
}

/** Copies a file. */
int _klpm_copyfile(const char *src, const char *dest)
{
	char *buf;
	int in, out, ret = 1;
	ssize_t nread;
	struct stat st;

	MALLOC(buf, (size_t)KUZPKG_BUFFER_SIZE, return 1);

	OPEN(in, src, O_RDONLY | O_CLOEXEC);

	do {
		out = open(dest, O_WRONLY | O_CREAT | O_BINARY | O_CLOEXEC, 0000);
	} while(out == -1 && errno == EINTR);

	if(in < 0 || out < 0) {
		goto cleanup;
	}

	if(fstat(in, &st) || fchmod(out, st.st_mode)) {
		goto cleanup;
	}

	while((nread = read(in, buf, KUZPKG_BUFFER_SIZE)) > 0 || errno == EINTR) {
		ssize_t nwrite = 0;

		if(nread < 0) {
			continue;
		}

		do {
			nwrite = write(out, buf + nwrite, nread);
			if(nwrite >= 0) {
				nread -= nwrite;
			} else if(errno != EINTR) {
				goto cleanup;
			}
		} while(nread > 0);
	}

	ret = 0;

cleanup:
	free(buf);

	if(in >= 0) {
		close(in);
	}

	if(out >= 0) {
		close(out);
	}

	return ret;
}

/** Combines a directory, filename and suffix into a full path. */
char *_klpm_get_fullpath(const char *path, const char *filename,
		const char *suffix)
{
	char *filepath;
	size_t len = strlen(path) + strlen(filename) + strlen(suffix) + 1;

	MALLOC(filepath, len, return NULL);
	snprintf(filepath, len, "%s%s%s", path, filename, suffix);

	return filepath;
}

/** Trim trailing newlines from a string. */
size_t _klpm_strip_newline(char *str, size_t len)
{
	if(*str == '\0') {
		return 0;
	}

	if(len == 0) {
		len = strlen(str);
	}

	while(len > 0 && str[len - 1] == '\n') {
		len--;
	}

	str[len] = '\0';

	return len;
}

/* Compression functions */

/**
 * Open an archive for reading.
 */
int _klpm_open_archive(klpm_handle_t *handle, const char *path,
		struct stat *buf, struct archive **archive, klpm_errno_t error)
{
	int fd;
	size_t bufsize = KUZPKG_BUFFER_SIZE;
	errno = 0;

	if((*archive = archive_read_new()) == NULL) {
		RET_ERR(handle, KUZPKG_ERR_LIBARCHIVE, -1);
	}

	_klpm_archive_read_support_filter_all(*archive);
	archive_read_support_format_all(*archive);

	_klpm_log(handle, KUZPKG_LOG_DEBUG, "opening archive %s\n", path);

	OPEN(fd, path, O_RDONLY | O_CLOEXEC);

	if(fd < 0) {
		_klpm_log(handle, KUZPKG_LOG_ERROR,
				_("could not open file %s: %s\n"),
				path, strerror(errno));
		goto error;
	}

	if(fstat(fd, buf) != 0) {
		_klpm_log(handle, KUZPKG_LOG_ERROR,
				_("could not stat file %s: %s\n"),
				path, strerror(errno));
		goto error;
	}

#ifdef HAVE_STRUCT_STAT_ST_BLKSIZE
	if(buf->st_blksize > KUZPKG_BUFFER_SIZE) {
		bufsize = buf->st_blksize;
	}
#endif

	if(archive_read_open_fd(*archive, fd, bufsize) != ARCHIVE_OK) {
		_klpm_log(handle, KUZPKG_LOG_ERROR,
				_("could not open file %s: %s\n"),
				path, archive_error_string(*archive));
		goto error;
	}

	return fd;

error:
	_klpm_archive_read_free(*archive);
	*archive = NULL;

	if(fd >= 0) {
		close(fd);
	}

	RET_ERR(handle, error, -1);
}

/** Unpack a specific file in an archive. */
int _klpm_unpack_single(klpm_handle_t *handle, const char *archive,
		const char *prefix, const char *filename)
{
	klpm_list_t *list = NULL;
	int ret = 0;

	if(filename == NULL) {
		return 1;
	}

	list = klpm_list_add(list, (void *)filename);
	ret = _klpm_unpack(handle, archive, prefix, list, 1);
	klpm_list_free(list);

	return ret;
}

/** Unpack a list of files in an archive. */
int _klpm_unpack(klpm_handle_t *handle, const char *path, const char *prefix,
		klpm_list_t *list, int breakfirst)
{
	int ret = 0;
	mode_t oldmask;
	struct archive *archive;
	struct archive_entry *entry;
	struct stat buf;
	int fd, cwdfd;

	fd = _klpm_open_archive(handle, path, &buf, &archive,
			KUZPKG_ERR_PKG_OPEN);

	if(fd < 0) {
		return 1;
	}

	oldmask = umask(0022);

	OPEN(cwdfd, ".", O_RDONLY | O_CLOEXEC);

	if(cwdfd < 0) {
		_klpm_log(handle, KUZPKG_LOG_ERROR,
				_("could not get current working directory\n"));
	}

	if(chdir(prefix) != 0) {
		_klpm_log(handle, KUZPKG_LOG_ERROR,
				_("could not change directory to %s (%s)\n"),
				prefix, strerror(errno));
		ret = 1;
		goto cleanup;
	}

	while(archive_read_next_header(archive, &entry) == ARCHIVE_OK) {
		const char *entryname;
		mode_t mode;

		entryname = archive_entry_pathname(entry);

		if(entryname == NULL) {
			ret = 1;
			goto cleanup;
		}

		if(list) {
			char *entry_prefix = NULL;
			char *p;
			char *found;

			STRDUP(entry_prefix, entryname,
					ret = 1; goto cleanup);

			p = strstr(entry_prefix, "/");
			if(p) {
				*(p + 1) = '\0';
			}

			found = klpm_list_find_str(list, entry_prefix);
			free(entry_prefix);

			if(!found) {
				if(archive_read_data_skip(archive) != ARCHIVE_OK) {
					ret = 1;
					goto cleanup;
				}
				continue;
			}

			_klpm_log(handle, KUZPKG_LOG_DEBUG,
					"extracting: %s\n", entryname);
		}

		mode = archive_entry_mode(entry);

		if(S_ISREG(mode)) {
			archive_entry_set_perm(entry, 0644);
		} else if(S_ISDIR(mode)) {
			archive_entry_set_perm(entry, 0755);
		}

		{
			int readret = archive_read_extract(archive, entry, 0);

			if(readret == ARCHIVE_WARN) {
				_klpm_log(handle, KUZPKG_LOG_WARNING,
						_("warning given when extracting %s (%s)\n"),
						entryname, archive_error_string(archive));
			} else if(readret != ARCHIVE_OK) {
				_klpm_log(handle, KUZPKG_LOG_ERROR,
						_("could not extract %s (%s)\n"),
						entryname, archive_error_string(archive));
				ret = 1;
				goto cleanup;
			}
		}

		if(breakfirst) {
			break;
		}
	}

cleanup:
	umask(oldmask);
	_klpm_archive_read_free(archive);
	close(fd);

	if(cwdfd >= 0) {
		if(fchdir(cwdfd) != 0) {
			_klpm_log(handle, KUZPKG_LOG_ERROR,
					_("could not restore working directory (%s)\n"),
					strerror(errno));
		}
		close(cwdfd);
	}

	return ret;
}

/** Determine if there are files in a directory. */
ssize_t _klpm_files_in_directory(klpm_handle_t *handle, const char *path,
		int full_count)
{
	ssize_t files = 0;
	struct dirent *ent;
	DIR *dir = opendir(path);

	if(!dir) {
		if(errno == ENOTDIR) {
			_klpm_log(handle, KUZPKG_LOG_DEBUG,
					"%s was not a directory\n", path);
		} else {
			_klpm_log(handle, KUZPKG_LOG_DEBUG,
					"could not read directory %s\n", path);
		}
		return -1;
	}

	while((ent = readdir(dir)) != NULL) {
		const char *name = ent->d_name;

		if(strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
			continue;
		}

		files++;

		if(!full_count) {
			break;
		}
	}

	closedir(dir);
	return files;
}

static int should_retry(int errnum)
{
	return errnum == EAGAIN
#if EAGAIN != EWOULDBLOCK
		|| errnum == EWOULDBLOCK
#endif
		|| errnum == EINTR;
}

static int _klpm_chroot_write_to_child(klpm_handle_t *handle, int fd,
		char *buf, ssize_t *buf_size, ssize_t buf_limit,
		_klpm_cb_io out_cb, void *cb_ctx)
{
	ssize_t nwrite;

	if(*buf_size == 0) {
		if((*buf_size = out_cb(buf, buf_limit, cb_ctx)) == 0) {
			return -1;
		}
	}

	nwrite = send(fd, buf, *buf_size, MSG_NOSIGNAL);

	if(nwrite != -1) {
		*buf_size -= nwrite;
		memmove(buf, buf + nwrite, *buf_size);
	} else if(should_retry(errno)) {
	} else {
		_klpm_log(handle, KUZPKG_LOG_ERROR,
				_("unable to write to pipe (%s)\n"),
				strerror(errno));
		return -1;
	}

	return 0;
}

static void _klpm_chroot_process_output(klpm_handle_t *handle,
		const char *line)
{
	klpm_event_scriptlet_info_t event = {
		.type = KUZPKG_EVENT_SCRIPTLET_INFO,
		.line = line
	};

	klpm_logaction(handle, "KUZPKG-SCRIPTLET", "%s", line);
	EVENT(handle, &event);
}

static int _klpm_chroot_read_from_child(klpm_handle_t *handle, int fd,
		char *buf, ssize_t *buf_size, ssize_t buf_limit)
{
	ssize_t space = buf_limit - *buf_size - 2;
	ssize_t nread = read(fd, buf + *buf_size, space);

	if(nread > 0) {
		char *newline = memchr(buf + *buf_size, '\n', nread);

		*buf_size += nread;

		if(newline) {
			while(newline) {
				size_t linelen = newline - buf + 1;
				char old = buf[linelen];

				buf[linelen] = '\0';
				_klpm_chroot_process_output(handle, buf);
				buf[linelen] = old;

				*buf_size -= linelen;
				memmove(buf, buf + linelen, *buf_size);

				newline = memchr(buf, '\n', *buf_size);
			}
		} else if(nread == space) {
			strcpy(buf + *buf_size, "\n");
			_klpm_chroot_process_output(handle, buf);
			*buf_size = 0;
		}
	} else if(nread == 0) {
		if(*buf_size) {
			strcpy(buf + *buf_size, "\n");
			_klpm_chroot_process_output(handle, buf);
		}
		return -1;
	} else if(should_retry(errno)) {
	} else {
		if(*buf_size) {
			strcpy(buf + *buf_size, "\n");
			_klpm_chroot_process_output(handle, buf);
		}

		_klpm_log(handle, KUZPKG_LOG_ERROR,
				_("unable to read from pipe (%s)\n"),
				strerror(errno));
		return -1;
	}

	return 0;
}

void _klpm_reset_signals(void)
{
	int *i, signals[] = {
		SIGABRT, SIGALRM, SIGBUS, SIGCHLD, SIGCONT, SIGFPE, SIGHUP,
		SIGILL, SIGINT, SIGKILL, SIGPIPE, SIGQUIT, SIGSEGV, SIGSTOP,
		SIGTERM, SIGTSTP, SIGTTIN, SIGTTOU, SIGUSR1, SIGUSR2, SIGPROF,
		SIGSYS, SIGTRAP, SIGURG, SIGVTALRM, SIGXCPU, SIGXFSZ,
#if defined(SIGPOLL)
		SIGPOLL,
#endif
		0
	};

	struct sigaction def = { .sa_handler = SIG_DFL };

	sigemptyset(&def.sa_mask);

	for(i = signals; *i; i++) {
		sigaction(*i, &def, NULL);
	}
}

int _klpm_run_chroot(klpm_handle_t *handle, const char *cmd,
		char *const argv[], _klpm_cb_io stdin_cb, void *stdin_ctx)
{
	pid_t pid;
	int child2parent_pipefd[2], parent2child_pipefd[2];
	int cwdfd;
	int retval = 0;

#define HEAD 1
#define TAIL 0

	OPEN(cwdfd, ".", O_RDONLY | O_CLOEXEC);

	if(cwdfd < 0) {
		_klpm_log(handle, KUZPKG_LOG_ERROR,
				_("could not get current working directory\n"));
	}

	if(chdir(handle->root) != 0) {
		_klpm_log(handle, KUZPKG_LOG_ERROR,
				_("could not change directory to %s (%s)\n"),
				handle->root, strerror(errno));
		goto cleanup;
	}

	_klpm_log(handle, KUZPKG_LOG_DEBUG,
			"executing \"%s\" under chroot \"%s\"\n",
			cmd, handle->root);

	fflush(NULL);

	if(socketpair(AF_UNIX, SOCK_STREAM, 0, child2parent_pipefd) == -1) {
		_klpm_log(handle, KUZPKG_LOG_ERROR,
				_("could not create pipe (%s)\n"),
				strerror(errno));
		retval = 1;
		goto cleanup;
	}

	if(socketpair(AF_UNIX, SOCK_STREAM, 0, parent2child_pipefd) == -1) {
		_klpm_log(handle, KUZPKG_LOG_ERROR,
				_("could not create pipe (%s)\n"),
				strerror(errno));
		retval = 1;
		goto cleanup;
	}

	pid = fork();

	if(pid == -1) {
		_klpm_log(handle, KUZPKG_LOG_ERROR,
				_("could not fork a new process (%s)\n"),
				strerror(errno));
		retval = 1;
		goto cleanup;
	}

	if(pid == 0) {
		close(0);
		close(1);
		close(2);

		while(dup2(child2parent_pipefd[HEAD], 1) == -1 &&
				errno == EINTR);
		while(dup2(child2parent_pipefd[HEAD], 2) == -1 &&
				errno == EINTR);
		while(dup2(parent2child_pipefd[TAIL], 0) == -1 &&
				errno == EINTR);

		close(parent2child_pipefd[TAIL]);
		close(parent2child_pipefd[HEAD]);
		close(child2parent_pipefd[TAIL]);
		close(child2parent_pipefd[HEAD]);

		if(cwdfd >= 0) {
			close(cwdfd);
		}

		if(strcmp(handle->root, "/") != 0 &&
				chroot(handle->root) != 0) {
			fprintf(stderr,
					_("could not change the root directory (%s)\n"),
					strerror(errno));
			exit(1);
		}

		if(chdir("/") != 0) {
			fprintf(stderr,
					_("could not change directory to %s (%s)\n"),
					"/", strerror(errno));
			exit(1);
		}

		setenv("SHLVL", "1", 0);
		unsetenv("BASH_ENV");
		umask(0022);
		_klpm_reset_signals();
		_klpm_handle_free(handle);

		execv(cmd, argv);

		fprintf(stderr, _("call to execv failed (%s)\n"),
				strerror(errno));
		exit(1);
	} else {
		int status;
		char obuf[PIPE_BUF];
		char ibuf[LINE_MAX];
		ssize_t olen = 0, ilen = 0;
		nfds_t nfds = 2;
		struct pollfd fds[2];
		struct pollfd *child2parent = &(fds[0]);
		struct pollfd *parent2child = &(fds[1]);
		int poll_ret;

		child2parent->fd = child2parent_pipefd[TAIL];
		child2parent->events = POLLIN;

		fcntl(child2parent->fd, F_SETFL, O_NONBLOCK);

		close(child2parent_pipefd[HEAD]);
		close(parent2child_pipefd[TAIL]);

		if(stdin_cb) {
			parent2child->fd = parent2child_pipefd[HEAD];
			parent2child->events = POLLOUT;
			fcntl(parent2child->fd, F_SETFL, O_NONBLOCK);
		} else {
			parent2child->fd = -1;
			parent2child->events = 0;
			close(parent2child_pipefd[HEAD]);
		}

#define STOP_POLLING(p) do { \
	close((p)->fd); \
	(p)->fd = -1; \
} while(0)

		while((child2parent->fd != -1 ||
				parent2child->fd != -1) &&
				(poll_ret = poll(fds, nfds, -1)) != 0) {
			if(poll_ret == -1) {
				if(errno == EINTR) {
					continue;
				}
				break;
			}

			if(child2parent->revents & POLLIN) {
				if(_klpm_chroot_read_from_child(handle,
						child2parent->fd,
						ibuf, &ilen,
						sizeof(ibuf)) != 0) {
					STOP_POLLING(child2parent);
				}
			} else if(child2parent->revents) {
				STOP_POLLING(child2parent);
			}

			if(parent2child->revents & POLLOUT) {
				if(_klpm_chroot_write_to_child(handle,
						parent2child->fd,
						obuf, &olen,
						sizeof(obuf),
						stdin_cb,
						stdin_ctx) != 0) {
					STOP_POLLING(parent2child);
				}
			} else if(parent2child->revents) {
				STOP_POLLING(parent2child);
			}
		}

		if(ilen) {
			strcpy(ibuf + ilen, "\n");
			_klpm_chroot_process_output(handle, ibuf);
		}

#undef STOP_POLLING
#undef HEAD
#undef TAIL

		if(parent2child->fd != -1) {
			close(parent2child->fd);
		}

		if(child2parent->fd != -1) {
			close(child2parent->fd);
		}

		while(waitpid(pid, &status, 0) == -1) {
			if(errno != EINTR) {
				_klpm_log(handle, KUZPKG_LOG_ERROR,
						_("call to waitpid failed (%s)\n"),
						strerror(errno));
				retval = 1;
				goto cleanup;
			}
		}

		if(WIFEXITED(status)) {
			_klpm_log(handle, KUZPKG_LOG_DEBUG,
					"call to waitpid succeeded\n");

			if(WEXITSTATUS(status) != 0) {
				_klpm_log(handle, KUZPKG_LOG_ERROR,
						_("command failed to execute correctly\n"));
				retval = 1;
			}
		} else if(WIFSIGNALED(status)) {
			char *signal_description = strsignal(WTERMSIG(status));

			if(signal_description == NULL) {
				signal_description = _("Unknown signal");
			}

			_klpm_log(handle, KUZPKG_LOG_ERROR,
					_("command terminated by signal %d: %s\n"),
					WTERMSIG(status), signal_description);
			retval = 1;
		}
	}

cleanup:
	if(cwdfd >= 0) {
		if(fchdir(cwdfd) != 0) {
			_klpm_log(handle, KUZPKG_LOG_ERROR,
					_("could not restore working directory (%s)\n"),
					strerror(errno));
		}

		close(cwdfd);
	}

	return retval;
}

/** Run ldconfig in a chroot. */
int _klpm_ldconfig(klpm_handle_t *handle)
{
	char line[PATH_MAX];

	_klpm_log(handle, KUZPKG_LOG_DEBUG, "running ldconfig\n");

	snprintf(line, PATH_MAX, "%setc/ld.so.conf", handle->root);

	if(access(line, F_OK) == 0) {
		snprintf(line, PATH_MAX, "%s%s", handle->root, LDCONFIG);

		if(access(line, X_OK) == 0) {
			char arg0[32];
			char *argv[] = { arg0, NULL };

			strcpy(arg0, "ldconfig");

			return _klpm_run_chroot(handle, LDCONFIG, argv,
					NULL, NULL);
		}
	}

	return 0;
}

/** Compare two strings using klpm's compare signature. */
int _klpm_str_cmp(const void *s1, const void *s2)
{
	return strcmp(s1, s2);
}

/** Find a filename in a registered klpm cachedir. */
char *_klpm_filecache_find(klpm_handle_t *handle, const char *filename)
{
	char path[PATH_MAX];
	char *retpath;
	klpm_list_t *i;
	struct stat buf;

	for(i = handle->cachedirs; i; i = i->next) {
		snprintf(path, PATH_MAX, "%s%s",
				(char *)i->data, filename);

		if(stat(path, &buf) == 0) {
			if(S_ISREG(buf.st_mode)) {
				retpath = strdup(path);
				_klpm_log(handle, KUZPKG_LOG_DEBUG,
						"found cached pkg: %s\n",
						retpath);
				return retpath;
			}

			_klpm_log(handle, KUZPKG_LOG_WARNING,
					"cached pkg '%s' is not a regular file: mode=%i\n",
					path, buf.st_mode);
		} else if(errno != ENOENT) {
			_klpm_log(handle, KUZPKG_LOG_WARNING,
					"could not open '%s': %s",
					path, strerror(errno));
		}
	}

	return NULL;
}

/** Check whether a filename exists in a registered klpm cachedir. */
int _klpm_filecache_exists(klpm_handle_t *handle, const char *filename)
{
	int res;
	char *fpath = _klpm_filecache_find(handle, filename);

	res = (fpath != NULL);
	FREE(fpath);

	return res;
}

/** Find a writable package cache directory. */
const char *_klpm_filecache_setup(klpm_handle_t *handle)
{
	struct stat buf;
	klpm_list_t *i;
	char *cachedir;
	const char *tmpdir;

	for(i = handle->cachedirs; i; i = i->next) {
		cachedir = i->data;

		if(stat(cachedir, &buf) != 0) {
			_klpm_log(handle, KUZPKG_LOG_WARNING,
					_("no %s cache exists, creating...\n"),
					cachedir);

			if(_klpm_makepath(cachedir) == 0) {
				_klpm_log(handle, KUZPKG_LOG_DEBUG,
						"using cachedir: %s\n",
						cachedir);
				return cachedir;
			}
		} else if(!S_ISDIR(buf.st_mode)) {
			_klpm_log(handle, KUZPKG_LOG_DEBUG,
					"skipping cachedir, not a directory: %s\n",
					cachedir);
		} else if(_klpm_access(handle, NULL, cachedir, W_OK) != 0) {
			_klpm_log(handle, KUZPKG_LOG_DEBUG,
					"skipping cachedir, not writable: %s\n",
					cachedir);
		} else if(!(buf.st_mode &
				(S_IWUSR | S_IWGRP | S_IWOTH))) {
			_klpm_log(handle, KUZPKG_LOG_DEBUG,
					"skipping cachedir, no write bits set: %s\n",
					cachedir);
		} else {
			_klpm_log(handle, KUZPKG_LOG_DEBUG,
					"using cachedir: %s\n",
					cachedir);
			return cachedir;
		}
	}

	if((tmpdir = getenv("TMPDIR")) &&
			stat(tmpdir, &buf) == 0 &&
			S_ISDIR(buf.st_mode)) {
	} else {
		tmpdir = "/tmp";
	}

	klpm_option_add_cachedir(handle, tmpdir);
	cachedir = handle->cachedirs->prev->data;

	_klpm_log(handle, KUZPKG_LOG_DEBUG,
			"using cachedir: %s\n", cachedir);

	_klpm_log(handle, KUZPKG_LOG_WARNING,
			_("couldn't find or create package cache, using %s instead\n"),
			cachedir);

	return cachedir;
}

/** Setup directory for downloading files. */
char *_klpm_download_dir_setup(klpm_handle_t *handle, const char *dir)
{
	char *newdir = NULL;

	ASSERT(dir != NULL,
			RET_ERR(handle, KUZPKG_ERR_WRONG_ARGS, NULL));

	if(_klpm_use_sandbox(handle)) {
		struct passwd const *pw = NULL;
		errno = 0;

		pw = getpwnam(handle->sandboxuser);

		if(pw == NULL) {
			if(errno == 0) {
				_klpm_log(handle, KUZPKG_LOG_ERROR,
						_("download user '%s' does not exist\n"),
						handle->sandboxuser);
			} else {
				_klpm_log(handle, KUZPKG_LOG_ERROR,
						_("failed to get download user '%s': %s\n"),
						handle->sandboxuser,
						strerror(errno));
			}

			RET_ERR(handle,
					KUZPKG_ERR_RETRIEVE_PREPARE, NULL);
		}

		{
			const char template[] = "download-XXXXXX";
			size_t newdirlen =
				strlen(dir) + sizeof(template) + 1;

			MALLOC(newdir, newdirlen,
					RET_ERR(handle,
						KUZPKG_ERR_MEMORY, NULL));

			snprintf(newdir, newdirlen - 1,
					"%s%s", dir, template);

			if(mkdtemp(newdir) == NULL) {
				_klpm_log(handle, KUZPKG_LOG_ERROR,
						_("failed to create temporary download "
						  "directory %s: %s\n"),
						newdir, strerror(errno));

				free(newdir);

				RET_ERR(handle,
						KUZPKG_ERR_RETRIEVE_PREPARE,
						NULL);
			}

			if(chown(newdir, pw->pw_uid, pw->pw_gid) == -1) {
				_klpm_log(handle, KUZPKG_LOG_ERROR,
						_("failed to chown temporary download "
						  "directory %s: %s\n"),
						newdir, strerror(errno));

				free(newdir);

				RET_ERR(handle,
						KUZPKG_ERR_RETRIEVE_PREPARE,
						NULL);
			}

			newdir[newdirlen - 2] = '/';
			newdir[newdirlen - 1] = '\0';
		}
	} else {
		STRDUP(newdir, dir, return NULL);
	}

	return newdir;
}

/** Remove a temporary download directory. */
void _klpm_remove_temporary_download_dir(const char *dir)
{
	ASSERT(dir != NULL, return);

	{
		size_t dirlen = strlen(dir);
		struct dirent *dp = NULL;
		DIR *dirp = opendir(dir);

		if(!dirp) {
			return;
		}

		for(dp = readdir(dirp); dp != NULL; dp = readdir(dirp)) {
			if(strcmp(dp->d_name, "..") != 0 &&
					strcmp(dp->d_name, ".") != 0) {
				char name[PATH_MAX];

				if(dirlen + strlen(dp->d_name) + 2 > PATH_MAX) {
					continue;
				}

				snprintf(name, sizeof(name), "%s/%s",
						dir, dp->d_name);

				if(unlink(name)) {
					continue;
				}
			}
		}

		closedir(dirp);
		rmdir(dir);
	}
}


/*
 * Checksum support
 *
 * IMPORTANT:
 *
 * This intentionally does NOT use:
 *
 *     #if defined(HAVE_LIBSSL) || defined(HAVE_LIBNETTLE)
 *
 * with an unconditional Nettle else branch.
 *
 * When crypto=none is selected, neither HAVE_LIBSSL nor HAVE_LIBNETTLE
 * is defined. In that case the functions below compile without referencing
 * any crypto library and simply report that checksum calculation is
 * unavailable.
 */

static int md5_file(const char *path, unsigned char output[16])
{
	unsigned char *buf;
	ssize_t n;
	int fd;

#if HAVE_LIBSSL
	EVP_MD_CTX *ctx;
	const EVP_MD *md;

#elif HAVE_LIBNETTLE
	struct md5_ctx ctx;

#else
	(void)path;
	(void)output;
	return 1;
#endif

	MALLOC(buf, (size_t)KUZPKG_BUFFER_SIZE, return 1);

	OPEN(fd, path, O_RDONLY | O_CLOEXEC);

	if(fd < 0) {
		free(buf);
		return 1;
	}

#if HAVE_LIBSSL
	ctx = EVP_MD_CTX_create();
	if(ctx == NULL) {
		close(fd);
		free(buf);
		return 1;
	}

	md = EVP_get_digestbyname("MD5");
	if(md == NULL || EVP_DigestInit_ex(ctx, md, NULL) != 1) {
		EVP_MD_CTX_destroy(ctx);
		close(fd);
		free(buf);
		return 1;
	}

#elif HAVE_LIBNETTLE
	md5_init(&ctx);
#endif

	while((n = read(fd, buf, KUZPKG_BUFFER_SIZE)) > 0 ||
			errno == EINTR) {
		if(n < 0) {
			continue;
		}

#if HAVE_LIBSSL
		if(EVP_DigestUpdate(ctx, buf, (size_t)n) != 1) {
			EVP_MD_CTX_destroy(ctx);
			close(fd);
			free(buf);
			return 1;
		}

#elif HAVE_LIBNETTLE
		md5_update(&ctx, (size_t)n, buf);
#endif
	}

	close(fd);
	free(buf);

	if(n < 0) {
#if HAVE_LIBSSL
		EVP_MD_CTX_destroy(ctx);
#endif
		return 2;
	}

#if HAVE_LIBSSL
	if(EVP_DigestFinal_ex(ctx, output, NULL) != 1) {
		EVP_MD_CTX_destroy(ctx);
		return 1;
	}

	EVP_MD_CTX_destroy(ctx);

#elif HAVE_LIBNETTLE
	md5_digest(&ctx, MD5_DIGEST_SIZE, output);
#endif

	return 0;
}

static int sha256_file(const char *path, unsigned char output[32])
{
	unsigned char *buf;
	ssize_t n;
	int fd;

#if HAVE_LIBSSL
	EVP_MD_CTX *ctx;
	const EVP_MD *md;

#elif HAVE_LIBNETTLE
	struct sha256_ctx ctx;

#else
	(void)path;
	(void)output;
	return 1;
#endif

	MALLOC(buf, (size_t)KUZPKG_BUFFER_SIZE, return 1);

	OPEN(fd, path, O_RDONLY | O_CLOEXEC);

	if(fd < 0) {
		free(buf);
		return 1;
	}

#if HAVE_LIBSSL
	ctx = EVP_MD_CTX_create();
	if(ctx == NULL) {
		close(fd);
		free(buf);
		return 1;
	}

	md = EVP_get_digestbyname("SHA256");
	if(md == NULL || EVP_DigestInit_ex(ctx, md, NULL) != 1) {
		EVP_MD_CTX_destroy(ctx);
		close(fd);
		free(buf);
		return 1;
	}

#elif HAVE_LIBNETTLE
	sha256_init(&ctx);
#endif

	while((n = read(fd, buf, KUZPKG_BUFFER_SIZE)) > 0 ||
			errno == EINTR) {
		if(n < 0) {
			continue;
		}

#if HAVE_LIBSSL
		if(EVP_DigestUpdate(ctx, buf, (size_t)n) != 1) {
			EVP_MD_CTX_destroy(ctx);
			close(fd);
			free(buf);
			return 1;
		}

#elif HAVE_LIBNETTLE
		sha256_update(&ctx, (size_t)n, buf);
#endif
	}

	close(fd);
	free(buf);

	if(n < 0) {
#if HAVE_LIBSSL
		EVP_MD_CTX_destroy(ctx);
#endif
		return 2;
	}

#if HAVE_LIBSSL
	if(EVP_DigestFinal_ex(ctx, output, NULL) != 1) {
		EVP_MD_CTX_destroy(ctx);
		return 1;
	}

	EVP_MD_CTX_destroy(ctx);

#elif HAVE_LIBNETTLE
	sha256_digest(&ctx, SHA256_DIGEST_SIZE, output);
#endif

	return 0;
}

char SYMEXPORT *klpm_compute_md5sum(const char *filename)
{
	unsigned char output[16];

	ASSERT(filename != NULL, return NULL);

	if(md5_file(filename, output) > 0) {
		return NULL;
	}

	return hex_representation(output, sizeof(output));
}

char SYMEXPORT *klpm_compute_sha256sum(const char *filename)
{
	unsigned char output[32];

	ASSERT(filename != NULL, return NULL);

	if(sha256_file(filename, output) > 0) {
		return NULL;
	}

	return hex_representation(output, sizeof(output));
}

/**
 * Calculates a file's MD5 or SHA-256 digest and compares it
 * to an expected value.
 */
int _klpm_test_checksum(const char *filepath, const char *expected,
		klpm_pkgvalidation_t type)
{
	char *computed;
	int ret;

	if(type == KUZPKG_PKG_VALIDATION_MD5SUM) {
		computed = klpm_compute_md5sum(filepath);
	} else if(type == KUZPKG_PKG_VALIDATION_SHA256SUM) {
		computed = klpm_compute_sha256sum(filepath);
	} else {
		return -1;
	}

	if(expected == NULL || computed == NULL) {
		ret = -1;
	} else if(strcmp(expected, computed) != 0) {
		ret = 1;
	} else {
		ret = 0;
	}

	FREE(computed);
	return ret;
}

/* Note: does NOT handle sparse files on purpose for speed. */
int _klpm_archive_fgets(struct archive *a, struct archive_read_buffer *b)
{
	b->line_offset = b->line;

	while(1) {
		size_t block_remaining;
		char *eol;

		if(b->block + b->block_size == b->block_offset) {
			int64_t offset;

			if(b->ret == ARCHIVE_EOF) {
				goto cleanup;
			}

			b->ret = archive_read_data_block(a,
					(void *)&b->block,
					&b->block_size,
					&offset);

			b->block_offset = b->block;
			block_remaining = b->block_size;

			if(b->ret < ARCHIVE_OK) {
				goto cleanup;
			}
		} else {
			block_remaining =
				b->block + b->block_size - b->block_offset;
		}

		eol = memchr(b->block_offset, '\n', block_remaining);

		if(!eol) {
			eol = memchr(b->block_offset, '\0',
					block_remaining);
		}

		if(!b->line) {
			CALLOC(b->line,
					b->block_size + 1,
					sizeof(char),
					b->ret = -ENOMEM;
					goto cleanup);

			b->line_size = b->block_size + 1;
			b->line_offset = b->line;
		} else {
			size_t new = eol ?
				(size_t)(eol - b->block_offset) :
				block_remaining;

			size_t needed =
				(size_t)((b->line_offset - b->line) +
						new + 1);

			if(needed > b->max_line_size) {
				b->ret = -ERANGE;
				goto cleanup;
			}

			if(needed > b->line_size) {
				char *new_line;

				CALLOC(new_line,
						needed,
						sizeof(char),
						b->ret = -ENOMEM;
						goto cleanup);

				memcpy(new_line,
						b->line,
						b->line_size);

				b->line_size = needed;
				b->line_offset =
					new_line +
					(b->line_offset - b->line);

				free(b->line);
				b->line = new_line;
			}
		}

		if(eol) {
			size_t len =
				(size_t)(eol - b->block_offset);

			memcpy(b->line_offset,
					b->block_offset,
					len);

			b->line_offset[len] = '\0';
			b->block_offset = eol + 1;
			b->real_line_size =
				b->line_offset + len - b->line;

			return ARCHIVE_OK;
		}

		{
			size_t len =
				(size_t)(b->block +
						b->block_size -
						b->block_offset);

			memcpy(b->line_offset,
					b->block_offset,
					len);

			b->line_offset += len;
			b->block_offset =
				b->block + b->block_size;

			if(len == 0) {
				b->line_offset[0] = '\0';
				b->real_line_size =
					b->line_offset - b->line;
				return ARCHIVE_OK;
			}
		}
	}

cleanup:
	{
		int ret = b->ret;

		FREE(b->line);
		*b = (struct archive_read_buffer){0};

		return ret;
	}
}

/** Parse a full package specifier. */
int _klpm_splitname(const char *target, char **name, char **version,
		unsigned long *name_hash)
{
	const char *pkgver, *end;

	if(target == NULL) {
		return -1;
	}

	end = strchr(target, '/');

	if(!end) {
		end = target + strlen(target);
	}

	for(pkgver = end - 1;
			*pkgver && *pkgver != '-';
			pkgver--);

	for(pkgver = pkgver - 1;
			*pkgver && *pkgver != '-';
			pkgver--);

	if(*pkgver != '-' || pkgver == target) {
		return -1;
	}

	if(version) {
		if(*version) {
			FREE(*version);
		}

		STRNDUP(*version,
				pkgver + 1,
				end - pkgver - 1,
				return -1);
	}

	if(name) {
		if(*name) {
			FREE(*name);
		}

		STRNDUP(*name,
				target,
				pkgver - target,
				return -1);

		if(name_hash) {
			*name_hash = _klpm_hash_sdbm(*name);
		}
	}

	return 0;
}

/** Hash the given string using sdbm. */
unsigned long _klpm_hash_sdbm(const char *str)
{
	unsigned long hash = 0;
	int c;

	if(!str) {
		return hash;
	}

	while((c = *str++)) {
		hash = c + hash * 65599;
	}

	return hash;
}

/** Convert a string to a file offset. */
off_t _klpm_strtoofft(const char *line)
{
	char *end;
	unsigned long long result;

	errno = 0;

	if(!isdigit((unsigned char)line[0])) {
		return (off_t)-1;
	}

	result = strtoull(line, &end, 10);

	if(result == 0 && end == line) {
		return (off_t)-1;
	}

	if(result == ULLONG_MAX && errno == ERANGE) {
		return (off_t)-1;
	}

	if(*end) {
		return (off_t)-1;
	}

	return (off_t)result;
}

/** Parse a date into a klpm_time_t. */
klpm_time_t _klpm_parsedate(const char *line)
{
	char *end;
	long long result;

	errno = 0;

	result = strtoll(line, &end, 10);

	if(result == 0 && end == line) {
		errno = EINVAL;
		return 0;
	}

	if(errno == ERANGE) {
		return 0;
	}

	if(*end) {
		errno = EINVAL;
		return 0;
	}

	return (klpm_time_t)result;
}

/** Wrapper around access(). */
int _klpm_access(klpm_handle_t *handle, const char *dir,
		const char *file, int amode)
{
	size_t len = 0;
	int ret = 0;
	int flag = 0;

#ifdef AT_SYMLINK_NOFOLLOW
	flag |= AT_SYMLINK_NOFOLLOW;
#endif

	if(dir) {
		char *check_path;

		len = strlen(dir) + strlen(file) + 1;

		CALLOC(check_path,
				len,
				sizeof(char),
				RET_ERR(handle, KUZPKG_ERR_MEMORY, -1));

		snprintf(check_path, len, "%s%s", dir, file);

		ret = faccessat(AT_FDCWD,
				check_path,
				amode,
				flag);

		free(check_path);
	} else {
		dir = "";
		ret = faccessat(AT_FDCWD,
				file,
				amode,
				flag);
	}

	if(ret != 0) {
		if(amode & R_OK) {
			_klpm_log(handle, KUZPKG_LOG_DEBUG,
					"\"%s%s\" is not readable: %s\n",
					dir, file, strerror(errno));
		}

		if(amode & W_OK) {
			_klpm_log(handle, KUZPKG_LOG_DEBUG,
					"\"%s%s\" is not writable: %s\n",
					dir, file, strerror(errno));
		}

		if(amode & X_OK) {
			_klpm_log(handle, KUZPKG_LOG_DEBUG,
					"\"%s%s\" is not executable: %s\n",
					dir, file, strerror(errno));
		}

		if(amode == F_OK) {
			_klpm_log(handle, KUZPKG_LOG_DEBUG,
					"\"%s%s\" does not exist: %s\n",
					dir, file, strerror(errno));
		}
	}

	return ret;
}

/** Check whether a string matches a shell wildcard pattern. */
int _klpm_fnmatch_patterns(klpm_list_t *patterns, const char *string)
{
	klpm_list_t *i;
	char *pattern;
	short inverted;

	for(i = klpm_list_last(patterns);
			i;
			i = klpm_list_previous(i)) {
		pattern = i->data;

		inverted = pattern[0] == '!';

		if(inverted || pattern[0] == '\\') {
			pattern++;
		}

		if(_klpm_fnmatch(pattern, string) == 0) {
			return inverted;
		}
	}

	return -1;
}

int _klpm_fnmatch(const void *pattern, const void *string)
{
	return fnmatch(pattern, string, 0);
}

void *_klpm_realloc(void **data, size_t *current,
		const size_t required)
{
	REALLOC(*data, required, return NULL);

	if(*current < required) {
		memset((char *)*data + *current,
				0,
				required - *current);
	}

	*current = required;
	return *data;
}

void *_klpm_greedy_grow(void **data, size_t *current,
		const size_t required)
{
	size_t newsize = 0;

	if(*current >= required) {
		return data;
	}

	if(*current == 0) {
		newsize = required;
	} else {
		newsize = *current * 2;
	}

	if(newsize < required) {
		return NULL;
	}

	return _klpm_realloc(data, current, newsize);
}

void _klpm_alloc_fail(size_t size)
{
	fprintf(stderr,
			"alloc failure: could not allocate %zu bytes\n",
			size);
}

/** Read file content into a newly allocated buffer. */
klpm_errno_t _klpm_read_file(const char *filepath,
		unsigned char **data, size_t *data_len)
{
	struct stat st;
	FILE *fp;

	if((fp = fopen(filepath, "rb")) == NULL) {
		return KUZPKG_ERR_NOT_A_FILE;
	}

	if(fstat(fileno(fp), &st) != 0) {
		fclose(fp);
		return KUZPKG_ERR_NOT_A_FILE;
	}

	*data_len = st.st_size;

	MALLOC(*data,
			*data_len,
			fclose(fp);
			return KUZPKG_ERR_MEMORY);

	if(fread(*data, *data_len, 1, fp) != 1) {
		FREE(*data);
		fclose(fp);
		return KUZPKG_ERR_SYSTEM;
	}

	fclose(fp);
	return KUZPKG_ERR_OK;
}
