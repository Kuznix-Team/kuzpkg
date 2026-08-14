/*
 *  sandbox.h
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

#ifndef KUZPKG_SANDBOX_H
#define KUZPKG_SANDBOX_H

#include <stdbool.h>

/* utility function to test if sandboxing should be used */
bool _klpm_use_sandbox(klpm_handle_t *handle);

/* The type of callbacks that can happen during a sandboxed operation */
typedef enum {
	KUZPKG_SANDBOX_CB_LOG,
	KUZPKG_SANDBOX_CB_DOWNLOAD
} _klpm_sandbox_callback_t;

typedef struct {
	int callback_pipe;
} _klpm_sandbox_callback_context;


/* Sandbox callbacks */

__attribute__((format(printf, 3, 0)))
void _klpm_sandbox_cb_log(void *ctx, klpm_loglevel_t level, const char *fmt, va_list args);

void _klpm_sandbox_cb_dl(void *ctx, const char *filename, klpm_download_event_type_t event, void *data);


/* Functions to capture sandbox callbacks and convert them to klpm callbacks */

bool _klpm_sandbox_process_cb_log(klpm_handle_t *handle, int callback_pipe);
bool _klpm_sandbox_process_cb_download(klpm_handle_t *handle, int callback_pipe);


#endif /* KUZPKG_SANDBOX_H */
