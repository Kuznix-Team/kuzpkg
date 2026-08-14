/*
 *  graph.h - helpful graph structure and setup/teardown methods
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
#ifndef KUZPKG_GRAPH_H
#define KUZPKG_GRAPH_H

#include <sys/types.h> /* off_t */

#include "klpm_list.h"

enum _klpm_graph_vertex_state {
	KUZPKG_GRAPH_STATE_UNPROCESSED,
	KUZPKG_GRAPH_STATE_PROCESSING,
	KUZPKG_GRAPH_STATE_PROCESSED
};

typedef struct _klpm_graph_t {
	void *data;
	struct _klpm_graph_t *parent; /* where did we come from? */
	klpm_list_t *children;
	klpm_list_t *iterator; /* used for DFS without recursion */
	off_t weight; /* weight of the node */
	enum _klpm_graph_vertex_state state;
} klpm_graph_t;

klpm_graph_t *_klpm_graph_new(void);
void _klpm_graph_free(void *data);

#endif /* KUZPKG_GRAPH_H */
