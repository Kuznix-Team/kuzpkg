/*
 *  klpm_list.h
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


#ifndef KUZPKG_LIST_H
#define KUZPKG_LIST_H

#include <stdlib.h> /* size_t */

/* Note: klpm_list.{c,h} are intended to be standalone files. Do not include
 * any other libkuzpkg headers.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @ingroup libkuzpkg
 * @addtogroup libkuzpkg_list libkuzpkg_list(3)
 * @brief Functions to manipulate klpm_list_t lists.
 *
 * These functions are designed to create, destroy, and modify lists of
 * type klpm_list_t. This is an internal list type used by libkuzpkg that is
 * publicly exposed for use by frontends if desired.
 *
 * It is exposed so front ends can use it to prevent the need to reimplement
 * lists of their own; however, it is not required that the front end uses
 * it.
 * @{
 */

/** A doubly linked list */
typedef struct _klpm_list_t {
	/** data held by the list node */
	void *data;
	/** pointer to the previous node */
	struct _klpm_list_t *prev;
	/** pointer to the next node */
	struct _klpm_list_t *next;
} klpm_list_t;

/** Frees a list and its contents */
#define FREELIST(p) do { klpm_list_free_inner(p, free); klpm_list_free(p); p = NULL; } while(0)

/** item deallocation callback.
 * @param item the item to free
 */
typedef void (*klpm_list_fn_free)(void * item);

/** item comparison callback */
typedef int (*klpm_list_fn_cmp)(const void *, const void *);

/* allocation */

/** Free a list, but not the contained data.
 *
 * @param list the list to free
 */
void klpm_list_free(klpm_list_t *list);

/** Free the internal data of a list structure but not the list itself.
 *
 * @param list the list to free
 * @param fn a free function for the internal data
 */
void klpm_list_free_inner(klpm_list_t *list, klpm_list_fn_free fn);

/* item mutators */

/** Add a new item to the end of the list.
 *
 * @param list the list to add to
 * @param data the new item to be added to the list
 *
 * @return the resultant list
 */
klpm_list_t *klpm_list_add(klpm_list_t *list, void *data);

/**
 * @brief Add a new item to the end of the list.
 *
 * @param list the list to add to
 * @param data the new item to be added to the list
 *
 * @return the newly added item
 */
klpm_list_t *klpm_list_append(klpm_list_t **list, void *data);

/**
 * @brief Duplicate and append a string to a list.
 *
 * @param list the list to append to
 * @param data the string to duplicate and append
 *
 * @return the newly added item
 */
klpm_list_t *klpm_list_append_strdup(klpm_list_t **list, const char *data);

/**
 * @brief Add items to a list in sorted order.
 *
 * @param list the list to add to
 * @param data the new item to be added to the list
 * @param fn   the comparison function to use to determine order
 *
 * @return the resultant list
 */
klpm_list_t *klpm_list_add_sorted(klpm_list_t *list, void *data, klpm_list_fn_cmp fn);

/**
 * @brief Join two lists.
 * The two lists must be independent. Do not free the original lists after
 * calling this function, as this is not a copy operation. The list pointers
 * passed in should be considered invalid after calling this function.
 *
 * @param first  the first list
 * @param second the second list
 *
 * @return the resultant joined list
 */
klpm_list_t *klpm_list_join(klpm_list_t *first, klpm_list_t *second);

/**
 * @brief Merge the two sorted sublists into one sorted list.
 *
 * @param left  the first list
 * @param right the second list
 * @param fn    comparison function for determining merge order
 *
 * @return the resultant list
 */
klpm_list_t *klpm_list_mmerge(klpm_list_t *left, klpm_list_t *right, klpm_list_fn_cmp fn);

/**
 * @brief Sort a list of size `n` using mergesort algorithm.
 *
 * @param list the list to sort
 * @param n    the size of the list
 * @param fn   the comparison function for determining order
 *
 * @return the resultant list
 */
klpm_list_t *klpm_list_msort(klpm_list_t *list, size_t n, klpm_list_fn_cmp fn);

/**
 * @brief Remove an item from the list.
 * item is not freed; this is the responsibility of the caller.
 *
 * @param haystack the list to remove the item from
 * @param item the item to remove from the list
 *
 * @return the resultant list
 */
klpm_list_t *klpm_list_remove_item(klpm_list_t *haystack, klpm_list_t *item);

/**
 * @brief Remove an item from the list.
 *
 * @param haystack the list to remove the item from
 * @param needle   the data member of the item we're removing
 * @param fn       the comparison function for searching
 * @param data     output parameter containing data of the removed item
 *
 * @return the resultant list
 */
klpm_list_t *klpm_list_remove(klpm_list_t *haystack, const void *needle, klpm_list_fn_cmp fn, void **data);

/**
 * @brief Remove a string from a list.
 *
 * @param haystack the list to remove the item from
 * @param needle   the data member of the item we're removing
 * @param data     output parameter containing data of the removed item
 *
 * @return the resultant list
 */
klpm_list_t *klpm_list_remove_str(klpm_list_t *haystack, const char *needle, char **data);

/**
 * @brief Create a new list without any duplicates.
 *
 * This does NOT copy data members.
 *
 * @param list the list to copy
 *
 * @return a new list containing non-duplicate items
 */
klpm_list_t *klpm_list_remove_dupes(const klpm_list_t *list);

/**
 * @brief Copy a string list, including data.
 *
 * @param list the list to copy
 *
 * @return a copy of the original list
 */
klpm_list_t *klpm_list_strdup(const klpm_list_t *list);

/**
 * @brief Copy a list, without copying data.
 *
 * @param list the list to copy
 *
 * @return a copy of the original list
 */
klpm_list_t *klpm_list_copy(const klpm_list_t *list);

/**
 * @brief Copy a list and copy the data.
 * Note that the data elements to be copied should not contain pointers
 * and should also be of constant size.
 *
 * @param list the list to copy
 * @param size the size of each data element
 *
 * @return a copy of the original list, data copied as well
 */
klpm_list_t *klpm_list_copy_data(const klpm_list_t *list, size_t size);

/**
 * @brief Create a new list in reverse order.
 *
 * @param list the list to copy
 *
 * @return a new list in reverse order
 */
klpm_list_t *klpm_list_reverse(klpm_list_t *list);

/* item accessors */


/**
 * @brief Return nth element from list (starting from 0).
 *
 * @param list the list
 * @param n    the index of the item to find (n < klpm_list_count(list) IS needed)
 *
 * @return an klpm_list_t node for index `n`
 */
klpm_list_t *klpm_list_nth(const klpm_list_t *list, size_t n);

/**
 * @brief Get the next element of a list.
 *
 * @param list the list node
 *
 * @return the next element, or NULL when no more elements exist
 */
klpm_list_t *klpm_list_next(const klpm_list_t *list);

/**
 * @brief Get the previous element of a list.
 *
 * @param list the list head
 *
 * @return the previous element, or NULL when no previous element exist
 */
klpm_list_t *klpm_list_previous(const klpm_list_t *list);

/**
 * @brief Get the last item in the list.
 *
 * @param list the list
 *
 * @return the last element in the list
 */
klpm_list_t *klpm_list_last(const klpm_list_t *list);

/* misc */

/**
 * @brief Get the number of items in a list.
 *
 * @param list the list
 *
 * @return the number of list items
 */
size_t klpm_list_count(const klpm_list_t *list);

/**
 * @brief Find an item in a list.
 *
 * @param needle   the item to search
 * @param haystack the list
 * @param fn       the comparison function for searching (!= NULL)
 *
 * @return `needle` if found, NULL otherwise
 */
void *klpm_list_find(const klpm_list_t *haystack, const void *needle, klpm_list_fn_cmp fn);

/**
 * @brief Find an item in a list.
 *
 * Search for the item whose data matches that of the `needle`.
 *
 * @param needle   the data to search for (== comparison)
 * @param haystack the list
 *
 * @return `needle` if found, NULL otherwise
 */
void *klpm_list_find_ptr(const klpm_list_t *haystack, const void *needle);

/**
 * @brief Find a string in a list.
 *
 * @param needle   the string to search for
 * @param haystack the list
 *
 * @return `needle` if found, NULL otherwise
 */
char *klpm_list_find_str(const klpm_list_t *haystack, const char *needle);


/**
 * @brief Check if two lists contain the same data, ignoring order.
 *
 * Lists are considered equal if they both contain the same data regardless
 * of order.
 *
 * @param left      the first list
 * @param right     the second list
 * @param fn        the comparison function
 *
 * @return 1 if the lists are equal, 0 if not equal, -1 on error.
 */
int klpm_list_cmp_unsorted(const klpm_list_t *left,
		const klpm_list_t *right, klpm_list_fn_cmp fn);

/**
 * @brief Find the differences between list `left` and list `right`
 *
 * The two lists must be sorted. Items only in list `left` are added to the
 * `onlyleft` list. Items only in list `right` are added to the `onlyright`
 * list.
 *
 * @param left      the first list
 * @param right     the second list
 * @param fn        the comparison function
 * @param onlyleft  pointer to the first result list
 * @param onlyright pointer to the second result list
 *
 */
void klpm_list_diff_sorted(const klpm_list_t *left, const klpm_list_t *right,
		klpm_list_fn_cmp fn, klpm_list_t **onlyleft, klpm_list_t **onlyright);

/**
 * @brief Find the items in list `lhs` that are not present in list `rhs`.
 *
 * @param lhs the first list
 * @param rhs the second list
 * @param fn  the comparison function
 *
 * @return a list containing all items in `lhs` not present in `rhs`
 */

klpm_list_t *klpm_list_diff(const klpm_list_t *lhs, const klpm_list_t *rhs, klpm_list_fn_cmp fn);

/**
 * @brief Copy a list and data into a standard C array of fixed length.
 * Note that the data elements are shallow copied so any contained pointers
 * will point to the original data.
 *
 * @param list the list to copy
 * @param n    the size of the list
 * @param size the size of each data element
 *
 * @return an array version of the original list, data copied as well
 */
void *klpm_list_to_array(const klpm_list_t *list, size_t n, size_t size);

/* End of klpm_list */
/** @} */

#ifdef __cplusplus
}
#endif
#endif /* KUZPKG_LIST_H */
