/*
 *  callback.h
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
#ifndef PM_CALLBACK_H
#define PM_CALLBACK_H

#include <stdbool.h>
#include <sys/types.h> /* off_t */

#include <klpm.h>

/* callback to handle messages/notifications from libkuzpkg */
void cb_event(void *ctx, klpm_event_t *event);

/* callback to handle questions from libkuzpkg (yes/no) */
void cb_question(void *ctx, klpm_question_t *question);

/* callback to handle display of progress */
void cb_progress(void *ctx, klpm_progress_t event, const char *pkgname,
		int percent, size_t howmany, size_t remain);

/* callback to handle display of download progress */
void cb_download(void *ctx, const char *filename, klpm_download_event_type_t event,
		void *data);

/* callback to handle messages/notifications from kuzpkg library */
__attribute__((format(printf, 3, 0)))
void cb_log(void *ctx, klpm_loglevel_t level, const char *fmt, va_list args);

/* specify if multibar UI should move completed bars to the top of the screen */
void multibar_move_completed_up(bool value);

#endif /* PM_CALLBACK_H */
