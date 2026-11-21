// contextVarDef.c
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

// Looks up variables in Pyxplot's namespaces. Namespace 0 holds the built-in functions and constants, namespace 1 the
// user's global variables, and higher namespaces the local variables of subroutines being executed. It also resolves
// dotted names such as a.b.c, used by commands which set variables, and provides a safe way to overwrite a variable.

#define _CONTEXT_C 1

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "coreUtils/errorReport.h"
#include "stringTools/asciidouble.h"
#include "coreUtils/dict.h"
#include "coreUtils/memAlloc.h"

#include "expressions/traceback_fns.h"

#include "settings/settings_fns.h"

#include "stringTools/strConstants.h"

#include "userspace/context.h"
#include "userspace/contextVarDef.h"
#include "userspace/garbageCollector.h"
#include "userspace/pplObj_fns.h"
#include "userspace/pplObjUnits.h"

#include "defaultObjs/defaultUnits.h"
#include "defaultObjs/defaultVars.h"


//! ppl_contextVarHierLookup - Look up a variable referred to by a hierarchical name, such as a.b.c, where a is a
//! dictionary, module or instance and b and c are members of it. If the final member does not exist it is created,
//! so that it can be assigned to.
//! \param [in] c - The Pyxplot context
//! \param [in] srcLineN - The line number of the command, used to report errors
//! \param [in] srcId - The ID of the source of the command, used to report errors
//! \param [in] srcFname - The filename of the source of the command, used to report errors
//! \param [in] linetxt - The text of the command, used to report errors
//! \param [in] stk - The parser output stack, containing a linked list of the components of the name
//! \param [in] stkPos - Positions in <linetxt> of each item in <stk>, used to report errors
//! \param [out] out - Set to point to the variable
//! \param [in] base - The position in <stk> of the head of the linked list of name components
//! \param [in] offset - The offset from each list item to the name component it contains

void ppl_contextVarHierLookup(ppl_context *c, const int srcLineN, const int srcId, const char *srcFname,
                              const char *linetxt, pplObj *stk, const int *stkPos, pplObj **out, const int base,
                              int offset) {
    int first = 1;
    int pos = base;

    // The parser stores the parts of a dotted name as a list on its output stack, in which each item holds the position
    // of the next. Walk along the list, descending into a dictionary, module or class instance at each step.
    while (stk[pos].objType == PPLOBJ_NUM) {
        pos = (int) round(stk[pos].real);
        if (pos <= 0) break;
        const int last = (stk[pos].objType != PPLOBJ_NUM);
        char *name = (char *) stk[pos + offset].auxil;
        // The first part of a name with several parts must be an existing variable which has members
        if (first && !last) {
            ppl_contextVarLookup(c, name, out, 0);
            // The variable does not exist
            if (*out == NULL) {
                ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "No such variable, '%s'.", name);
                ppl_tbAdd(c, srcLineN, srcId, srcFname, 0, ERR_NAMESPACE, stkPos[pos + offset], linetxt, "");
                return;
            }
            if (((*out)->objType != PPLOBJ_DICT) && ((*out)->objType != PPLOBJ_MOD) &&
                ((*out)->objType != PPLOBJ_USER)) {
                sprintf(c->errStat.errBuff, "Cannot reference members of object of type '%s'.",
                        pplObjTypeNames[(*out)->objType]);
                ppl_tbAdd(c, srcLineN, srcId, srcFname, 0, ERR_TYPE, stkPos[pos + offset], linetxt, "");
                return;
            }
        } else if (first) {
            pplObj tmp;
            ppl_contextGetVarPointer(c, name, out, &tmp);
            ppl_contextRestoreVarPointer(c, name, &tmp);
            // A variable declared global within a subroutine refers to namespace 1
            if ((*out)->objType == PPLOBJ_GLOB) {
                const int ns_ptr = c->ns_ptr;
                c->ns_ptr = 1;
                ppl_contextGetVarPointer(c, name, out, &tmp);
                ppl_contextRestoreVarPointer(c, name, &tmp);
                c->ns_ptr = ns_ptr;
            }
        } else if (!last) {
            dict *d = (dict *) (*out)->auxil;
            // Members of immutable objects, such as built-in modules, cannot be changed
            if (d->immutable) {
                ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "Cannot %s '%s'.",
                                  last ? "modify the immutable variable" : "set variables in the immutable namespace",
                                  name);
                ppl_tbAdd(c, srcLineN, srcId, srcFname, 0, ERR_NAMESPACE, stkPos[pos + offset], linetxt, "");
                return;
            }
            *out = (pplObj *) ppl_dictLookup(d, name);
            if ((*out) == NULL) {
                ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "No such variable, '%s'.", name);
                ppl_tbAdd(c, srcLineN, srcId, srcFname, 0, ERR_NAMESPACE, stkPos[pos + offset], linetxt, "");
                return;
            }
            if (((*out)->objType != PPLOBJ_DICT) && ((*out)->objType != PPLOBJ_MOD) &&
                ((*out)->objType != PPLOBJ_USER)) {
                sprintf(c->errStat.errBuff, "Cannot reference members of object of type '%s'.",
                        pplObjTypeNames[(*out)->objType]);
                ppl_tbAdd(c, srcLineN, srcId, srcFname, 0, ERR_TYPE, stkPos[pos + offset], linetxt, "");
                return;
            }
        } else {
            dict *d = (dict *) (*out)->auxil;
            // Members of immutable objects, such as built-in modules, cannot be changed
            if (d->immutable) {
                ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "Cannot %s '%s'.",
                                  last ? "modify the immutable variable" : "set variables in the immutable namespace",
                                  name);
                ppl_tbAdd(c, srcLineN, srcId, srcFname, 0, ERR_NAMESPACE, stkPos[pos + offset], linetxt, "");
                return;
            }
            *out = (pplObj *) ppl_dictLookup((dict *) (*out)->auxil, name);
            if ((*out) == NULL) {
                pplObj tmp;
                tmp.refCount = 1;
                pplObjNum(&tmp, 1, 1, 0);
                ppl_dictAppendCpy(d, name, &tmp, sizeof(pplObj));
                *out = (pplObj *) ppl_dictLookup(d, name);
            }
        }
        // Immutable variables cannot be changed
        if (((*out) != NULL) && ((*out)->immutable)) {
            ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "Cannot %s '%s'.",
                              last ? "modify the immutable variable" : "set variables in the immutable namespace",
                              name);
            ppl_tbAdd(c, srcLineN, srcId, srcFname, 0, ERR_NAMESPACE, stkPos[pos + offset], linetxt, "");
            return;
        }
        first = 0;
    }
    // The name could not be resolved
    if ((*out) == NULL) {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "No such variable.");
        ppl_tbAdd(c, srcLineN, srcId, srcFname, 0, ERR_NAMESPACE, 0, linetxt, "");
        return;
    }

    // The variable has been found
    return;
}

//! ppl_contextVarLookup - Look up a variable by name, searching the local namespace, then the global namespace, and
//! then the default namespace
//! \param [in] c - The Pyxplot context
//! \param [in] name - The name of the variable
//! \param [out] output - Set to point to the variable, or NULL if it was not found
//! \param [in] returnGlobObjs - If true, return placeholder objects which mark variables as global or deleted;
//! otherwise skip over them

void ppl_contextVarLookup(ppl_context *c, char *name, pplObj **output, const int returnGlobObjs) {
    // Search the current namespace, then the global namespace (1), and then the built-in namespace (0); the namespaces
    // of any subroutines which called the current one are not searched
    for (int i = c->ns_ptr; i >= 0; i = (i > 1) ? 1 : i - 1) {
        pplObj *obj = (pplObj *) ppl_dictLookup(c->namespaces[i], name);
        if (obj == NULL) continue;
        if ((!returnGlobObjs) && ((obj->objType == PPLOBJ_GLOB) || (obj->objType == PPLOBJ_ZOM))) continue;
        *output = obj;
        return;
    }
    *output = NULL;
    return;
}

//! ppl_contextGetVarPointer - Get a pointer to a variable in the current namespace, so that it can be overwritten.
//! If the variable already exists, its value is moved into <temp> and it is temporarily set to a number; otherwise
//! it is created. ppl_contextRestoreVarPointer() must be called afterwards.
//! \param [in] c - The Pyxplot context
//! \param [in] name - The name of the variable
//! \param [out] output - Set to point to the variable
//! \param [out] temp - Holds the previous value of the variable

void ppl_contextGetVarPointer(const ppl_context *c, char *name, pplObj **output, pplObj *temp) {
    dict *d = c->namespaces[c->ns_ptr];
    *output = (pplObj *) ppl_dictLookup(d, name);
    if (*output != NULL) {
        const int om = (*output)->amMalloced;
        *temp = **output;
        pplObjNum(*output, om, 1, 0);
    } else // If variable is not defined, create it now
    {
        temp->refCount = 1;
        pplObjNum(temp, 1, 1, 0);
        ppl_dictAppendCpy(d, name, temp, sizeof(pplObj));
        *output = (pplObj *) ppl_dictLookup(d, name);
        pplObjZom(temp, 1);
    }
}

//! ppl_contextRestoreVarPointer - Store a new value in a variable after a call to ppl_contextGetVarPointer(),
//! freeing its temporary value
//! \param [in] c - The Pyxplot context
//! \param [in] name - The name of the variable
//! \param [in] temp - The value to store in the variable

void ppl_contextRestoreVarPointer(const ppl_context *c, char *name, pplObj *temp) {
    dict *d = c->namespaces[c->ns_ptr];
    pplObj *obj = (pplObj *) ppl_dictLookup(d, name);
    // Free the placeholder value, and move the new value into the variable, keeping the variable's own allocation flag
    // and reference count
    if (obj != NULL) {
        const int om = obj->amMalloced;
        const int tm = temp->amMalloced;
        const int rc = obj->refCount;
        obj->amMalloced = 0;
        ppl_garbageObject(obj);
        memcpy(obj, temp, sizeof(pplObj));
        obj->amMalloced = om;
        obj->refCount = rc;
        pplObjZom(temp, tm);
    } else {
        const int tm = temp->amMalloced;
        temp->amMalloced = 1;
        temp->refCount = 1;
        ppl_dictAppendCpy(d, name, temp, sizeof(pplObj));
        pplObjZom(temp, tm);
    }
}
