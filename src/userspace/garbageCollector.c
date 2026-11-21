// garbageCollector.c
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

// Pyxplot's reference-counting garbage collector, and the operations on the stack on which expressions are evaluated.
// Every pplObj carries a reference count; ppl_garbageObject() decrements it, and when it reaches zero frees whatever
// the object owns (strings, lists, dictionaries, matrices, files, functions...), recursively. Reference counts are
// changed atomically, so that objects may be shared between threads which evaluate functions in parallel.

#include "stdlib.h"
#include "stdio.h"

#include "gsl/gsl_matrix.h"
#include "gsl/gsl_vector.h"

#include "coreUtils/dict.h"
#include "coreUtils/list.h"
#include "expressions/expCompile_fns.h"
#include "parser/parser.h"
#include "userspace/garbageCollector.h"
#include "userspace/pplObj.h"
#include "userspace/pplObj_fns.h"
#include "userspace/pplObjFunc_fns.h"
#include "userspace/stack.h"
#include "coreUtils/errorReport.h"
#include "pplConstants.h"

//! ppl_garbageNamespace - Free a namespace dictionary, releasing a reference to each of the objects in it
//! \param [in] n - The namespace to free. Has no effect unless it was allocated with malloc().

void ppl_garbageNamespace(dict *n) {
    dictItem *ptr, *ptrnext;
    if ((n == NULL) || (!n->useMalloc)) return;
    ptr = n->first;
    // Release the value of each variable, and free its key and item. Each value is freed by the garbage collector
    // itself, since the dictionary was created with malloc().
    while (ptr != NULL) {
        ptrnext = ptr->next;
        ppl_garbageObject((pplObj *) ptr->data);
        free(ptr->key);
        ptr->key = NULL;
        // free(ptr->data); -- already done by garbage collector
        free(ptr);
        ptr = ptrnext;
    }
    ppl_dictFreeTree(n->hashTree);
    free(n);
    n = NULL;
    return;
}

//! ppl_garbageList - Free a list of objects, releasing a reference to each of the objects in it
//! \param [in] l - The list to free. Has no effect unless it was allocated with malloc().

void ppl_garbageList(list *l) {
    listItem *ptr, *ptrnext;
    if ((l == NULL) || (!l->useMalloc)) return;
    ptr = l->first;
    // Release each item in the list, and free the list item which held it
    while (ptr != NULL) {
        ptrnext = ptr->next;
        ppl_garbageObject((pplObj *) ptr->data);
        // free(ptr->data); -- already done by garbage collector
        free(ptr);
        ptr = ptrnext;
    }
    free(l);
    l = NULL;
    return;
}

//! ppl_garbageObject - Release a reference to an object. When its reference count reaches zero, any storage it
//! owns (strings, lists, dictionaries, files, etc.) is freed, and it becomes a zombie.
//! \param [in] o - The object to release

void ppl_garbageObject(pplObj *o) {
    // Drop one reference; if others remain, the object stays alive. Otherwise mark it as a zombie, so that any stray
    // use of it is harmless, and free what it owns.
    int objType;
    if (o == NULL) return;
    if (__sync_sub_and_fetch(&o->refCount, 1) > 0) return;
    objType = o->objType;
    o->objType = PPLOBJ_ZOM; // Object is now a zombie
    // Release the variable from which the object was read, kept so that methods such as list.append() can modify it in
    // place
    if (o->self_lval != NULL) {
        ppl_garbageObject(o->self_lval);
        o->self_lval = NULL;
    }
    // Release the object to which a method belongs, kept so that the method can be called
    if (o->self_this != NULL) {
        ppl_garbageObject(o->self_this);
        o->self_this = NULL;
    }
    // Free what the object owns, according to its type. Objects with sub-structures shared between copies, e.g. lists
    // and vectors, are only freed when the sub-structure's own reference count reaches zero.
    switch (objType) {
        case PPLOBJ_STR:
        case PPLOBJ_EXC:
            // Strings and exception messages may be static, in which case they are not freed
            if (o->auxilMalloced) {
                void *old = o->auxil;
                o->auxil = NULL;
                // Free the text
                if (old != NULL) {
                    free(old);
                    old = NULL;
                }
            }
            break;
        case PPLOBJ_EXP: {
            void *old = o->auxil;
            o->auxil = NULL;
            // Free the compiled expression
            if ((old != NULL) && o->auxilMalloced) {
                pplExpr_free((pplExpr *) old);
                old = NULL;
            }
            break;
        }
        case PPLOBJ_BYT: {
            void *old = o->auxil;
            o->auxil = NULL;
            if ((old != NULL) && (o->auxilMalloced)) ppl_parserLineFree((parserLine *) old);
            break;
        }
        case PPLOBJ_FILE: {
            pplFile *f = (pplFile *) (o->auxil);
            o->auxil = NULL;
            // When the last reference to a file handle goes, close the file
            if ((f != NULL) && (__sync_sub_and_fetch(&f->refCount, 1) <= 0)) {
                // Close the file, or the pipe; pipe == 2 marks stdin, which is never closed
                if ((f->open) && (f->file != NULL)) {
                    FILE *old = f->file;
                    f->file = NULL;
                    if (f->pipe == 2) { /* never close stdin */
                    } else if (f->pipe) pclose(old);
                    else fclose(old);
                }
                free(f);
                f = NULL;
            }
            break;
        }
        case PPLOBJ_FUNC: {
            pplFunc *f = (pplFunc *) (o->auxil);
            o->auxil = NULL;
            if ((f != NULL) && (__sync_sub_and_fetch(&f->refCount, 1) <= 0)) pplObjFuncDestroyChain(f);
            break;
        }
        case PPLOBJ_TYPE: {
            pplType *t = (pplType *) (o->auxil);
            o->auxil = NULL;
            if (t != NULL) { __sync_sub_and_fetch(&t->refCount, 1); }
            break; // Types don't ever get garbage collected
        }
        case PPLOBJ_LIST: {
            list *l = (list *) (o->auxil);
            o->auxil = NULL;
            if ((l != NULL) && (__sync_sub_and_fetch(&l->refCount, 1) <= 0)) ppl_garbageList(l);
            break;
        }
        case PPLOBJ_VEC: {
            pplVector *v = (pplVector *) (o->auxil);
            pplVectorRaw *vr = v->raw;
            pplMatrixRaw *vrm = v->rawm;
            o->auxil = NULL;
            // A vector holds a reference to its raw storage, which may be shared with other vectors (e.g. slices of
            // it), and possibly to the matrix of which it is a row or column. Free each when its last reference goes.
            if ((vr != NULL) && (__sync_sub_and_fetch(&vr->refCount, 1) <= 0)) {
                gsl_vector_free(vr->v);
                vr->v = NULL;
                // Free the raw storage structure itself
                if (o->auxilMalloced) {
                    free(vr);
                    vr = NULL;
                }
            }
            // The matrix of which the vector is a row or column
            if ((vrm != NULL) && (__sync_sub_and_fetch(&vrm->refCount, 1) <= 0)) {
                gsl_matrix_free(vrm->m);
                vrm->m = NULL;
                // Free the raw matrix structure itself
                if (o->auxilMalloced) {
                    free(vrm);
                    vrm = NULL;
                }
            }
            // Free the vector's own descriptor
            if ((v != NULL) && (__sync_sub_and_fetch(&v->refCount, 1) <= 0) && (o->auxilMalloced)) {
                free(v);
                v = NULL;
            }
            break;
        }
        case PPLOBJ_MAT: {
            pplMatrix *m = (pplMatrix *) (o->auxil);
            pplMatrixRaw *mr = m->raw;
            o->auxil = NULL;
            // A matrix holds a reference to its raw storage, which may be shared with other matrices
            if ((mr != NULL) && (__sync_sub_and_fetch(&mr->refCount, 1) <= 0)) {
                gsl_matrix_free(mr->m);
                mr->m = NULL;
                // Free the raw storage structure itself
                if (o->auxilMalloced) {
                    free(mr);
                    mr = NULL;
                }
            }
            // Free the matrix's own descriptor
            if ((m != NULL) && (__sync_sub_and_fetch(&m->refCount, 1) <= 0) && (o->auxilMalloced)) {
                free(m);
                m = NULL;
            }
            break;
        }
        // An instance of a class also holds a reference to its class, and then has a dictionary of members like a
        // module
        case PPLOBJ_USER: {
            ppl_garbageObject(o->objPrototype);
        }
        case PPLOBJ_DICT:
        case PPLOBJ_MOD: {
            dict *d = (dict *) (o->auxil);
            if ((d != NULL) && (__sync_sub_and_fetch(&d->refCount, 1) <= 0)) ppl_garbageNamespace(d);
            break;
        }
    }

    // Objects which were themselves malloced, e.g. variables in a namespace, are freed too
    if (o->amMalloced) {
        free(o);
        o = NULL;
    }
    return;
}

//! ppl_stackPop - Pop the object on top of the evaluation stack, and free it. If anything still refers to the object,
//! which should never happen, report an internal error; the object is then left allocated.
//! \param [in] c - The Pyxplot context
//! \return Zero on success, or one if the object was still referenced

int ppl_stackPop(ppl_context *c) {
    c->stackPtr--;
    ppl_garbageObject(&c->stack[c->stackPtr]);
    // Something else still refers to the object which has just been popped, which should never happen
    if (c->stack[c->stackPtr].refCount != 0) {
        ppl_error(&c->errcontext, ERR_INTERNAL, -1, -1, "Stack forward reference detected.");
        return 1;
    }
    return 0;
}

//! ppl_stackPushCopy - Push a copy of an object onto the top of the evaluation stack. The caller must already have
//! checked that there is room (e.g. with ppl_stackMustHave()).
//! \param [in] c - The Pyxplot context
//! \param [in] o - The object to copy onto the stack
//! \param [in] lval - If true, the copy records that it was derived from <o>, so that assignments to it update <o>

void ppl_stackPushCopy(ppl_context *c, pplObj *o, const int lval) {
    pplObj *top = &c->stack[c->stackPtr];
    pplObjCpy(top, o, lval, 0, 1);
    top->refCount = 1;
    c->stackPtr++;
}

//! ppl_stackClean - Pop objects from the evaluation stack until it is no deeper than a given level
//! \param [in] c - The Pyxplot context
//! \param [in] level - The stack depth to return to
//! \return Zero on success, or one if any object popped was still referenced

int ppl_stackClean(ppl_context *c, const int level) {
    int status = 0;
    while (c->stackPtr > level) status |= ppl_stackPop(c);
    return status;
}

//! ppl_stackRequest - Make sure there is room on the evaluation stack for <n> more objects, enlarging the stack if
//! necessary. Sets <c->stackFull> if this fails. This must only be called when nothing holds pointers into the stack.
//! \param [in] c - The Pyxplot context
//! \param [in] n - The number of objects which are about to be pushed

void ppl_stackRequest(ppl_context *c, const int n) {
    c->stackFull = 0;
    // If the stack is nearly full, double its size; if that fails, flag the stack as full, which is reported as an
    // error
    if (c->stackPtr > c->stackSize - 1024 - n) {
        const int newStackSize = 2 * c->stackSize;
        pplObj *newStack = (pplObj *) realloc(c->stack, newStackSize * sizeof(pplObj));
        // Use the enlarged stack
        if (newStack != NULL) {
            c->stack = newStack;
            c->stackSize = newStackSize;
        } else c->stackFull = 1;
    }
}

//! ppl_stackMustHave - Check whether there is room on the evaluation stack for <n> more objects, without enlarging
//! it, and set <c->stackFull> if there is not
//! \param [in] c - The Pyxplot context
//! \param [in] n - The number of objects which are about to be pushed

void ppl_stackMustHave(ppl_context *c, const int n) {
    c->stackFull = (c->stackPtr > c->stackSize - 4 - n);
}

//! ppl_stackReinit - Empty the evaluation stack, and if it has grown larger than its default size, shrink it back
//! \param [in] c - The Pyxplot context

void ppl_stackReinit(ppl_context *c) {
    ppl_stackClean(c, 0);
    // Shrink a stack which has grown back to its default size
    if (c->stackSize > STACK_DEFAULT) {
        pplObj *newStack = (pplObj *) malloc(STACK_DEFAULT * sizeof(pplObj));
        // Replace the stack, which is now empty
        if (newStack != NULL) {
            free(c->stack);
            c->stack = newStack;
            c->stackSize = STACK_DEFAULT;
        }
    }
}
