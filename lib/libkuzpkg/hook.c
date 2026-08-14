/*
 *  hook.c
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

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <string.h>

#include "handle.h"
#include "hook.h"
#include "ini.h"
#include "log.h"
#include "trans.h"
#include "util.h"

enum _klpm_hook_op_t {
	KUZPKG_HOOK_OP_INSTALL = (1 << 0),
	KUZPKG_HOOK_OP_UPGRADE = (1 << 1),
	KUZPKG_HOOK_OP_REMOVE = (1 << 2),
};

enum _klpm_trigger_type_t {
	KUZPKG_HOOK_TYPE_PACKAGE = 1,
	KUZPKG_HOOK_TYPE_PATH,
};

struct _klpm_trigger_t {
	enum _klpm_hook_op_t op;
	enum _klpm_trigger_type_t type;
	klpm_list_t *targets;
};

struct _klpm_hook_t {
	char *name;
	char *desc;
	klpm_list_t *triggers;
	klpm_list_t *depends;
	char **cmd;
	klpm_list_t *matches;
	klpm_hook_when_t when;
	int abort_on_fail, needs_targets;
};

struct _klpm_hook_cb_ctx {
	klpm_handle_t *handle;
	struct _klpm_hook_t *hook;
};

static void _klpm_trigger_free(struct _klpm_trigger_t *trigger)
{
	if(trigger) {
		FREELIST(trigger->targets);
		free(trigger);
	}
}

static void _klpm_hook_free(struct _klpm_hook_t *hook)
{
	if(hook) {
		free(hook->name);
		free(hook->desc);
		wordsplit_free(hook->cmd);
		klpm_list_free_inner(hook->triggers, (klpm_list_fn_free) _klpm_trigger_free);
		klpm_list_free(hook->triggers);
		klpm_list_free(hook->matches);
		FREELIST(hook->depends);
		free(hook);
	}
}

static int _klpm_trigger_validate(klpm_handle_t *handle,
		struct _klpm_trigger_t *trigger, const char *file)
{
	int ret = 0;

	if(trigger->targets == NULL) {
		ret = -1;
		_klpm_log(handle, KUZPKG_LOG_ERROR,
				_("Missing trigger targets in hook: %s\n"), file);
	}

	if(trigger->type == 0) {
		ret = -1;
		_klpm_log(handle, KUZPKG_LOG_ERROR,
				_("Missing trigger type in hook: %s\n"), file);
	}

	if(trigger->op == 0) {
		ret = -1;
		_klpm_log(handle, KUZPKG_LOG_ERROR,
				_("Missing trigger operation in hook: %s\n"), file);
	}

	return ret;
}

static int _klpm_hook_validate(klpm_handle_t *handle,
		struct _klpm_hook_t *hook, const char *file)
{
	klpm_list_t *i;
	int ret = 0;

	if(hook->triggers == NULL) {
		/* special case: allow triggerless hooks as a way of creating dummy
		 * hooks that can be used to mask lower priority hooks */
		return 0;
	}

	for(i = hook->triggers; i; i = i->next) {
		if(_klpm_trigger_validate(handle, i->data, file) != 0) {
			ret = -1;
		}
	}

	if(hook->cmd == NULL) {
		ret = -1;
		_klpm_log(handle, KUZPKG_LOG_ERROR,
				_("Missing Exec option in hook: %s\n"), file);
	}

	if(hook->when == 0) {
		ret = -1;
		_klpm_log(handle, KUZPKG_LOG_ERROR,
				_("Missing When option in hook: %s\n"), file);
	} else if(hook->when != KUZPKG_HOOK_PRE_TRANSACTION && hook->abort_on_fail) {
		_klpm_log(handle, KUZPKG_LOG_WARNING,
				_("AbortOnFail set for PostTransaction hook: %s\n"), file);
	}

	return ret;
}

static int _klpm_hook_parse_cb(const char *file, int line,
		const char *section, char *key, char *value, void *data)
{
	struct _klpm_hook_cb_ctx *ctx = data;
	klpm_handle_t *handle = ctx->handle;
	struct _klpm_hook_t *hook = ctx->hook;

#define error(...) _klpm_log(handle, KUZPKG_LOG_ERROR, __VA_ARGS__); return 1;
#define warning(...) _klpm_log(handle, KUZPKG_LOG_WARNING, __VA_ARGS__);

	if(!section && !key) {
		error(_("error while reading hook %s: %s\n"), file, strerror(errno));
	} else if(!section) {
		error(_("hook %s line %d: invalid option %s\n"), file, line, key);
	} else if(!key) {
		/* beginning a new section */
		if(strcmp(section, "Trigger") == 0) {
			struct _klpm_trigger_t *t;
			CALLOC(t, sizeof(struct _klpm_trigger_t), 1, return 1);
			hook->triggers = klpm_list_add(hook->triggers, t);
		} else if(strcmp(section, "Action") == 0) {
			/* no special processing required */
		} else {
			error(_("hook %s line %d: invalid section %s\n"), file, line, section);
		}
	} else if(strcmp(section, "Trigger") == 0) {
		struct _klpm_trigger_t *t = hook->triggers->prev->data;
		if(strcmp(key, "Operation") == 0) {
			if(strcmp(value, "Install") == 0) {
				t->op |= KUZPKG_HOOK_OP_INSTALL;
			} else if(strcmp(value, "Upgrade") == 0) {
				t->op |= KUZPKG_HOOK_OP_UPGRADE;
			} else if(strcmp(value, "Remove") == 0) {
				t->op |= KUZPKG_HOOK_OP_REMOVE;
			} else {
				error(_("hook %s line %d: invalid value %s\n"), file, line, value);
			}
		} else if(strcmp(key, "Type") == 0) {
			if(t->type != 0) {
				warning(_("hook %s line %d: overwriting previous definition of %s\n"), file, line, "Type");
			}
			if(strcmp(value, "Package") == 0) {
				t->type = KUZPKG_HOOK_TYPE_PACKAGE;
			} else if(strcmp(value, "Path") == 0) {
				t->type = KUZPKG_HOOK_TYPE_PATH;
			} else {
				error(_("hook %s line %d: invalid value %s\n"), file, line, value);
			}
		} else if(strcmp(key, "Target") == 0) {
			char *val;
			STRDUP(val, value, return 1);
			t->targets = klpm_list_add(t->targets, val);
		} else {
			error(_("hook %s line %d: invalid option %s\n"), file, line, key);
		}
	} else if(strcmp(section, "Action") == 0) {
		if(strcmp(key, "When") == 0) {
			if(hook->when != 0) {
				warning(_("hook %s line %d: overwriting previous definition of %s\n"), file, line, "When");
			}
			if(strcmp(value, "PreTransaction") == 0) {
				hook->when = KUZPKG_HOOK_PRE_TRANSACTION;
			} else if(strcmp(value, "PostTransaction") == 0) {
				hook->when = KUZPKG_HOOK_POST_TRANSACTION;
			} else {
				error(_("hook %s line %d: invalid value %s\n"), file, line, value);
			}
		} else if(strcmp(key, "Description") == 0) {
			if(hook->desc != NULL) {
				warning(_("hook %s line %d: overwriting previous definition of %s\n"), file, line, "Description");
				FREE(hook->desc);
			}
			STRDUP(hook->desc, value, return 1);
		} else if(strcmp(key, "Depends") == 0) {
			char *val;
			STRDUP(val, value, return 1);
			hook->depends = klpm_list_add(hook->depends, val);
		} else if(strcmp(key, "AbortOnFail") == 0) {
			hook->abort_on_fail = 1;
		} else if(strcmp(key, "NeedsTargets") == 0) {
			hook->needs_targets = 1;
		} else if(strcmp(key, "Exec") == 0) {
			if(hook->cmd != NULL) {
				warning(_("hook %s line %d: overwriting previous definition of %s\n"), file, line, "Exec");
				wordsplit_free(hook->cmd);
			}
			if((hook->cmd = wordsplit(value)) == NULL) {
				if(errno == EINVAL) {
					error(_("hook %s line %d: invalid value %s\n"), file, line, value);
				} else {
					error(_("hook %s line %d: unable to set option (%s)\n"),
							file, line, strerror(errno));
				}
			}
		} else {
			error(_("hook %s line %d: invalid option %s\n"), file, line, key);
		}
	}

#undef error
#undef warning

	return 0;
}

static int _klpm_hook_trigger_match_file(klpm_handle_t *handle,
		struct _klpm_hook_t *hook, struct _klpm_trigger_t *t)
{
	klpm_list_t *i, *j, *install = NULL, *upgrade = NULL, *remove = NULL;
	size_t isize = 0, rsize = 0;
	int ret = 0;

	/* check if file will be installed */
	for(i = handle->trans->add; i; i = i->next) {
		klpm_pkg_t *pkg = i->data;
		klpm_filelist_t filelist = pkg->files;
		size_t f;
		for(f = 0; f < filelist.count; f++) {
			if(klpm_option_match_noextract(handle, filelist.files[f].name) == 0) {
				continue;
			}
			if(_klpm_fnmatch_patterns(t->targets, filelist.files[f].name) == 0) {
				install = klpm_list_add(install, filelist.files[f].name);
				isize++;
			}
		}
	}

	/* check if file will be removed due to package upgrade */
	for(i = handle->trans->add; i; i = i->next) {
		klpm_pkg_t *spkg = i->data;
		klpm_pkg_t *pkg = spkg->oldpkg;
		if(pkg) {
			klpm_filelist_t filelist = pkg->files;
			size_t f;
			for(f = 0; f < filelist.count; f++) {
				if(_klpm_fnmatch_patterns(t->targets, filelist.files[f].name) == 0) {
					remove = klpm_list_add(remove, filelist.files[f].name);
					rsize++;
				}
			}
		}
	}

	/* check if file will be removed due to package removal */
	for(i = handle->trans->remove; i; i = i->next) {
		klpm_pkg_t *pkg = i->data;
		klpm_filelist_t filelist = pkg->files;
		size_t f;
		for(f = 0; f < filelist.count; f++) {
			if(_klpm_fnmatch_patterns(t->targets, filelist.files[f].name) == 0) {
				remove = klpm_list_add(remove, filelist.files[f].name);
				rsize++;
			}
		}
	}

	i = install = klpm_list_msort(install, isize, (klpm_list_fn_cmp)strcmp);
	j = remove = klpm_list_msort(remove, rsize, (klpm_list_fn_cmp)strcmp);
	while(i) {
		while(j && strcmp(i->data, j->data) > 0) {
			j = j->next;
		}
		if(j == NULL) {
			break;
		}
		if(strcmp(i->data, j->data) == 0) {
			char *path = i->data;
			upgrade = klpm_list_add(upgrade, path);
			while(i && strcmp(i->data, path) == 0) {
				klpm_list_t *next = i->next;
				install = klpm_list_remove_item(install, i);
				free(i);
				i = next;
			}
			while(j && strcmp(j->data, path) == 0) {
				klpm_list_t *next = j->next;
				remove = klpm_list_remove_item(remove, j);
				free(j);
				j = next;
			}
		} else {
			i = i->next;
		}
	}

	ret = (t->op & KUZPKG_HOOK_OP_INSTALL && install)
			|| (t->op & KUZPKG_HOOK_OP_UPGRADE && upgrade)
			|| (t->op & KUZPKG_HOOK_OP_REMOVE && remove);

	if(hook->needs_targets) {
#define _save_matches(_op, _matches) \
	if(t->op & _op && _matches) { \
		hook->matches = klpm_list_join(hook->matches, _matches); \
	} else { \
		klpm_list_free(_matches); \
	}
		_save_matches(KUZPKG_HOOK_OP_INSTALL, install);
		_save_matches(KUZPKG_HOOK_OP_UPGRADE, upgrade);
		_save_matches(KUZPKG_HOOK_OP_REMOVE, remove);
#undef _save_matches
	} else {
		klpm_list_free(install);
		klpm_list_free(upgrade);
		klpm_list_free(remove);
	}

	return ret;
}

static int _klpm_hook_trigger_match_pkg(klpm_handle_t *handle,
		struct _klpm_hook_t *hook, struct _klpm_trigger_t *t)
{
	klpm_list_t *install = NULL, *upgrade = NULL, *remove = NULL;

	if(t->op & KUZPKG_HOOK_OP_INSTALL || t->op & KUZPKG_HOOK_OP_UPGRADE) {
		klpm_list_t *i;
		for(i = handle->trans->add; i; i = i->next) {
			klpm_pkg_t *pkg = i->data;
			if(_klpm_fnmatch_patterns(t->targets, pkg->name) == 0) {
				if(pkg->oldpkg) {
					if(t->op & KUZPKG_HOOK_OP_UPGRADE) {
						if(hook->needs_targets) {
							upgrade = klpm_list_add(upgrade, pkg->name);
						} else {
							return 1;
						}
					}
				} else {
					if(t->op & KUZPKG_HOOK_OP_INSTALL) {
						if(hook->needs_targets) {
							install = klpm_list_add(install, pkg->name);
						} else {
							return 1;
						}
					}
				}
			}
		}
	}

	if(t->op & KUZPKG_HOOK_OP_REMOVE) {
		klpm_list_t *i;
		for(i = handle->trans->remove; i; i = i->next) {
			klpm_pkg_t *pkg = i->data;
			if(pkg && _klpm_fnmatch_patterns(t->targets, pkg->name) == 0) {
				if(!klpm_list_find(handle->trans->add, pkg, _klpm_pkg_cmp)) {
					if(hook->needs_targets) {
						remove = klpm_list_add(remove, pkg->name);
					} else {
						return 1;
					}
				}
			}
		}
	}

	/* if we reached this point we either need the target lists or we didn't
	 * match anything and the following calls will all be no-ops */
	hook->matches = klpm_list_join(hook->matches, install);
	hook->matches = klpm_list_join(hook->matches, upgrade);
	hook->matches = klpm_list_join(hook->matches, remove);

	return install || upgrade || remove;
}

static int _klpm_hook_trigger_match(klpm_handle_t *handle,
		struct _klpm_hook_t *hook, struct _klpm_trigger_t *t)
{
	return t->type == KUZPKG_HOOK_TYPE_PACKAGE
		? _klpm_hook_trigger_match_pkg(handle, hook, t)
		: _klpm_hook_trigger_match_file(handle, hook, t);
}

static int _klpm_hook_triggered(klpm_handle_t *handle, struct _klpm_hook_t *hook)
{
	klpm_list_t *i;
	int ret = 0;
	for(i = hook->triggers; i; i = i->next) {
		if(_klpm_hook_trigger_match(handle, hook, i->data)) {
			if(!hook->needs_targets) {
				return 1;
			} else {
				ret = 1;
			}
		}
	}
	return ret;
}

static int _klpm_hook_cmp(struct _klpm_hook_t *h1, struct _klpm_hook_t *h2)
{
	size_t suflen = strlen(KUZPKG_HOOK_SUFFIX), l1, l2;
	int ret;
	l1 = strlen(h1->name) - suflen;
	l2 = strlen(h2->name) - suflen;
	/* exclude the suffixes from comparison */
	ret = strncmp(h1->name, h2->name, l1 <= l2 ? l1 : l2);
	if(ret == 0 && l1 != l2) {
		return l1 < l2 ? -1 : 1;
	}
	return ret;
}

static klpm_list_t *find_hook(klpm_list_t *haystack, const void *needle)
{
	while(haystack) {
		struct _klpm_hook_t *h = haystack->data;
		if(h && strcmp(h->name, needle) == 0) {
			return haystack;
		}
		haystack = haystack->next;
	}
	return NULL;
}

static ssize_t _klpm_hook_feed_targets(char *buf, ssize_t needed, klpm_list_t **pos)
{
	size_t remaining = needed, written = 0;;
	size_t len;

	while(*pos && (len = strlen((*pos)->data)) + 1 <= remaining) {
		memcpy(buf, (*pos)->data, len);
		buf[len++] = '\n';
		*pos = (*pos)->next;
		buf += len;
		remaining -= len;
		written += len;
	}

	if(*pos && remaining) {
		memcpy(buf, (*pos)->data, remaining);
		(*pos)->data = (char*) (*pos)->data + remaining;
		written += remaining;
	}

	return written;
}

static klpm_list_t *_klpm_strlist_dedup(klpm_list_t *list)
{
	klpm_list_t *i = list;
	while(i) {
		klpm_list_t *next = i->next;
		while(next && strcmp(i->data, next->data) == 0) {
			list = klpm_list_remove_item(list, next);
			free(next);
			next = i->next;
		}
		i = next;
	}
	return list;
}

static int _klpm_hook_run_hook(klpm_handle_t *handle, struct _klpm_hook_t *hook)
{
	klpm_list_t *i, *pkgs = _klpm_db_get_pkgcache(handle->db_local);

	for(i = hook->depends; i; i = i->next) {
		if(!klpm_find_satisfier(pkgs, i->data)) {
			_klpm_log(handle, KUZPKG_LOG_ERROR, _("unable to run hook %s: %s\n"),
					hook->name, _("could not satisfy dependencies"));
			return -1;
		}
	}

	if(hook->needs_targets) {
		klpm_list_t *ctx;
		hook->matches = klpm_list_msort(hook->matches,
				klpm_list_count(hook->matches), (klpm_list_fn_cmp)strcmp);
		/* hooks with multiple triggers could have duplicate matches */
		ctx = hook->matches = _klpm_strlist_dedup(hook->matches);
		return _klpm_run_chroot(handle, hook->cmd[0], hook->cmd,
				(_klpm_cb_io) _klpm_hook_feed_targets, &ctx);
	} else {
		return _klpm_run_chroot(handle, hook->cmd[0], hook->cmd, NULL, NULL);
	}
}

int _klpm_hook_run(klpm_handle_t *handle, klpm_hook_when_t when)
{
	klpm_event_hook_t event = { .when = when };
	klpm_event_hook_run_t hook_event;
	klpm_list_t *i, *hooks = NULL, *hooks_triggered = NULL;
	size_t suflen = strlen(KUZPKG_HOOK_SUFFIX), triggered = 0;
	int ret = 0;

	for(i = klpm_list_last(handle->hookdirs); i; i = klpm_list_previous(i)) {
		char path[PATH_MAX];
		size_t dirlen;
		struct dirent *entry;
		DIR *d;

		if((dirlen = strlen(i->data)) >= PATH_MAX) {
			_klpm_log(handle, KUZPKG_LOG_ERROR, _("could not open directory: %s: %s\n"),
					(char *)i->data, strerror(ENAMETOOLONG));
			ret = -1;
			continue;
		}
		memcpy(path, i->data, dirlen + 1);

		if(!(d = opendir(path))) {
			if(errno == ENOENT) {
				continue;
			} else {
				_klpm_log(handle, KUZPKG_LOG_ERROR,
						_("could not open directory: %s: %s\n"), path, strerror(errno));
				ret = -1;
				continue;
			}
		}

		while((errno = 0, entry = readdir(d))) {
			struct _klpm_hook_cb_ctx ctx = { handle, NULL };
			struct stat buf;
			size_t name_len;

			if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
				continue;
			}

			if((name_len = strlen(entry->d_name)) >= PATH_MAX - dirlen) {
				_klpm_log(handle, KUZPKG_LOG_ERROR, _("could not open file: %s%s: %s\n"),
						path, entry->d_name, strerror(ENAMETOOLONG));
				ret = -1;
				continue;
			}
			memcpy(path + dirlen, entry->d_name, name_len + 1);

			if(name_len < suflen
					|| strcmp(entry->d_name + name_len - suflen, KUZPKG_HOOK_SUFFIX) != 0) {
				_klpm_log(handle, KUZPKG_LOG_DEBUG, "skipping non-hook file %s\n", path);
				continue;
			}

			if(find_hook(hooks, entry->d_name)) {
				_klpm_log(handle, KUZPKG_LOG_DEBUG, "skipping overridden hook %s\n", path);
				continue;
			}

			if(stat(path, &buf) != 0) {
				_klpm_log(handle, KUZPKG_LOG_ERROR,
						_("could not stat file %s: %s\n"), path, strerror(errno));
				ret = -1;
				continue;
			}

			if(S_ISDIR(buf.st_mode)) {
				_klpm_log(handle, KUZPKG_LOG_DEBUG, "skipping directory %s\n", path);
				continue;
			}

			CALLOC(ctx.hook, sizeof(struct _klpm_hook_t), 1,
					ret = -1; closedir(d); goto cleanup);

			_klpm_log(handle, KUZPKG_LOG_DEBUG, "parsing hook file %s\n", path);
			if(parse_ini(path, _klpm_hook_parse_cb, &ctx) != 0
					|| _klpm_hook_validate(handle, ctx.hook, path)) {
				_klpm_log(handle, KUZPKG_LOG_DEBUG, "parsing hook file %s failed\n", path);
				_klpm_hook_free(ctx.hook);
				ret = -1;
				continue;
			}

			STRDUP(ctx.hook->name, entry->d_name, ret = -1; closedir(d); goto cleanup);
			hooks = klpm_list_add(hooks, ctx.hook);
		}
		if(errno != 0) {
			_klpm_log(handle, KUZPKG_LOG_ERROR, _("could not read directory: %s: %s\n"),
					(char *) i->data, strerror(errno));
			ret = -1;
		}

		closedir(d);
	}

	if(ret != 0 && when == KUZPKG_HOOK_PRE_TRANSACTION) {
		goto cleanup;
	}

	hooks = klpm_list_msort(hooks, klpm_list_count(hooks),
			(klpm_list_fn_cmp)_klpm_hook_cmp);

	for(i = hooks; i; i = i->next) {
		struct _klpm_hook_t *hook = i->data;
		if(hook && hook->when == when && _klpm_hook_triggered(handle, hook)) {
			hooks_triggered = klpm_list_add(hooks_triggered, hook);
			triggered++;
		}
	}

	if(hooks_triggered != NULL) {
		event.type = KUZPKG_EVENT_HOOK_START;
		EVENT(handle, (void *)&event);

		hook_event.position = 1;
		hook_event.total = triggered;

		for(i = hooks_triggered; i; i = i->next, hook_event.position++) {
			struct _klpm_hook_t *hook = i->data;
			klpm_logaction(handle, KUZPKG_CALLER_PREFIX, "running '%s'...\n", hook->name);

			hook_event.type = KUZPKG_EVENT_HOOK_RUN_START;
			hook_event.name = hook->name;
			hook_event.desc = hook->desc;
			EVENT(handle, &hook_event);

			if(_klpm_hook_run_hook(handle, hook) != 0 && hook->abort_on_fail) {
				ret = -1;
			}

			hook_event.type = KUZPKG_EVENT_HOOK_RUN_DONE;
			EVENT(handle, &hook_event);

			if(ret != 0 && when == KUZPKG_HOOK_PRE_TRANSACTION) {
				break;
			}
		}

		klpm_list_free(hooks_triggered);

		event.type = KUZPKG_EVENT_HOOK_DONE;
		EVENT(handle, (void *)&event);
	}

cleanup:
	klpm_list_free_inner(hooks, (klpm_list_fn_free) _klpm_hook_free);
	klpm_list_free(hooks);

	return ret;
}
