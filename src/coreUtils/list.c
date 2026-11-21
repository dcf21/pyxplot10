// list.c
//
// The code in this file is part of Pyxplot
// <http://www.pyxplot.org.uk>
//
// Copyright (C) 2006-2026 Dominic Ford <coders@pyxplot.org.uk>
//               2008-2013 Ross Church
//
// $Id$
//
// Pyxplot is free software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation; either version 2 of the License, or (at your option) any later
// version.
//
// You should have received a copy of the GNU General Public License along with
// Pyxplot; if not, write to the Free Software Foundation, Inc., 51 Franklin
// Street, Fifth Floor, Boston, MA  02110-1301, USA

// ----------------------------------------------------------------------------

// Pyxplot's doubly-linked list type. Lists hold user-level list objects, and are used internally wherever a sequence of
// items of unknown length is needed, e.g. the items on the multiplot canvas or the words of a command. Items may be
// allocated with malloc() or in a temporary memory context from memAlloc.c.

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "coreUtils/list.h"
#include "coreUtils/memAlloc.h"

//! ppl_listInit - Create a new, empty linked list
//! \param [in] useMalloc - If true, the list and its items are allocated with malloc(), and must be freed with
//! ppl_listFree(). If false, they are allocated in the current ppl_memAlloc context, and are freed when that context
//! is freed.
//! \return The new list, or NULL if memory allocation failed

list *ppl_listInit(const int useMalloc) {
    list *out;
    if (useMalloc) out = (list *) malloc(sizeof(list));
    else out = (list *) ppl_memAlloc(sizeof(list));
    if (out == NULL) return NULL;
    out->first = NULL;
    out->last = NULL;
    out->length = 0;
    out->refCount = 1;
    out->immutable = 0;
    out->useMalloc = useMalloc;
    out->memory_context = ppl_memAlloc_GetMemContext();
    return out;
}

//! ppl_listFree - Free a linked list, including the data stored in each item. Has no effect on lists which were not
//! created with <useMalloc> set.
//! \param [in] in - The list to free
//! \return Zero

int ppl_listFree(list *in) {
    if ((in == NULL) || (!in->useMalloc)) return 0;
    listItem *ptr = in->first;
    // Free each item in turn, with its data
    while (ptr != NULL) {
        listItem *ptrnext = ptr->next;
        free(ptr->data);
        ptr->data = NULL;
        free(ptr);
        ptr = ptrnext;
    }
    free(in);
    in = NULL;
    return 0;
}

//! ppl_listLen - Return the number of items in a linked list
//! \param [in] in - The list to query (may be NULL)
//! \return The number of items in the list, or zero if <in> is NULL

int ppl_listLen(list *in) {
    if (in == NULL) return 0;
    return in->length;
}

//! listAlloc - Allocate memory for a list, using malloc() or the memory context of the list, as it requires
//! \param [in] in - The list
//! \param [in] size - The number of bytes to allocate
//! \return Pointer to the allocated memory, or NULL on failure

static void *listAlloc(const list *in, const int size) {
    return in->useMalloc ? malloc(size) : ppl_memAlloc_incontext(size, in->memory_context);
}


//! ppl_listCpy - Make a copy of a linked list, also copying the data stored in each item
//! \param [in] in - The list to copy
//! \param [in] useMalloc - Whether the new list should be allocated with malloc() (see ppl_listInit())
//! \param [in] itemSize - The number of bytes of data to copy from each list item
//! \return The new list, or NULL if memory allocation failed

list *ppl_listCpy(list *in, const int useMalloc, const int itemSize) {
    if (in == NULL) return NULL;
    list *out = ppl_listInit(useMalloc);
    if (out == NULL) return NULL;
    listItem *i = in->first;
    // Append a copy of each item's data to the new list
    while (i != NULL) {
        ppl_listAppendCpy(out, i->data, itemSize);
        i = i->next;
    }
    return out;
}

//! ppl_listAppend - Append an item to the end of a linked list. The item is not copied; the list stores the pointer.
//! \param [in] in - The list to append to
//! \param [in] item - Pointer to the data to store in the list
//! \return Zero on success, or one if memory allocation failed

int ppl_listAppend(list *in, void *item) {
    if (in == NULL) return 1;
    listItem *ptrnew = (listItem *) listAlloc(in, sizeof(listItem));
    if (ptrnew == NULL) return 1;
    ptrnew->prev = in->last;
    ptrnew->next = NULL;
    ptrnew->data = item;
    if (in->first == NULL) in->first = ptrnew;
    if (in->last != NULL) in->last->next = ptrnew;
    in->last = ptrnew;
    in->length++;
    return 0;
}

//! ppl_listAppendCpy - Append a copy of an item to the end of a linked list
//! \param [in] in - The list to append to
//! \param [in] item - Pointer to the data to copy into the list
//! \param [in] size - The number of bytes of data to copy
//! \return Zero on success, or one if memory allocation failed

int ppl_listAppendCpy(list *in, const void *item, const int size) {
    if (in == NULL) return 1;
    listItem *ptrnew = (listItem *) listAlloc(in, sizeof(listItem));
    if (ptrnew == NULL) return 1;
    ptrnew->prev = in->last;
    ptrnew->next = NULL;
    ptrnew->data = listAlloc(in, size);
    // Allocating space for the data failed: free the new item and give up
    if (ptrnew->data == NULL) {
        // Memory from a memory context cannot be freed individually
        if (in->useMalloc) {
            free(ptrnew);
            ptrnew = NULL;
        }
        return 1;
    }
    memcpy(ptrnew->data, item, size);
    if (in->first == NULL) in->first = ptrnew;
    if (in->last != NULL) in->last->next = ptrnew;
    in->last = ptrnew;
    in->length++;
    return 0;
}

//! ppl_listInsertCpy - Insert a copy of an item into a linked list at a specified position
//! \param [in] in - The list to insert into
//! \param [in] N - The position at which to insert the item; zero inserts at the start. If N exceeds the length of
//! the list, the item is appended to the end.
//! \param [in] item - Pointer to the data to copy into the list
//! \param [in] size - The number of bytes of data to copy
//! \return Zero on success, or one if memory allocation failed

int ppl_listInsertCpy(list *in, const int N, const void *item, const int size) {
    if (in == NULL) return 1;
    listItem *ptrnew = (listItem *) listAlloc(in, sizeof(listItem));
    if (ptrnew == NULL) return 1;
    ptrnew->data = listAlloc(in, size);
    // Allocating space for the data failed: free the new item and give up
    if (ptrnew->data == NULL) {
        // Memory from a memory context cannot be freed individually
        if (in->useMalloc) {
            free(ptrnew);
            ptrnew = NULL;
        }
        return 1;
    }
    memcpy(ptrnew->data, item, size);
    listItem **ptr = &in->first;
    // Find the link which points to the Nth item (or the end of the list), and splice the new item in there
    for (int i = 0; ((i < N) && (*ptr != NULL)); i++, ptr = &(*ptr)->next);
    ptrnew->prev = (*ptr == NULL) ? in->last : (*ptr)->prev;
    ptrnew->next = *ptr;
    if (*ptr != NULL) (*ptr)->prev = ptrnew;
    *ptr = ptrnew;
    in->length++;
    return 0;
}

//! ppl_listRemoveEngine - Remove an item from a linked list. The data it points to is not freed.
//! \param [in] in - The list to remove the item from
//! \param [in] ptr - The list item to remove

static void ppl_listRemoveEngine(list *in, listItem *ptr) {
    // Rather than unlinking <ptr> itself, a neighbouring item's data is moved into <ptr> and the neighbour is unlinked.
    // This keeps <ptr> valid, but means that any iterator which points to the neighbour must not be used afterwards.
    listItem *ptrnext;
    if (ptr->next != NULL) // We are not the last item in the list
    {
        ptrnext = ptr->next;
        ptr->data = ptrnext->data;
        ptr->next = ptrnext->next;
        if (in->last == ptrnext) in->last = ptr;
        else ptr->next->prev = ptr;
        // Free the unlinked neighbour, unless it is in a memory context which will be freed as a whole
        if (in->useMalloc) {
            free(ptrnext);
            ptrnext = NULL;
        }
    } else if (ptr->prev != NULL) // We are the last item in the list, but not the first item
    {
        ptrnext = ptr->prev;
        ptr->data = ptrnext->data;
        ptr->prev = ptrnext->prev;
        if (in->first == ptrnext) in->first = ptr;
        else ptrnext->prev->next = ptr;
        // Free the unlinked neighbour, unless it is in a memory context which will be freed as a whole
        if (in->useMalloc) {
            free(ptrnext);
            ptrnext = NULL;
        }
    } else // We are the only item in the list
    {
        in->first = NULL;
        in->last = NULL;
        // Free the item, unless it is in a memory context which will be freed as a whole
        if (in->useMalloc) {
            free(ptr);
            ptr = NULL;
        }
    }
    in->length--;
    return;
}

//! ppl_listRemove - Remove the first item in a linked list which points to a specified piece of data
//! \param [in] in - The list to remove the item from
//! \param [in] item - The data pointer to search for
//! \return Zero if an item was removed, or one if no matching item was found

int ppl_listRemove(list *in, const void *item) {
    if (in == NULL) return 1;
    listItem *ptr = in->first;
    // Search for an item with this data pointer
    while (ptr != NULL) {
        // Found it: remove it
        if (ptr->data == item) {
            ppl_listRemoveEngine(in, ptr);
            return 0;
        }
        ptr = ptr->next;
    }
    return 1;
}

//! ppl_listRemoveAll - Remove all items in a linked list which point to a specified piece of data
//! \param [in] in - The list to remove items from
//! \param [in] item - The data pointer to search for
//! \return Zero

int ppl_listRemoveAll(list *in, void *item) {
    while (!ppl_listRemove(in, item));
    return 0;
}

//! ppl_listGetItem - Return the data stored in the Nth item of a linked list
//! \param [in] in - The list to query
//! \param [in] N - The index of the item to return, counting from zero
//! \return The data pointer stored in the Nth item, or NULL if the list has fewer than N+1 items

void *ppl_listGetItem(const list *in, const int N) {
    if (in == NULL) return NULL;
    listItem *ptr = in->first;
    // Walk along the list to the Nth item
    for (int i = 0; ((i < N) && (ptr != NULL)); i++, ptr = ptr->next);
    if (ptr == NULL) return NULL;
    return ptr->data;
}

//! ppl_listPop - Remove the last item from a linked list, and return the data it pointed to
//! \param [in] in - The list to pop an item from
//! \return The data pointer stored in the last item, or NULL if the list is empty

void *ppl_listPop(list *in) {
    if (in->last == NULL) return NULL;
    void *out = in->last->data;
    // Unlink the last item; the item itself is not freed
    if (in->first == in->last) {
        in->first = in->last = NULL;
    } else {
        in->last = in->last->prev;
        in->last->next = NULL;
    }
    in->length--;
    return out;
}

//! ppl_listPopItem - Remove the Nth item from a linked list, and return the data it pointed to
//! \param [in] in - The list to pop an item from
//! \param [in] N - The index of the item to remove, counting from zero
//! \return The data pointer stored in the Nth item, or NULL if the list has fewer than N+1 items

void *ppl_listPopItem(list *in, const int N) {
    void *out = NULL;
    if (in == NULL) return NULL;
    listItem *ptr = in->first;
    // Walk along the list to the Nth item
    for (int i = 0; ((i < N) && (ptr != NULL)); i++, ptr = ptr->next);
    if (ptr == NULL) return NULL;
    // Take the item's data, and remove the item from the list
    else {
        out = ptr->data;
        ppl_listRemoveEngine(in, ptr);
    }
    return out;
}

//! ppl_listLast - Return the data stored in the last item of a linked list, without removing it
//! \param [in] in - The list to query
//! \return The data pointer stored in the last item, or NULL if the list is empty

void *ppl_listLast(const list *in) {
    if (in->last == NULL) return NULL;
    return in->last->data;
}

//! ppl_listIterateInit - Begin iterating over the items in a linked list
//! \param [in] in - The list to iterate over
//! \return An iterator pointing to the first item in the list, to pass to ppl_listIterate(), or NULL if the list is
//! empty

listIterator *ppl_listIterateInit(const list *in) {
    if (in == NULL) return NULL;
    return in->first;
}

//! ppl_listIterate - Return the data stored in the list item an iterator points to, and advance the iterator to the
//! next item
//! \param [in|out] in - Pointer to the iterator, which is advanced to the next item
//! \return The data pointer stored in the current item, or NULL if the iterator has reached the end of the list

void *ppl_listIterate(listIterator **in) {
    if ((in == NULL) || (*in == NULL)) return NULL;
    void *out = (*in)->data;
    *in = (*in)->next;
    return out;
}
