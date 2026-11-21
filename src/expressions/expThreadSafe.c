// expThreadSafe.c
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

// Support for evaluating expressions on several threads at once, e.g. when sampling a function at many points.
//
// Each thread evaluates expressions in its own worker context, created by ppl_contextWorkerNew(), which has its own
// evaluation stack, error status and buffers. The worker shares the parent context's settings, unit database and
// namespaces of variables, which are read but not written while the threads run. Each worker has one extra namespace
// of its own, on top of the parent's, in which the dummy variables (e.g. x) are bound, and into which user-defined
// functions bind their arguments.
//
// Sharing variables is only safe if the expressions evaluated do not modify them, and do not call functions with side
// effects. ppl_expThreadSafe() checks this before an expression is evaluated in parallel; expressions which fail the
// check are evaluated serially, as before. The check is conservative: it accepts arithmetic, built-in functions which
// have no side effects, and user-defined functions whose definitions themselves pass the check.
//
// Reference counts of objects are updated with atomic operations throughout Pyxplot, so several threads may read the
// same variables safely. Expression evaluation allocates memory with malloc(), which is thread-safe, rather than with
// ppl_memAlloc(), which is not.

#define _EXPTHREADSAFE_C 1

#include <stdlib.h>
#include <string.h>

#include "coreUtils/dict.h"
#include "coreUtils/list.h"
#include "expressions/dollarOp.h"
#include "expressions/expThreadSafe.h"
#include "expressions/traceback_fns.h"
#include "userspace/contextVarDef.h"
#include "userspace/garbageCollector.h"
#include "userspace/pplObj_fns.h"
#include "userspace/pplObjFunc.h"
#include "userspace/stack.h"
#include "pplConstants.h"

// The maximum depth to which calls between user-defined functions are followed when checking them
#define MAX_CHECK_DEPTH 32

// The maximum number of names which may be bound locally (dummy variables, and the arguments of user-defined
// functions being checked)
#define MAX_LOCALS 256

//! threadSafeCheck - The state of a check of whether an expression is safe to evaluate on several threads at once
typedef struct threadSafeCheck {
    ppl_context *c; // The context in which the expression would be evaluated
    const char *locals[MAX_LOCALS]; // Names bound in the worker's own namespace (dummy variables and arguments)
    int nLocals;
    const pplFunc *checking[MAX_CHECK_DEPTH]; // The user-defined functions currently being checked, to stop recursion
    int depth;
} threadSafeCheck;

static int exprThreadSafe(threadSafeCheck *t, const pplExpr *e);
static int objThreadSafe(threadSafeCheck *t, const pplObj *o, int depth);

//! isLocal - Test whether a name is bound in the worker's own namespace
//! \param [in] t - The state of the check
//! \param [in] name - The name
//! \return True if <name> is a dummy variable, or an argument of a user-defined function being checked

static int isLocal(const threadSafeCheck *t, const char *name) {
    int i;
    for (i = 0; i < t->nLocals; i++)
        if (strcmp(t->locals[i], name) == 0) return 1;
    return 0;
}

//! funcThreadSafe - Test whether a function may be called on several threads at once
//! \param [in,out] t - The state of the check
//! \param [in] f - The function
//! \return True if the function is safe to call

static int funcThreadSafe(threadSafeCheck *t, const pplFunc *f) {
    // Whether each kind of function can be called from several threads at once
    switch (f->functionType) {
        case PPL_FUNC_SYSTEM:
        case PPL_FUNC_MAGIC:
            return f->threadSafe; // Set when the function is created; see markThreadUnsafeFuncs() in defaultVars.c
        case PPL_FUNC_HISTOGRAM:
        case PPL_FUNC_FFT:
            return 1; // Evaluating these only reads their tables of data
        case PPL_FUNC_USERDEF: {
            const pplFunc *g;
            int i, nLocalsOld = t->nLocals, safe = 1;

            // A function which calls itself is only checked once: if it is safe, so are the calls it makes to itself
            for (i = 0; i < t->depth; i++)
                if (t->checking[i] == f) return 1;
            if (t->depth >= MAX_CHECK_DEPTH) return 0;
            t->checking[t->depth++] = f;

            // A function may have several definitions, which apply over different ranges of its arguments
            for (g = f; (g != NULL) && safe; g = g->next) {
                int k, j;
                if (g->functionPtr == NULL) continue;
                // The function's arguments are bound in the worker's own namespace while it runs
                for (j = k = 0; (j < g->minArgs) && (g->argList != NULL); j++) {
                    // A function with too many arguments to track is treated as unsafe
                    if (t->nLocals >= MAX_LOCALS) {
                        safe = 0;
                        break;
                    }
                    t->locals[t->nLocals++] = g->argList + k;
                    k += strlen(g->argList + k) + 1;
                }
                if (safe) safe = exprThreadSafe(t, (const pplExpr *) g->functionPtr);
                t->nLocals = nLocalsOld;
            }
            t->depth--;
            return safe;
        }
        default:
            // Subroutines run commands; splines and 2D interpolations share a mutable GSL lookup cache
            return 0;
    }
}

//! objThreadSafe - Test whether an object which an expression may read is safe to share between threads. Functions
//! must be safe to call, and so must any functions held in lists or dictionaries, since an expression could extract
//! and call them.
//! \param [in,out] t - The state of the check
//! \param [in] o - The object
//! \param [in] depth - The depth of nesting of lists and dictionaries, to stop infinite recursion
//! \return True if the object is safe to share

static int objThreadSafe(threadSafeCheck *t, const pplObj *o, const int depth) {
    // Objects nested too deeply to check are treated as unsafe. Containers are safe if everything they contain is.
    if (depth > MAX_CHECK_DEPTH) return 0;
    // Functions are checked as functions; lists, dictionaries and modules are checked item by item; other objects are
    // always safe
    switch (o->objType) {
        case PPLOBJ_FUNC:
            return funcThreadSafe(t, (const pplFunc *) o->auxil);
        case PPLOBJ_LIST: {
            listIterator *li = ppl_listIterateInit((list *) o->auxil);
            const pplObj *item;
            while ((item = (const pplObj *) ppl_listIterate(&li)) != NULL)
                if (!objThreadSafe(t, item, depth + 1)) return 0;
            return 1;
        }
        case PPLOBJ_DICT: {
            dictIterator *di = ppl_dictIterateInit((dict *) o->auxil);
            const pplObj *item;
            char *key;
            while ((item = (const pplObj *) ppl_dictIterate(&di, &key)) != NULL)
                if (!objThreadSafe(t, item, depth + 1)) return 0;
            return 1;
        }
        case PPLOBJ_USER:
        case PPLOBJ_TYPE:
        case PPLOBJ_FILE:
            return 0; // Instances of user-defined classes have methods; files have positions which reading changes
        default:
            return 1; // Numbers, strings, booleans, dates, colours, vectors, matrices and modules are only read
    }
}

//! exprThreadSafe - Test whether an expression may be evaluated on several threads at once
//! \param [in,out] t - The state of the check
//! \param [in] e - The compiled expression
//! \return True if the expression is safe to evaluate in parallel

static int exprThreadSafe(threadSafeCheck *t, const pplExpr *e) {
    const pplObj *module = NULL; // The module produced by the previous instruction, if it looked one up
    int i;
    if (e == NULL) return 0;
    // Check each instruction of the expression's bytecode, looking for anything unsafe: variables which are assigned
    // to, or which hold unsafe functions, and calls to unsafe methods
    for (i = 0; i < e->bcLen; i++) {
        const pplExprInstruction *in = &e->bytecode[i];
        const pplObj *previousModule = module;
        module = NULL;
        // Instructions which look up or assign variables, or call methods, need checking; all others are safe
        switch (in->opcode) {
            case PPL_BC_LOOKUP: {
                char *name = e->strings + in->operand.string;
                pplObj *obj = NULL;
                if (isLocal(t, name)) break; // Bound in the worker's own namespace
                ppl_contextVarLookup(t->c, name, &obj, 0);
                if (obj == NULL) break; // An undefined variable is an error, which is reported in the usual way
                if (obj->objType == PPLOBJ_MOD) module = obj;
                else if (!objThreadSafe(t, obj, 0)) return 0;
                break;
            }
            case PPL_BC_DEREFERENCE: {
                // Only members of modules, such as phy.c or ast.Lcdm_age, may be looked up. Members of other
                // objects include their methods, some of which modify the object.
                const pplObj *member;
                if (previousModule == NULL) return 0;
                member = (const pplObj *) ppl_dictLookup((dict *) previousModule->auxil,
                                                         e->strings + in->operand.string);
                if (member == NULL) break; // An error, which is reported in the usual way
                // Remember a module, so that the next instruction, which looks up one of its members, can be checked
                if (member->objType == PPLOBJ_MOD) {
                    module = member;
                    break;
                }
                // A user-defined function in a module would bind its arguments in the module's own namespace
                if ((member->objType == PPLOBJ_FUNC) &&
                    (((const pplFunc *) member->auxil)->functionType == PPL_FUNC_USERDEF))
                    return 0;
                if (!objThreadSafe(t, member, 0)) return 0;
                break;
            }
            case PPL_BC_LOOKUP_POINTER:
            case PPL_BC_DEREFERENCE_POINTER:
            case PPL_BC_SLICE_POINTER:
            case PPL_BC_ASSIGN:
            case PPL_BC_INCREMENT:
                return 0; // Assignments would modify variables which the threads share
            case PPL_BC_RETURN:
            case PPL_BC_PUSH_NUMBER:
            case PPL_BC_PUSH_STRING:
            case PPL_BC_SLICE:
            case PPL_BC_MAKE_DICT:
            case PPL_BC_MAKE_LIST:
            case PPL_BC_CALL: // Every function which could be called has been checked where it was looked up
            case PPL_BC_OPERATOR:
            case PPL_BC_STRING_SUBST:
            case PPL_BC_DOLLAR:
            case PPL_BC_BRANCH_IF_FALSE:
            case PPL_BC_BRANCH_IF_TRUE:
            case PPL_BC_GOTO:
            case PPL_BC_MAKE_BOOLEAN:
                break;
            default:
                return 0;
        }
    }
    return 1;
}

//! ppl_expThreadSafe - Test whether an expression may be evaluated on several threads at once, in worker contexts
//! created by ppl_contextWorkerNew(), without changing the result
//! \param [in] c - The context in which the expression would otherwise be evaluated; its variables are inspected
//! \param [in] e - The compiled expression
//! \param [in] locals - The names of the dummy variables which each worker binds in its own namespace, e.g. "x"
//! \param [in] nLocals - The number of names in <locals>
//! \return True if the expression is safe to evaluate in parallel

int ppl_expThreadSafe(ppl_context *c, const pplExpr *e, const char *const *locals, const int nLocals) {
    threadSafeCheck t;
    int i;
    if ((nLocals > MAX_LOCALS) || (c->ns_ptr != 1)) return 0; // Only commands at the top level are supported
    t.c = c;
    t.nLocals = nLocals;
    t.depth = 0;
    for (i = 0; i < nLocals; i++) t.locals[i] = locals[i];
    return exprThreadSafe(&t, e);
}

//! ppl_contextWorkerNew - Create a worker context, in which one thread may evaluate expressions while others do the
//! same in their own worker contexts. Expressions must first be checked with ppl_expThreadSafe().
//! \param [in] parent - The context whose settings and variables the worker shares
//! \return The worker context, or NULL if memory ran out. Free with ppl_contextWorkerFree().

ppl_context *ppl_contextWorkerNew(ppl_context *parent) {
    // Create the worker's context
    ppl_context *w = (ppl_context *) malloc(sizeof(ppl_context));
    if (w == NULL) return NULL;

    // Share the parent's settings, unit database and namespaces
    memcpy(w, parent, sizeof(ppl_context));

    // The worker has no cloned functions yet
    w->workerClones = NULL;
    w->workerNClones = 0;

    // Give the worker its own evaluation stack and expression-compiler buffers
    w->stackSize = STACK_DEFAULT;
    w->stack = (pplObj *) malloc(STACK_DEFAULT * sizeof(pplObj));
    w->stackPtr = 0;
    w->stackFull = 0;
    w->tokenBuff = NULL;
    w->tokenBuffLen = 0;
    w->parserStack = NULL;
    w->parserStackLen = 0;
    w->inputLineBuffer = NULL;
    w->inputLineAddBuffer = NULL;

    // Give the worker its own error status
    w->errStat.status = w->errStat.tracebackDepth = 0;
    w->errStat.errPosExpr = w->errStat.oldErrPosExpr = -1;
    w->errStat.errPosCmd = w->errStat.oldErrPosCmd = -1;
    w->algebraErrPos = -1;
    ppl_tbClear(w);
    ppl_dollarOp_deconfig(w);

    // Give the worker a namespace of its own, on top of the parent's, in which to bind dummy variables
    w->ns_ptr = parent->ns_ptr + 1;
    w->ns_branch = parent->ns_ptr;
    w->namespaces[w->ns_ptr] = ppl_dictInit(1);

    // Failure to allocate memory: free what has been allocated, and return NULL
    if ((w->stack == NULL) || (w->namespaces[w->ns_ptr] == NULL)) {
        if (w->namespaces[w->ns_ptr] != NULL) ppl_garbageNamespace(w->namespaces[w->ns_ptr]);
        free(w->stack);
        free(w);
        return NULL;
    }
    return w;
}

static int privatiseExpr(ppl_context *w, threadSafeCheck *t, const pplExpr *e);

//! cloneFunc - Give a worker context a private shallow copy of a function descriptor. Copying a function object would
//! share its descriptor, whose reference count every thread would then update. The copy has a reference count of its
//! own, and is freed by ppl_contextWorkerFree().
//! \param [in,out] w - The worker context
//! \param [in] f - The function descriptor
//! \return The copy, or NULL if memory ran out

static pplFunc *cloneFunc(ppl_context *w, const pplFunc *f) {
    pplFunc *clone = (pplFunc *) malloc(sizeof(pplFunc));
    void **clones = (void **) realloc(w->workerClones, (w->workerNClones + 1) * sizeof(void *));
    if (clones != NULL) w->workerClones = clones;
    // Failure to allocate memory returns NULL
    if ((clone == NULL) || (clones == NULL)) {
        free(clone);
        return NULL;
    }
    memcpy(clone, f, sizeof(pplFunc));
    clone->refCount = 1; // Held by the worker until ppl_contextWorkerFree()
    w->workerClones[w->workerNClones++] = clone;
    return clone;
}

//! privatiseFuncBody - Give a worker context private copies of the shared variables which a user-defined function
//! reads. Other kinds of function read no variables.
//! \param [in,out] w - The worker context
//! \param [in,out] t - The state of the walk, holding the names bound locally
//! \param [in] f - The function
//! \return Zero on success, or nonzero if memory ran out

static int privatiseFuncBody(ppl_context *w, threadSafeCheck *t, const pplFunc *f) {
    const pplFunc *g;
    int status = 0;
    if (f->functionType != PPL_FUNC_USERDEF) return 0;
    if (t->depth >= MAX_CHECK_DEPTH) return 1;
    t->depth++;
    // Privatise the variables used by each definition in the function's chain
    for (g = f; (g != NULL) && (!status); g = g->next) {
        const int nLocalsOld = t->nLocals;
        int j, k;
        if (g->functionPtr == NULL) continue;
        // The function's arguments are bound in the worker's own namespace, so are not copied
        for (j = k = 0; (j < g->minArgs) && (g->argList != NULL) && (t->nLocals < MAX_LOCALS); j++) {
            t->locals[t->nLocals++] = g->argList + k;
            k += strlen(g->argList + k) + 1;
        }
        status = privatiseExpr(w, t, (const pplExpr *) g->functionPtr);
        t->nLocals = nLocalsOld;
    }
    t->depth--;
    return status;
}

//! privatiseExpr - Give a worker context private copies of the shared variables which an expression reads, and
//! which any user-defined functions it calls read. See ppl_contextWorkerPrivatise().
//! \param [in,out] w - The worker context
//! \param [in,out] t - The state of the walk through the expression, holding the names bound locally
//! \param [in] e - The expression
//! \return Zero on success, or nonzero if memory ran out

static int privatiseExpr(ppl_context *w, threadSafeCheck *t, const pplExpr *e) {
    // own is the worker's own namespace, into which private copies of variables are placed
    dict *own = w->namespaces[w->ns_ptr];
    int i;
    // Find each variable which the expression looks up, and copy it into the worker's own namespace
    for (i = 0; i < e->bcLen; i++) {
        const pplExprInstruction *in = &e->bytecode[i];
        pplObj *obj = NULL, v;
        if (in->opcode != PPL_BC_LOOKUP) continue;
        char *name = e->strings + in->operand.string;
        if (isLocal(t, name) || (ppl_dictLookup(own, name) != NULL)) continue;
        ppl_contextVarLookup(t->c, name, &obj, 0); // Look the variable up in the parent context
        if ((obj == NULL) || (obj->objType == PPLOBJ_MOD)) continue;

        // Functions are cloned, so that the worker can bind their arguments without disturbing other threads; other
        // variables are copied
        v.refCount = 1;
        // Clone a function; copy any other object
        if (obj->objType == PPLOBJ_FUNC) {
            const pplFunc *f = (const pplFunc *) obj->auxil;
            pplFunc *clone = cloneFunc(w, f);
            if (clone == NULL) return 1;
            pplObjFunc(&v, 1, 0, clone);
            ppl_dictAppendCpy(own, name, &v, sizeof(pplObj));
            if (privatiseFuncBody(w, t, f)) return 1;
        } else {
            if (pplObjCpy(&v, obj, 0, 1, 1) == NULL) return 1;
            ppl_dictAppendCpy(own, name, &v, sizeof(pplObj));
        }
    }
    return 0;
}

//! ppl_contextWorkerPrivatise - Give a worker context private copies of all of the shared variables (including
//! functions) which a set of expressions read. Otherwise, every time a thread looked a variable up, it would update
//! the variable's reference count, and threads updating the same reference counts slow each other down a lot. The
//! expressions must already have passed ppl_expThreadSafe(), so none of them modify the variables.
//! \param [in,out] w - The worker context
//! \param [in] parent - The context whose variables the worker shares
//! \param [in] exprs - The expressions which the worker will evaluate
//! \param [in] nExprs - The number of expressions in <exprs>
//! \param [in] locals - The names of the dummy variables which the worker binds in its own namespace
//! \param [in] nLocals - The number of names in <locals>
//! \return Zero on success, or nonzero if memory ran out (in which case the worker can still be used)

int ppl_contextWorkerPrivatise(ppl_context *w, ppl_context *parent, pplExpr **exprs, const int nExprs,
                               const char *const *locals, const int nLocals) {
    threadSafeCheck t;
    int i;
    if (nLocals > MAX_LOCALS) return 1;
    t.c = parent;
    t.nLocals = nLocals;
    t.depth = 0;
    for (i = 0; i < nLocals; i++) t.locals[i] = locals[i];
    for (i = 0; i < nExprs; i++)
        if (privatiseExpr(w, &t, exprs[i])) return 1;
    return 0;
}

//! ppl_funcThreadSafe - Test whether a function object may be called on several threads at once, in worker contexts
//! created by ppl_contextWorkerNew(), without changing the result
//! \param [in] c - The context in which the function would otherwise be called; its variables are inspected
//! \param [in] fn - The function object
//! \return True if the function is safe to call in parallel

int ppl_funcThreadSafe(ppl_context *c, const pplObj *fn) {
    threadSafeCheck t;
    if ((c->ns_ptr != 1) || (fn->objType != PPLOBJ_FUNC)) return 0; // Only commands at the top level are supported
    t.c = c;
    t.nLocals = 0;
    t.depth = 0;
    return funcThreadSafe(&t, (const pplFunc *) fn->auxil);
}

//! ppl_contextWorkerPrivatiseFunc - Give a worker context a private copy of a function object which it will call,
//! and private copies of the shared variables the function reads. See ppl_contextWorkerPrivatise().
//! \param [in,out] w - The worker context
//! \param [in] parent - The context whose variables the worker shares
//! \param [in] fn - The function object, which must already have passed ppl_funcThreadSafe()
//! \param [out] out - Set to the worker's private copy of the function object. It belongs to the worker, and must
//! not be passed to the garbage collector; it is freed by ppl_contextWorkerFree().
//! \return Zero on success, or nonzero if memory ran out, in which case <out> is a zombie and <fn> should be called
//! instead

int ppl_contextWorkerPrivatiseFunc(ppl_context *w, ppl_context *parent, const pplObj *fn, pplObj *out) {
    threadSafeCheck t;
    t.c = parent;
    t.nLocals = 0;
    t.depth = 0;
    out->refCount = 1;
    pplFunc *clone = cloneFunc(w, (const pplFunc *) fn->auxil);
    // Failure to allocate memory leaves the output as a zombie
    if (clone == NULL) {
        pplObjZom(out, 0);
        return 1;
    }
    pplObjFunc(out, 0, 0, clone);
    privatiseFuncBody(w, &t, (const pplFunc *) fn->auxil); // If this fails, the function reads the shared variables
    return 0;
}

//! ppl_contextWorkerFree - Free a worker context created by ppl_contextWorkerNew()
//! \param [in] w - The worker context

void ppl_contextWorkerFree(ppl_context *w) {
    pplObj *item;
    char *key;
    int i;
    if (w == NULL) return;
    ppl_stackClean(w, 0); // Discard anything left on the worker's stack

    // The private copies of function descriptors are freed directly. They share their contents (e.g. the bytecode
    // of user-defined functions) with the originals, so must not be passed to the garbage collector.
    dictIterator *di = ppl_dictIterateInit(w->namespaces[w->ns_ptr]);
    // Detach the cloned functions from the worker's namespace before freeing it, since the clones are freed separately
    while ((item = (pplObj *) ppl_dictIterate(&di, &key)) != NULL) {
        if (item->objType != PPLOBJ_FUNC) continue;
        for (i = 0; i < w->workerNClones; i++)
            if (item->auxil == w->workerClones[i]) pplObjZom(item, item->amMalloced);
    }
    for (i = 0; i < w->workerNClones; i++) free(w->workerClones[i]);
    free(w->workerClones);
    ppl_garbageNamespace(w->namespaces[w->ns_ptr]);
    free(w->stack);
    free(w->tokenBuff);
    free(w->parserStack);
    free(w);
}
