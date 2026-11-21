// pplObjMethods.c
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

// The methods of Pyxplot's built-in types, e.g. "abc".len(), [1,2].append(3) and date.toYear(), and the methods common
// to all objects, such as str() and type(). Each method is a system function stored in the dictionary of methods of its
// type, which pplObjMethodsInit() populates when Pyxplot starts; the expression evaluator calls them with the object in
// in[-1].self_this. This file also contains the methods of vectors and matrices, which do linear algebra with GSL.

#define _PPLOBJMETHODS_C 1

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>

#include "gsl/gsl_blas.h"
#include "userspace/stack.h"
#include "gsl/gsl_eigen.h"
#include "gsl/gsl_sort_vector.h"
#include "gsl/gsl_linalg.h"
#include "gsl/gsl_math.h"
#include "gsl/gsl_matrix.h"
#include "gsl/gsl_permutation.h"
#include "gsl/gsl_vector.h"

#include "coreUtils/errorReport.h"
#include "coreUtils/list.h"
#include "coreUtils/dict.h"

#include "defaultObjs/defaultFuncs.h"
#include "defaultObjs/defaultFuncsChecks.h"

#include "expressions/expEval.h"
#include "expressions/traceback_fns.h"
#include "expressions/fnCall.h"

#include "mathsTools/dcfmath.h"

#include "stringTools/asciidouble.h"

#include "userspace/calendars.h"
#include "userspace/context.h"
#include "userspace/garbageCollector.h"
#include "userspace/pplObj_fns.h"
#include "userspace/pplObjCmp.h"
#include "userspace/pplObjDump.h"
#include "userspace/pplObjFunc.h"
#include "userspace/pplObjMethods.h"
#include "userspace/pplObjPrint.h"
#include "userspace/pplObjUnits.h"
#include "userspace/unitsArithmetic.h"
#include "userspace/unitsDisp.h"

// The dictionary of methods of each type, indexed by PPLOBJ_* code
dict **pplObjMethods;


// Universal methods of all objects

//! pplmethod_class - Implement the class() method of all objects, which returns the class prototype of an object
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_class(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObj *t = in[-1].self_this;
    if (t == NULL) pplObjNull(&OUTPUT, 0);
    else pplObjCpy(&OUTPUT, t->objPrototype, 0, 0, 1);
    OUTPUT.self_lval = NULL;
}

//! listMembers - Implement the data(), contents() and methods() methods of all objects, which list the names of some
//! or all of the members of an object. The members of the object itself are listed first, then those of its class,
//! and so on up the chain of prototypes, and finally the built-in methods of the object's type. Members of an
//! instance hide those of its class, so each name is listed only once.
//! \param [in] call - The details of the method call, including its arguments and where to report errors
//! \param [in] functions - 1 to list only functions (methods); 0 to list only other members; -1 to list all members

static void listMembers(pplFuncCall *call, const int functions) {
    pplObj *in = call->in;
    pplObj *iter = in[-1].self_this;
    dict *const builtIn = pplObjMethods[iter->objType];
    char *keys[4096], *key;
    int count = 0;
    // The output is a new list
    if (pplObjList(&OUTPUT, 0, 1, NULL) == NULL) {
        ppl_funcError(call->status, call->errType, call->errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    list *l = (list *) OUTPUT.auxil;
    // Work through the dictionaries of members of each object in the chain, followed by the built-in methods
    for (int last = 0; !last; iter = iter->objPrototype) {
        const int t = iter->objType;
        last = (t != PPLOBJ_MOD) && (t != PPLOBJ_USER);
        dictIterator *di = ppl_dictIterateInit(last ? builtIn : (dict *) iter->auxil);
        const pplObj *item;
        // Consider each member of this dictionary
        while ((item = (pplObj *) ppl_dictIterate(&di, &key)) != NULL) {
            int i;
            // Skip names already listed, and members of the wrong kind
            for (i = 0; (i < count) && (strcmp(key, keys[i]) != 0); i++);
            if (i < count) continue;
            if ((functions >= 0) && ((item->objType == PPLOBJ_FUNC) != functions)) continue;
            // Add the name to the output list
            char *tmp = ppl_funcStrdup(call, key);
            if (tmp == NULL) return;
            pplObj v;
            v.refCount = 1;
            pplObjStr(&v, 1, 1, tmp);
            ppl_listAppendCpy(l, (void *) &v, sizeof(pplObj));
            keys[count++] = key;
            if (count > 4094) break;
        }
        if (last) break;
    }
}

//! pplmethod_data - Implement the data() method of all objects, which returns a list of all the internal variables (not
//! methods) of an object
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_data(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplFuncCall call = {c, in, status, errType, errText, "pplmethod_data()"};
    listMembers(&call, 0);
}

//! pplmethod_contents - Implement the contents() method of all objects, which returns a list of all the methods and
//! internal variables of an object
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_contents(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplFuncCall call = {c, in, status, errType, errText, "pplmethod_contents()"};
    listMembers(&call, -1);
}

//! pplmethod_methods - Implement the methods() method of all objects, which returns a list of the methods of an object
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_methods(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplFuncCall call = {c, in, status, errType, errText, "pplmethod_methods()"};
    listMembers(&call, 1);
}

//! pplmethod_str - Implement the str() method of all objects, which returns a string representation of an object
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_str(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObj *t = in[-1].self_this;
    char *tmp = (char *) malloc(LSTR_LENGTH);
    // Failure to allocate memory is reported as an error
    if (tmp == NULL) {
        *status = 1;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Out of memory.");
        return;
    }
    pplObjPrint(c, t, NULL, tmp, LSTR_LENGTH, 0, 0);
    pplObjStr(&OUTPUT, 0, 1, tmp);
}

//! pplmethod_type - Implement the type() method of all objects, which returns the type of an object
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_type(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObj *t = in[-1].self_this;
    if (t == NULL) pplObjNull(&OUTPUT, 0);
    else pplObjCpy(&OUTPUT, &pplObjPrototypes[t->objType], 0, 0, 1);
    OUTPUT.self_lval = NULL;
}

// String methods

//! pplmethod_strUpper - Implement the upper() method of strings, which converts a string to uppercase
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_strUpper(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplFuncCall call = {c, in, status, errType, errText, "pplmethod_strUpper()"};
    int i = 0;
    pplObj *t = in[-1].self_this;
    char *tmp, *work = (char *) t->auxil;
    if ((tmp = ppl_funcStrdup(&call, work)) == NULL) return;
    // Convert each byte to upper case; only ASCII letters are affected
    while (tmp[i] != '\0') {
        tmp[i] = toupper(tmp[i]);
        i++;
    }
    pplObjStr(&OUTPUT, 0, 1, tmp);
}

//! pplmethod_strLower - Implement the lower() method of strings, which converts a string to lowercase
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_strLower(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplFuncCall call = {c, in, status, errType, errText, "pplmethod_strLower()"};
    int i = 0;
    pplObj *t = in[-1].self_this;
    char *tmp, *work = (char *) t->auxil;
    if ((tmp = ppl_funcStrdup(&call, work)) == NULL) return;
    // Convert each byte to lower case; only ASCII letters are affected
    while (tmp[i] != '\0') {
        tmp[i] = tolower(tmp[i]);
        i++;
    }
    pplObjStr(&OUTPUT, 0, 1, tmp);
}

//! pplmethod_strSplit - Implement the split() method of strings, which returns a list of all the whitespace-separated
//! words in a string
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_strSplit(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    // t is the string on which the method was called
    pplObj *t = in[-1].self_this;
    char *instr = (char *) t->auxil;
    int i, j;
    list *lst;
    // The output is a new list
    if (pplObjList(&OUTPUT, 0, 1, NULL) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    lst = (list *) OUTPUT.auxil;
    for (i = 0, j = 0;; i++)
        // At each whitespace character, or the end of the string, add the word which has just ended to the list, if it
        // is not empty
        if (((unsigned char) instr[i] <= ' ') && (instr[i] >= '\0')) {
            // Copy the word between j and i into a new string, and add it to the list
            if (j < i) {
                pplObj v;
                char *s = (char *) malloc(i - j + 1);
                if (s == NULL) break;
                memcpy(s, instr + j, i - j);
                s[i - j] = '\0';
                pplObjStr(&v, 1, 1, s);
                v.refCount = 1;
                ppl_listAppendCpy(lst, &v, sizeof(v));
            }
            j = i + 1;
            if (instr[i] == '\0') break;
        }
}

//! pplmethod_strSplitOn - Implement the splitOn(x,...) method of strings, which splits a string whenever it encounters
//! any of the substrings supplied as arguments, and returns a list of the split string segments
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_strSplitOn(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                 char *errText) {
    pplObj *t = in[-1].self_this;
    char *instr = (char *) t->auxil;
    int i, j;
    list *lst;
    for (i = 0; i < nArgs; i++)
        // Every separator must be a string
        if (in[i].objType != PPLOBJ_STR) {
            ppl_funcError(status, errType, errText, ERR_TYPE,
                    "The splitOn() method requires all its arguments to be strings; argument %d has type <%s>.", i + 1,
                    pplObjTypeNames[in[i].objType]);
            return;
        }
    // The output is a new list
    if (pplObjList(&OUTPUT, 0, 1, NULL) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    lst = (list *) OUTPUT.auxil;
    // Scan the string; j marks the start of the current piece. At each position, check whether any of the separators
    // starts there.
    for (i = 0, j = 0;; i++) {
        int breaking = (instr[i] == '\0');
        int k, l = 0;
        // Test each separator against the string at position i
        for (k = 0; ((k < nArgs) && (!breaking)); k++) {
            char *cmp = (char *) in[k].auxil;
            // Compare the separator with the string, character by character
            for (l = 0; cmp[l] != '\0'; l++) {
                if (instr[i + l] == '\0') break;
                if (instr[i + l] != cmp[l]) break;
            }
            if (cmp[l] == '\0') breaking = 1;
            else l = 0;
        }
        // Add the piece which has just ended to the list, and skip over the separator
        if (breaking) {
            pplObj v;
            char *s = (char *) malloc(i - j + 1);
            if (s == NULL) break;
            memcpy(s, instr + j, i - j);
            s[i - j] = '\0';
            pplObjStr(&v, 1, 1, s);
            v.refCount = 1;
            ppl_listAppendCpy(lst, &v, sizeof(v));
            j = i + l;
            i = ppl_max(i, j - 1);
        }
        if (instr[i] == '\0') break;
    }
}

//! pplmethod_strStrip - Implement the strip() method of strings, which strips whitespace off the beginning and end of a
//! string
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_strStrip(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplFuncCall call = {c, in, status, errType, errText, "pplmethod_strStrip()"};
    int i, j;
    pplObj *t = in[-1].self_this;
    char *tmp, *work = (char *) t->auxil;
    if ((tmp = ppl_funcStrdup(&call, work)) == NULL) return;
    for (i = 0; ((tmp[i] > '\0') && ((unsigned char) tmp[i] <= ' ')); i++);
    for (j = 0; tmp[i] != '\0'; i++, j++) tmp[j] = tmp[i];
    j--;
    for (; ((j >= 0) && (tmp[j] > '\0') && ((unsigned char) tmp[j] <= ' ')); j--) tmp[j] = '\0';
    tmp[j + 1] = '\0';
    pplObjStr(&OUTPUT, 0, 1, tmp);
}

//! pplmethod_strLStrip - Implement the lstrip() method of strings, which strips whitespace off the beginning of a
//! string
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_strLStrip(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplFuncCall call = {c, in, status, errType, errText, "pplmethod_strLStrip()"};
    int i = 0, j = 0;
    pplObj *t = in[-1].self_this;
    char *tmp, *work = (char *) t->auxil;
    if ((tmp = ppl_funcStrdup(&call, work)) == NULL) return;
    for (i = 0; ((tmp[i] > '\0') && ((unsigned char) tmp[i] <= ' ')); i++);
    for (j = 0; tmp[i] != '\0'; i++, j++) tmp[j] = tmp[i];
    tmp[j] = '\0';
    pplObjStr(&OUTPUT, 0, 1, tmp);
}

//! pplmethod_strRStrip - Implement the rstrip() method of strings, which strips whitespace off the end of a string
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_strRStrip(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplFuncCall call = {c, in, status, errType, errText, "pplmethod_strRStrip()"};
    int i = 0, j = 0;
    pplObj *t = in[-1].self_this;
    char *tmp, *work = (char *) t->auxil;
    if ((tmp = ppl_funcStrdup(&call, work)) == NULL) return;
    for (j = 0; tmp[i] != '\0'; i++, j++) tmp[j] = tmp[i];
    j--;
    for (; ((j >= 0) && (tmp[j] > '\0') && ((unsigned char) tmp[j] <= ' ')); j--) tmp[j] = '\0';
    tmp[j + 1] = '\0';
    pplObjStr(&OUTPUT, 0, 1, tmp);
}

//! pplmethod_strisalpha - Implement the isalpha() method of strings, which returns a boolean indicating whether all of
//! the characters of a string are alphabetic
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_strisalpha(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                 char *errText) {
    pplObj *t = in[-1].self_this;
    char *s = (char *) t->auxil;
    int p = 0, out = 1;
    // Every character must be a letter
    while (s[p] != '\0') {
        // Decode each UTF-8 character in turn, so that letters such as é are recognised
        const long cp = ppl_utf8NextChar(s, &p);
        // Any other character means the answer is false
        if (!ppl_utf8IsAlpha(cp)) {
            out = 0;
            break;
        }
    }
    pplObjBool(&OUTPUT, 0, out);
}

//! pplmethod_strisdigit - Implement the isdigit() method of strings, which returns a boolean indicating whether all of
//! the characters of a string are numeric
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_strisdigit(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                 char *errText) {
    pplObj *t = in[-1].self_this;
    char *s = (char *) t->auxil;
    int p = 0, out = 1;
    // Every character must be a digit
    while (s[p] != '\0') {
        // Decode each UTF-8 character in turn, so that letters such as é are recognised
        const long cp = ppl_utf8NextChar(s, &p);
        // Any other character means the answer is false
        if (!ppl_utf8IsDigit(cp)) {
            out = 0;
            break;
        }
    }
    pplObjBool(&OUTPUT, 0, out);
}

//! pplmethod_strisalnum - Implement the isalnum() method of strings, which returns a boolean indicating whether all of
//! the characters of a string are alphanumeric
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_strisalnum(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                 char *errText) {
    pplObj *t = in[-1].self_this;
    char *s = (char *) t->auxil;
    int p = 0, out = 1;
    // Every character must be a letter or a digit
    while (s[p] != '\0') {
        // Decode each UTF-8 character in turn, so that letters such as é are recognised
        const long cp = ppl_utf8NextChar(s, &p);
        // Any other character means the answer is false
        if (!(ppl_utf8IsAlpha(cp) || ppl_utf8IsDigit(cp))) {
            out = 0;
            break;
        }
    }
    pplObjBool(&OUTPUT, 0, out);
}

//! pplmethod_strAppend - Implement the append(x) method of strings, which appends the string x to the end of a string
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_strAppend(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    // t is the string on which the method was called
    pplObj *t = in[-1].self_this;
    char *instr, *astr;
    int alen;
    // The argument must be a single string
    if ((nArgs != 1) || (in[0].objType != PPLOBJ_STR)) {
        ppl_funcError(status, errType, errText, ERR_TYPE, "The append() method requires a single string argument.");
        return;
    }
    // The string must be a variable, so that it can be extended in place
    if ((t == NULL) || (t->self_lval == NULL)) {
        ppl_funcError(status, errType, errText, ERR_TYPE, "The append() method can only be called on a string lvalue.");
        return;
    }
    // Immutable strings cannot be changed
    if ((t->immutable) || (t->self_lval->immutable)) {
        ppl_funcError(status, errType, errText, ERR_TYPE, "The append() method cannot act on immutable strings.");
        return;
    }
    t = t->self_lval;
    instr = (char *) t->auxil;
    astr = (char *) in[0].auxil;
    alen = strlen(astr);
    // Extend the string's buffer with realloc() if it was allocated with malloc(); otherwise copy both strings into a
    // new buffer from malloc(), leaving the old one to be freed with its memory context
    if (t->auxilMalloced) {
        const int ilen = t->auxilLen - 1;
        int newlen = t->auxilLen + alen;
        instr = (char *) realloc(t->auxil, newlen);
        // Failure to allocate memory is reported as an error
        if (instr == NULL) {
            ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
            return;
        }
        t->auxilLen = newlen;
        strcpy(instr + ilen, astr);
    } else {
        const int ilen = strlen(instr);
        int newlen = ilen + alen + 1;
        char *ns = (char *) malloc(newlen);
        // Failure to allocate memory is reported as an error
        if (ns == NULL) {
            ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
            return;
        }
        t->auxilLen = newlen;
        t->auxilMalloced = 1;
        sprintf(ns, "%s%s", instr, astr);
        instr = ns;
    }
    t->auxil = (void *) instr;
    pplObjCpy(&OUTPUT, t, 0, 0, 1);
}

//! pplmethod_strBeginsWith - Implement the beginsWith(x) method of strings, which returns a boolean indicating whether
//! a string begins with the substring x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_strBeginsWith(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                    char *errText) {
    pplObj *t = in[-1].self_this;
    char *instr = (char *) t->auxil, *cmpstr;
    // The argument must be a single string
    if ((nArgs != 1) || (in[0].objType != PPLOBJ_STR)) {
        ppl_funcError(status, errType, errText, ERR_TYPE, "The beginsWith() method requires a single string argument.");
        return;
    }
    cmpstr = (char *) in[0].auxil;
    pplObjBool(&OUTPUT, 0, strncmp(instr, cmpstr, strlen(cmpstr)) == 0);
}

//! pplmethod_strEndsWith - Implement the endsWith(x) method of strings, which returns a boolean indicating whether a
//! string ends with the substring x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_strEndsWith(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                  char *errText) {
    pplObj *t = in[-1].self_this;
    char *instr = (char *) t->auxil, *cmpstr;
    int il, cl;
    // The argument must be a single string
    if ((nArgs != 1) || (in[0].objType != PPLOBJ_STR)) {
        ppl_funcError(status, errType, errText, ERR_TYPE, "The endsWith() method requires a single string argument.");
        return;
    }
    cmpstr = (char *) in[0].auxil;
    il = strlen(instr);
    cl = strlen(cmpstr);
    instr += il - cl;
    // A string cannot end with a longer string
    if (il < cl) {
        pplObjBool(&OUTPUT, 0, 0);
    } else {
        pplObjBool(&OUTPUT, 0, strncmp(instr, cmpstr, cl) == 0);
    }
}

//! pplmethod_strFind - Implement the find(x) method of strings, which returns the position of the first occurrence of x
//! in a string, or -1 if it is not found
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_strFind(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObj *t = in[-1].self_this;
    char *instr = (char *) t->auxil, *cmpstr;
    int il, cl, pmax, p;
    // The argument must be a single string
    if ((nArgs != 1) || (in[0].objType != PPLOBJ_STR)) {
        ppl_funcError(status, errType, errText, ERR_TYPE, "The find() method requires a single string argument.");
        return;
    }
    cmpstr = (char *) in[0].auxil;
    il = strlen(instr);
    cl = strlen(cmpstr);
    pmax = il - cl;
    for (p = 0; p <= pmax; p++)
        // Return the position of the first match
        if (strncmp(instr + p, cmpstr, cl) == 0) {
            pplObjNum(&OUTPUT, 0, ppl_utf8CharIndex(instr, p), 0); // Position in characters, not bytes
            return;
        }
    pplObjNum(&OUTPUT, 0, -1, 0);
}

//! pplmethod_strFindAll - Implement the findAll(x) method of strings, which returns a list of the positions where the
//! substring x occurs in a string
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_strFindAll(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                 char *errText) {
    pplObj *t = in[-1].self_this;
    char *instr = (char *) t->auxil, *cmpstr;
    int il, cl, pmax, p;
    pplObj v;
    list *l;
    // The argument must be a single string
    if ((nArgs != 1) || (in[0].objType != PPLOBJ_STR)) {
        ppl_funcError(status, errType, errText, ERR_TYPE, "The findAll() method requires a single string argument.");
        return;
    }
    // The output is a new list
    if (pplObjList(&OUTPUT, 0, 1, NULL) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    v.refCount = 1;
    l = (list *) OUTPUT.auxil;
    cmpstr = (char *) in[0].auxil;
    il = strlen(instr);
    cl = strlen(cmpstr);
    pmax = il - cl;
    for (p = 0; p <= pmax; p++)
        // Add the position of each match to the list
        if (strncmp(instr + p, cmpstr, cl) == 0) {
            pplObjNum(&v, 1, ppl_utf8CharIndex(instr, p), 0); // Position in characters, not bytes
            ppl_listAppendCpy(l, &v, sizeof(v));
        }
}

//! pplmethod_strLen - Implement the len() method of strings, which returns the length of a string
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_strLen(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObj *t = in[-1].self_this;
    char *instr = (char *) t->auxil;
    pplObjNum(&OUTPUT, 0, ppl_utf8Length(instr), 0); // Length in characters, not bytes
}

// Date methods


//! methodTimezoneOffset - Work out the offset from UTC of the timezone in which a date method should work, which is
//! given as an optional string argument, or else is the timezone set by the user (set timezone)
//! \param [in] f - The call to the method
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [in] t - The date object
//! \param [out] offset - Set to the offset from UTC, in seconds
//! \return Zero on success, or nonzero if the timezone argument was not a string (which has been reported)

static int methodTimezoneOffset(pplFuncCall *f, const int nArgs, const pplObj *t, double *offset) {
    const pplObj *in = f->in;
    // The optional timezone must be a string
    if ((nArgs > 0) && (in[0].objType != PPLOBJ_STR)) {
        ppl_funcError(f->status, f->errType, f->errText, ERR_TYPE,
                "The function %s requires a string as its first argument; supplied argument had type <%s>.",
                f->description, pplObjTypeNames[in[0].objType]);
        return 1;
    }
    if (nArgs > 0) ppl_calendarTimezoneSet(f->c, 1, (char *) in[0].auxil);
    else ppl_calendarTimezoneSet(f->c, 0, NULL);
    ppl_calendarTimezoneOffset(f->c, t->real, NULL, offset);
    ppl_calendarTimezoneUnset(f->c);
    return 0;
}

//! pplmethod_dateToDayOfMonth - Implement the toDayOfMonth(<timezone>) method of dates, which returns the day of the
//! month of a date object in the current calendar
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_dateToDayOfMonth(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                       char *errText) {
    char *FunctionDescription = "toDayOfMonth(<timezone>)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    int year, month, day, hour, minute;
    double second;
    pplObj *t = in[-1].self_this;
    double offset;
    if (methodTimezoneOffset(&call, nArgs, t, &offset)) return;
    ppl_fromUnixTime(c, t->real + offset, &year, &month, &day, &hour, &minute, &second, status, errText);
    // Report a date which cannot be expressed in the current calendar
    if (*status) {
        *errType = ERR_RANGE;
        return;
    }
    pplObjNum(&OUTPUT, 0, day, 0);
}

//! pplmethod_dateToDayWeekName - Implement the toDayWeekName(<timezone>) method of dates, which returns the name of the
//! day of the week of a date object
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_dateToDayWeekName(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                        char *errText) {
    char *FunctionDescription = "toDayWeekName(<timezone>)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    pplObj *t = in[-1].self_this;
    char *tmp;
    double offset;
    if (methodTimezoneOffset(&call, nArgs, t, &offset)) return;
    if ((tmp = ppl_funcStrdup(&call, ppl_getWeekDayName(c, floor(fmod((t->real + offset) / 3600 / 24 + 3, 7))))) ==
        NULL)
        return;
    pplObjStr(&OUTPUT, 0, 1, tmp);
}

//! pplmethod_dateToDayWeekNum - Implement the toDayWeekNum(<timezone>) method of dates, which returns the day of the
//! week (1-7) of a date object
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_dateToDayWeekNum(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                       char *errText) {
    char *FunctionDescription = "toDayWeekNum(<timezone>)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    pplObj *t = in[-1].self_this;
    double offset;
    if (methodTimezoneOffset(&call, nArgs, t, &offset)) return;
    {
        // Days since Thursday 1 January 1970, plus four, modulo seven, counts from Sunday. The modulo must not be negative
        // for dates before 1970.
        const double days = floor((t->real + offset) / 86400) + 4;
        pplObjNum(&OUTPUT, 0, days - 7 * floor(days / 7) + 1, 0);
    }
}

//! pplmethod_dateToHour - Implement the toHour(<timezone>) method of dates, which returns the integer hour component
//! (0-23) of a date object
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_dateToHour(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                 char *errText) {
    char *FunctionDescription = "toHour(<timezone>)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    int year, month, day, hour, minute;
    double second;
    pplObj *t = in[-1].self_this;
    double offset;
    if (methodTimezoneOffset(&call, nArgs, t, &offset)) return;
    ppl_fromUnixTime(c, t->real + offset, &year, &month, &day, &hour, &minute, &second, status, errText);
    // Report a date which cannot be expressed in the current calendar
    if (*status) {
        *errType = ERR_RANGE;
        return;
    }
    pplObjNum(&OUTPUT, 0, hour, 0);
}

//! pplmethod_dateToJD - Implement the toJD() method of dates, which converts a date object to a numerical Julian date
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_dateToJD(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObj *t = in[-1].self_this;
    pplObjNum(&OUTPUT, 0, t->real / 86400.0 + 2440587.5, 0);
}

//! pplmethod_dateToMinute - Implement the toMinute(<timezone>) method of dates, which returns the integer minute
//! component (0-59) of a date object
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_dateToMinute(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                   char *errText) {
    char *FunctionDescription = "toMinute(<timezone>)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    int year, month, day, hour, minute;
    double second;
    pplObj *t = in[-1].self_this;
    double offset;
    if (methodTimezoneOffset(&call, nArgs, t, &offset)) return;
    ppl_fromUnixTime(c, t->real + offset, &year, &month, &day, &hour, &minute, &second, status, errText);
    // Report a date which cannot be expressed in the current calendar
    if (*status) {
        *errType = ERR_RANGE;
        return;
    }
    pplObjNum(&OUTPUT, 0, minute, 0);
}

//! pplmethod_dateToMJD - Implement the toMJD() method of dates, which converts a date object to a modified Julian date
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_dateToMJD(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObj *t = in[-1].self_this;
    pplObjNum(&OUTPUT, 0, t->real / 86400.0 + 40587.0, 0);
}

//! pplmethod_dateToMonthName - Implement the toMonthName(<timezone>) method of dates, which returns the name of the
//! month in which a date object falls
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_dateToMonthName(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                      char *errText) {
    char *FunctionDescription = "toMonthName(<timezone>)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    int year, month, day, hour, minute;
    double second;
    char *tmp;
    pplObj *t = in[-1].self_this;
    double offset;
    if (methodTimezoneOffset(&call, nArgs, t, &offset)) return;
    ppl_fromUnixTime(c, t->real + offset, &year, &month, &day, &hour, &minute, &second, status, errText);
    // Report a date which cannot be expressed in the current calendar
    if (*status) {
        *errType = ERR_RANGE;
        return;
    }
    if ((tmp = ppl_funcStrdup(&call, ppl_getMonthName(c, month))) == NULL) return;
    pplObjStr(&OUTPUT, 0, 1, tmp);
}

//! pplmethod_dateToMonthNum - Implement the toMonthNum(<timezone>) method of dates, which returns the number (1-12) of
//! the month in which a date object falls
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_dateToMonthNum(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                     char *errText) {
    char *FunctionDescription = "toMonthNum(<timezone>)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    int year, month, day, hour, minute;
    double second;
    pplObj *t = in[-1].self_this;
    double offset;
    if (methodTimezoneOffset(&call, nArgs, t, &offset)) return;
    ppl_fromUnixTime(c, t->real + offset, &year, &month, &day, &hour, &minute, &second, status, errText);
    // Report a date which cannot be expressed in the current calendar
    if (*status) {
        *errType = ERR_RANGE;
        return;
    }
    pplObjNum(&OUTPUT, 0, month, 0);
}

//! pplmethod_dateToSecond - Implement the toSecond(<timezone>) method of dates, which returns the seconds component
//! (0.0-60.0) of a date object
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_dateToSecond(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                   char *errText) {
    char *FunctionDescription = "toSecond(<timezone>)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    int year, month, day, hour, minute;
    double second;
    pplObj *t = in[-1].self_this;
    double offset;
    if (methodTimezoneOffset(&call, nArgs, t, &offset)) return;
    ppl_fromUnixTime(c, t->real + offset, &year, &month, &day, &hour, &minute, &second, status, errText);
    // Report a date which cannot be expressed in the current calendar
    if (*status) {
        *errType = ERR_RANGE;
        return;
    }
    pplObjNum(&OUTPUT, 0, second, 0);
}

//! pplmethod_dateToStr - Implement the str(<s>,<timezone>) method of dates, which converts a date object to a string
//! with an optional format string
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_dateToStr(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    // t is the date on which the method was called
    char *FunctionDescription = "str(<s>,<timezone>)";
    pplObj *t = in[-1].self_this;
    char *format = NULL, *out = NULL, timezone[FNAME_LENGTH];
    double offset;
    // The optional format must be a string
    if ((nArgs > 0) && (in[0].objType != PPLOBJ_STR)) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The function %s requires a string as its first argument; supplied argument had type <%s>.",
                FunctionDescription, pplObjTypeNames[in[0].objType]);
        return;
    }
    // The optional timezone must be a string
    if ((nArgs > 1) && (in[1].objType != PPLOBJ_STR)) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The function %s requires a string as its second argument; supplied argument had type <%s>.",
                FunctionDescription, pplObjTypeNames[in[1].objType]);
        return;
    }
    out = (char *) malloc(8192);
    // Failure to allocate memory is reported as an error
    if (out == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "%s", "Out of memory.");
        return;
    }

    if (nArgs > 0) format = (char *) in[0].auxil; // Format specified
    else format = NULL; // Format not specified
    if (nArgs > 1) ppl_calendarTimezoneSet(c, 1, (char *) in[1].auxil);
    else ppl_calendarTimezoneSet(c, 0, NULL);
    ppl_calendarTimezoneOffset(c, t->real, timezone, &offset);
    ppl_dateString(c, out, 8192, t->real + offset, format, timezone, status, errText);
    ppl_calendarTimezoneUnset(c);
    // Report a date which cannot be expressed in the current calendar, or a bad format string
    if (*status) {
        *errType = ERR_NUMERICAL;
        return;
    }
    pplObjStr(&OUTPUT, 0, 1, out);
}

//! pplmethod_dateToUnix - Implement the toUnix() method of dates, which converts a date object to a Unix time
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_dateToUnix(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                 char *errText) {
    pplObj *t = in[-1].self_this;
    pplObjNum(&OUTPUT, 0, t->real, 0);
}

//! pplmethod_dateToYear - Implement the toYear(<timezone>) method of dates, which returns the year in which a date
//! object falls in the current calendar
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_dateToYear(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                 char *errText) {
    char *FunctionDescription = "toYear(<timezone>)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    int year, month, day, hour, minute;
    double second;
    pplObj *t = in[-1].self_this;
    double offset;
    if (methodTimezoneOffset(&call, nArgs, t, &offset)) return;
    ppl_fromUnixTime(c, t->real + offset, &year, &month, &day, &hour, &minute, &second, status, errText);
    // Report a date which cannot be expressed in the current calendar
    if (*status) {
        *errType = ERR_RANGE;
        return;
    }
    pplObjNum(&OUTPUT, 0, year, 0);
}

// Color methods

//! pplmethod_colCompRGB - Implement the componentsRGB() method of colors, which returns a vector RGB representation of
//! a color
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_colCompRGB(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                 char *errText) {
    pplObj *a = in[-1].self_this;
    double r, g, b;
    gsl_vector *v;
    // A color's color space is stored in exponent[0], and its components in exponent[8] to exponent[11]; convert them
    // to RGB
    pplcol_toRGB(a, &r, &g, &b);
    pplObjVector(&OUTPUT, 0, 1, 3);
    v = ((pplVector *) OUTPUT.auxil)->v;
    gsl_vector_set(v, 0, r);
    gsl_vector_set(v, 1, g);
    gsl_vector_set(v, 2, b);
}

//! pplmethod_colCompCMYK - Implement the componentsCMYK() method of colors, which returns a vector CMYK representation
//! of a color
//! \param [in] dummy - The Pyxplot context (unused)
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_colCompCMYK(ppl_context *dummy, pplObj *in, const int nArgs, int *status, int *errType,
                                  char *errText) {
    pplObj *a = in[-1].self_this;
    double c, m, y, k;
    gsl_vector *v;
    // Convert the components to CMYK, if they are not already
    pplcol_toCMYK(a, &c, &m, &y, &k);
    pplObjVector(&OUTPUT, 0, 1, 4);
    v = ((pplVector *) OUTPUT.auxil)->v;
    gsl_vector_set(v, 0, c);
    gsl_vector_set(v, 1, m);
    gsl_vector_set(v, 2, y);
    gsl_vector_set(v, 3, k);
}

//! pplmethod_colCompHSB - Implement the componentsHSB() method of colors, which returns a vector HSB representation of
//! a color
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_colCompHSB(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                 char *errText) {
    pplObj *a = in[-1].self_this;
    double h, s, b;
    gsl_vector *v;
    // Convert the components to HSB, if they are not already
    pplcol_toHSB(a, &h, &s, &b);
    pplObjVector(&OUTPUT, 0, 1, 3);
    v = ((pplVector *) OUTPUT.auxil)->v;
    gsl_vector_set(v, 0, h);
    gsl_vector_set(v, 1, s);
    gsl_vector_set(v, 2, b);
}

//! pplmethod_colToRGB - Implement the toRGB() method of colors, which returns an RGB representation of a color
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_colToRGB(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObj *a = in[-1].self_this;
    double r, g, b;
    // Convert the components to RGB, if they are not already
    pplcol_toRGB(a, &r, &g, &b);
    pplObjColor(&OUTPUT, 0, SW_COLSPACE_RGB, r, g, b, 0);
}

//! pplmethod_colToCMYK - Implement the toCMYK() method of colors, which returns a CMYK representation of a color
//! \param [in] dummy - The Pyxplot context (unused)
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_colToCMYK(ppl_context *dummy, pplObj *in, const int nArgs, int *status, int *errType,
                                char *errText) {
    pplObj *a = in[-1].self_this;
    double c, m, y, k;
    // Convert the components to CMYK, if they are not already
    pplcol_toCMYK(a, &c, &m, &y, &k);
    pplObjColor(&OUTPUT, 0, SW_COLSPACE_CMYK, c, m, y, k);
}

//! pplmethod_colToHSB - Implement the toHSB() method of colors, which returns an HSB representation of a color
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_colToHSB(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObj *a = in[-1].self_this;
    double h, s, b;
    // Convert the components to HSB, if they are not already
    pplcol_toHSB(a, &h, &s, &b);
    pplObjColor(&OUTPUT, 0, SW_COLSPACE_HSB, h, s, b, 0);
}

//! vectorUnitError - Report an attempt to add a number to a vector with conflicting physical dimensions
//! \param [in] c - The Pyxplot context
//! \param [in] vec - The vector
//! \param [in] num - The number
//! \param [out] status - Set to one, to indicate that an error occurred
//! \param [out] errType - Set to the type of error
//! \param [out] errText - Buffer of LSTR_LENGTH bytes into which the error message is written

static void vectorUnitError(ppl_context *c, const pplObj *vec, const pplObj *num, int *status, int *errType,
                            char *errText) {
    if (vec->dimensionless)
        ppl_funcError(status, errType, errText, ERR_UNIT,
                      "Attempt to append a number to a vector with conflicting dimensions: vector is dimensionless, but number has units of <%s>.",
                      ppl_printUnit(c, num, NULL, NULL, 1, 1, 0));
    else if (num->dimensionless)
        ppl_funcError(status, errType, errText, ERR_UNIT,
                      "Attempt to append a number to a vector with conflicting dimensions: vector has units of <%s>, while number is dimensionless.",
                      ppl_printUnit(c, vec, NULL, NULL, 0, 1, 0));
    else
        ppl_funcError(status, errType, errText, ERR_UNIT,
                      "Attempt to append a number to a vector with conflicting dimensions: vector has units of <%s>, while number has units of <%s>.",
                      ppl_printUnit(c, vec, NULL, NULL, 0, 1, 0), ppl_printUnit(c, num, NULL, NULL, 1, 1, 0));
}

//! updateInPlace - If the object a method was called on is a variable, replace its value with the method's result,
//! so that the method changes it in place
//! \param [in] st - The object the method was called on
//! \param [in] out - The result of the method

static void updateInPlace(const pplObj *st, const pplObj *out) {
    if (st->self_lval != NULL) ppl_objReplace(st->self_lval, out);
}

// Vector methods

//! pplmethod_vectorAppend - Implement the append(x) method of vectors, which appends the object x to a vector
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_vectorAppend(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                   char *errText) {
    int i;
    pplObj *st = in[-1].self_this;
    gsl_vector *v = ((pplVector *) st->auxil)->v;
    gsl_vector *vo;
    // The new element must have the same units as the vector
    if (!ppl_unitsDimEqual(st, &in[0])) {
        // Describe the conflicting units in the error message
        vectorUnitError(c, st, &in[0], status, errType, errText);
        return;
    }
    // Make a new vector, one element longer
    if (pplObjVector(&OUTPUT, 0, 1, v->size + 1) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    vo = ((pplVector *) (OUTPUT.auxil))->v;
    for (i = 0; i < v->size; i++) gsl_vector_set(vo, i, gsl_vector_get(v, i));
    gsl_vector_set(vo, i, in[0].real);
    ppl_unitsDimCpy(&OUTPUT, st);
    // If the vector is a variable, replace its value with the new vector, so that the method changes it in place
    updateInPlace(st, &OUTPUT);
}

//! pplmethod_vectorExtend - Implement the extend(x) method of vectors, which appends the members of the list x to the
//! end of a vector
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_vectorExtend(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                   char *errText) {
    int i, l = 0;
    pplObj *st = in[-1].self_this;
    gsl_vector *va = ((pplVector *) st->auxil)->v;
    gsl_vector *vo;
    int t = in[0].objType;

    // The vector may be extended by another vector, which must have the same units, or by a list of numbers
    if (t == PPLOBJ_VEC) {
        // The vector must have the same units as this one
        if (!ppl_unitsDimEqual(st, &in[0])) {
            // Describe the conflicting units in the error message
            vectorUnitError(c, st, &in[0], status, errType, errText);
            return;
        }
        l = ((pplVector *) in[0].auxil)->v->size;
    } else if (t == PPLOBJ_LIST) {
        l = ((list *) in[0].auxil)->length;
    } else {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "Argument to the extend(x) method must be either a list or a vector. Supplied argument had type <%s>.",
                pplObjTypeNames[t]);
        return;
    }

    // Make a new vector, long enough for both
    if (pplObjVector(&OUTPUT, 0, 1, va->size + l) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    vo = ((pplVector *) (OUTPUT.auxil))->v;
    for (i = 0; i < va->size; i++) gsl_vector_set(vo, i, gsl_vector_get(va, i));
    ppl_unitsDimCpy(&OUTPUT, st);

    // Copy in the elements of the vector or list
    if (t == PPLOBJ_VEC) {
        int j;
        gsl_vector *vi = ((pplVector *) in[0].auxil)->v;
        for (j = 0; j < vi->size; j++, i++) gsl_vector_set(vo, i, gsl_vector_get(vi, j));
    } else if (t == PPLOBJ_LIST) {
        list *listin = (list *) in[0].auxil;
        listIterator *li = ppl_listIterateInit(listin);
        pplObj *item;
        // Each item in the list must be a real number with the same units as the vector
        while ((item = (pplObj *) ppl_listIterate(&li)) != NULL) {
            // Report an item which is not a number
            if (item->objType != PPLOBJ_NUM) {
                ppl_funcError(status, errType, errText, ERR_TYPE,
                        "Can only append numbers to vectors; supplied object has type <%s>.",
                        pplObjTypeNames[item->objType]);
                return;
            }
            // Check the item's units
            if (!ppl_unitsDimEqual(st, item)) {
                // Describe the conflicting units in the error message
                vectorUnitError(c, st, item, status, errType, errText);
                return;
            }
            // Vectors cannot hold complex numbers
            if (item->flagComplex) {
                ppl_funcError(status, errType, errText, ERR_TYPE,
                        "Can only append real numbers to vectors; supplied value is complex.");
                return;
            }
            gsl_vector_set(vo, i++, item->real);
        }
    }
    // If the vector is a variable, replace its value with the new vector, so that the method changes it in place
    updateInPlace(st, &OUTPUT);
}

//! pplmethod_vectorFilter - Implement the filter(f) method of vectors, which takes a pointer to a function of one
//! argument, f(a). It calls the function for every element of the vector, and returns a new vector of those elements
//! for which f(a) tests true
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_vectorFilter(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                   char *errText) {
    char *FunctionDescription = "filter(f)";
    pplObj *st = in[-1].self_this;
    gsl_vector *va = ((pplVector *) st->auxil)->v;
    gsl_vector *vo;
    double *ovec;
    int ovlen = 0, i = 0;
    pplObj v;
    pplFunc *fi;
    pplExpr dummy;

    // Calling the function needs room on the stack
    ppl_stackMustHave(c, 4);
    // Report a full stack
    if (c->stackFull) {
        ppl_funcError(status, errType, errText, ERR_TYPE, "%s", "Stack overflow.");
        return;
    }

    // The argument must be a function
    if (in[0].objType != PPLOBJ_FUNC) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The %s method requires a function object as its first argument. Supplied object is of type <%s>.",
                FunctionDescription, pplObjTypeNames[in[0].objType]);
        return;
    }
    fi = (pplFunc *) in[0].auxil;
    // Magic functions, such as diff_d, cannot be called in this way
    if ((fi == NULL) || (fi->functionType == PPL_FUNC_MAGIC)) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The %s method requires a function object as its first argument. Integration and differentiation operators are not suitable functions.",
                FunctionDescription);
        return;
    }

    // A buffer for the elements which pass the filter
    ovec = (double *) malloc(va->size * sizeof(double));
    // Failure to allocate memory is reported as an error
    if (ovec == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "%s", "Out of memory.");
        return;
    }

    // Dummy expression object with dummy line number information
    dummy.srcLineN = 0;
    dummy.srcId = 0;
    dummy.srcFname = "<dummy>";
    dummy.ascii = "<filter(f) function>";

    // v holds each element in turn, with the vector's units, to pass to the function
    pplObjNum(&v, 0, 0, 0);
    v.refCount = 1;
    ppl_unitsDimCpy(&v, st);

    // Call the function on each element, and keep the element if the result is true
    for (i = 0; i < va->size; i++) {
        const int stkLevelOld = c->stackPtr;

        // Push the function to call, followed by its arguments
        ppl_stackPushCopy(c, &in[0], 0);

        // The argument: an item of the list
        v.real = gsl_vector_get(va, i);
        ppl_stackPushCopy(c, &v, 0); // The argument: an element of the vector

        // Call function
        c->errStat.errMsgExpr[0] = '\0';
        ppl_fnCall(c, &dummy, 0, 1, 1, 1);

        // Propagate error if function failed
        if (c->errStat.status) {
            ppl_funcError(status, errType, errText, ERR_TYPE,
                    "Error inside function supplied to the %s function: %s",
                    FunctionDescription, c->errStat.errMsgExpr);
            free(ovec);
            ovec = NULL;
            return;
        }

        // Keep the element if the function returned a true value
        ppl_castToBool(c, &c->stack[c->stackPtr - 1]);
        if (c->stack[c->stackPtr - 1].real) ovec[ovlen++] = v.real;
        ppl_stackClean(c, stkLevelOld); // Discard the condition's value, now that it has been tested
    }

    // Make a vector of the elements which passed
    if (pplObjVector(&OUTPUT, 0, 1, ovlen) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        free(ovec);
        ovec = NULL;
        return;
    }
    vo = ((pplVector *) (OUTPUT.auxil))->v;
    for (i = 0; i < ovlen; i++) gsl_vector_set(vo, i, ovec[i]);
    ppl_unitsDimCpy(&OUTPUT, st);
    return;
}

//! pplmethod_vectorInsert - Implement the insert(n,x) method of vectors, which inserts the object x into a vector at
//! position n
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_vectorInsert(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                   char *errText) {
    long p, i;
    pplObj *st = in[-1].self_this;
    gsl_vector *v = ((pplVector *) st->auxil)->v;
    gsl_vector *vo;
    // The position must be a real number, within or at the end of the vector
    if (in[0].objType != PPLOBJ_NUM) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "First argument to the insert(n,x) method must be a number. Supplied argument had type <%s>.",
                pplObjTypeNames[in[0].objType]);
        return;
    }
    // The position must be real
    if (in[0].flagComplex) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "First argument to the insert(n,x) method must be a real number. Supplied argument is complex.");
        return;
    }
    if (in[0].real < 0) in[0].real += v->size + 1;
    p = (long) round(in[0].real);
    // The position cannot be before the start of the vector
    if (p < 0) {
        ppl_funcError(status, errType, errText, ERR_RANGE,
                "Attempt to insert a vector item before the beginning of a vector.");
        return;
    }
    // The position cannot be beyond the end of the vector
    if (p > v->size) {
        ppl_funcError(status, errType, errText, ERR_RANGE, "Vector index out of range.");
        return;
    }
    // The new element must have the same units as the vector
    if (!ppl_unitsDimEqual(st, &in[1])) {
        // Describe the conflicting units in the error message
        vectorUnitError(c, st, &in[1], status, errType, errText);
        return;
    }
    // Make a new vector, one element longer
    if (pplObjVector(&OUTPUT, 0, 1, v->size + 1) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    vo = ((pplVector *) (OUTPUT.auxil))->v;
    gsl_vector_set(vo, p, in[1].real);
    for (i = 0; i < v->size; i++) gsl_vector_set(vo, i + (i >= p), gsl_vector_get(v, i));
    ppl_unitsDimCpy(&OUTPUT, st);
    // If the vector is a variable, replace its value with the new vector, so that the method changes it in place
    updateInPlace(st, &OUTPUT);
}

//! pplmethod_vectorLen - Implement the len() method of vectors, which returns the number of dimensions of a vector
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_vectorLen(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    gsl_vector *v = ((pplVector *) in[-1].self_this->auxil)->v;
    pplObjNum(&OUTPUT, 0, v->size, 0);
}

//! pplmethod_vectorList - Implement the list() method of vectors, which returns a list representation of a vector
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_vectorList(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                 char *errText) {
    pplObj *st = in[-1].self_this;
    gsl_vector *vec = ((pplVector *) st->auxil)->v;
    int i;
    const int l = vec->size;
    pplObj v;
    list *lo;
    // The output is a new list
    if (pplObjList(&OUTPUT, 0, 1, NULL) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "%s", "Out of memory.");
        return;
    }
    lo = (list *) OUTPUT.auxil;
    v.refCount = 1;
    pplObjNum(&v, 1, 0, 0);
    ppl_unitsDimCpy(&v, st);
    // Add each element to the list, as a number with the vector's units
    for (i = 0; i < l; i++) {
        v.real = gsl_vector_get(vec, i);
        ppl_listAppendCpy(lo, &v, sizeof(pplObj));
    }
}

//! pplmethod_vectorMap - Implement the map(f) method of vectors, which takes a pointer to a function of one argument,
//! f(a). It calls the function for every element of the vector, and returns a vector of the results
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_vectorMap(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    // st is the vector on which the method was called
    char *FunctionDescription = "map(f)";
    pplObj *st = in[-1].self_this;
    gsl_vector *vec = ((pplVector *) st->auxil)->v;
    int i;
    gsl_vector *ovec;
    pplObj v, val2;
    pplFunc *fi;
    pplExpr dummy;

    // Calling the function needs room on the stack
    ppl_stackMustHave(c, 4);
    // Report a full stack
    if (c->stackFull) {
        ppl_funcError(status, errType, errText, ERR_TYPE, "%s", "Stack overflow.");
        return;
    }

    // The argument must be a function
    if (in[0].objType != PPLOBJ_FUNC) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The %s method requires a function object as its first argument. Supplied object is of type <%s>.",
                FunctionDescription, pplObjTypeNames[in[0].objType]);
        return;
    }
    fi = (pplFunc *) in[0].auxil;
    // Magic functions, such as diff_d, cannot be called in this way
    if ((fi == NULL) || (fi->functionType == PPL_FUNC_MAGIC)) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The %s method requires a function object as its first argument. Integration and differentiation operators are not suitable functions.",
                FunctionDescription);
        return;
    }
    // The output is a vector of the same length
    if (pplObjVector(&OUTPUT, 0, 1, vec->size) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    ovec = ((pplVector *) OUTPUT.auxil)->v;

    // Dummy expression object with dummy line number information
    dummy.srcLineN = 0;
    dummy.srcId = 0;
    dummy.srcFname = "<dummy>";
    dummy.ascii = "<filter(f) function>";

    // v holds each element in turn, with the vector's units, to pass to the function
    pplObjNum(&v, 0, 0, 0);
    v.refCount = 1;
    ppl_unitsDimCpy(&v, st);

    // Call the function on each element
    for (i = 0; i < vec->size; i++) {
        const int stkLevelOld = c->stackPtr;

        // Push the function to call, followed by its arguments
        ppl_stackPushCopy(c, &in[0], 0);

        // The argument: an item of the list
        v.real = gsl_vector_get(vec, i);
        ppl_stackPushCopy(c, &v, 0); // The argument: an element of the vector

        // Call function
        c->errStat.errMsgExpr[0] = '\0';
        ppl_fnCall(c, &dummy, 0, 1, 1, 1);

        // Propagate error if function failed
        if (c->errStat.status) {
            ppl_funcError(status, errType, errText, ERR_TYPE,
                    "Error inside function supplied to the %s function: %s",
                    FunctionDescription, c->errStat.errMsgExpr);
            return;
        }

        // The function must return a number
        if (c->stack[c->stackPtr - 1].objType != PPLOBJ_NUM) {
            ppl_funcError(status, errType, errText, ERR_TYPE,
                    "Error inside function supplied to the %s function: function should have returned number, but returned object of type <%s> for element %d.",
                    FunctionDescription, pplObjTypeNames[c->stack[c->stackPtr - 1].objType], i);
            return;
        }

        // Every result must have the same units as the first, which become the units of the output vector
        if (i == 0) memcpy(&val2, &c->stack[c->stackPtr - 1], sizeof(pplObj));
        // Report a result with different units
        else if (!ppl_unitsDimEqual(&val2, &c->stack[c->stackPtr - 1])) {
            ppl_funcError(status, errType, errText, ERR_UNIT,
                    "Error inside function supplied to the %s function: function returned values with inconsistent units of <%s> and <%s>. All of the elements of a vector must have matching dimensions.",
                    FunctionDescription, ppl_printUnit(c, &val2, NULL, NULL, 0, 1, 0),
                    ppl_printUnit(c, &c->stack[c->stackPtr - 1], NULL, NULL, 1, 1, 0));
            return;
        }

        // Vectors cannot hold complex numbers
        if (c->stack[c->stackPtr - 1].flagComplex) {
            ppl_funcError(status, errType, errText, ERR_NUMERICAL,
                    "Error inside function supplied to the %s function: function returned a complex number for element %d; vectors can only hold real numbers.",
                    FunctionDescription, i);
            return;
        }

        // Store the result, and discard it from the stack
        gsl_vector_set(ovec, i, c->stack[c->stackPtr - 1].real);
        ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack before giving up
    }

    // Done
    return;
}

//! pplmethod_vectorNorm - Implement the norm() method of vectors, which returns the norm (quadrature sum) of a vector's
//! elements
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_vectorNorm(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                 char *errText) {
    pplObj *st = in[-1].self_this;
    gsl_vector *v = ((pplVector *) st->auxil)->v;
    pplObjNum(&OUTPUT, 0, gsl_blas_dnrm2(v), 0);
    ppl_unitsDimCpy(&OUTPUT, st);
}

//! pplmethod_vectorReduce - Implement the reduce(f) method of vectors, which takes a pointer to a function of two
//! arguments. It first calls f(a,b) on the first two elements of the vector, and then continues through the vector
//! calling f(a,b) on the result and the next item in the vector. The final result is returned
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_vectorReduce(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                   char *errText) {
    char *FunctionDescription = "reduce(f)";
    const int stkLevelOld = c->stackPtr;
    int i;
    pplExpr dummy;
    pplObj *st = in[-1].self_this;
    gsl_vector *v = ((pplVector *) st->auxil)->v;
    pplObj val, val2;
    pplFunc *fi;

    // Calling the function needs room on the stack
    ppl_stackMustHave(c, 4);
    // Report a full stack
    if (c->stackFull) {
        ppl_funcError(status, errType, errText, ERR_TYPE, "%s", "Stack overflow.");
        return;
    }

    // The argument must be a function
    if (in[0].objType != PPLOBJ_FUNC) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The %s method requires a function object as its first argument. Supplied object is of type <%s>.",
                FunctionDescription, pplObjTypeNames[in[0].objType]);
        return;
    }
    fi = (pplFunc *) in[0].auxil;
    // Magic functions, such as diff_d, cannot be called in this way
    if ((fi == NULL) || (fi->functionType == PPL_FUNC_MAGIC)) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The %s method requires a function object as its first argument. Integration and differentiation operators are not suitable functions.",
                FunctionDescription);
        return;
    }
    // Reducing needs at least two elements
    if (v->size < 2) {
        ppl_funcError(status, errType, errText, ERR_RANGE,
                "The %s method cannot be called on vectors containing fewer than two elements. Supplied vector has %d element.",
                FunctionDescription, (int) v->size);
        return;
    }

    // Dummy expression object with dummy line number information
    dummy.srcLineN = 0;
    dummy.srcId = 0;
    dummy.srcFname = "<dummy>";
    dummy.ascii = "<reduce(f) function>";

    // Fetch first item from vector
    pplObjNum(&val, 0, gsl_vector_get(v, 0), 0);
    val.refCount = 1;
    ppl_unitsDimCpy(&val, st);
    memcpy(&val2, &val, sizeof(pplObj));

    // Combine the running value with each further element in turn: val = f(val, element)
    for (i = 1; i < v->size; i++) {
        // Push the function to call, followed by its arguments
        ppl_stackPushCopy(c, &in[0], 0);

        // The first argument: the result accumulated so far
        ppl_stackPushCopy(c, &val, 0);

        // The second argument: the next item
        val2.real = gsl_vector_get(v, i);
        ppl_stackPushCopy(c, &val2, 0); // The second argument: the next element of the vector

        // Call function
        c->errStat.errMsgExpr[0] = '\0';
        ppl_fnCall(c, &dummy, 0, 2, 1, 1);

        // Garbage collect val
        ppl_garbageObject(&val);

        // Propagate error if function failed
        if (c->errStat.status) {
            ppl_funcError(status, errType, errText, ERR_TYPE,
                    "Error inside function supplied to the %s function: %s",
                    FunctionDescription, c->errStat.errMsgExpr);
            return;
        }

        // Keep the result as the running value, and discard it from the stack
        pplObjCpy(&val, &c->stack[c->stackPtr - 1], 0, 0, 1);
        val.refCount = 1;
        ppl_stackClean(c, stkLevelOld); // Discard the expression's value, now that it has been copied
    }
    pplObjCpy(&OUTPUT, &val, 0, 0, 1);
    ppl_garbageObject(&val);
    return;
}

//! pplmethod_vectorReverse - Implement the reverse() method of vectors, which reverses the order of the elements of a
//! vector
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_vectorReverse(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                    char *errText) {
    pplObj *st = in[-1].self_this;
    gsl_vector *v = ((pplVector *) st->auxil)->v;
    long i, j;
    const long n = v->size;
    pplObjCpy(&OUTPUT, st, 0, 0, 1);
    if (v->size < 2) return;
    // Swap elements from the two ends, working inwards
    for (i = 0, j = n - 1; i < j; i++, j--) {
        const double tmp = gsl_vector_get(v, i);
        gsl_vector_set(v, i, gsl_vector_get(v, j));
        gsl_vector_set(v, j, tmp);
    }
}

//! pplmethod_vectorSort - Implement the sort() method of vectors, which sorts the members of a vector
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_vectorSort(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                 char *errText) {
    pplObj *st = in[-1].self_this;
    gsl_vector *v = ((pplVector *) st->auxil)->v;
    pplObjCpy(&OUTPUT, st, 0, 0, 1);
    if (v->size < 2) return;
    qsort((void *) v->data, v->size, sizeof(double) * v->stride, ppl_dblSort);
}

// List methods

//! pplmethod_listAppend - Implement the append(x) method of lists, which appends the object x to a list
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_listAppend(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                 char *errText) {
    pplObj v;
    pplObj *st = in[-1].self_this;
    list *l = (list *) st->auxil;
    v.refCount = 1;
    pplObjCpy(&v, &in[0], 0, 1, 1);
    ppl_listAppendCpy(l, &v, sizeof(pplObj));
    pplObjCpy(&OUTPUT, st, 0, 0, 1);
}

//! pplmethod_listCount - Implement the count(x) method of lists, which returns the number of items in a list that equal
//! x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_listCount(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObj *st = in[-1].self_this;
    list *l = (list *) st->auxil;
    int count = 0;
    listIterator *li = ppl_listIterateInit(l);
    pplObj *item;
    // Count the items equal to the argument
    while ((item = (pplObj *) ppl_listIterate(&li)) != NULL) {
        pplObj *a = item;
        pplObj *b = &in[0];
        if (pplObjCmpQuiet(&a, &b) == 0) count++;
    }
    OUTPUT.real = count;
    return;
}

//! pplmethod_listExtend - Implement the extend(x) method of lists, which appends the members of the list x to the end
//! of a list
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_listExtend(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                 char *errText) {
    pplObj v;
    pplObj *st = in[-1].self_this;
    list *l = (list *) st->auxil;
    int t = in[0].objType;

    // Append the elements of a vector, as numbers with the vector's units, or the items of a list
    if (t == PPLOBJ_VEC) {
        int i;
        gsl_vector *vi = ((pplVector *) in[0].auxil)->v;
        v.refCount = 1; // Each copy appended to the list holds one reference
        pplObjNum(&v, 1, 0, 0);
        ppl_unitsDimCpy(&v, &in[0]);
        // Append each element of the vector
        for (i = 0; i < vi->size; i++) {
            v.real = gsl_vector_get(vi, i);
            ppl_listAppendCpy(l, &v, sizeof(pplObj));
        }
    } else if (t == PPLOBJ_LIST) {
        list *listin = (list *) in[0].auxil;
        listIterator *li = ppl_listIterateInit(listin);
        pplObj *item;
        // Append a copy of each item of the list
        while ((item = (pplObj *) ppl_listIterate(&li)) != NULL) {
            v.refCount = 1;
            pplObjCpy(&v, item, 0, 1, 1);
            ppl_listAppendCpy(l, &v, sizeof(pplObj));
        }
    } else {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "Argument to the extend(x) method must be either a list or a vector. Supplied argument had type <%s>.",
                pplObjTypeNames[t]);
        return;
    }
    pplObjCpy(&OUTPUT, st, 0, 0, 1);
}

//! pplmethod_listFilter - Implement the filter(f) method of lists, which takes a pointer to a function of one argument,
//! f(a). It calls the function for every element of the list, and returns a new list of those elements for which f(a)
//! tests true
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_listFilter(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                 char *errText) {
    char *FunctionDescription = "filter(f)";
    pplObj *st = in[-1].self_this;
    list *l = (list *) st->auxil;
    list *ol;
    pplObj *item;
    pplFunc *fi;
    listIterator *li = ppl_listIterateInit(l);
    pplExpr dummy;

    // Calling the function needs room on the stack
    ppl_stackMustHave(c, 4);
    // Report a full stack
    if (c->stackFull) {
        ppl_funcError(status, errType, errText, ERR_TYPE, "%s", "Stack overflow.");
        return;
    }

    // The argument must be a function
    if (in[0].objType != PPLOBJ_FUNC) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The %s method requires a function object as its first argument. Supplied object is of type <%s>.",
                FunctionDescription, pplObjTypeNames[in[0].objType]);
        return;
    }
    fi = (pplFunc *) in[0].auxil;
    // Magic functions, such as diff_d, cannot be called in this way
    if ((fi == NULL) || (fi->functionType == PPL_FUNC_MAGIC)) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The %s method requires a function object as its first argument. Integration and differentiation operators are not suitable functions.",
                FunctionDescription);
        return;
    }
    // The output is a new list
    if (pplObjList(&OUTPUT, 0, 1, NULL) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    ol = (list *) OUTPUT.auxil;

    // Dummy expression object with dummy line number information
    dummy.srcLineN = 0;
    dummy.srcId = 0;
    dummy.srcFname = "<dummy>";
    dummy.ascii = "<filter(f) function>";

    // Call the function on each item, and keep the item if the result is true
    while ((item = (pplObj *) ppl_listIterate(&li)) != NULL) {
        const int stkLevelOld = c->stackPtr;

        // Push the function to call, followed by its arguments
        ppl_stackPushCopy(c, &in[0], 0);

        // The argument: an item of the list
        ppl_stackPushCopy(c, item, 0);

        // Call function
        c->errStat.errMsgExpr[0] = '\0';
        ppl_fnCall(c, &dummy, 0, 1, 1, 1);

        // Propagate error if function failed
        if (c->errStat.status) {
            ppl_funcError(status, errType, errText, ERR_TYPE,
                    "Error inside function supplied to the %s function: %s",
                    FunctionDescription, c->errStat.errMsgExpr);
            return;
        }

        // Keep the item if the function returned a true value
        ppl_castToBool(c, &c->stack[c->stackPtr - 1]);
        // Add a copy of the item to the output list
        if (c->stack[c->stackPtr - 1].real) {
            pplObj v;
            pplObjCpy(&v, item, 0, 1, 1);
            v.refCount = 1;
            ppl_listAppendCpy(ol, &v, sizeof(v));
        }
        ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack before giving up
    }

    // Done
    return;
}

//! pplmethod_listIndex - Implement the index(x) method of lists, which returns the index of the first element of a list
//! that equals x, or -1 if no elements match
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_listIndex(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObj *st = in[-1].self_this;
    list *l = (list *) st->auxil;
    int i = 0;
    listIterator *li = ppl_listIterateInit(l);
    pplObj *item;
    // Find the first item equal to the argument, counting from zero
    while ((item = (pplObj *) ppl_listIterate(&li)) != NULL) {
        pplObj *a = item;
        pplObj *b = &in[0];
        // Return the position of the first match
        if (pplObjCmpQuiet(&a, &b) == 0) {
            OUTPUT.real = i;
            return;
        }
        i++;
    }
    OUTPUT.real = -1;
    return;
}

//! pplmethod_listInsert - Implement the insert(n,x) method of lists, which inserts the object x into a list at position
//! n
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_listInsert(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                 char *errText) {
    long p;
    pplObj v;
    pplObj *st = in[-1].self_this;
    list *l = (list *) st->auxil;
    // The position must be a real number, within or at the end of the list
    if (in[0].objType != PPLOBJ_NUM) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "First argument to the insert(n,x) method must be a number. Supplied argument had type <%s>.",
                pplObjTypeNames[in[0].objType]);
        return;
    }
    // The position must be real
    if (in[0].flagComplex) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "First argument to the insert(n,x) method must be a real number. Supplied argument is complex.");
        return;
    }
    if (in[0].real < 0) in[0].real += l->length + 1;
    p = (long) round(in[0].real);
    // The position cannot be before the start of the list
    if (p < 0) {
        ppl_funcError(status, errType, errText, ERR_RANGE,
                "Attempt to insert a list item before the beginning of a list.");
        return;
    }
    // The position cannot be beyond the end of the list
    if (p > l->length) {
        ppl_funcError(status, errType, errText, ERR_RANGE, "List index out of range.");
        return;
    }
    v.refCount = 1;
    pplObjCpy(&v, &in[1], 0, 1, 1);
    ppl_listInsertCpy(l, p, &v, sizeof(pplObj));
    pplObjCpy(&OUTPUT, st, 0, 0, 1);
}

//! pplmethod_listLen - Implement the len() method of lists, which returns the length of a list
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_listLen(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    list *l = (list *) in[-1].self_this->auxil;
    pplObjNum(&OUTPUT, 0, l->length, 0);
}

//! pplmethod_listMap - Implement the map(f) method of lists, which takes a pointer to a function of one argument, f(a).
//! It calls the function for every element of the list, and returns a list of the results
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_listMap(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    // st is the list on which the method was called
    char *FunctionDescription = "map(f)";
    pplObj *st = in[-1].self_this;
    list *l = (list *) st->auxil;
    list *ol;
    pplObj *item;
    pplFunc *fi;
    pplExpr dummy;
    listIterator *li = ppl_listIterateInit(l);

    // Calling the function needs room on the stack
    ppl_stackMustHave(c, 4);
    // Report a full stack
    if (c->stackFull) {
        ppl_funcError(status, errType, errText, ERR_TYPE, "%s", "Stack overflow.");
        return;
    }

    // The argument must be a function
    if (in[0].objType != PPLOBJ_FUNC) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The %s method requires a function object as its first argument. Supplied object is of type <%s>.",
                FunctionDescription, pplObjTypeNames[in[0].objType]);
        return;
    }
    fi = (pplFunc *) in[0].auxil;
    // Magic functions, such as diff_d, cannot be called in this way
    if ((fi == NULL) || (fi->functionType == PPL_FUNC_MAGIC)) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The %s method requires a function object as its first argument. Integration and differentiation operators are not suitable functions.",
                FunctionDescription);
        return;
    }
    // The output is a new list
    if (pplObjList(&OUTPUT, 0, 1, NULL) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    ol = (list *) OUTPUT.auxil;

    // Dummy expression object with dummy line number information
    dummy.srcLineN = 0;
    dummy.srcId = 0;
    dummy.srcFname = "<dummy>";
    dummy.ascii = "<map(f) function>";

    // Call the function on each item, and add the result to the output list
    while ((item = (pplObj *) ppl_listIterate(&li)) != NULL) {
        const int stkLevelOld = c->stackPtr;

        // Push the function to call, followed by its arguments
        ppl_stackPushCopy(c, &in[0], 0);

        // The argument: an item of the list
        ppl_stackPushCopy(c, item, 0);

        // Call function
        c->errStat.errMsgExpr[0] = '\0';
        ppl_fnCall(c, &dummy, 0, 1, 1, 1);

        // Propagate error if function failed
        if (c->errStat.status) {
            ppl_funcError(status, errType, errText, ERR_TYPE,
                    "Error inside function supplied to the %s function: %s",
                    FunctionDescription, c->errStat.errMsgExpr);
            return;
        }

        {
            pplObj v;
            pplObjCpy(&v, &c->stack[c->stackPtr - 1], 0, 1, 1);
            v.refCount = 1;
            ppl_listAppendCpy(ol, &v, sizeof(v));
        }
        ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack before giving up
    }

    // Done
    return;
}

//! pplmethod_listMax - Implement the max() method of lists, which returns the highest-valued item in a list
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_listMax(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObj *st = in[-1].self_this;
    list *l = (list *) st->auxil;
    pplObj *item, *best = NULL;
    listIterator *li = ppl_listIterateInit(l);
    while ((item = (pplObj *) ppl_listIterate(&li)) != NULL) {
        if ((best == NULL) || (pplObjCmpQuiet((void *) &item, (void *) &best) == 1)) best = item;
    }
    if (best == NULL) pplObjNull(&OUTPUT, 0);
    else pplObjCpy(&OUTPUT, best, 0, 0, 1);
}

//! pplmethod_listMin - Implement the min() method of lists, which returns the lowest-valued item in a list
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_listMin(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObj *st = in[-1].self_this;
    list *l = (list *) st->auxil;
    pplObj *item, *best = NULL;
    listIterator *li = ppl_listIterateInit(l);
    while ((item = (pplObj *) ppl_listIterate(&li)) != NULL) {
        if ((best == NULL) || (pplObjCmpQuiet((void *) &item, (void *) &best) == -1)) best = item;
    }
    if (best == NULL) pplObjNull(&OUTPUT, 0);
    else pplObjCpy(&OUTPUT, best, 0, 0, 1);
}

//! pplmethod_listPop - Implement the pop() method of lists, which pop(n) removes the nth item from a list and returns
//! it. If n is not specified, the last list item is popped
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_listPop(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    // st is the list on which the method was called. With no argument, pop the last item; otherwise pop the item at the
    // given position.
    pplObj *obj;
    pplObj *st = in[-1].self_this;
    list *l = (list *) st->auxil;
    // With no argument, pop the last item; otherwise pop the item at the given position
    if (nArgs == 0) {
        obj = ppl_listPop(l);
    } else {
        long p;
        // The position must be a real number within the list
        if (in[0].objType != PPLOBJ_NUM) {
            ppl_funcError(status, errType, errText, ERR_TYPE,
                    "Optional argument to the pop(n) method must be a number. Supplied argument had type <%s>.",
                    pplObjTypeNames[in[0].objType]);
            return;
        }
        // The position must be real
        if (in[0].flagComplex) {
            ppl_funcError(status, errType, errText, ERR_TYPE,
                    "Optional argument to the pop(n) method must be a real number. Supplied argument is complex.");
            return;
        }
        if (in[0].real < 0) in[0].real += l->length;
        p = (long) round(in[0].real);
        // The position cannot be before the start of the list
        if (p < 0) {
            ppl_funcError(status, errType, errText, ERR_RANGE,
                    "Attempt to pop a list item before the beginning of a list.");
            return;
        }
        // The position cannot be beyond the end of the list
        if (p >= l->length) {
            ppl_funcError(status, errType, errText, ERR_RANGE, "List index out of range.");
            return;
        }
        obj = ppl_listPopItem(l, p);
    }
    pplObjCpy(&OUTPUT, obj, 0, 0, 1);
    ppl_garbageObject(obj);
}

//! pplmethod_listReduce - Implement the reduce(f) method of lists, which takes a pointer to a function of two
//! arguments. It first calls f(a,b) on the first two elements of the list, and then continues through the list calling
//! f(a,b) on the result and the next item in the list. The final result is returned
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_listReduce(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                 char *errText) {
    char *FunctionDescription = "reduce(f)";
    const int stkLevelOld = c->stackPtr;
    pplExpr dummy;
    pplObj *st = in[-1].self_this;
    list *l = (list *) st->auxil;
    pplObj *item, v;
    pplFunc *fi;
    listIterator *li = ppl_listIterateInit(l);

    // Calling the function needs room on the stack
    ppl_stackMustHave(c, 4);
    // Report a full stack
    if (c->stackFull) {
        ppl_funcError(status, errType, errText, ERR_TYPE, "%s", "Stack overflow.");
        return;
    }

    // The argument must be a function
    if (in[0].objType != PPLOBJ_FUNC) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The %s method requires a function object as its first argument. Supplied object is of type <%s>.",
                FunctionDescription, pplObjTypeNames[in[0].objType]);
        return;
    }
    fi = (pplFunc *) in[0].auxil;
    // Magic functions, such as diff_d, cannot be called in this way
    if ((fi == NULL) || (fi->functionType == PPL_FUNC_MAGIC)) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The %s method requires a function object as its first argument. Integration and differentiation operators are not suitable functions.",
                FunctionDescription);
        return;
    }
    // Reducing needs at least two items
    if (l->length < 2) {
        ppl_funcError(status, errType, errText, ERR_RANGE,
                "The %s method cannot be called on lists containing fewer than two items. Supplied list has length %d.",
                FunctionDescription, l->length);
        return;
    }

    // Dummy expression object with dummy line number information
    dummy.srcLineN = 0;
    dummy.srcId = 0;
    dummy.srcFname = "<dummy>";
    dummy.ascii = "<reduce(f) function>";

    // Fetch first item from list
    item = (pplObj *) ppl_listIterate(&li);
    pplObjCpy(&v, item, 0, 0, 1);
    v.refCount = 1;

    // Combine the running value with each further item in turn: v = f(v, item)
    while ((item = (pplObj *) ppl_listIterate(&li)) != NULL) {
        // Push the function to call, followed by its arguments
        ppl_stackPushCopy(c, &in[0], 0);

        // The first argument: the result accumulated so far
        ppl_stackPushCopy(c, &v, 0);

        // The second argument: the next item
        ppl_stackPushCopy(c, item, 0);

        // Call function
        c->errStat.errMsgExpr[0] = '\0';
        ppl_fnCall(c, &dummy, 0, 2, 1, 1);

        // Garbage collect v
        ppl_garbageObject(&v);

        // Propagate error if function failed
        if (c->errStat.status) {
            ppl_funcError(status, errType, errText, ERR_TYPE,
                    "Error inside function supplied to the %s function: %s",
                    FunctionDescription, c->errStat.errMsgExpr);
            return;
        }

        // Keep the result as the running value, and discard it from the stack
        pplObjCpy(&v, &c->stack[c->stackPtr - 1], 0, 0, 1);
        v.refCount = 1;
        ppl_stackClean(c, stkLevelOld); // Discard the expression's value, now that it has been copied
    }
    pplObjCpy(&OUTPUT, &v, 0, 0, 1);
    ppl_garbageObject(&v);
    return;
}

//! pplmethod_listReverse - Implement the reverse() method of lists, which reverses the order of the members of a list
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_listReverse(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                  char *errText) {
    long i;
    pplObj *st = in[-1].self_this;
    list *l = (list *) st->auxil;
    listItem *l1 = l->first;
    listItem *l2 = l->last;
    const long n = l->length;
    pplObjCpy(&OUTPUT, st, 0, 0, 1);
    if (n < 2) return;

    // Swap the data of items from the two ends of the list, working inwards
    for (i = 0; i < n / 2; i++) {
        void *tmp = l1->data;
        l1->data = l2->data;
        l2->data = tmp;
        l1 = l1->next;
        l2 = l2->prev;
    }
}

//! pplmethod_listSort - Implement the sort() method of lists, which sorts the members of a list
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_listSort(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    // st is the list on which the method was called; its items are sorted by copying pointers to them into an array,
    // sorting the array, and copying the pointers back
    long i;
    pplObj *st = in[-1].self_this;
    list *l = (list *) st->auxil;
    long n = l->length;
    pplObj **items;
    listIterator *li = ppl_listIterateInit(l);
    pplObjCpy(&OUTPUT, st, 0, 0, 1);
    if (n < 2) return;
    items = (pplObj **) malloc(n * sizeof(pplObj *));
    // Failure to allocate memory is reported as an error
    if (items == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "%s", "Out of memory.");
        return;
    }
    i = 0;
    // Copy the pointers to the items into the array
    while (li != NULL) {
        items[i++] = (pplObj *) li->data;
        ppl_listIterate(&li);
    }
    qsort(items, n, sizeof(pplObj *), pplObjCmpQuiet);
    li = ppl_listIterateInit(l);
    i = 0;
    // Copy the sorted pointers back into the list
    while (li != NULL) {
        li->data = (void *) items[i++];
        ppl_listIterate(&li);
    }
    free(items);
    items = NULL;
}

static pplObj *pplmethod_listSortOnCustom_fn = NULL;
static ppl_context *pplmethod_listSortOnCustom_context = NULL;
static int pplmethod_listSortOnCustom_errFlag = 0;

//! pplmethod_listSortOnCustom_slave - Comparison function for qsort(), used by the sortOn() method of lists. It calls
//! the user-supplied comparison function stored in <pplmethod_listSortOnCustom_fn>. If an error occurs, it sets
//! <pplmethod_listSortOnCustom_errFlag>, after which all comparisons return zero.
//! \param [in] a - Pointer to a pointer to the first object to compare
//! \param [in] b - Pointer to a pointer to the second object to compare
//! \return The value returned by the user-supplied comparison function: -1, 0 or 1

static int pplmethod_listSortOnCustom_slave(const void *a, const void *b) {
    // qsort() cannot pass extra arguments to its comparison function, so the context and the user's function are passed
    // in static variables
    ppl_context *c = pplmethod_listSortOnCustom_context;
    pplExpr dummy;
    int stkLevelOld;
    int out = 0;
    pplObj **ao = (pplObj **) a;
    pplObj **bo = (pplObj **) b;
    if (pplmethod_listSortOnCustom_errFlag) return 0;
    if ((c == NULL) || (a == NULL) || (b == NULL)) return 0;
    stkLevelOld = c->stackPtr;

    // Dummy expression object with dummy line number information
    dummy.srcLineN = 0;
    dummy.srcId = 0;
    dummy.srcFname = "<dummy>";
    dummy.ascii = "<sortOn(f) method>";

    // Push the function to call, followed by its arguments
    ppl_stackPushCopy(c, pplmethod_listSortOnCustom_fn, 1);

    // The argument: an item of the list
    ppl_stackPushCopy(c, *ao, 0);

    // The second argument: another item of the list
    ppl_stackPushCopy(c, *bo, 0);

    // Call function
    c->errStat.errMsgExpr[0] = '\0';
    ppl_fnCall(c, &dummy, 0, 2, 1, 1);

    // Propagate error if function failed
    if (c->errStat.status) {
        pplmethod_listSortOnCustom_errFlag = 1;
        return 0;
    }

    // Return error if function didn't return a number
    if (c->stack[c->stackPtr - 1].objType != PPLOBJ_NUM) {
        ppl_snprintfTrunc(
                c->errStat.errBuff, sizeof(c->errStat.errBuff),
                "The sortOn(f) function requires a comparison function that returns a number. Supplied function returned an object of type <%s>.",
                pplObjTypeNames[c->stack[c->stackPtr - 1].objType]);
        ppl_tbAddExpr(c, &dummy, ERR_TYPE, 0, "");
        pplmethod_listSortOnCustom_errFlag = 1;
        return 0;
    }

    // Get number back and clean stack
    if (c->stack[c->stackPtr - 1].real < 0) out = -1;
    else if (c->stack[c->stackPtr - 1].real > 0) out = 1;
    else out = 0;
    ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack before giving up

    // The result of the user's function: negative, zero or positive
    return out;
}

//! pplmethod_listSortOnCustom - Implement the sortOn(f) method of lists, which sorts the members of a list using the
//! user-supplied function f(a,b) to determine the sort order. f should return 1, 0 or -1 depending whether a>b, a==b or
//! a<b
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_listSortOnCustom(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                       char *errText) {
    char *FunctionDescription = "sortOn(f)";
    long i;
    pplObj *st = in[-1].self_this;
    list *l = (list *) st->auxil;
    long n = l->length;
    pplObj **items;
    pplFunc *fi;
    int fail;
    listIterator *li = ppl_listIterateInit(l);

    // Calling the function needs room on the stack
    ppl_stackMustHave(c, 4);
    // Report a full stack
    if (c->stackFull) {
        ppl_funcError(status, errType, errText, ERR_TYPE, "%s", "Stack overflow.");
        return;
    }

    // The argument must be a function
    if (in[0].objType != PPLOBJ_FUNC) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The %s method requires a function object as its first argument. Supplied object is of type <%s>.",
                FunctionDescription, pplObjTypeNames[in[0].objType]);
        return;
    }
    fi = (pplFunc *) in[0].auxil;
    // Magic functions, such as diff_d, cannot be called in this way
    if ((fi == NULL) || (fi->functionType == PPL_FUNC_MAGIC)) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The %s method requires a function object as its first argument. Integration and differentiation operators are not suitable functions.",
                FunctionDescription);
        return;
    }
    pplObjCpy(&OUTPUT, st, 0, 0, 1);
    if (n < 2) return;
    items = (pplObj **) malloc(n * sizeof(pplObj *));
    // Failure to allocate memory is reported as an error
    if (items == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "%s", "Out of memory.");
        return;
    }
    i = 0;
    // Copy the pointers to the items into the array
    while (li != NULL) {
        items[i++] = (pplObj *) li->data;
        ppl_listIterate(&li);
    }
    pplmethod_listSortOnCustom_fn = &in[0];
    pplmethod_listSortOnCustom_errFlag = 0;
    pplmethod_listSortOnCustom_context = c;
    qsort(items, n, sizeof(pplObj *), pplmethod_listSortOnCustom_slave);
    fail = pplmethod_listSortOnCustom_errFlag;
    pplmethod_listSortOnCustom_fn = NULL;
    pplmethod_listSortOnCustom_errFlag = 0;
    pplmethod_listSortOnCustom_context = NULL;
    // The user's comparison function failed
    if (fail) {
        ppl_funcError(status, errType, errText, ERR_GENERIC, "%s", "Failure of user-supplied comparison function.");
        return;
    }
    li = ppl_listIterateInit(l);
    i = 0;
    // Copy the sorted pointers back into the list
    while (li != NULL) {
        li->data = (void *) items[i++];
        ppl_listIterate(&li);
    }
    free(items);
    items = NULL;
}

//! pplmethod_listSortOnElement - Implement the sortOnElement(n) method of lists, which sorts a list of lists on the nth
//! element of each sublist
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_listSortOnElement(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                        char *errText) {
    long i;
    pplObj *st = in[-1].self_this;
    list *l = (list *) st->auxil;
    long n = l->length;
    pplObj **items;
    int eNum = (int) floor(in[0].real);
    listIterator *li = ppl_listIterateInit(l);
    i = 0;
    // Every item of the list must itself be a list
    while (li != NULL) {
        pplObj *o = (pplObj *) li->data;
        ppl_listIterate(&li);
        i++;
        // Report an item which is not a list
        if (o->objType != PPLOBJ_LIST) {
            ppl_funcError(status, errType, errText, ERR_TYPE,
                    "The sortOnElement() method expects to be sorting a list of lists. Element %ld of list is not a list, but has type <%s>.",
                    i, pplObjTypeNames[o->objType]);
            return;
        }
    }
    li = ppl_listIterateInit(l);
    i = 0;
    // Every sublist must have the element to sort on; negative indices count from the end
    while (li != NULL) {
        list *o = (list *) ((pplObj *) li->data)->auxil;
        ppl_listIterate(&li);
        i++;
        // Report a sublist which is too short
        if ((eNum >= o->length) || (eNum < -o->length)) {
            ppl_funcError(status, errType, errText, ERR_RANGE,
                    "The sortOnElement() method is sorting on element number %d of each sublist. However, sublist %ld only has elements 0-%d.",
                    eNum, i, o->length - 1);
            return;
        }
    }
    pplObjCpy(&OUTPUT, st, 0, 0, 1);
    if (n < 2) return;
    items = (pplObj **) malloc(n * 2 * sizeof(pplObj *));
    // Failure to allocate memory is reported as an error
    if (items == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "%s", "Out of memory.");
        return;
    }
    li = ppl_listIterateInit(l);
    i = 0;
    // Fill the array with pairs of pointers: the element to sort on, followed by the sublist. qsort() then sorts the
    // pairs on their first members.
    while (li != NULL) {
        pplObj *o = (pplObj *) li->data;
        list *l = (list *) o->auxil;
        items[i++] = ppl_listGetItem(l, (eNum >= 0) ? eNum : (l->length + eNum));
        items[i++] = (pplObj *) li->data;
        ppl_listIterate(&li);
    }
    qsort(items, n, 2 * sizeof(pplObj *), pplObjCmpQuiet);
    li = ppl_listIterateInit(l);
    i = 0;
    // Copy the second member of each sorted pair, the sublist, back into the list
    while (li != NULL) {
        i++;
        li->data = (void *) items[i++];
        ppl_listIterate(&li);
    }
    free(items);
    items = NULL;
}

//! pplmethod_listVector - Implement the vector() method of lists, which turns a list into a vector, providing all the
//! list elements are numbers with the same physical dimensions
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_listVector(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                 char *errText) {
    long i = 0;
    pplObj *st = in[-1].self_this;
    list *listin = (list *) st->auxil;
    listIterator *li = ppl_listIterateInit(listin);
    const long len = listin->length;
    gsl_vector *v;
    // A vector cannot be empty
    if (len == 0) {
        *errType = ERR_MEMORY;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Cannot create a vector of length zero.");
        *status = 1;
        return;
    }
    // Make a vector of the same length as the list
    if (pplObjVector(&OUTPUT, 0, 1, len) == NULL) {
        *status = 1;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Out of memory.");
        *errType = ERR_MEMORY;
        return;
    }
    v = ((pplVector *) OUTPUT.auxil)->v;
    // The first item must be a real number; its units become the units of the vector
    if (len > 0) {
        pplObj *item = (pplObj *) ppl_listIterate(&li);
        // Report an item which is not a number
        if (item->objType != PPLOBJ_NUM) {
            *status = 1;
            ppl_snprintfTrunc(errText, LSTR_LENGTH,
                              "Vectors can only hold numeric values. Attempt to add object of type <%s> to vector.",
                              pplObjTypeNames[item->objType]);
            *errType = ERR_TYPE;
            return;
        }
        // Vectors cannot hold complex numbers
        if (item->flagComplex) {
            *status = 1;
            ppl_snprintfTrunc(errText, LSTR_LENGTH,
                              "Vectors can only hold real numeric values. Attempt to add a complex number.");
            *errType = ERR_TYPE;
            return;
        }
        ppl_unitsDimCpy(&OUTPUT, item);
        gsl_vector_set(v, i, item->real);
    }
    // Every further item must be a real number with the same units as the first
    for (i = 1; i < len; i++) {
        pplObj *item = (pplObj *) ppl_listIterate(&li);
        // Report an item which is not a number
        if (item->objType != PPLOBJ_NUM) {
            *status = 1;
            ppl_snprintfTrunc(errText, LSTR_LENGTH,
                              "Vectors can only hold numeric values. Attempt to add object of type <%s> to vector.",
                              pplObjTypeNames[item->objType]);
            *errType = ERR_TYPE;
            return;
        }
        // Vectors cannot hold complex numbers
        if (item->flagComplex) {
            *status = 1;
            ppl_snprintfTrunc(errText, LSTR_LENGTH,
                              "Vectors can only hold real numeric values. Attempt to add a complex number.");
            *errType = ERR_TYPE;
            return;
        }
        // Check the item's units
        if (!ppl_unitsDimEqual(item, &OUTPUT)) {
            // Describe the conflicting units in the error message
            if (OUTPUT.dimensionless) {
                ppl_snprintfTrunc(
                        errText, LSTR_LENGTH,
                        "Attempt to append a number (argument %ld) to a vector with conflicting dimensions: vector is dimensionless, but number has units of <%s>.",
                        i + 1, ppl_printUnit(c, item, NULL, NULL, 1, 1, 0));
            } else if (item->dimensionless) {
                ppl_snprintfTrunc(
                        errText, LSTR_LENGTH,
                        "Attempt to append a number (argument %ld) to a vector with conflicting dimensions: vector has units of <%s>, while number is dimensionless.",
                        i + 1, ppl_printUnit(c, &OUTPUT, NULL, NULL, 0, 1, 0));
            } else {
                ppl_snprintfTrunc(
                        errText, LSTR_LENGTH,
                        "Attempt to append a number (argument %ld) to a vector with conflicting dimensions: vector has units of <%s>, while number has units of <%s>.",
                        i + 1, ppl_printUnit(c, &OUTPUT, NULL, NULL, 0, 1, 0),
                        ppl_printUnit(c, item, NULL, NULL, 1, 1, 0));
            }
            *status = 1;
            *errType = ERR_UNIT;
            return;
        }
        gsl_vector_set(v, i, item->real);
    }
    return;
}


// Dictionary methods

//! pplmethod_dictDelete - Implement the delete(x) method of pplobj, which deletes the key x from the dictionary, if it
//! exists
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_dictDelete(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                 char *errText) {
    pplObj *item;
    pplObj *st = in[-1].self_this;
    dict *di = (dict *) st->auxil;
    char *instr;
    // The key must be a string
    if ((nArgs != 1) || (in[0].objType != PPLOBJ_STR)) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The method delete(x) requires a string as its argument; supplied argument had type <%s>.",
                pplObjTypeNames[in[0].objType]);
        return;
    }
    // Immutable dictionaries, and immutable items, cannot be deleted from
    if (in[-1].self_this->immutable) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The method delete(x) cannot be called on an immutable object.");
        return;
    }
    instr = (char *) in[0].auxil;
    item = ppl_dictLookup(di, instr);
    if (item == NULL) return;
    // The item itself may be immutable
    if (item->immutable) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The method delete(x) cannot be called on an immutable object.");
        return;
    }
    item->amMalloced = 0;
    ppl_garbageObject(item);
    ppl_dictRemoveKey(di, instr);
    pplObjCpy(&OUTPUT, st, 0, 0, 1);
    return;
}

//! pplmethod_dictHasKey - Implement the hasKey(x) method of pplobj, which returns a boolean indicating whether the key
//! x exists in the dictionary
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_dictHasKey(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                 char *errText) {
    pplObj *st = in[-1].self_this;
    dict *di = (dict *) st->auxil;
    char *instr;
    // The key must be a string
    if ((nArgs != 1) || (in[0].objType != PPLOBJ_STR)) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The method hasKey() requires a string as its argument; supplied argument had type <%s>.",
                pplObjTypeNames[in[0].objType]);
        return;
    }
    instr = (char *) in[0].auxil;
    pplObjBool(&OUTPUT, 0, ppl_dictContains(di, instr));
    return;
}

//! pplmethod_dictItems - Implement the items() method of pplobj, which returns a list of the [key,value] pairs in a
//! dictionary
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_dictItems(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    // di iterates over the dictionary on which the method was called
    pplFuncCall call = {c, in, status, errType, errText, "pplmethod_dictItems()"};
    char *key, *tmp;
    pplObj v, va, vb, *item;
    list *l, *l2;
    dictIterator *di = ppl_dictIterateInit((dict *) in[-1].self_this->auxil);
    // The output is a new list
    if (pplObjList(&OUTPUT, 0, 1, NULL) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    l = (list *) OUTPUT.auxil;
    // Add a two-item list [key, value] for each item in the dictionary
    while ((item = (pplObj *) ppl_dictIterate(&di, &key)) != NULL) {
        v.refCount = 1;
        va.refCount = 1;
        vb.refCount = 1;
        if ((tmp = ppl_funcStrdup(&call, key)) == NULL) return;
        // The key
        if (pplObjStr(&va, 1, 1, tmp) == NULL) {
            ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
            return;
        }
        pplObjCpy(&vb, item, 0, 1, 1);
        // The list holding the key and value
        if (pplObjList(&v, 1, 1, NULL) == NULL) {
            ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
            return;
        }
        l2 = (list *) v.auxil;
        ppl_listAppendCpy(l2, (void *) &va, sizeof(pplObj));
        ppl_listAppendCpy(l2, (void *) &vb, sizeof(pplObj));
        ppl_listAppendCpy(l, (void *) &v, sizeof(pplObj));
    }
}

//! pplmethod_dictKeys - Implement the keys() method of pplobj, which returns a list of the keys defined in a dictionary
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_dictKeys(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplFuncCall call = {c, in, status, errType, errText, "pplmethod_dictKeys()"};
    char *key, *tmp;
    pplObj v, *item;
    list *l;
    dictIterator *di = ppl_dictIterateInit((dict *) in[-1].self_this->auxil);
    // The output is a new list
    if (pplObjList(&OUTPUT, 0, 1, NULL) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    v.refCount = 1;
    l = (list *) OUTPUT.auxil;
    // Add each key to the list
    while ((item = (pplObj *) ppl_dictIterate(&di, &key)) != NULL) {
        if ((tmp = ppl_funcStrdup(&call, key)) == NULL) return;
        pplObjStr(&v, 1, 1, tmp);
        ppl_listAppendCpy(l, (void *) &v, sizeof(pplObj));
    }
}

//! pplmethod_dictLen - Implement the len() method of pplobj, which returns the number of entries in a dictionary
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_dictLen(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    dict *d = (dict *) in[-1].self_this->auxil;
    pplObjNum(&OUTPUT, 0, d->length, 0);
}

//! pplmethod_dictValues - Implement the values() method of pplobj, which returns a list of the values in a dictionary
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_dictValues(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                 char *errText) {
    pplFuncCall call = {c, in, status, errType, errText, "pplmethod_dictValues()"};
    char *key, *tmp;
    pplObj v, *item;
    list *l;
    dictIterator *di = ppl_dictIterateInit((dict *) in[-1].self_this->auxil);
    // The output is a new list
    if (pplObjList(&OUTPUT, 0, 1, NULL) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    v.refCount = 1;
    l = (list *) OUTPUT.auxil;
    // Add a copy of each value to the list
    while ((item = (pplObj *) ppl_dictIterate(&di, &key)) != NULL) {
        if ((tmp = ppl_funcStrdup(&call, key)) == NULL) return;
        pplObjCpy(&v, item, 0, 1, 1);
        ppl_listAppendCpy(l, (void *) &v, sizeof(pplObj));
    }
}

// Matrix methods

//! pplmethod_matrixDet - Implement the det() method of matrices, which returns the determinant of a square matrix
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_matrixDet(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    // st is the matrix on which the method was called. The determinant is found from its LU decomposition, as the
    // product of the diagonal elements of U, with the sign of the permutation.
    pplObj *st = in[-1].self_this;
    gsl_matrix *m = ((pplMatrix *) in[-1].self_this->auxil)->m;
    double d = 0;
    int n = m->size1, i;
    int s;
    gsl_matrix *tmp = NULL;
    gsl_permutation *p = NULL;
    // Only square matrices have determinants
    if (m->size1 != m->size2) {
        ppl_funcError(status, errType, errText, ERR_NUMERICAL,
                "%s", "The determinant is only defined for square matrices.");
        return;
    }
    // Work on a copy, since the decomposition overwrites the matrix
    if ((tmp = gsl_matrix_alloc(n, n)) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    gsl_matrix_memcpy(tmp, m);
    // The permutation of the rows made by the decomposition
    if ((p = gsl_permutation_alloc(n)) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    // Decompose the matrix
    if (gsl_linalg_LU_decomp(tmp, p, &s) != 0) {
        ppl_funcError(status, errType, errText, ERR_NUMERICAL,
                "%s", "LU decomposition failed whilst computing matrix determinant.");
        return;
    }
    d = gsl_linalg_LU_det(tmp, s);
    gsl_permutation_free(p);
    p = NULL;
    gsl_matrix_free(tmp);
    tmp = NULL;
    pplObjNum(&OUTPUT, 0, d, 0);
    ppl_unitsDimCpy(&OUTPUT, st);
    for (i = 0; i < UNITS_MAX_BASEUNITS; i++) OUTPUT.exponent[i] *= n;
}

//! pplmethod_matrixDiagonal - Implement the diagonal() method of matrices, which returns a boolean indicating whether a
//! matrix is diagonal
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_matrixDiagonal(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                     char *errText) {
    int i, j;
    gsl_matrix *m = ((pplMatrix *) in[-1].self_this->auxil)->m;
    // Only square matrices can be diagonal
    if (m->size1 != m->size2) {
        pplObjBool(&OUTPUT, 0, 0);
        return;
    }
    for (i = 0; i < m->size1; i++)
        for (j = 0; j < m->size2; j++)
            // Every off-diagonal element must be zero
            if ((i != j) && (gsl_matrix_get(m, i, j) != 0)) {
                pplObjBool(&OUTPUT, 0, 0);
                return;
            }
    pplObjBool(&OUTPUT, 0, 1);
}

//! pplmethod_matrixEigenvalues - Implement the eigenvalues() method of matrices, which returns a vector containing the
//! eigenvalues of a square symmetric matrix
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_matrixEigenvalues(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                        char *errText) {
    int i, j;
    pplObj *st = in[-1].self_this;
    gsl_matrix *m = ((pplMatrix *) st->auxil)->m;
    gsl_matrix *tmp = NULL;
    gsl_vector *vo;
    const int n = m->size1;
    gsl_eigen_symm_workspace *w;
    // Eigenvalues are only computed for square, symmetric matrices, whose eigenvalues are real
    if (m->size1 != m->size2) {
        ppl_funcError(status, errType, errText, ERR_NUMERICAL,
                "%s", "Eigenvalues are only defined for square matrices.");
        return;
    }
    for (i = 0; i < m->size1; i++)
        for (j = 0; j < i; j++)
            // The matrix must be symmetric
            if (gsl_matrix_get(m, i, j) != gsl_matrix_get(m, j, i)) {
                ppl_funcError(status, errType, errText, ERR_NUMERICAL,
                        "%s",
                        "Eigenvalues can only be computed for symmetric matrices; supplied matrix is not symmetric.");
                return;
            }
    // GSL's workspace for the calculation
    if ((w = gsl_eigen_symm_alloc(n)) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    // Work on a copy, since the calculation overwrites the matrix
    if ((tmp = gsl_matrix_alloc(n, n)) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    gsl_matrix_memcpy(tmp, m);
    // The output is a vector of the eigenvalues
    if (pplObjVector(&OUTPUT, 0, 1, n) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    vo = ((pplVector *) OUTPUT.auxil)->v;
    // Compute the eigenvalues
    if (gsl_eigen_symm(tmp, vo, w) != 0) {
        ppl_funcError(status, errType, errText, ERR_NUMERICAL,
                "%s", "Numerical failure whilst trying to compute eigenvalues.");
        return;
    }
    gsl_matrix_free(tmp);
    tmp = NULL;
    gsl_eigen_symm_free(w);
    w = NULL;
    gsl_sort_vector(vo); // List the eigenvalues in ascending order, the same order as eigenvectors() lists eigenvectors
    ppl_unitsDimCpy(&OUTPUT, st);
}

//! pplmethod_matrixEigenvectors - Implement the eigenvectors() method of matrices, which returns a list of the
//! eigenvectors of a square symmetric matrix
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_matrixEigenvectors(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                         char *errText) {
    int i, j;
    gsl_matrix *m = ((pplMatrix *) in[-1].self_this->auxil)->m;
    gsl_matrix *tmp1 = NULL;
    gsl_matrix *tmp2 = NULL;
    gsl_vector *vtmp = NULL;
    list *lo;
    pplObj v;
    gsl_vector *vo;
    const int n = m->size1;
    gsl_eigen_symmv_workspace *w;
    v.refCount = 1;
    // Eigenvectors are only computed for square, symmetric matrices, whose eigenvalues are real
    if (m->size1 != m->size2) {
        ppl_funcError(status, errType, errText, ERR_NUMERICAL,
                "%s", "Eigenvectors are only defined for square matrices.");
        return;
    }
    for (i = 0; i < m->size1; i++)
        for (j = 0; j < i; j++)
            // The matrix must be symmetric
            if (gsl_matrix_get(m, i, j) != gsl_matrix_get(m, j, i)) {
                ppl_funcError(status, errType, errText, ERR_NUMERICAL,
                        "%s",
                        "Eigenvectors can only be computed for symmetric matrices; supplied matrix is not symmetric.");
                return;
            }
    // GSL's workspace for the calculation
    if ((w = gsl_eigen_symmv_alloc(n)) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    // A copy of the matrix, since the calculation overwrites it
    if ((tmp1 = gsl_matrix_alloc(n, n)) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    // A matrix whose columns will be the eigenvectors
    if ((tmp2 = gsl_matrix_alloc(n, n)) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    // A vector of the eigenvalues
    if ((vtmp = gsl_vector_alloc(n)) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    gsl_matrix_memcpy(tmp1, m);
    // Compute the eigenvalues and eigenvectors
    if (gsl_eigen_symmv(tmp1, vtmp, tmp2, w) != 0) {
        ppl_funcError(status, errType, errText, ERR_NUMERICAL,
                "%s", "Numerical failure whilst trying to compute eigenvectors.");
        return;
    }
    gsl_matrix_free(tmp1);
    tmp1 = NULL;
    gsl_eigen_symmv_free(w);
    w = NULL;
    gsl_eigen_symmv_sort(vtmp, tmp2, GSL_EIGEN_SORT_VAL_ASC); // In the same order as eigenvalues() lists eigenvalues
    // The output is a list of vectors
    if (pplObjList(&OUTPUT, 0, 1, NULL) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    lo = (list *) OUTPUT.auxil;
    // Each eigenvector is a column of tmp2
    for (i = 0; i < n; i++) {
        // Make a vector for the eigenvector
        if (pplObjVector(&v, 1, 1, n) == NULL) {
            ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
            return;
        }
        vo = ((pplVector *) v.auxil)->v;
        for (j = 0; j < n; j++) gsl_vector_set(vo, j, gsl_matrix_get(tmp2, j, i)); // GSL puts eigenvectors in columns
        ppl_listAppendCpy(lo, (void *) &v, sizeof(pplObj));
    }
    gsl_matrix_free(tmp2);
    tmp2 = NULL;
}

//! pplmethod_matrixInv - Implement the inv() method of matrices, which returns the inverse of a square matrix
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_matrixInv(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    // st is the matrix on which the method was called. The inverse is found from its LU decomposition.
    pplObj *st = in[-1].self_this;
    gsl_matrix *m = ((pplMatrix *) st->auxil)->m;
    const int n = m->size1;
    int s;
    gsl_matrix *tmp = NULL;
    gsl_matrix *mo = NULL;
    gsl_permutation *p = NULL;
    // Only square matrices have inverses
    if (m->size1 != m->size2) {
        ppl_funcError(status, errType, errText, ERR_NUMERICAL,
                "%s", "The inverse is only defined for square matrices.");
        return;
    }
    // Work on a copy, since the decomposition overwrites the matrix
    if ((tmp = gsl_matrix_alloc(n, n)) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    gsl_matrix_memcpy(tmp, m);
    // The permutation of the rows made by the decomposition
    if ((p = gsl_permutation_alloc(n)) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    // Decompose the matrix
    if (gsl_linalg_LU_decomp(tmp, p, &s) != 0) {
        ppl_funcError(status, errType, errText, ERR_NUMERICAL,
                "%s", "LU decomposition failed whilst computing matrix determinant.");
        return;
    }
    // The output is a new matrix
    if (pplObjMatrix(&OUTPUT, 0, 1, n, n) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    mo = ((pplMatrix *) OUTPUT.auxil)->m;
    // Compute the inverse
    if (gsl_linalg_LU_invert(tmp, p, mo)) {
        ppl_funcError(status, errType, errText, ERR_NUMERICAL,
                "%s", "Numerical failure while computing matrix inverse.");
        return;
    }
    gsl_permutation_free(p);
    p = NULL;
    gsl_matrix_free(tmp);
    tmp = NULL;
    ppl_unitsDimInverse(&OUTPUT, st);
}

//! pplmethod_matrixSize - Implement the size() method of matrices, which returns the dimensions of a matrix
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_matrixSize(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                 char *errText) {
    gsl_matrix *m = ((pplMatrix *) in[-1].self_this->auxil)->m;
    gsl_vector *vo;
    // The output is a vector of the numbers of rows and columns
    if (pplObjVector(&OUTPUT, 0, 1, 2) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "%s", "Out of memory.");
        return;
    }
    vo = ((pplVector *) OUTPUT.auxil)->v;
    gsl_vector_set(vo, 0, m->size1);
    gsl_vector_set(vo, 1, m->size2);
}

//! pplmethod_matrixSymmetric - Implement the symmetric() method of matrices, which returns a boolean indicating whether
//! a matrix is symmetric
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_matrixSymmetric(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                      char *errText) {
    int i, j;
    gsl_matrix *m = ((pplMatrix *) in[-1].self_this->auxil)->m;
    // Only square matrices can be symmetric
    if (m->size1 != m->size2) {
        pplObjBool(&OUTPUT, 0, 0);
        return;
    }
    for (i = 0; i < m->size1; i++)
        for (j = 0; j < i; j++)
            // Every element must equal its reflection in the diagonal
            if (gsl_matrix_get(m, i, j) != gsl_matrix_get(m, j, i)) {
                pplObjBool(&OUTPUT, 0, 0);
                return;
            }
    pplObjBool(&OUTPUT, 0, 1);
}

//! pplmethod_matrixTrans - Implement the transpose() method of matrices, which returns the transpose of a matrix
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_matrixTrans(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                  char *errText) {
    pplObj *st = in[-1].self_this;
    gsl_matrix *m = ((pplMatrix *) st->auxil)->m;
    int n = m->size1, i, j;
    gsl_matrix *vo = NULL;
    // Only square matrices can be transposed by this method
    if (m->size1 != m->size2) {
        ppl_funcError(status, errType, errText, ERR_NUMERICAL,
                "%s", "The transpose is only defined for square matrices.");
        return;
    }
    // The output is a new matrix
    if (pplObjMatrix(&OUTPUT, 0, 1, n, n) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    vo = ((pplMatrix *) OUTPUT.auxil)->m;
    for (i = 0; i < m->size1; i++)
        for (j = 0; j < m->size2; j++) gsl_matrix_set(vo, i, j, gsl_matrix_get(m, j, i));
    ppl_unitsDimCpy(&OUTPUT, st);
}

// File methods

//! pplmethod_fileClose - Implement the close() method of file handles, which closes a file handle
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_fileClose(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplFile *f = (pplFile *) in[-1].self_this->auxil;
    // Close the file, unless it is stdin (pipe == 2), which may not be closed
    if (f->open) {
        // stdin may not be closed
        if (f->pipe == 2) {
            ppl_funcError(status, errType, errText, ERR_TYPE, "%s", "It is not permitted to close this file handle.");
            return;
        }
        f->open = 0;
        if (!f->pipe) fclose(f->file);
        else pplObjNum(&OUTPUT, 0, pclose(f->file), 0);
        f->file = NULL;
    }
}

//! pplmethod_fileDump - Implement the dump() method of file handles, which dump(x) writes a typeable ASCII
//! representation of the object x to the file handle
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_fileDump(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplFile *f = (pplFile *) in[-1].self_this->auxil;
    pplObjDump(c, &in[0], f->file);
    return;
}

//! pplmethod_fileEof - Implement the eof() method of file handles, which returns a boolean flag to indicate whether the
//! end of a file has been reached
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_fileEof(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplFile *f = (pplFile *) in[-1].self_this->auxil;
    // A closed file returns null
    if (!f->open) {
        pplObjNull(&OUTPUT, 0);
        return;
    }
    pplObjBool(&OUTPUT, 0, feof(f->file) != 0);
}

//! pplmethod_fileFlush - Implement the flush() method of file handles, which flushes any buffered data which has not
//! yet physically been written to a file
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_fileFlush(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplFile *f = (pplFile *) in[-1].self_this->auxil;
    // A closed file returns null
    if (!f->open) {
        pplObjNull(&OUTPUT, 0);
        return;
    }
    // Report failure to flush the file
    if (fflush(f->file) < 0) {
        ppl_funcError(status, errType, errText, ERR_FILE, "%s", strerror(errno));
        return;
    }
}

//! pplmethod_fileGetpos - Implement the getPos() method of file handles, which returns a file handle's current position
//! in a file
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_fileGetpos(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                 char *errText) {
    long int fp;
    pplFile *f = (pplFile *) in[-1].self_this->auxil;
    // A closed file returns null
    if (!f->open) {
        pplObjNull(&OUTPUT, 0);
        return;
    }
    // Report failure to read the position
    if ((fp = ftell(f->file)) < 0) {
        ppl_funcError(status, errType, errText, ERR_FILE, "%s", strerror(errno));
        return;
    }
    pplObjNum(&OUTPUT, 0, fp * 8, 0);
    ppl_funcSetOutputUnit(c, &OUTPUT, UNIT_BIT);
}

//! pplmethod_fileIsOpen - Implement the isOpen() method of file handles, which returns a boolean flag indicating
//! whether a file is open
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_fileIsOpen(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                 char *errText) {
    pplFile *f = (pplFile *) in[-1].self_this->auxil;
    pplObjBool(&OUTPUT, 0, f->open != 0);
}

//! pplmethod_fileRead - Implement the read() method of file handles, which returns the contents of a file as a string
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_fileRead(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    // The file is read in chunks of 8192 bytes into a buffer which grows as needed; pos is the number of bytes read so
    // far
    char *out = NULL, *new;
    int chunk = 8192;
    long pos = 0;
    long size = chunk;
    pplFile *f = (pplFile *) in[-1].self_this->auxil;
    // A closed file returns null
    if (!f->open) {
        pplObjNull(&OUTPUT, 0);
        return;
    }
    new = (char *) malloc(chunk + 4);
    // Read chunks until the end of the file
    while (!feof(f->file)) {
        int n;
        // Failure to allocate memory: free the buffer, and report an error
        if (new == NULL) {
            // Free the buffer
            free(out);
            out = NULL;
            ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
            return;
        }
        out = new;
        n = fread(out + pos, 1, chunk, f->file);
        pos += n;
        if (n < chunk) break;
        size += chunk;
        new = (char *) realloc(out, size + 4);
    }
    out[pos] = '\0';
    pplObjStr(&OUTPUT, 0, 1, out);
}

//! pplmethod_fileReadline - Implement the readline() method of file handles, which returns a single line of a file as a
//! string
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_fileReadline(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                   char *errText) {
    char *out = NULL;
    int chunk = 8192;
    long pos = 0;
    long size = chunk;
    pplFile *f = (pplFile *) in[-1].self_this->auxil;
    // A closed file returns null
    if (!f->open) {
        pplObjNull(&OUTPUT, 0);
        return;
    }
    out = (char *) malloc(chunk);
    // Failure to allocate memory is reported as an error
    if (out == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    // Read one byte at a time until the end of the line or the file
    while (!feof(f->file)) {
        int n;
        n = fread(out + pos, 1, 1, f->file);
        if ((n == 0) || (out[pos] == '\n')) break;
        pos++;
        // Grow the buffer when it is nearly full
        if (pos > size - 4) {
            char *new;
            size += chunk;
            new = (char *) realloc(out, size);
            // Failure to allocate memory: free the buffer, and report an error
            if (new == NULL) {
                // Free the buffer
                free(out);
                out = NULL;
                ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
                return;
            }
            out = new;
        }
    }
    out[pos] = '\0';
    pplObjStr(&OUTPUT, 0, 1, out);
}

//! pplmethod_fileReadlines - Implement the readlines() method of file handles, which returns the lines of a file as a
//! list of strings
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_fileReadlines(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                    char *errText) {
    pplObj *st = in[-1].self_this;
    pplObj v;
    pplFile *f = (pplFile *) st->auxil;
    list *l;
    v.refCount = 1;
    // A closed file returns null
    if (!f->open) {
        pplObjNull(&OUTPUT, 0);
        return;
    }
    // The output is a list of lines
    if (pplObjList(&v, 0, 1, NULL) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    l = (list *) v.auxil;
    // Read each line in turn with the readline() method, and add it to the list
    while (!feof(f->file)) {
        in[-1].self_this = st;
        pplmethod_fileReadline(c, in, 0, status, errType, errText);
        // On error, discard the list of lines
        if (*status) {
            ppl_garbageObject(&v);
            return;
        }
        OUTPUT.amMalloced = 1;
        ppl_listAppendCpy(l, &OUTPUT, sizeof(pplObj));
        pplObjZom(&OUTPUT, 0);
    }
    memcpy(&OUTPUT, &v, sizeof(pplObj));
}

//! pplmethod_fileSetpos - Implement the setPos(x) method of file handles, which sets a file handle's current position
//! in a file
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_fileSetpos(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType,
                                 char *errText) {
    char FunctionDescription[] = "setPos(x)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    long int fp = (long int) in[0].real;
    pplFile *f = (pplFile *) in[-1].self_this->auxil;
    if (ppl_funcCheckUnit(&call, &in[0], "first", "a number of bytes", UNIT_BIT, 1)) return;
    // A closed file returns null. A position with units of bits is converted to bytes.
    if (!f->open) {
        pplObjNull(&OUTPUT, 0);
        return;
    }
    if (!in[0].dimensionless) fp /= 8;
    // Move to the new position
    if (fseek(f->file, fp, SEEK_SET) != 0) {
        ppl_funcError(status, errType, errText, ERR_FILE, "%s", strerror(errno));
        return;
    }

    // Return the new position, in bytes
    if ((fp = ftell(f->file)) < 0) {
        ppl_funcError(status, errType, errText, ERR_FILE, "%s", strerror(errno));
        return;
    }
    pplObjNum(&OUTPUT, 0, fp * 8, 0);
    ppl_funcSetOutputUnit(c, &OUTPUT, UNIT_BIT);
}

//! pplmethod_fileWrite - Implement the write(x) method of file handles, which writes the string x to a file
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_fileWrite(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplFile *f = (pplFile *) in[-1].self_this->auxil;
    char *s = (char *) in[0].auxil;
    long int l, o;
    // The argument must be a string
    if ((nArgs != 1) || (in[0].objType != PPLOBJ_STR)) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The function write() requires a string as its argument; supplied argument had type <%s>.",
                pplObjTypeNames[in[0].objType]);
        return;
    }
    // A closed file returns null
    if (!f->open) {
        pplObjNull(&OUTPUT, 0);
        return;
    }
    l = strlen(s);
    o = fwrite(s, 1, l, f->file);
    // Report failure to write the whole string
    if (o != l) {
        ppl_funcError(status, errType, errText, ERR_FILE, "%s", strerror(errno));
        return;
    }
}

// Exception methods

//! pplmethod_excRaise - Implement the raise(s) method of exceptions, which raises an exception with error string s
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the method. in[-1] is the method object itself, whose
//! <self_this> member points to the object the method was called on. The return value is written into
//! in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the method
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

static void pplmethod_excRaise(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    // The argument must be a string, which becomes the error message
    if ((nArgs != 1) || (in[0].objType != PPLOBJ_STR)) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The function raise() requires a string as its argument; supplied argument had type <%s>.",
                pplObjTypeNames[in[0].objType]);
        return;
    }
    *status = 1;
    *errType = (int) round(in[-1].self_this->real);
    strncpy(errText, in[0].auxil, FNAME_LENGTH);
    errText[FNAME_LENGTH - 1] = '\0';
    return;
}

// Build dictionaries of the above methods

//! pplObjMethodsInit - Build the dictionaries of methods available on each of Pyxplot's object types, including the
//! universal methods common to all objects
//! \param [in] c - The Pyxplot context

void pplObjMethodsInit(ppl_context *c) {
    // Create a dictionary of methods for each type
    int i;
    const int n = PPLOBJ_USER + 1;
    pplObjMethods = (dict **) malloc(n * sizeof(dict *));
    if (pplObjMethods == NULL) ppl_fatal(&c->errcontext, __FILE__, __LINE__, "Out of memory.");
    // Every type has the universal methods, such as class(), str() and type()
    for (i = 0; i < n; i++) {
        dict *d = pplObjMethods[i] = ppl_dictInit(1);
        if (d == NULL) ppl_fatal(&c->errcontext, __FILE__, __LINE__, "Out of memory.");
        ppl_addSystemMethod(d, "class", 0, 0, 1, 1, 1, 1, (void *) &pplmethod_class, "class()", "\\mathrm{class}@<@>",
                            "class() returns the class prototype of an object");
        ppl_addSystemMethod(d, "contents", 0, 0, 1, 1, 1, 1, (void *) &pplmethod_contents, "contents()",
                            "\\mathrm{contents}@<@>",
                            "contents() returns a list of all the methods and internal variables of an object");
        ppl_addSystemMethod(d, "data", 0, 0, 1, 1, 1, 1, (void *) &pplmethod_data, "data()", "\\mathrm{data}@<@>",
                            "data() returns a list of all the internal variables (not methods) of an object");
        ppl_addSystemMethod(d, "methods", 0, 0, 1, 1, 1, 1, (void *) &pplmethod_methods, "methods()",
                            "\\mathrm{methods}@<@>", "methods() returns a list of the methods of an object");
        ppl_addSystemMethod(d, "str", 0, 0, 1, 1, 1, 1, (void *) &pplmethod_str, "str()", "\\mathrm{str}@<@>",
                            "str() returns a string representation of an object");
        ppl_addSystemMethod(d, "type", 0, 0, 1, 1, 1, 1, (void *) &pplmethod_type, "type()", "\\mathrm{type}@<@>",
                            "type() returns the type of an object");
    }

    // String methods
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_STR], "append", 1, 1, 0, 0, 0, 0, (void *) &pplmethod_strAppend,
                        "append(x)", "\\mathrm{append}@<@1@>", "append(x) appends the string x to the end of a string");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_STR], "beginsWith", 1, 1, 0, 0, 0, 0, (void *) &pplmethod_strBeginsWith,
                        "beginsWith(x)", "\\mathrm{beginsWith}@<@1@>",
                        "beginsWith(x) returns a boolean indicating whether a string begins with the substring x");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_STR], "endsWith", 1, 1, 0, 0, 0, 0, (void *) &pplmethod_strEndsWith,
                        "endsWith(x)", "\\mathrm{endsWith}@<@1@>",
                        "endsWith(x) returns a boolean indicating whether a string ends with the substring x");
    ppl_addSystemMethod(
            pplObjMethods[PPLOBJ_STR], "find", 1, 1, 0, 0, 0, 0, (void *) &pplmethod_strFind, "find(x)",
            "\\mathrm{find}@<@1@>",
            "find(x) returns the position, in characters, of the first occurrence of x in a string, or -1 if it is not "
            "found");
    ppl_addSystemMethod(
            pplObjMethods[PPLOBJ_STR], "findAll", 1, 1, 0, 0, 0, 0, (void *) &pplmethod_strFindAll, "findAll(x)",
            "\\mathrm{findAll}@<@1@>",
            "findAll(x) returns a list of the positions, in characters, where the substring x occurs in a string");
    ppl_addSystemMethod(
            pplObjMethods[PPLOBJ_STR], "isalnum", 0, 0, 1, 1, 1, 1, (void *) &pplmethod_strisalnum, "isalnum()",
            "\\mathrm{isalnum}@<@>",
            "isalnum() returns a boolean indicating whether all of the characters of a string are letters (in any "
            "alphabet) or the digits 0-9");
    ppl_addSystemMethod(
            pplObjMethods[PPLOBJ_STR], "isalpha", 0, 0, 1, 1, 1, 1, (void *) &pplmethod_strisalpha, "isalpha()",
            "\\mathrm{isalpha}@<@>",
            "isalpha() returns a boolean indicating whether all of the characters of a string are letters, in any "
            "alphabet");
    ppl_addSystemMethod(
            pplObjMethods[PPLOBJ_STR], "isdigit", 0, 0, 1, 1, 1, 1, (void *) &pplmethod_strisdigit, "isdigit()",
            "\\mathrm{isdigit}@<@>",
            "isdigit() returns a boolean indicating whether all of the characters of a string are the digits 0-9");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_STR], "len", 0, 0, 1, 1, 1, 1, (void *) &pplmethod_strLen, "len()",
                        "\\mathrm{len}@<@>", "len() returns the length of a string, in characters");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_STR], "lower", 0, 0, 1, 1, 1, 1, (void *) &pplmethod_strLower, "lower()",
                        "\\mathrm{lower}@<@>", "lower() converts a string to lowercase");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_STR], "split", 0, 0, 1, 1, 1, 1, (void *) &pplmethod_strSplit, "split()",
                        "\\mathrm{split}@<@>",
                        "split() returns a list of all the whitespace-separated words in a string");
    ppl_addSystemMethod(
            pplObjMethods[PPLOBJ_STR], "splitOn", 1, 1e3, 0, 0, 0, 0, (void *) &pplmethod_strSplitOn, "splitOn(x,...)",
            "\\mathrm{splitOn}@<@0@>",
            "splitOn(x,...) splits a string whenever it encounters any of the substrings supplied as arguments, and returns a list of the split string segments");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_STR], "lstrip", 0, 0, 1, 1, 1, 1, (void *) &pplmethod_strLStrip,
                        "lstrip()", "\\mathrm{lstrip}@<@>", "lstrip() strips whitespace off the beginning of a string");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_STR], "strip", 0, 0, 1, 1, 1, 1, (void *) &pplmethod_strStrip, "strip()",
                        "\\mathrm{strip}@<@>", "strip() strips whitespace off the beginning and end of a string");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_STR], "rstrip", 0, 0, 1, 1, 1, 1, (void *) &pplmethod_strRStrip,
                        "rstrip()", "\\mathrm{rstrip}@<@>", "rstrip() strips whitespace off the end of a string");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_STR], "upper", 0, 0, 1, 1, 1, 1, (void *) &pplmethod_strUpper, "upper()",
                        "\\mathrm{upper}@<@>", "upper() converts a string to uppercase");

    // Date methods
    ppl_addSystemMethod(
            pplObjMethods[PPLOBJ_DATE], "toDayOfMonth", 0, 1, 0, 0, 0, 0, (void *) pplmethod_dateToDayOfMonth,
            "toDayOfMonth(<timezone>)", "\\mathrm{toDayOfMonth}@<@0@>",
            "toDayOfMonth(<timezone>) returns the day of the month of a date object in the current calendar");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_DATE], "toDayWeekName", 0, 1, 0, 0, 0, 0,
                        (void *) pplmethod_dateToDayWeekName, "toDayWeekName(<timezone>)",
                        "\\mathrm{toDayWeekName}@<@0@>",
                        "toDayWeekName(<timezone>) returns the name of the day of the week of a date object");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_DATE], "toDayWeekNum", 0, 1, 0, 0, 0, 0,
                        (void *) pplmethod_dateToDayWeekNum, "toDayWeekNum(<timezone>)", "\\mathrm{toDayWeekNum}@<@0@>",
                        "toDayWeekNum(<timezone>) returns the day of the week (1-7) of a date object");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_DATE], "toHour", 0, 1, 0, 0, 0, 0, (void *) pplmethod_dateToHour,
                        "toHour(<timezone>)", "\\mathrm{toHour}@<@0@>",
                        "toHour(<timezone>) returns the integer hour component (0-23) of a date object");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_DATE], "toJD", 0, 0, 0, 0, 0, 0, (void *) pplmethod_dateToJD, "toJD()",
                        "\\mathrm{toJD}@<@>", "toJD() converts a date object to a numerical Julian date");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_DATE], "toMinute", 0, 1, 0, 0, 0, 0, (void *) pplmethod_dateToMinute,
                        "toMinute(<timezone>)", "\\mathrm{toMinute}@<@0@>",
                        "toMinute(<timezone>) returns the integer minute component (0-59) of a date object");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_DATE], "toMJD", 0, 0, 0, 0, 0, 0, (void *) pplmethod_dateToMJD, "toMJD()",
                        "\\mathrm{toMJD}@<@>", "toMJD() converts a date object to a modified Julian date");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_DATE], "toMonthName", 0, 1, 0, 0, 0, 0, (void *) pplmethod_dateToMonthName,
                        "toMonthName(<timezone>)", "\\mathrm{toMonthName}@<@0@>",
                        "toMonthName(<timezone>) returns the name of the month in which a date object falls");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_DATE], "toMonthNum", 0, 1, 0, 0, 0, 0, (void *) pplmethod_dateToMonthNum,
                        "toMonthNum(<timezone>)", "\\mathrm{toMonthNum}@<@0@>",
                        "toMonthNum(<timezone>) returns the number (1-12) of the month in which a date object falls");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_DATE], "toSecond", 0, 1, 0, 0, 0, 0, (void *) pplmethod_dateToSecond,
                        "toSecond(<timezone>)", "\\mathrm{toSecond}@<@0@>",
                        "toSecond(<timezone>) returns the seconds component (0.0-60.0) of a date object");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_DATE], "str", 0, 2, 0, 0, 0, 0, (void *) pplmethod_dateToStr,
                        "str(<s>,<timezone>)", "\\mathrm{str}@<@0@>",
                        "str(<s>,<timezone>) converts a date object to a string with an optional format string");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_DATE], "toUnix", 0, 0, 0, 0, 0, 0, (void *) pplmethod_dateToUnix,
                        "toUnix()", "\\mathrm{toUnix}@<@>", "toUnix() converts a date object to a Unix time");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_DATE], "toYear", 0, 1, 0, 0, 0, 0, (void *) pplmethod_dateToYear,
                        "toYear(<timezone>)", "\\mathrm{toYear}@<@0@>",
                        "toYear(<timezone>) returns the year in which a date object falls in the current calendar");

    // Color methods
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_COL], "componentsCMYK", 0, 0, 1, 1, 1, 1, (void *) pplmethod_colCompCMYK,
                        "componentsCMYK()", "\\mathrm{componentsCMYK}@<@>",
                        "componentsCMYK() returns a vector CMYK representation of a color");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_COL], "componentsHSB", 0, 0, 1, 1, 1, 1, (void *) pplmethod_colCompHSB,
                        "componentsHSB()", "\\mathrm{componentsHSB}@<@>",
                        "componentsHSB() returns a vector HSB representation of a color");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_COL], "componentsRGB", 0, 0, 1, 1, 1, 1, (void *) pplmethod_colCompRGB,
                        "componentsRGB()", "\\mathrm{componentsRGB}@<@>",
                        "componentsRGB() returns a vector RGB representation of a color");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_COL], "toCMYK", 0, 0, 1, 1, 1, 1, (void *) pplmethod_colToCMYK, "toCMYK()",
                        "\\mathrm{toCMYK}@<@>", "toCMYK() returns a CMYK representation of a color");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_COL], "toHSB", 0, 0, 1, 1, 1, 1, (void *) pplmethod_colToHSB, "toHSB()",
                        "\\mathrm{toHSB}@<@>", "toHSB() returns an HSB representation of a color");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_COL], "toRGB", 0, 0, 1, 1, 1, 1, (void *) pplmethod_colToRGB, "toRGB()",
                        "\\mathrm{toRGB}@<@>", "toRGB() returns an RGB representation of a color");

    // Vector methods
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_VEC], "append", 1, 1, 1, 1, 1, 0, (void *) pplmethod_vectorAppend,
                        "append(x)", "\\mathrm{append}@<@0@>", "append(x) appends the object x to a vector");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_VEC], "extend", 1, 1, 0, 0, 0, 0, (void *) pplmethod_vectorExtend,
                        "extend(x)", "\\mathrm{extend}@<@0@>",
                        "extend(x) appends the members of the list x to the end of a vector");
    ppl_addSystemMethod(
            pplObjMethods[PPLOBJ_VEC], "filter", 1, 1, 0, 0, 0, 0, (void *) pplmethod_vectorFilter, "filter(f)",
            "\\mathrm{filter}@<@0}@>",
            "filter(f) takes a pointer to a function of one argument, f(a). It calls the function for every element of the vector, and returns a new vector of those elements for which f(a) tests true");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_VEC], "insert", 2, 2, 0, 0, 0, 0, (void *) pplmethod_vectorInsert,
                        "insert(n,x)", "\\mathrm{insert}@<@0@>",
                        "insert(n,x) inserts the object x into a vector at position n");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_VEC], "len", 0, 0, 1, 1, 1, 1, (void *) pplmethod_vectorLen, "len()",
                        "\\mathrm{len}@<@>", "len() returns the number of dimensions of a vector");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_VEC], "list", 0, 0, 1, 1, 1, 1, (void *) pplmethod_vectorList, "list()",
                        "\\mathrm{list}@<@>", "list() returns a list representation of a vector");
    ppl_addSystemMethod(
            pplObjMethods[PPLOBJ_VEC], "map", 1, 1, 0, 0, 0, 0, (void *) pplmethod_vectorMap, "map(f)",
            "\\mathrm{map}@<@0}@>",
            "map(f) takes a pointer to a function of one argument, f(a). It calls the function for every element of the vector, and returns a vector of the results");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_VEC], "norm", 0, 0, 1, 1, 1, 1, (void *) pplmethod_vectorNorm, "norm()",
                        "\\mathrm{norm}@<@>", "norm() returns the norm (quadrature sum) of a vector's elements");
    ppl_addSystemMethod(
            pplObjMethods[PPLOBJ_VEC], "reduce", 1, 1, 0, 0, 0, 0, (void *) pplmethod_vectorReduce, "reduce(f)",
            "\\mathrm{reduce}@<@0}@>",
            "reduce(f) takes a pointer to a function of two arguments. It first calls f(a,b) on the first two elements of the vector, and then continues through the vector calling f(a,b) on the result and the next item in the vector. The final result is returned");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_VEC], "reverse", 0, 0, 1, 1, 1, 1, (void *) pplmethod_vectorReverse,
                        "reverse()", "\\mathrm{reverse}@<@>",
                        "reverse() reverses the order of the elements of a vector");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_VEC], "sort", 0, 0, 1, 1, 1, 1, (void *) pplmethod_vectorSort, "sort()",
                        "\\mathrm{sort}@<@>", "sort() sorts the members of a vector");

    // List methods
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_LIST], "append", 1, 1, 0, 0, 0, 0, (void *) pplmethod_listAppend,
                        "append(x)", "\\mathrm{append}@<@0}@>", "append(x) appends the object x to a list");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_LIST], "count", 1, 1, 0, 0, 0, 0, (void *) pplmethod_listCount, "count(x)",
                        "\\mathrm{count}@<@0}@>", "count(x) returns the number of items in a list that equal x");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_LIST], "extend", 1, 1, 0, 0, 0, 0, (void *) pplmethod_listExtend,
                        "extend(x)", "\\mathrm{extend}@<@0}@>",
                        "extend(x) appends the members of the list x to the end of a list");
    ppl_addSystemMethod(
            pplObjMethods[PPLOBJ_LIST], "filter", 1, 1, 0, 0, 0, 0, (void *) pplmethod_listFilter, "filter(f)",
            "\\mathrm{filter}@<@0}@>",
            "filter(f) takes a pointer to a function of one argument, f(a). It calls the function for every element of the list, and returns a new list of those elements for which f(a) tests true");
    ppl_addSystemMethod(
            pplObjMethods[PPLOBJ_LIST], "index", 1, 1, 0, 0, 0, 0, (void *) pplmethod_listIndex, "index(x)",
            "\\mathrm{index}@<@0}@>",
            "index(x) returns the index of the first element of a list that equals x, or -1 if no elements match");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_LIST], "insert", 2, 2, 0, 0, 0, 0, (void *) pplmethod_listInsert,
                        "insert(n,x)", "\\mathrm{insert}@<@0@>",
                        "insert(n,x) inserts the object x into a list at position n");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_LIST], "len", 0, 0, 1, 1, 1, 1, (void *) pplmethod_listLen, "len()",
                        "\\mathrm{len}@<@>", "len() returns the length of a list");
    ppl_addSystemMethod(
            pplObjMethods[PPLOBJ_LIST], "map", 1, 1, 0, 0, 0, 0, (void *) pplmethod_listMap, "map(f)",
            "\\mathrm{map}@<@0}@>",
            "map(f) takes a pointer to a function of one argument, f(a). It calls the function for every element of the list, and returns a list of the results");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_LIST], "max", 0, 0, 1, 1, 1, 1, (void *) pplmethod_listMax, "max()",
                        "\\mathrm{max}@<@>", "max() returns the highest-valued item in a list");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_LIST], "min", 0, 0, 1, 1, 1, 1, (void *) pplmethod_listMin, "min()",
                        "\\mathrm{min}@<@>", "min() returns the lowest-valued item in a list");
    ppl_addSystemMethod(
            pplObjMethods[PPLOBJ_LIST], "pop", 0, 1, 1, 1, 1, 1, (void *) pplmethod_listPop, "pop()",
            "\\mathrm{pop}@<@0@>",
            "pop(n) removes the nth item from a list and returns it. If n is not specified, the last list item is popped");
    ppl_addSystemMethod(
            pplObjMethods[PPLOBJ_LIST], "reduce", 1, 1, 0, 0, 0, 0, (void *) pplmethod_listReduce, "reduce(f)",
            "\\mathrm{reduce}@<@0}@>",
            "reduce(f) takes a pointer to a function of two arguments. It first calls f(a,b) on the first two elements of the list, and then continues through the list calling f(a,b) on the result and the next item in the list. The final result is returned");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_LIST], "reverse", 0, 0, 1, 1, 1, 1, (void *) pplmethod_listReverse,
                        "reverse()", "\\mathrm{reverse}@<@>", "reverse() reverses the order of the members of a list");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_LIST], "sort", 0, 0, 1, 1, 1, 1, (void *) pplmethod_listSort, "sort()",
                        "\\mathrm{sort}@<@>", "sort() sorts the members of a list");
    ppl_addSystemMethod(
            pplObjMethods[PPLOBJ_LIST], "sortOn", 1, 1, 0, 0, 0, 0, (void *) pplmethod_listSortOnCustom, "sortOn(f)",
            "\\mathrm{sortOn}@<@0@>",
            "sortOn(f) sorts the members of a list using the user-supplied function f(a,b) to determine the sort order. f should return 1, 0 or -1 depending whether a>b, a==b or a<b");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_LIST], "sortOnElement", 1, 1, 1, 1, 1, 1,
                        (void *) pplmethod_listSortOnElement, "sortOnElement(n)", "\\mathrm{sortOnElement}@<@1@>",
                        "sortOnElement(n) sorts a list of lists on the nth element of each sublist");
    ppl_addSystemMethod(
            pplObjMethods[PPLOBJ_LIST], "vector", 0, 0, 1, 1, 1, 1, (void *) pplmethod_listVector, "vector()",
            "\\mathrm{vector}@<@>",
            "vector() turns a list into a vector, providing all the list elements are numbers with the same physical dimensions");

    // Dictionary methods
    for (i = 0; i < 3; i++) {
        int pplobj;
        if (i == 0) pplobj = PPLOBJ_DICT;
        else if (i == 1) pplobj = PPLOBJ_MOD;
        else pplobj = PPLOBJ_USER;

        // Dictionaries, modules and class instances share the methods of dictionaries
        ppl_addSystemMethod(pplObjMethods[pplobj], "delete", 1, 1, 0, 0, 0, 0, (void *) pplmethod_dictDelete,
                            "delete(x)", "\\mathrm{delete}@<@1@>",
                            "delete(x) deletes the key x from the dictionary, if it exists");
        ppl_addSystemMethod(pplObjMethods[pplobj], "hasKey", 1, 1, 0, 0, 0, 0, (void *) pplmethod_dictHasKey,
                            "hasKey(x)", "\\mathrm{hasKey}@<@1@>",
                            "hasKey(x) returns a boolean indicating whether the key x exists in the dictionary");
        ppl_addSystemMethod(pplObjMethods[pplobj], "items", 0, 0, 1, 1, 1, 1, (void *) pplmethod_dictItems, "items()",
                            "\\mathrm{items}@<@>", "items() returns a list of the [key,value] pairs in a dictionary");
        ppl_addSystemMethod(pplObjMethods[pplobj], "keys", 0, 0, 1, 1, 1, 1, (void *) pplmethod_dictKeys, "keys()",
                            "\\mathrm{keys}@<@>", "keys() returns a list of the keys defined in a dictionary");
        ppl_addSystemMethod(pplObjMethods[pplobj], "len", 0, 0, 1, 1, 1, 1, (void *) pplmethod_dictLen, "len()",
                            "\\mathrm{len}@<@>", "len() returns the number of entries in a dictionary");
        ppl_addSystemMethod(pplObjMethods[pplobj], "values", 0, 0, 1, 1, 1, 1, (void *) pplmethod_dictValues,
                            "values()", "\\mathrm{values}@<@>",
                            "values() returns a list of the values in a dictionary");
    }

    // Matrix methods
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_MAT], "det", 0, 0, 1, 1, 1, 1, (void *) pplmethod_matrixDet, "det()",
                        "\\mathrm{det}@<@>", "det() returns the determinant of a square matrix");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_MAT], "diagonal", 0, 0, 1, 1, 1, 1, (void *) pplmethod_matrixDiagonal,
                        "diagonal()", "\\mathrm{diagonal}@<@>",
                        "diagonal() returns a boolean indicating whether a matrix is diagonal");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_MAT], "eigenvalues", 0, 0, 1, 1, 1, 1,
                        (void *) pplmethod_matrixEigenvalues, "eigenvalues()", "\\mathrm{eigenvalues}@<@>",
                        "eigenvalues() returns a vector containing the eigenvalues of a square symmetric matrix");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_MAT], "eigenvectors", 0, 0, 1, 1, 1, 1,
                        (void *) pplmethod_matrixEigenvectors, "eigenvectors()", "\\mathrm{eigenvectors}@<@>",
                        "eigenvectors() returns a list of the eigenvectors of a square symmetric matrix");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_MAT], "inv", 0, 0, 1, 1, 1, 1, (void *) pplmethod_matrixInv, "inv()",
                        "\\mathrm{inv}@<@>", "inv() returns the inverse of a square matrix");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_MAT], "size", 0, 0, 1, 1, 1, 1, (void *) pplmethod_matrixSize, "size()",
                        "\\mathrm{size}@<@>", "size() returns the dimensions of a matrix");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_MAT], "symmetric", 0, 0, 1, 1, 1, 1, (void *) pplmethod_matrixSymmetric,
                        "symmetric()", "\\mathrm{symmetric}@<@>",
                        "symmetric() returns a boolean indicating whether a matrix is symmetric");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_MAT], "transpose", 0, 0, 1, 1, 1, 1, (void *) pplmethod_matrixTrans,
                        "transpose()", "\\mathrm{transpose}@<@>", "transpose() returns the transpose of a matrix");

    // File methods
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_FILE], "close", 0, 0, 1, 1, 1, 1, (void *) pplmethod_fileClose, "close()",
                        "\\mathrm{close}@<@>", "close() closes a file handle");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_FILE], "dump", 1, 1, 0, 0, 0, 0, (void *) pplmethod_fileDump, "dump()",
                        "\\mathrm{dump@<@0@>",
                        "dump(x) writes a typeable ASCII representation of the object x to the file handle");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_FILE], "eof", 0, 0, 1, 1, 1, 1, (void *) pplmethod_fileEof, "eof()",
                        "\\mathrm{eof}@<@>",
                        "eof() returns a boolean flag to indicate whether the end of a file has been reached");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_FILE], "flush", 0, 0, 1, 1, 1, 1, (void *) pplmethod_fileFlush, "flush()",
                        "\\mathrm{flush}@<@>",
                        "flush() flushes any buffered data which has not yet physically been written to a file");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_FILE], "getPos", 0, 0, 1, 1, 1, 1, (void *) pplmethod_fileGetpos,
                        "getPos()", "\\mathrm{getPos}@<@>",
                        "getPos() returns a file handle's current position in a file");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_FILE], "isOpen", 0, 0, 1, 1, 1, 1, (void *) pplmethod_fileIsOpen,
                        "isOpen()", "\\mathrm{isOpen}@<@>",
                        "isOpen() returns a boolean flag indicating whether a file is open");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_FILE], "read", 0, 0, 1, 1, 1, 1, (void *) pplmethod_fileRead, "read()",
                        "\\mathrm{read}@<@>", "read() returns the contents of a file as a string");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_FILE], "readline", 0, 0, 1, 1, 1, 1, (void *) pplmethod_fileReadline,
                        "readline()", "\\mathrm{readline}@<@>",
                        "readline() returns a single line of a file as a string");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_FILE], "readlines", 0, 0, 1, 1, 1, 1, (void *) pplmethod_fileReadlines,
                        "readlines()", "\\mathrm{readlines}@<@>",
                        "readlines() returns the lines of a file as a list of strings");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_FILE], "setPos", 1, 1, 1, 1, 1, 0, (void *) pplmethod_fileSetpos,
                        "setPos(x)", "\\mathrm{setPos}@<@0@>",
                        "setPos(x) sets a file handle's current position in a file");
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_FILE], "write", 1, 1, 0, 0, 0, 0, (void *) pplmethod_fileWrite, "write(x)",
                        "\\mathrm{write}@<@0@>", "write(x) writes the string x to a file");

    // Exception methods
    ppl_addSystemMethod(pplObjMethods[PPLOBJ_EXC], "raise", 1, 1, 0, 0, 0, 0, (void *) pplmethod_excRaise, "raise(s)",
                        "\\mathrm{raise}@<@0@>", "raise(s) raises an exception with error string s");

    // All the methods have been registered
    return;
}
