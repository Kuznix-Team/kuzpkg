/*
 *  klpm_list.c
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
#include <string.h>

/* Note: klpm_list.{c,h} are intended to be standalone files. Do not include
 * any other libkuzpkg headers.
 */

/* libkuzpkg */
#include "klpm_list.h"

/* check exported library symbols with: nm -C -D <lib> */
#define SYMEXPORT __attribute__((visibility("default")))

/* Allocation */

void SYMEXPORT klpm_list_free(klpm_list_t *list)
{
	klpm_list_t *it = list;

	while(it) {
		klpm_list_t *tmp = it->next;
		free(it);
		it = tmp;
	}
}

void SYMEXPORT klpm_list_free_inner(klpm_list_t *list, klpm_list_fn_free fn)
{
	klpm_list_t *it = list;

	if(fn) {
		while(it) {
			if(it->data) {
				fn(it->data);
			}
			it = it->next;
		}
	}
}


/* Mutators */

klpm_list_t SYMEXPORT *klpm_list_add(klpm_list_t *list, void *data)
{
	klpm_list_append(&list, data);
	return list;
}

klpm_list_t SYMEXPORT *klpm_list_append(klpm_list_t **list, void *data)
{
	klpm_list_t *ptr;

	ptr = malloc(sizeof(klpm_list_t));
	if(ptr == NULL) {
		return NULL;
	}

	ptr->data = data;
	ptr->next = NULL;

	/* Special case: the input list is empty */
	if(*list == NULL) {
		*list = ptr;
		ptr->prev = ptr;
	} else {
		klpm_list_t *lp = klpm_list_last(*list);
		lp->next = ptr;
		ptr->prev = lp;
		(*list)->prev = ptr;
	}

	return ptr;
}

klpm_list_t SYMEXPORT *klpm_list_append_strdup(klpm_list_t **list, const char *data)
{
	klpm_list_t *ret;
	char *dup;
	if((dup = strdup(data)) && (ret = klpm_list_append(list, dup))) {
		return ret;
	} else {
		free(dup);
		return NULL;
	}
}

klpm_list_t SYMEXPORT *klpm_list_add_sorted(klpm_list_t *list, void *data, klpm_list_fn_cmp fn)
{
	if(!fn || !list) {
		return klpm_list_add(list, data);
	} else {
		klpm_list_t *add = NULL, *prev = NULL, *next = list;

		add = malloc(sizeof(klpm_list_t));
		if(add == NULL) {
			return list;
		}
		add->data = data;

		/* Find insertion point. */
		while(next) {
			if(fn(add->data, next->data) <= 0) break;
			prev = next;
			next = next->next;
		}

		/* Insert the add node to the list */
		if(prev == NULL) { /* special case: we insert add as the first element */
			add->prev = list->prev; /* list != NULL */
			add->next = list;
			list->prev = add;
			return add;
		} else if(next == NULL) { /* another special case: add last element */
			add->prev = prev;
			add->next = NULL;
			prev->next = add;
			list->prev = add;
			return list;
		} else {
			add->prev = prev;
			add->next = next;
			next->prev = add;
			prev->next = add;
			return list;
		}
	}
}

klpm_list_t SYMEXPORT *klpm_list_join(klpm_list_t *first, klpm_list_t *second)
{
	klpm_list_t *tmp;

	if(first == NULL) {
		return second;
	}
	if(second == NULL) {
		return first;
	}
	/* tmp is the last element of the first list */
	tmp = first->prev;
	/* link the first list to the second */
	tmp->next = second;
	/* link the second list to the first */
	first->prev = second->prev;
	/* set the back reference to the tail */
	second->prev = tmp;

	return first;
}

klpm_list_t SYMEXPORT *klpm_list_mmerge(klpm_list_t *left, klpm_list_t *right,
		klpm_list_fn_cmp fn)
{
	klpm_list_t *newlist, *lp, *tail_ptr, *left_tail_ptr, *right_tail_ptr;

	if(left == NULL) {
		return right;
	}
	if(right == NULL) {
		return left;
	}

	/* Save tail node pointers for future use */
	left_tail_ptr = left->prev;
	right_tail_ptr = right->prev;

	if(fn(left->data, right->data) <= 0) {
		newlist = left;
		left = left->next;
	}
	else {
		newlist = right;
		right = right->next;
	}
	newlist->prev = NULL;
	newlist->next = NULL;
	lp = newlist;

	while((left != NULL) && (right != NULL)) {
		if(fn(left->data, right->data) <= 0) {
			lp->next = left;
			left->prev = lp;
			left = left->next;
		}
		else {
			lp->next = right;
			right->prev = lp;
			right = right->next;
		}
		lp = lp->next;
		lp->next = NULL;
	}
	if(left != NULL) {
		lp->next = left;
		left->prev = lp;
		tail_ptr = left_tail_ptr;
	}
	else if(right != NULL) {
		lp->next = right;
		right->prev = lp;
		tail_ptr = right_tail_ptr;
	}
	else {
		tail_ptr = lp;
	}

	newlist->prev = tail_ptr;

	return newlist;
}

klpm_list_t SYMEXPORT *klpm_list_msort(klpm_list_t *list, size_t n,
		klpm_list_fn_cmp fn)
{
	if(n > 1) {
		size_t half = n / 2;
		size_t i = half - 1;
		klpm_list_t *left = list, *lastleft = list, *right;

		while(i--) {
			lastleft = lastleft->next;
		}
		right = lastleft->next;

		/* tidy new lists */
		lastleft->next = NULL;
		right->prev = left->prev;
		left->prev = lastleft;

		left = klpm_list_msort(left, half, fn);
		right = klpm_list_msort(right, n - half, fn);
		list = klpm_list_mmerge(left, right, fn);
	}
	return list;
}

klpm_list_t SYMEXPORT *klpm_list_remove_item(klpm_list_t *haystack,
		klpm_list_t *item)
{
	if(haystack == NULL || item == NULL) {
		return haystack;
	}

	if(item == haystack) {
		/* Special case: removing the head node which has a back reference to
		 * the tail node */
		haystack = item->next;
		if(haystack) {
			haystack->prev = item->prev;
		}
		item->prev = NULL;
	} else if(item == haystack->prev) {
		/* Special case: removing the tail node, so we need to fix the back
		 * reference on the head node. We also know tail != head. */
		if(item->prev) {
			/* i->next should always be null */
			item->prev->next = item->next;
			haystack->prev = item->prev;
			item->prev = NULL;
		}
	} else {
		/* Normal case, non-head and non-tail node */
		if(item->next) {
			item->next->prev = item->prev;
		}
		if(item->prev) {
			item->prev->next = item->next;
		}
	}

	return haystack;
}

klpm_list_t SYMEXPORT *klpm_list_remove(klpm_list_t *haystack,
		const void *needle, klpm_list_fn_cmp fn, void **data)
{
	klpm_list_t *i = haystack;

	if(data) {
		*data = NULL;
	}

	if(needle == NULL) {
		return haystack;
	}

	while(i) {
		if(i->data == NULL) {
			i = i->next;
			continue;
		}
		if(fn(i->data, needle) == 0) {
			haystack = klpm_list_remove_item(haystack, i);

			if(data) {
				*data = i->data;
			}
			free(i);
			break;
		} else {
			i = i->next;
		}
	}

	return haystack;
}

klpm_list_t SYMEXPORT *klpm_list_remove_str(klpm_list_t *haystack,
		const char *needle, char **data)
{
	return klpm_list_remove(haystack, (const void *)needle,
			(klpm_list_fn_cmp)strcmp, (void **)data);
}

klpm_list_t SYMEXPORT *klpm_list_remove_dupes(const klpm_list_t *list)
{
	const klpm_list_t *lp = list;
	klpm_list_t *newlist = NULL;
	while(lp) {
		if(!klpm_list_find_ptr(newlist, lp->data)) {
			if(klpm_list_append(&newlist, lp->data) == NULL) {
				klpm_list_free(newlist);
				return NULL;
			}
		}
		lp = lp->next;
	}
	return newlist;
}

klpm_list_t SYMEXPORT *klpm_list_strdup(const klpm_list_t *list)
{
	const klpm_list_t *lp = list;
	klpm_list_t *newlist = NULL;
	while(lp) {
		if(klpm_list_append_strdup(&newlist, lp->data) == NULL) {
			FREELIST(newlist);
			return NULL;
		}
		lp = lp->next;
	}
	return newlist;
}

klpm_list_t SYMEXPORT *klpm_list_copy(const klpm_list_t *list)
{
	const klpm_list_t *lp = list;
	klpm_list_t *newlist = NULL;
	while(lp) {
		if(klpm_list_append(&newlist, lp->data) == NULL) {
			klpm_list_free(newlist);
			return NULL;
		}
		lp = lp->next;
	}
	return newlist;
}

klpm_list_t SYMEXPORT *klpm_list_copy_data(const klpm_list_t *list,
		size_t size)
{
	const klpm_list_t *lp = list;
	klpm_list_t *newlist = NULL;
	while(lp) {
		void *newdata = malloc(size);
		if(newdata) {
			memcpy(newdata, lp->data, size);
			if(klpm_list_append(&newlist, newdata) == NULL) {
				free(newdata);
				FREELIST(newlist);
				return NULL;
			}
			lp = lp->next;
		} else {
			FREELIST(newlist);
			return NULL;
		}
	}
	return newlist;
}

klpm_list_t SYMEXPORT *klpm_list_reverse(klpm_list_t *list)
{
	const klpm_list_t *lp;
	klpm_list_t *newlist = NULL, *backup;

	if(list == NULL) {
		return NULL;
	}

	lp = klpm_list_last(list);
	/* break our reverse circular list */
	backup = list->prev;
	list->prev = NULL;

	while(lp) {
		if(klpm_list_append(&newlist, lp->data) == NULL) {
			klpm_list_free(newlist);
			list->prev = backup;
			return NULL;
		}
		lp = lp->prev;
	}
	list->prev = backup; /* restore tail pointer */
	return newlist;
}

/* Accessors */

klpm_list_t SYMEXPORT *klpm_list_nth(const klpm_list_t *list, size_t n)
{
	const klpm_list_t *i = list;
	while(n--) {
		i = i->next;
	}
	return (klpm_list_t *)i;
}

inline klpm_list_t SYMEXPORT *klpm_list_next(const klpm_list_t *node)
{
	if(node) {
		return node->next;
	} else {
		return NULL;
	}
}

inline klpm_list_t SYMEXPORT *klpm_list_previous(const klpm_list_t *list)
{
	if(list && list->prev->next) {
		return list->prev;
	} else {
		return NULL;
	}
}

klpm_list_t SYMEXPORT *klpm_list_last(const klpm_list_t *list)
{
	if(list) {
		return list->prev;
	} else {
		return NULL;
	}
}

/* Misc */

size_t SYMEXPORT klpm_list_count(const klpm_list_t *list)
{
	size_t i = 0;
	const klpm_list_t *lp = list;
	while(lp) {
		++i;
		lp = lp->next;
	}
	return i;
}

void SYMEXPORT *klpm_list_find(const klpm_list_t *haystack, const void *needle,
		klpm_list_fn_cmp fn)
{
	const klpm_list_t *lp = haystack;
	while(lp) {
		if(lp->data && fn(lp->data, needle) == 0) {
			return lp->data;
		}
		lp = lp->next;
	}
	return NULL;
}

/* trivial helper function for klpm_list_find_ptr */
static int ptr_cmp(const void *p, const void *q)
{
	return (p != q);
}

void SYMEXPORT *klpm_list_find_ptr(const klpm_list_t *haystack,
		const void *needle)
{
	return klpm_list_find(haystack, needle, ptr_cmp);
}

char SYMEXPORT *klpm_list_find_str(const klpm_list_t *haystack,
		const char *needle)
{
	return (char *)klpm_list_find(haystack, (const void *)needle,
			(klpm_list_fn_cmp)strcmp);
}

int SYMEXPORT klpm_list_cmp_unsorted(const klpm_list_t *left,
		const klpm_list_t *right, klpm_list_fn_cmp fn)
{
	const klpm_list_t *l = left;
	const klpm_list_t *r = right;
	int *matched;

	/* short circuiting length comparison */
	while(l && r) {
		l = l->next;
		r = r->next;
	}
	if(l || r) {
		return 0;
	}

	/* faster comparison for if the lists happen to be in the same order */
	while(left && fn(left->data, right->data) == 0) {
		left = left->next;
		right = right->next;
	}
	if(!left) {
		return 1;
	}

	matched = calloc(klpm_list_count(right), sizeof(int));
	if(matched == NULL) {
		return -1;
	}

	for(l = left; l; l = l->next) {
		int found = 0;
		int n = 0;

		for(r = right; r; r = r->next, n++) {
			/* make sure we don't match the same value twice */
			if(matched[n]) {
				continue;
			}
			if(fn(l->data, r->data) == 0) {
				found = 1;
				matched[n] = 1;
				break;
			}

		}

		if(!found) {
			free(matched);
			return 0;
		}
	}

	free(matched);
	return 1;
}

void SYMEXPORT klpm_list_diff_sorted(const klpm_list_t *left,
		const klpm_list_t *right, klpm_list_fn_cmp fn,
		klpm_list_t **onlyleft, klpm_list_t **onlyright)
{
	const klpm_list_t *l = left;
	const klpm_list_t *r = right;

	if(!onlyleft && !onlyright) {
		return;
	}

	while(l != NULL && r != NULL) {
		int cmp = fn(l->data, r->data);
		if(cmp < 0) {
			if(onlyleft) {
				*onlyleft = klpm_list_add(*onlyleft, l->data);
			}
			l = l->next;
		}
		else if(cmp > 0) {
			if(onlyright) {
				*onlyright = klpm_list_add(*onlyright, r->data);
			}
			r = r->next;
		} else {
			l = l->next;
			r = r->next;
		}
	}
	while(l != NULL) {
		if(onlyleft) {
			*onlyleft = klpm_list_add(*onlyleft, l->data);
		}
		l = l->next;
	}
	while(r != NULL) {
		if(onlyright) {
			*onlyright = klpm_list_add(*onlyright, r->data);
		}
		r = r->next;
	}
}


klpm_list_t SYMEXPORT *klpm_list_diff(const klpm_list_t *lhs,
		const klpm_list_t *rhs, klpm_list_fn_cmp fn)
{
	klpm_list_t *left, *right;
	klpm_list_t *ret = NULL;

	left = klpm_list_copy(lhs);
	left = klpm_list_msort(left, klpm_list_count(left), fn);
	right = klpm_list_copy(rhs);
	right = klpm_list_msort(right, klpm_list_count(right), fn);

	klpm_list_diff_sorted(left, right, fn, &ret, NULL);

	klpm_list_free(left);
	klpm_list_free(right);
	return ret;
}

void SYMEXPORT *klpm_list_to_array(const klpm_list_t *list, size_t n,
		size_t size)
{
	size_t i;
	const klpm_list_t *item;
	char *array;

	if(n == 0) {
		return NULL;
	}

	array = malloc(n * size);
	if(array == NULL) {
		return NULL;
	}
	for(i = 0, item = list; i < n && item; i++, item = item->next) {
		memcpy(array + i * size, item->data, size);
	}
	return array;
}
