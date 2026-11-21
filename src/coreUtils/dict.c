// lt_dict.c
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

// Pyxplot's dictionary type: a list of (key, value) pairs, kept sorted by key, with a binary tree of key hashes for
// fast lookup. Dictionaries hold Pyxplot's variable namespaces, the members of modules and class instances, and
// user-level dict objects, and are used for many internal tables. Items may be allocated with malloc() or in a
// temporary memory context from memAlloc.c.

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "coreUtils/dict.h"
#include "coreUtils/memAlloc.h"

#include "stringTools/asciidouble.h"

void _ppl_dictRemoveEngine(dict *in, dictItem *ptr);

//! ppl_dictInit - Create a new, empty dictionary. Dictionaries are stored as linked lists sorted alphabetically by
//! key, with a binary tree of key hashes to speed up lookups.
//! \param [in] useMalloc - If true, the dictionary and its items are allocated with malloc(), and must be freed with
//! ppl_dictFree(). If false, they are allocated in the current ppl_memAlloc context, and are freed when that context
//! is freed.
//! \return The new dictionary, or NULL if memory allocation failed

dict *ppl_dictInit(const int useMalloc) {
    dict *out;
    if (useMalloc) out = (dict *) malloc(sizeof(dict));
    else out = (dict *) ppl_memAlloc(sizeof(dict));
    if (out == NULL) return NULL;
    out->first = NULL;
    out->last = NULL;
    out->length = 0;
    out->refCount = 1;
    out->immutable = 0;
    out->hashTree = NULL;
    out->useMalloc = useMalloc;
    out->memory_context = ppl_memAlloc_GetMemContext();
    return out;
}

//! ppl_dictFreeTree - Recursively free a binary tree of dictionary key hashes
//! \param [in] i - The root of the tree to free

void ppl_dictFreeTree(dictHash *i) {
    if (i == NULL) return;
    ppl_dictFreeTree(i->next);
    ppl_dictFreeTree(i->prev);
    free(i);
    i = NULL;
    return;
}

//! ppl_dictFree - Free a dictionary, including the keys and data stored in each item. Has no effect on dictionaries
//! which were not created with <useMalloc> set.
//! \param [in] in - The dictionary to free
//! \return Zero

int ppl_dictFree(dict *in) {
    if ((in == NULL) || (!in->useMalloc)) return 0;
    dictItem *ptr = in->first;
    // Free each item in turn, with its key and data
    while (ptr != NULL) {
        dictItem *ptrnext = ptr->next;
        free(ptr->key);
        ptr->key = NULL;
        free(ptr->data);
        ptr->data = NULL;
        free(ptr);
        ptr = ptrnext;
    }
    ppl_dictFreeTree(in->hashTree);
    free(in);
    in = NULL;
    return 0;
}

//! ppl_dictHash - Compute the djb2 hash of a string, used to index dictionary keys
//! \param [in] str - The string to hash
//! \param [in] strLen - The number of characters of <str> to hash, or -1 to hash the whole string
//! \return The hash value

int ppl_dictHash(const char *str, const int strLen) {
    unsigned int hash = 5381;
    int c, i = 0;
    if (strLen < 0)
        // djb2: hash = hash * 33 + c, for each character in turn
        while ((c = *str++)) hash = ((hash << 5) + hash) + c;
    else
        while ((i++ < strLen) && ((c = *str++))) hash = ((hash << 5) + hash) + c;
    return hash;
}

//! dictAlloc - Allocate memory for a dict, using malloc() or the memory context of the dict, as it requires
//! \param [in] in - The dict
//! \param [in] size - The number of bytes to allocate
//! \return Pointer to the allocated memory, or NULL on failure

static void *dictAlloc(const dict *in, const int size) {
    return in->useMalloc ? malloc(size) : ppl_memAlloc_incontext(size, in->memory_context);
}


//! ppl_dictHashAdd - Add an entry to a dictionary's binary tree of key hashes. If an entry with the same hash
//! already exists for the same key it is updated; if it exists for a different key, the tree is left unchanged and
//! lookups of the new key fall back to a linear search.
//! \param [in] in - The dictionary to update
//! \param [in] str - The key of the new item
//! \param [in] hash - The hash of <str>, as returned by ppl_dictHash()
//! \param [in] item - The dictionary item which the hash should point to

static void ppl_dictHashAdd(dict *in, const char *str, const int hash, dictItem *item) {
    dictHash **ptr = &in->hashTree;

    // Descend the tree to the place where this hash belongs; larger hashes are stored down the prev branch, and smaller
    // ones down the next branch
    while ((*ptr) != NULL) {
        if ((*ptr)->hash < hash) ptr = &((*ptr)->prev);
        else if ((*ptr)->hash > hash) ptr = &((*ptr)->next);
        // An entry with this hash already exists
        else {
            // If it is for the same key, point it at the new item; otherwise this is a genuine hash clash, and the new
            // key is left out of the tree
            if (strcmp((*ptr)->item->key, str) == 0) {
                (*ptr)->item = item;
                return;
            } // Overwriting a pre-existing item
            else
                return; // Genuine clash. Nothing we can do.
        }
    }

    *ptr = dictAlloc(in, sizeof(dictHash));
    if (*ptr == NULL) return;
    (*ptr)->hash = hash;
    (*ptr)->item = item;
    (*ptr)->prev = NULL;
    (*ptr)->next = NULL;
}

//! ppl_dictHashLookup - Look up a key in a dictionary's binary tree of key hashes
//! \param [in] in - The dictionary to search
//! \param [in] str - The key to search for
//! \param [in] slen - The number of characters of <str> to compare, or zero or less to compare the whole string
//! \param [in] hash - The hash of the key, as returned by ppl_dictHash()
//! \param [out] clash - Set to one if an entry with the same hash but a different key was found, in which case the
//! caller must fall back to a linear search; zero otherwise
//! \return The matching dictionary item, or NULL if none was found

static dictItem *ppl_dictHashLookup(dict *in, const char *str, int slen, const int hash, int *clash) {
    dictHash **ptr = &in->hashTree;

    // Descend the tree towards the entry with this hash
    if (str == NULL) return NULL;
    if (slen <= 0) slen = strlen(str);
    *clash = 0;
    while ((*ptr) != NULL) {
        if ((*ptr)->hash < hash) ptr = &((*ptr)->prev);
        else if ((*ptr)->hash > hash) ptr = &((*ptr)->next);
        // An entry with this hash: either it is the key we want, or a different key with the same hash
        else {
            if (strncmp((*ptr)->item->key, str, slen) == 0) return (*ptr)->item; // Found it
            *clash = 1;
            return NULL;
        }
    }
    return NULL;
}

//! ppl_dictHashRemove - Remove a key from a dictionary's binary tree of key hashes
//! \param [in] in - The dictionary to update
//! \param [in] str - The key to remove
//! \param [in] hash - The hash of the key, as returned by ppl_dictHash()
//! \param [out] clash - Set to one if an entry with the same hash but a different key was found; zero otherwise
//! \return The dictionary item which the removed hash entry pointed to, or NULL if none was found

static dictItem *ppl_dictHashRemove(dict *in, const char *str, const int hash, int *clash) {
    // ptr points to the link in the tree which we are currently examining
    dictHash **ptr = &in->hashTree;

    // Descend the tree to find the entry for this key
    if (str == NULL) return NULL;
    *clash = 0;
    while ((*ptr) != NULL) {
        if ((*ptr)->hash < hash) ptr = &((*ptr)->prev);
        else if ((*ptr)->hash > hash) ptr = &((*ptr)->next);
        // An entry with this hash: check whether it is the key we want
        else {
            if (strcmp((*ptr)->item->key, str) == 0) // Found it
            {
                dictHash *old = *ptr;
                dictHash **near = ptr, *nearEntry;
                dictItem *out = (*ptr)->item;
                // An entry with no prev branch is replaced by its next branch
                if (old->prev == NULL) {
                    *ptr = old->next;
                    if (in->useMalloc) {
                        free(old);
                        old = NULL;
                    }
                    return out;
                }
                // An entry with no next branch is replaced by its prev branch
                if (old->next == NULL) {
                    *ptr = old->prev;
                    if (in->useMalloc) {
                        free(old);
                        old = NULL;
                    }
                    return out;
                }
                // An entry with two branches is replaced by the nearest entry in one of its subtrees, which is detached
                // from the subtree. Which side to take is chosen from the hash, to keep the tree balanced on average.
                if (hash & 1) {
                    while ((*near)->prev != NULL) near = &((*near)->prev);
                    nearEntry = *near;
                    *near = (*near)->next;
                } else {
                    while ((*near)->next != NULL) near = &((*near)->next);
                    nearEntry = *near;
                    *near = (*near)->prev;
                }
                *ptr = nearEntry;
                if (in->useMalloc) {
                    free(old);
                    old = NULL;
                }
                return out;
            } else {
                *clash = 1;
                return NULL; // Genuine clash. Nothing we can do.
            }
        }
    }
    return NULL;
}

//! ppl_dictLen - Return the number of items in a dictionary
//! \param [in] in - The dictionary to query (may be NULL)
//! \return The number of items in the dictionary, or zero if <in> is NULL

int ppl_dictLen(const dict *in) {
    if (in == NULL) return 0;
    return in->length;
}

//! ppl_dictAppend - Add an item to a dictionary, overwriting any existing item with the same key. The item is not
//! copied; the dictionary stores the pointer.
//! \param [in] in - The dictionary to add to
//! \param [in] key - The key under which to store the item. This is copied.
//! \param [in] item - Pointer to the data to store
//! \return Zero on success, or one if memory allocation failed

int ppl_dictAppend(dict *in, const char *key, void *item) {
    // ptr scans through the list; prev is the item before it; cmp records the comparison of keys
    dictItem *ptr = NULL, *ptrnew = NULL, *prev = NULL;
    int cmp = -1;

    // Find the position in the alphabetically-sorted list where the key belongs, or an existing item with this key.
    // Keys are sorted case-insensitively, with an exact comparison to find matching keys.
    if (key == NULL) return 1;
    ptr = in->first;
    // Walk along the list until we reach a key which sorts after the new one, or which matches it
    while (ptr != NULL) {
        if (((cmp = ppl_strCmpNoCase(ptr->key, key)) > 0) || ((cmp = strcmp(ptr->key, key)) == 0)) break;
        prev = ptr;
        ptr = ptr->next;
    }
    if (cmp == 0) // Overwrite an existing entry in dictionary
    {
        // Free the old data, unless it is in a memory context which will be freed as a whole
        if (in->useMalloc) {
            free(ptr->data);
            ptr->data = NULL;
        }
        ptr->data = item;
    } else {
        ptrnew = (dictItem *) dictAlloc(in, sizeof(dictItem));
        if (ptrnew == NULL) return 1;
        ptrnew->prev = prev;
        ptrnew->next = ptr;
        ptrnew->key = (char *) dictAlloc(in, (strlen(key) + 1));
        // Allocating the key failed: free the new item and give up
        if (ptrnew->key == NULL) {
            // Memory from a memory context cannot be freed individually
            if (in->useMalloc) {
                free(ptrnew);
                ptrnew = NULL;
            }
            return 1;
        }
        strcpy(ptrnew->key, key);
        ptrnew->data = item;
        if (prev == NULL) in->first = ptrnew;
        else prev->next = ptrnew;
        if (ptr == NULL) in->last = ptrnew;
        else ptr->prev = ptrnew;
        in->length++;

        // Add the new key to the tree of hashes
        const int hash = ppl_dictHash(key, -1);
        ppl_dictHashAdd(in, key, hash, ptrnew);
    }
    return 0;
}

//! ppl_dictAppendCpy - Add a copy of an item to a dictionary, overwriting any existing item with the same key
//! \param [in] in - The dictionary to add to
//! \param [in] key - The key under which to store the item. This is copied.
//! \param [in] item - Pointer to the data to copy into the dictionary
//! \param [in] size - The number of bytes of data to copy
//! \return Zero on success, or one if memory allocation failed

int ppl_dictAppendCpy(dict *in, const char *key, const void *item, const int size) {
    // ptr scans through the list; prev is the item before it; cpy is the copy of the data; cmp records the comparison
    // of keys
    dictItem *ptr = NULL, *ptrnew = NULL, *prev = NULL;
    int cmp = -1;

    // Copy the data
    if (key == NULL) return 1;
    void *cpy = dictAlloc(in, size);
    if (cpy == NULL) return 1;
    memcpy(cpy, item, size);

    // Find the position in the alphabetically-sorted list where the key belongs, or an existing item with this key
    ptr = in->first;
    // Walk along the list until we reach a key which sorts after the new one, or which matches it
    while (ptr != NULL) {
        if (((cmp = ppl_strCmpNoCase(ptr->key, key)) > 0) || ((cmp = strcmp(ptr->key, key)) == 0)) break;
        prev = ptr;
        ptr = ptr->next;
    }
    if (cmp == 0) // Overwrite an existing entry in dictionary
    {
        // Free the old data, unless it is in a memory context which will be freed as a whole
        if (in->useMalloc) {
            free(ptr->data);
            ptr->data = NULL;
        }
        ptr->data = cpy;
        ptrnew = ptr;
    } else {
        ptrnew = (dictItem *) dictAlloc(in, sizeof(dictItem));
        // Allocating the new item failed: free the copy of the data and give up
        if (ptrnew == NULL) {
            // Memory from a memory context cannot be freed individually
            if (in->useMalloc) {
                free(cpy);
                cpy = NULL;
            }
            return 1;
        }
        ptrnew->prev = prev;
        ptrnew->next = ptr;
        ptrnew->key = (char *) dictAlloc(in, (strlen(key) + 1));
        // Allocating the key failed: free the copy of the data and the new item, and give up
        if (ptrnew->key == NULL) {
            // Memory from a memory context cannot be freed individually
            if (in->useMalloc) {
                free(cpy);
                cpy = NULL;
                free(ptrnew);
                ptrnew = NULL;
            }
            return 1;
        }
        strcpy(ptrnew->key, key);
        ptrnew->data = cpy;
        memcpy(ptrnew->data, item, size);
        if (prev == NULL) in->first = ptrnew;
        else prev->next = ptrnew;
        if (ptr == NULL) in->last = ptrnew;
        else ptr->prev = ptrnew;
        in->length++;

        // Add the new key to the tree of hashes
        const int hash = ppl_dictHash(key, -1);
        ppl_dictHashAdd(in, key, hash, ptrnew);
    }
    return 0;
}

//! ppl_dictLookup - Look up a key in a dictionary
//! \param [in] in - The dictionary to search
//! \param [in] key - The key to search for
//! \return The data pointer stored under <key>, or NULL if it was not found

void *ppl_dictLookup(dict *in, const char *key) {
    if (key == NULL) return NULL;
    const int hash = ppl_dictHash(key, -1);
    return ppl_dictLookupHash(in, key, hash);
}

//! ppl_dictLookupHash - Look up a key in a dictionary, where the hash of the key has already been computed
//! \param [in] in - The dictionary to search
//! \param [in] key - The key to search for
//! \param [in] hash - The hash of <key>, as returned by ppl_dictHash()
//! \return The data pointer stored under <key>, or NULL if it was not found

void *ppl_dictLookupHash(dict *in, const char *key, const int hash) {
    int clash;

    // Nothing can be found in a missing dictionary, or under a missing key
    if (key == NULL) return NULL;
    if (in == NULL) { return NULL; }

    // Check hash table
    dictItem *ptr = ppl_dictHashLookup(in, key, -1, hash, &clash);
    // Without a clash, the hash tree gives the definitive answer
    if (!clash) {
        if (ptr == NULL) return NULL;
        return ptr->data;
    }

    // Hash table clash; need to exhaustively search dictionary
    ptr = in->first;
    // The list is sorted, so the search can stop once it passes the place where the key would be
    while (ptr != NULL) {
        if (strcmp(ptr->key, key) == 0) return ptr->data;
        else if (ppl_strCmpNoCase(ptr->key, key) > 0) break;
        ptr = ptr->next;
    }
    return NULL;
}

//! ppl_dictLookupWithWildcard - Look up the alphanumeric name at the start of a string in a dictionary. If it is not
//! found, also try matching the names of functions such as diff_d and int_d, which are followed by the name of a
//! dummy variable, e.g. int_dx.
//! \param [in] in - The dictionary to search
//! \param [in] key - The string whose leading alphanumeric word is to be looked up
//! \param [out] SubsString - Character buffer into which the name of the dummy variable is written, if a wildcard
//! function name was matched; otherwise set to an empty string
//! \param [in] SubsMaxLen - The size of the buffer <SubsString>
//! \param [out] ptrout - Set to the matching dictionary item, or NULL if none was found

void ppl_dictLookupWithWildcard(dict *in, char *key, char *SubsString, const int SubsMaxLen, dictItem **ptrout) {
    // The names of functions which may be followed by the name of a dummy variable, e.g. int_dx
    int clash, k;
    char *magicFns[] = {"diff_d", "int_d", NULL};

    // Nothing can be found under a missing key, or in a missing dictionary
    if (key == NULL) {
        *ptrout = NULL;
        return;
    }
    SubsString[0] = '\0';
    // A missing dictionary contains nothing
    if (in == NULL) {
        *ptrout = NULL;
        return;
    }

    // Check hash table
    for (k = 0; (isalnum(key[k]) || (key[k] == '_')); k++);
    const int keylen = k;
    const int hash = ppl_dictHash(key, keylen);
    dictItem *ptr = ppl_dictHashLookup(in, key, keylen, hash, &clash);
    // The hash tree found the name
    if (ptr != NULL) {
        *ptrout = ptr;
        return;
    }
    // After a hash clash, search the list for a key which matches the whole of the name
    if (clash) {
        ptr = in->first;
        // Compare each key with the start of <key>; the key must end where the name does
        while (ptr != NULL) {
            for (k = 0; (((unsigned char) ptr->key[k] > ' ') && (ptr->key[k] == key[k])); k++);
            // A match: the key has ended, and so has the alphanumeric name in <key>
            if ((ptr->key[k] == '\0') && (!(isalnum(key[k]) || (key[k] == '_')))) {
                *ptrout = ptr;
                return;
            }
            ptr = ptr->next;
        }
    }

    // Need to search "int_d?"-like wildcards
    for (int i = 0; magicFns[i] != NULL; i++) {
        for (k = 0; (((unsigned char) magicFns[i][k] > ' ') && (magicFns[i][k] == key[k])); k++);
        if (magicFns[i][k] == '\0') // each magic function name can be followed by a variable name
        {
            int l = 0;
            if (isalpha(key[k + l])) // first character of dummy variable name must be a letter
            {
                for (l = 0; ((isalnum(key[k + l]) || (key[k + l] == '_')) && (l < SubsMaxLen)); l++)
                    SubsString[l] = key[k + l];
            }
            if (l == SubsMaxLen) continue; // Dummy variable name was too long
            if (l == 0) continue; // Dummy variable was too short
            SubsString[l] = '\0';
            ptr = in->first;
            // Look for the function itself, e.g. int_d, in the dictionary
            while (ptr != NULL) {
                for (k = 0; (((unsigned char) ptr->key[k] > ' ') && (ptr->key[k] == magicFns[i][k])); k++);
                // A key which is exactly the function's name
                if ((magicFns[i][k] == '\0') && (ptr->key[k] == '\0')) {
                    *ptrout = ptr;
                    return;
                }
                ptr = ptr->next;
            }
        }
    }
    SubsString[0] = '\0';
    *ptrout = NULL;
    return;
}

//! ppl_dictContains - Test whether a dictionary contains a key
//! \param [in] in - The dictionary to search
//! \param [in] key - The key to search for
//! \return One if the key is present, zero otherwise

int ppl_dictContains(dict *in, const char *key) {
    int clash;
    if (key == NULL) return 0;
    if (in == NULL) return 0;

    // Check hash table
    const int hash = ppl_dictHash(key, -1);
    dictItem *ptr = ppl_dictHashLookup(in, key, -1, hash, &clash);
    if (!clash) return (ptr != NULL);

    // Hash table clash; need to exhaustively search dictionary
    ptr = in->first;
    // Search for an exact match
    while (ptr != NULL) {
        if (strcmp(ptr->key, key) == 0) return 1;
        ptr = ptr->next;
    }
    return 0;
}

//! ppl_dictRemoveKey - Remove the item with a specified key from a dictionary
//! \param [in] in - The dictionary to update
//! \param [in] key - The key to remove
//! \return Zero if an item was removed, or one if the key was not found

int ppl_dictRemoveKey(dict *in, const char *key) {
    // ptr is the item removed from the hash tree, or the item found by searching the list
    int clash;

    // Nothing can be removed under a missing key, or from a missing dictionary
    if (key == NULL) return 1;
    if (in == NULL) return 1;

    // Check hash table
    const int hash = ppl_dictHash(key, -1);
    dictItem *ptr = ppl_dictHashRemove(in, key, hash, &clash);
    if ((ptr == NULL) && (!clash)) return 1; // Key not present
    // The hash tree identified the item: unlink it from the list
    if (!clash) {
        _ppl_dictRemoveEngine(in, ptr);
        return 0;
    }

    // Hash table clash; need to exhaustively search dictionary
    ptr = in->first;
    // After a hash clash, search the sorted list for the key
    while (ptr != NULL) {
        // Found it: unlink it from the list; or stop once we pass the place where it would be
        if (strcmp(ptr->key, key) == 0) {
            _ppl_dictRemoveEngine(in, ptr);
            return 0;
        } else if (ppl_strCmpNoCase(ptr->key, key) > 0) break;
        ptr = ptr->next;
    }
    return 1;
}

//! ppl_dictRemove - Remove the first item in a dictionary which points to a specified piece of data
//! \param [in] in - The dictionary to update
//! \param [in] item - The data pointer to search for
//! \return Zero if an item was removed, or one if no matching item was found

int ppl_dictRemove(dict *in, const void *item) {
    if (in == NULL) return 1;
    dictItem *ptr = in->first;
    // Search for an item with this data pointer, and remove it by its key
    while (ptr != NULL) {
        if (ptr->data == item) { return ppl_dictRemoveKey(in, ptr->key); }
        ptr = ptr->next;
    }
    return 1;
}

//! _ppl_dictRemoveEngine - Unlink an item from a dictionary's linked list, freeing it if the dictionary uses malloc()
//! \param [in] in - The dictionary to update
//! \param [in] ptr - The dictionary item to remove

void _ppl_dictRemoveEngine(dict *in, dictItem *ptr) {
    if (in == NULL) return;
    if (ptr == NULL) return;

    // Free the item's data and key, unless they are in a memory context which will be freed as a whole
    if (in->useMalloc) {
        free(ptr->data);
        ptr->data = NULL;
        free(ptr->key);
        ptr->key = NULL;
    }

    if (ptr->next != NULL) ptr->next->prev = ptr->prev; // We are not the last item in the list
    if (ptr->prev != NULL) ptr->prev->next = ptr->next; // We are the last item in the list, but not the first item
    if (in->last == ptr) in->last = ptr->prev;
    if (in->first == ptr) in->first = ptr->next;
    // Free the item itself
    if (in->useMalloc) {
        free(ptr);
        ptr = NULL;
    }
    in->length--;

    // The item has been removed
    return;
}

//! ppl_dictRemoveAll - Remove all items in a dictionary which point to a specified piece of data
//! \param [in] in - The dictionary to update
//! \param [in] item - The data pointer to search for
//! \return Zero

int ppl_dictRemoveAll(dict *in, void *item) {
    while (!ppl_dictRemove(in, item));
    return 0;
}

//! ppl_dictIterateInit - Begin iterating over the items in a dictionary, in alphabetical order of key
//! \param [in] in - The dictionary to iterate over
//! \return An iterator pointing to the first item, to pass to ppl_dictIterate(), or NULL if the dictionary is empty

dictIterator *ppl_dictIterateInit(const dict *in) {
    if (in == NULL) return NULL;
    return in->first;
}

//! ppl_dictIterate - Return the data stored in the dictionary item an iterator points to, and advance the iterator
//! to the next item
//! \param [in|out] in - Pointer to the iterator, which is advanced to the next item
//! \param [out] key - Set to point to the key of the current item
//! \return The data pointer stored in the current item, or NULL if the iterator has reached the end

void *ppl_dictIterate(dictIterator **in, char **key) {
    if ((in == NULL) || (*in == NULL)) return NULL;
    *key = (*in)->key;
    void *out = (*in)->data;
    *in = (*in)->next;
    return out;
}
