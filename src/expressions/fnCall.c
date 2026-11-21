// fnCall.c
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

// Calls function objects from expressions: built-in functions (checking their arguments as each declares), user-defined
// algebraic functions (binding the arguments as local variables and evaluating the definition), subroutines, splines,
// histograms and Fourier transforms. Calling a module or class instantiates it, and calling a type object, e.g. list()
// or vector(3), constructs an object of that type. The expression evaluator calls ppl_fnCall() for every function call.

#define _FNCALL_C 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <time.h>

#include <gsl/gsl_math.h>

#include "commands/fft.h"
#include "userspace/stack.h"
#include "commands/histogram.h"
#include "commands/interpolate.h"
#include "coreUtils/dict.h"
#include "coreUtils/errorReport.h"
#include "coreUtils/list.h"
#include "expressions/expEval.h"
#include "expressions/expEvalCalculus.h"
#include "expressions/fnCall.h"
#include "expressions/traceback_fns.h"
#include "settings/colors.h"
#include "settings/settingTypes.h"
#include "stringTools/asciidouble.h"
#include "userspace/context.h"
#include "userspace/contextVarDef.h"
#include "userspace/garbageCollector.h"
#include "userspace/pplObj_fns.h"
#include "userspace/pplObjFunc.h"
#include "userspace/pplObjPrint.h"
#include "userspace/unitsArithmetic.h"
#include "userspace/unitsDisp.h"

#include "pplConstants.h"


//! ppl_fnCall - Evaluate a function call. The function object and its arguments are taken from the top of the
//! stack, and replaced by the result. Calling a module or class creates an instance of it, and calling a type
//! object constructs an object of that type.
//! \param [in] context - The Pyxplot context
//! \param [in] inExpr - The expression containing the function call, used to report errors
//! \param [in] inExprCharPos - The position of the call within <inExpr>, used to report errors
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [in] dollarAllowed - If true, the function may use the $ operator
//! vectorSetItem - Set one element of a vector being made by the vector() constructor, checking that the item is a
//! real number. The units of the first element become those of the vector; each further element must match them.
//! \param [in] context - The Pyxplot context
//! \param [in] inExpr - The expression being evaluated, used for error reporting
//! \param [in] inExprCharPos - The position of the function call within <inExpr>
//! \param [in,out] out - The vector being made
//! \param [in] i - The number of the element to set
//! \param [in] item - The value of the element
//! \return Zero on success, or one if an error was reported

static int vectorSetItem(ppl_context *context, const pplExpr *inExpr, const int inExprCharPos, pplObj *out,
                         const long i, const pplObj *item) {
    // Report an item which is not a number
    if (item->objType != PPLOBJ_NUM) {
        ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                          "Vectors can only hold numeric values. Attempt to add object of type <%s> to vector.",
                          pplObjTypeNames[item->objType]);
        ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
        return 1;
    }
    // Vectors cannot hold complex numbers
    if (item->flagComplex) {
        ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                          "Vectors can only hold real numeric values. Attempt to add a complex number.");
        ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
        return 1;
    }
    // The first item sets the units of the vector
    if (i == 0) {
        ppl_unitsDimCpy(out, item);
    }
    // Report an item with different units
    else if (!ppl_unitsDimEqual(item, out)) {
        // Describe the conflicting units in the error message
        if (out->dimensionless) {
            ppl_snprintfTrunc(
                    context->errStat.errBuff, sizeof(context->errStat.errBuff),
                    "Attempt to append a number (argument %ld) to a vector with conflicting dimensions: vector is dimensionless, but number has units of <%s>.",
                    i + 1, ppl_printUnit(context, item, NULL, NULL, 1, 1, 0));
        } else if (item->dimensionless) {
            ppl_snprintfTrunc(
                    context->errStat.errBuff, sizeof(context->errStat.errBuff),
                    "Attempt to append a number (argument %ld) to a vector with conflicting dimensions: vector has units of <%s>, while number is dimensionless.",
                    i + 1, ppl_printUnit(context, out, NULL, NULL, 0, 1, 0));
        } else {
            ppl_snprintfTrunc(
                    context->errStat.errBuff, sizeof(context->errStat.errBuff),
                    "Attempt to append a number (argument %ld) to a vector with conflicting dimensions: vector has units of <%s>, while number has units of <%s>.",
                    i + 1, ppl_printUnit(context, out, NULL, NULL, 0, 1, 0),
                    ppl_printUnit(context, item, NULL, NULL, 1, 1, 0));
        }
        ppl_tbAddExpr(context, inExpr, ERR_UNIT, inExprCharPos, "");
        return 1;
    }
    gsl_vector_set(((pplVector *) out->auxil)->v, i, item->real);
    return 0;
}

//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppl_fnCall(ppl_context *context, pplExpr *inExpr, const int inExprCharPos, int nArgs, const int dollarAllowed,
                const int iterDepth) {
    pplObj *out = &context->stack[context->stackPtr - 1 - nArgs];
    pplObj *args = context->stack + context->stackPtr - nArgs;
    pplObj called;
    int i;
    int t = out->objType;
    // The stack must hold the function and its arguments
    if (context->stackPtr < nArgs + 1) {
        ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                          "Attempt to call function with few items on the stack.");
        ppl_tbAddExpr(context, inExpr, ERR_INTERNAL, inExprCharPos, "");
        return;
    }
    memcpy(&called, out, sizeof(pplObj));
    pplObjNum(out, 0, 0, 0); // Overwrite function object on stack, but don't garbage collect it yet
    out->refCount = 1;
    out->self_this = called.self_this;

    // Attempt to call a module to generate a class instance?
    if ((t == PPLOBJ_MOD) || (t == PPLOBJ_USER)) {
        // Calling a module or class instance makes a new instance of it, which takes no arguments
        if (nArgs != 0) {
            ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                              "Instantiation takes zero arguments; %d supplied.", nArgs);
            ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
            goto cleanup;
        }
        pplObjUser(out, 0, 1, &called);
        goto cleanup;
    }

    // Attempt to call a type object as a constructor?
    else if (t == PPLOBJ_TYPE) {
        const int id = ((pplType *) (out->auxil))->id;
        // Calling a type object constructs an object of that type, from zero or one arguments
        switch (id) {
            case PPLOBJ_NUM:
                // number(): zero, or number(x): x cast to a number
                if (nArgs == 0) {
                    pplObjNum(out, 0, 0, 0);
                } else if (nArgs == 1) {
                    if (ppl_castToNum(context, &args[0], inExpr, inExprCharPos)) goto cast_fail;
                    pplObjCpy(out, &args[0], 0, 0, 1);
                } else {
                    ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                      "The numeric object constructor takes either zero or one arguments; %d supplied.",
                                      nArgs);
                    ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                }
                goto cleanup;
            case PPLOBJ_STR:
                // string(): an empty string, or string(x): x as text
                if (nArgs == 0) {
                    pplObjStr(out, 0, 0, "");
                } else if (nArgs == 1) {
                    if (args[0].objType == PPLOBJ_STR) pplObjCpy(out, &args[0], 0, 0, 1);
                    // Other objects are printed into a string
                    else {
                        char *outstr = (char *) malloc(65536);
                        pplObjPrint(context, &args[0], NULL, outstr, 65536, 0, 0);
                        pplObjStr(out, 0, 1, outstr);
                    }
                } else {
                    ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                      "The string object constructor takes either zero or one arguments; %d supplied.",
                                      nArgs);
                    ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                }
                goto cleanup;
            case PPLOBJ_BOOL:
                // boolean(): true, or boolean(x): x cast to a boolean
                if (nArgs == 0) {
                    pplObjBool(out, 0, 1);
                } else if (nArgs == 1) {
                    ppl_castToBool(context, &args[0]);
                    pplObjCpy(out, &args[0], 0, 0, 1);
                } else {
                    ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                      "The boolean object constructor takes either zero or one arguments; %d supplied.",
                                      nArgs);
                    ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                }
                goto cleanup;
            case PPLOBJ_DATE:
                // date(): now, or date(x): a copy of a date
                if (nArgs == 0) {
                    pplObjDate(out, 0, (double) time(NULL));
                } else if (nArgs == 1) {
                    if (args[0].objType == PPLOBJ_DATE) pplObjDate(out, 0, args[0].real);
                    // Other arguments are an error
                    else {
                        ppl_snprintfTrunc(
                                context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                "The first argument to the date object constructor should be a date; an object of type <%s> was supplied.",
                                pplObjTypeNames[args[0].objType]);
                        ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                    }
                } else {
                    ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                      "The date object constructor takes zero or one arguments; %d supplied.", nArgs);
                    ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                }
                goto cleanup;
            case PPLOBJ_COL:
                // color(): black, or color(x): a copy of a color or palette color
                if (nArgs == 0) {
                    pplObjColor(out, 0, SW_COLSPACE_RGB, 0, 0, 0, 0);
                } else if (nArgs == 1) {
                    // Colors, and palette numbers, can be converted
                    if ((args[0].objType == PPLOBJ_COL) || (args[0].objType == PPLOBJ_NUM)) {
                        unsigned char d1, d2;
                        int i1, i2;
                        pplObjColor(out, 0, SW_COLSPACE_RGB, 0, 0, 0, 0);
                        ppl_colorFromObj(context, &args[0], &i1, &i2, NULL, &out->exponent[8], &out->exponent[9],
                                         &out->exponent[10], &out->exponent[11], &d1, &d2, NULL);
                        out->exponent[2] = i1;
                        out->exponent[0] = i2;
                    } else {
                        ppl_snprintfTrunc(
                                context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                "The first argument to the color object constructor should be a color; an object of type <%s> was supplied.",
                                pplObjTypeNames[args[0].objType]);
                        ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                    }
                } else {
                    ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                      "The color object constructor takes zero or one arguments; %d supplied.", nArgs);
                    ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                }
                goto cleanup;
            case PPLOBJ_DICT:
                // dict(): an empty dictionary, or dict(d): a copy of a dictionary
                if (nArgs == 0) {
                    pplObjDict(out, 0, 1, NULL);
                } else if (nArgs == 1) {
                    if (args[0].objType == PPLOBJ_DICT) pplObjDeepCpy(out, &args[0], 0, 0, 1);
                    // Other arguments are an error
                    else {
                        ppl_snprintfTrunc(
                                context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                "The first argument to the dictionary object constructor should be a dictionary; an object of type <%s> was supplied.",
                                pplObjTypeNames[args[0].objType]);
                        ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                    }
                } else {
                    ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                      "The dictionary object constructor takes zero or one arguments; %d supplied.",
                                      nArgs);
                    ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                }
                goto cleanup;
            case PPLOBJ_MOD:
            case PPLOBJ_USER:
                // module(): an empty module, or module(m): a module made from a module or dictionary
                if (nArgs == 0) {
                    pplObjModule(out, 0, 1, 0);
                } else if (nArgs == 1) {
                    if ((args[0].objType == PPLOBJ_MOD) || (args[0].objType == PPLOBJ_USER) ||
                        (args[0].objType == PPLOBJ_DICT)) {
                        pplObjDeepCpy(out, &args[0], 0, 0, 1);
                        out->objType = PPLOBJ_MOD;
                    } else {
                        ppl_snprintfTrunc(
                                context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                "The first argument to the module/instance object constructor should be a module or instance; an object of type <%s> was supplied.",
                                pplObjTypeNames[args[0].objType]);
                        ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                    }
                } else {
                    ppl_snprintfTrunc(
                            context->errStat.errBuff, sizeof(context->errStat.errBuff),
                            "The module/instance object constructor takes zero or one arguments; %d supplied.", nArgs);
                    ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                }
                goto cleanup;
            case PPLOBJ_FILE:
                // fileHandle(): a new temporary file, or fileHandle(f): a copy of a file handle
                if (nArgs == 0) {
                    pplObjFile(out, 0, 1, tmpfile(), 0);
                } else if (nArgs == 1) {
                    if (args[0].objType == PPLOBJ_FILE) pplObjCpy(out, &args[0], 0, 0, 1);
                    // Other arguments are an error
                    else {
                        ppl_snprintfTrunc(
                                context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                "The first argument to the file object constructor should be a file object; an object of type <%s> was supplied.",
                                pplObjTypeNames[args[0].objType]);
                        ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                    }
                } else {
                    sprintf(context->errStat.errBuff,
                            "The file object constructor takes zero or one arguments; %d supplied.", nArgs);
                    ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                }
                goto cleanup;
            case PPLOBJ_TYPE:
                ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                  "Creation of new data type objects is not permitted.");
                ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                goto cleanup;
            case PPLOBJ_FUNC:
                ppl_snprintfTrunc(
                        context->errStat.errBuff, sizeof(context->errStat.errBuff),
                        "New function objects must be created with the syntax f(x)=... or subroutine f(x) { ... }.");
                ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                goto cleanup;
            case PPLOBJ_EXC:
                // exception(s): an exception with the message s, or a copy of an exception
                if (nArgs == 1) {
                    // An exception made from a message
                    if (args[0].objType == PPLOBJ_STR) {
                        pplObjCpy(out, &args[0], 0, 0, 1);
                        pplObjException(out, 0, 1, (char *) out->auxil, ERR_GENERIC);
                    } else if (args[0].objType == PPLOBJ_EXC) {
                        pplObjCpy(out, &args[0], 0, 0, 1);
                    } else {
                        sprintf(context->errStat.errBuff,
                                "The first argument to the exception object constructor should be a string; an object of type <%s> was supplied.",
                                pplObjTypeNames[args[0].objType]);
                        ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                    }
                } else {
                    ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                      "The exception object constructor takes one argument; %d supplied.", nArgs);
                    ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                }
                goto cleanup;
            case PPLOBJ_NULL:
                // null() takes no arguments
                if (nArgs == 0) {
                    pplObjNull(out, 0);
                } else {
                    sprintf(context->errStat.errBuff, "The null object constructor takes zero arguments; %d supplied.",
                            nArgs);
                    ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                }
                goto cleanup;
            case PPLOBJ_LIST:
                // list(l) copies a list; list(v) turns a vector into a list; list(a,b,...) makes a list of its
                // arguments
                if ((nArgs == 1) && (args[0].objType == PPLOBJ_LIST)) {
                    pplObjDeepCpy(out, &args[0], 0, 0, 1);
                    goto cleanup;
                }
                // The output is a new list
                if (pplObjList(out, 0, 1, NULL) == NULL) {
                    ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff), "Out of memory.");
                    ppl_tbAddExpr(context, inExpr, ERR_MEMORY, inExprCharPos, "");
                    goto cleanup;
                }
                if (nArgs < 1) goto cleanup;
                // Each element of the vector becomes an item of the list
                if ((nArgs == 1) && (args[0].objType == PPLOBJ_VEC)) {
                    int i;
                    gsl_vector *vec = ((pplVector *) args[0].auxil)->v;
                    const int l = vec->size;
                    pplObj v;
                    list *lo = (list *) out->auxil;
                    v.refCount = 1;
                    pplObjNum(&v, 1, 0, 0);
                    ppl_unitsDimCpy(&v, &args[0]);
                    // Append each element, as a number with the vector's units
                    for (i = 0; i < l; i++) {
                        v.real = gsl_vector_get(vec, i);
                        ppl_listAppendCpy(lo, &v, sizeof(pplObj));
                    }
                } else {
                    int i;
                    // Append a copy of each argument
                    for (i = 0; i < nArgs; i++) {
                        pplObj v;
                        list *lo = (list *) out->auxil;
                        v.refCount = 1;
                        pplObjCpy(&v, &args[i], 0, 1, 1);
                        ppl_listAppendCpy(lo, &v, sizeof(pplObj));
                    }
                }
                goto cleanup;
            case PPLOBJ_VEC:
                // vector(): a vector containing one zero
                if (nArgs < 1) {
                    pplObjVector(out, 0, 1, 1);
                    gsl_vector_set(((pplVector *) out->auxil)->v, 0, 0);
                    goto cleanup;
                }
                // vector(v) copies a vector; vector(n) makes n zeros; vector(l) turns a list into a vector;
                // vector(a,b,...) makes a vector of its arguments
                if (nArgs == 1) {
                    // A copy of a vector
                    if (args[0].objType == PPLOBJ_VEC) {
                        pplObjDeepCpy(out, &args[0], 0, 0, 1);
                        goto cleanup;
                    } else if (args[0].objType == PPLOBJ_NUM) {
                        long i;
                        // The length must be a real, dimensionless, positive integer
                        if (!args[0].dimensionless) {
                            ppl_snprintfTrunc(
                                    context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                    "Specified length of vector should be dimensionless; supplied length has units of <%s>.",
                                    ppl_printUnit(context, &args[0], NULL, NULL, 0, 1, 0));
                            ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                            goto cleanup;
                        }
                        // The length must be real
                        if (args[0].flagComplex) {
                            ppl_snprintfTrunc(
                                    context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                    "Specified length of vector should be real; supplied length is complex number.");
                            ppl_tbAddExpr(context, inExpr, ERR_NUMERICAL, inExprCharPos, "");
                            goto cleanup;
                        }
                        // The length must be positive
                        if ((args[0].real < 1) || (args[0].real > INT_MAX)) {
                            ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                              "Specified length of vector should be in the range 1<len<%d.", INT_MAX);
                            ppl_tbAddExpr(context, inExpr, ERR_RANGE, inExprCharPos, "");
                            goto cleanup;
                        }
                        long len = floor(args[0].real);
                        // Make a vector of zeros
                        if (pplObjVector(out, 0, 1, len) == NULL) {
                            ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                              "Out of memory.");
                            ppl_tbAddExpr(context, inExpr, ERR_MEMORY, inExprCharPos, "");
                            goto cleanup;
                        }
                        gsl_vector *v = ((pplVector *) out->auxil)->v;
                        for (i = 0; i < len; i++) { gsl_vector_set(v, i, 0); }
                        goto cleanup;
                    } else if (args[0].objType == PPLOBJ_LIST) {
                        list *listin = (list *) args[0].auxil;
                        listIterator *li = ppl_listIterateInit(listin);
                        const long len = listin->length;
                        // A vector cannot be empty
                        if (len == 0) {
                            ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                              "Cannot create a vector of length zero.");
                            ppl_tbAddExpr(context, inExpr, ERR_RANGE, inExprCharPos, "");
                            goto cleanup;
                        }
                        // Make a vector as long as the list
                        if (pplObjVector(out, 0, 1, len) == NULL) {
                            ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                              "Out of memory.");
                            ppl_tbAddExpr(context, inExpr, ERR_MEMORY, inExprCharPos, "");
                            goto cleanup;
                        }
                        // Copy each item into the vector
                        for (long i = 0; i < len; i++)
                            if (vectorSetItem(context, inExpr, inExprCharPos, out, i, (pplObj *) ppl_listIterate(&li)))
                                goto cleanup;
                        goto cleanup;
                    }
                }
                // List is to made up from arguments supplied to constructor
                {
                    // Make a vector with one element per argument
                    if (pplObjVector(out, 0, 1, nArgs) == NULL) {
                        ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff), "Out of memory.");
                        ppl_tbAddExpr(context, inExpr, ERR_MEMORY, inExprCharPos, "");
                        goto cleanup;
                    }
                    // Copy each argument into the vector
                    for (long i = 0; i < nArgs; i++)
                        if (vectorSetItem(context, inExpr, inExprCharPos, out, i, &args[i])) goto cleanup;
                    goto cleanup;
                }
            case PPLOBJ_MAT:
                // matrix(): a 1x1 matrix containing zero. Otherwise a matrix may be copied from a matrix, given a size,
                // or made from a list of column vectors or a list of lists of rows.
                if (nArgs < 1) {
                    pplObjMatrix(out, 0, 1, 1, 1);
                    gsl_matrix_set(((pplMatrix *) out->auxil)->m, 0, 0, 0);
                    goto cleanup;
                } else if (args[0].objType == PPLOBJ_MAT) {
                    // A matrix is copied from exactly one other matrix
                    if (nArgs != 1) {
                        ppl_snprintfTrunc(
                                context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                "When initialising a matrix from another matrix, only one argument should be supplied (the source matrix). %d have been provided.",
                                nArgs);
                        ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                        goto cleanup;
                    }
                    pplObjDeepCpy(out, &args[0], 0, 0, 1);
                    goto cleanup;
                } else if (args[0].objType == PPLOBJ_NUM) {
                    int i, j;
                    // matrix(rows, columns) needs two real, dimensionless, positive integers
                    if (nArgs != 2) {
                        ppl_snprintfTrunc(
                                context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                "When specifying the size of a matrix, two numerical arguments must be supplied. %d have been provided.",
                                nArgs);
                        ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                        goto cleanup;
                    }
                    // The second size must be a number
                    if (args[1].objType != PPLOBJ_NUM) {
                        ppl_snprintfTrunc(
                                context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                "When specifying the size of a matrix, two numerical arguments must be supplied. Second argument has type <%s>.",
                                pplObjTypeNames[args[1].objType]);
                        ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                        goto cleanup;
                    }
                    // The first size must be dimensionless
                    if (!args[0].dimensionless) {
                        ppl_snprintfTrunc(
                                context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                "When specifying the size of a matrix, both numerical arguments must be dimensionless. First has units of <%s>.",
                                ppl_printUnit(context, &args[0], NULL, NULL, 1, 1, 0));
                        ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                        goto cleanup;
                    }
                    // The second size must be dimensionless
                    if (!args[1].dimensionless) {
                        ppl_snprintfTrunc(
                                context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                "When specifying the size of a matrix, both numerical arguments must be dimensionless. Second has units of <%s>.",
                                ppl_printUnit(context, &args[1], NULL, NULL, 1, 1, 0));
                        ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                        goto cleanup;
                    }
                    // Both sizes must be real
                    if ((args[0].flagComplex) || (args[1].flagComplex)) {
                        ppl_snprintfTrunc(
                                context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                "When specifying the size of a matrix, both arguments must be real numbers. Supplied arguments are complex.");
                        ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                        goto cleanup;
                    }
                    int s1 = args[0].real;
                    int s2 = args[1].real;
                    // Both sizes must be positive
                    if ((s1 < 1) || (s1 > INT_MAX)) {
                        ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                          "Specified dimension of vector should be in the range 1<len<%d.", INT_MAX);
                        ppl_tbAddExpr(context, inExpr, ERR_RANGE, inExprCharPos, "");
                        goto cleanup;
                    }
                    // The second size must be positive
                    if ((s2 < 1) || (s2 > INT_MAX)) {
                        ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                          "Specified dimension of vector should be in the range 1<len<%d.", INT_MAX);
                        ppl_tbAddExpr(context, inExpr, ERR_RANGE, inExprCharPos, "");
                        goto cleanup;
                    }
                    // Make the matrix, filled with zeros
                    if (pplObjMatrix(out, 0, 1, s1, s2) == NULL) {
                        ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff), "Out of memory.");
                        ppl_tbAddExpr(context, inExpr, ERR_MEMORY, inExprCharPos, "");
                        goto cleanup;
                    }
                    gsl_matrix *m = ((pplMatrix *) out->auxil)->m;
                    for (i = 0; i < s1; i++)
                        for (j = 0; j < s2; j++) { gsl_matrix_set(m, i, j, 0); }
                    goto cleanup;
                } else if (args[0].objType == PPLOBJ_VEC) {
                    long i;
                    const long s1 = ((pplVector *) args[0].auxil)->v->size;
                    const long s2 = nArgs;
                    // matrix(v1, v2, ...): each vector becomes a column of the matrix
                    if (pplObjMatrix(out, 0, 1, s1, s2) == NULL) {
                        ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff), "Out of memory.");
                        ppl_tbAddExpr(context, inExpr, ERR_MEMORY, inExprCharPos, "");
                        goto cleanup;
                    }
                    gsl_matrix *m = ((pplMatrix *) out->auxil)->m;
                    ppl_unitsDimCpy(out, &args[0]);
                    // Copy each vector into a column
                    for (i = 0; i < s2; i++) {
                        long j;
                        // Every argument must be a vector
                        if (args[i].objType != PPLOBJ_VEC) {
                            ppl_snprintfTrunc(
                                    context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                    "When initialising a matrix from a list of vectors, all arguments must be vectors. Supplied argument has type <%s>.",
                                    pplObjTypeNames[args[i].objType]);
                            ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                            goto cleanup;
                        }
                        gsl_vector *v = ((pplVector *) args[i].auxil)->v;
                        // All the vectors must have the same length
                        if (v->size != s1) {
                            ppl_snprintfTrunc(
                                    context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                    "When initialising a matrix from a list of vectors, the vectors must have consistent lengths. Supplied vector has length %ld whereas previous arguments have had a length %ld.",
                                    (long) v->size, s1);
                            ppl_tbAddExpr(context, inExpr, ERR_RANGE, inExprCharPos, "");
                            goto cleanup;
                        }
                        // All the vectors must have the same units
                        if (!ppl_unitsDimEqual(out, &args[i])) {
                            ppl_snprintfTrunc(
                                    context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                    "When initialising a matrix from a list of vectors, the vectors must all have the same dimensions. Supplied vectors have units of <%s> and <%s>.",
                                    ppl_printUnit(context, out, NULL, NULL, 0, 1, 0),
                                    ppl_printUnit(context, &args[i], NULL, NULL, 1, 1, 0));
                            ppl_tbAddExpr(context, inExpr, ERR_UNIT, inExprCharPos, "");
                        }
                        for (j = 0; j < s1; j++) { gsl_matrix_set(m, j, i, gsl_vector_get(v, j)); }
                    }
                    goto cleanup;
                } else if (args[0].objType == PPLOBJ_LIST) {
                    list *listin = (list *) args[0].auxil;
                    listIterator *li = ppl_listIterateInit(listin);
                    const long len = listin->length;
                    // matrix([...], [...], ...): each list becomes a row of the matrix
                    if (len == 0) {
                        ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                          "Cannot create a matrix of dimension zero.");
                        ppl_tbAddExpr(context, inExpr, ERR_MEMORY, inExprCharPos, "");
                        goto cleanup;
                    }
                    pplObj *item = (pplObj *) listin->first->data;
                    // Lists of numbers
                    if (item->objType == PPLOBJ_NUM) {
                        long i;
                        const long s1 = len;
                        const long s2 = nArgs;
                        for (i = 0; i < s2; i++)
                            // Every argument must be a list
                            if (args[i].objType != PPLOBJ_LIST) {
                                ppl_snprintfTrunc(
                                        context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                        "When initialising a matrix from a list of lists, all arguments must be lists. Supplied argument has type <%s>.",
                                        pplObjTypeNames[args[i].objType]);
                                ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                                goto cleanup;
                            }
                        // Make the matrix, with one row per list
                        if (pplObjMatrix(out, 0, 1, s2, s1) == NULL) {
                            ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                              "Out of memory.");
                            ppl_tbAddExpr(context, inExpr, ERR_MEMORY, inExprCharPos, "");
                            goto cleanup;
                        }
                        gsl_matrix *m = ((pplMatrix *) out->auxil)->m;
                        ppl_unitsDimCpy(out, item);
                        // Copy each list into a row
                        for (i = 0; i < s2; i++) {
                            long j;
                            list *listin = (list *) args[i].auxil;
                            listIterator *li = ppl_listIterateInit(listin);
                            // All the lists must have the same length
                            if (listin->length != s1) {
                                ppl_snprintfTrunc(
                                        context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                        "When initialising a matrix from a list of lists, the lists must have consistent lengths. Supplied list has length %ld whereas previous lists have had a length %ld.",
                                        (long) listin->length, s1);
                                ppl_tbAddExpr(context, inExpr, ERR_RANGE, inExprCharPos, "");
                                goto cleanup;
                            }
                            // Each item must be a real number with the same units as the first
                            for (j = 0; j < s1; j++) {
                                pplObj *item = (pplObj *) ppl_listIterate(&li);
                                // Report an item which is not a number
                                if (item->objType != PPLOBJ_NUM) {
                                    ppl_snprintfTrunc(
                                            context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                            "When initialising a matrix from a list of lists, the elements must all be numerical values; supplied element has type <%s>.",
                                            pplObjTypeNames[item->objType]);
                                    ppl_tbAddExpr(context, inExpr, ERR_RANGE, inExprCharPos, "");
                                    goto cleanup;
                                }
                                // Every element must have the same units
                                if (!ppl_unitsDimEqual(out, item)) {
                                    ppl_snprintfTrunc(
                                            context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                            "When initialising a matrix from a list of lists, all of the elements must have the same dimensions. Supplied elements have units of <%s> and <%s>.",
                                            ppl_printUnit(context, out, NULL, NULL, 0, 1, 0),
                                            ppl_printUnit(context, item, NULL, NULL, 1, 1, 0));
                                    ppl_tbAddExpr(context, inExpr, ERR_UNIT, inExprCharPos, "");
                                    goto cleanup;
                                }
                                // Matrices cannot hold complex numbers
                                if (item->flagComplex) {
                                    ppl_snprintfTrunc(
                                            context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                            "Matrices can only hold real numbers; supplied elements are complex.");
                                    ppl_tbAddExpr(context, inExpr, ERR_NUMERICAL, inExprCharPos, "");
                                    goto cleanup;
                                }
                                gsl_matrix_set(m, i, j, item->real);
                            }
                        }
                        goto cleanup;
                    } else if (item->objType == PPLOBJ_VEC) {
                        long i;
                        const long s1 = ((pplVector *) item->auxil)->v->size;
                        const long s2 = len;
                        // matrix([v1, v2, ...]): a single list of vectors, each of which becomes a column
                        if (nArgs != 1) {
                            ppl_snprintfTrunc(
                                    context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                    "When initialising a matrix from a list of vectors, only one argument should be supplied. %d arguments were supplied.",
                                    nArgs);
                            ppl_tbAddExpr(context, inExpr, ERR_RANGE, inExprCharPos, "");
                            goto cleanup;
                        }
                        // Make the matrix
                        if (pplObjMatrix(out, 0, 1, s1, s2) == NULL) {
                            ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                              "Out of memory.");
                            ppl_tbAddExpr(context, inExpr, ERR_MEMORY, inExprCharPos, "");
                            goto cleanup;
                        }
                        gsl_matrix *m = ((pplMatrix *) out->auxil)->m;
                        ppl_unitsDimCpy(out, item);
                        // Copy each vector into a column
                        for (i = 0; i < s2; i++) {
                            long j;
                            pplObj *item = (pplObj *) ppl_listIterate(&li);
                            // Every item must be a vector
                            if (item->objType != PPLOBJ_VEC) {
                                ppl_snprintfTrunc(
                                        context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                        "When initialising a matrix from a list of vectors, all arguments must be vectors. Supplied argument has type <%s>.",
                                        pplObjTypeNames[item->objType]);
                                ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                                goto cleanup;
                            }
                            gsl_vector *v = ((pplVector *) item->auxil)->v;
                            // All the vectors must have the same length
                            if (v->size != s1) {
                                ppl_snprintfTrunc(
                                        context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                        "When initialising a matrix from a list of vectors, the vectors must have consistent lengths. Supplied vector has length %ld whereas previous arguments have had a length %ld.",
                                        (long) v->size, s1);
                                ppl_tbAddExpr(context, inExpr, ERR_RANGE, inExprCharPos, "");
                                goto cleanup;
                            }
                            // All the vectors must have the same units
                            if (!ppl_unitsDimEqual(out, item)) {
                                ppl_snprintfTrunc(
                                        context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                        "When initialising a matrix from a list of vectors, the vectors must all have the same dimensions. Supplied vectors have units of <%s> and <%s>.",
                                        ppl_printUnit(context, out, NULL, NULL, 0, 1, 0),
                                        ppl_printUnit(context, item, NULL, NULL, 1, 1, 0));
                                ppl_tbAddExpr(context, inExpr, ERR_UNIT, inExprCharPos, "");
                                goto cleanup;
                            }
                            for (j = 0; j < s1; j++) { gsl_matrix_set(m, j, i, gsl_vector_get(v, j)); }
                        }
                        goto cleanup;
                    } else if (item->objType == PPLOBJ_LIST) {
                        long i;
                        const long s1 = ((list *) item->auxil)->length;
                        const long s2 = len;
                        // matrix([[...], [...], ...]): a single list of lists, each of which becomes a row
                        if (nArgs != 1) {
                            ppl_snprintfTrunc(
                                    context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                    "When initialising a matrix from a list of lists, only one argument should be supplied. %d arguments were supplied.",
                                    nArgs);
                            ppl_tbAddExpr(context, inExpr, ERR_RANGE, inExprCharPos, "");
                            goto cleanup;
                        }
                        // The rows cannot be empty
                        if (s1 == 0) {
                            ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                              "Cannot create a matrix of dimension zero.");
                            ppl_tbAddExpr(context, inExpr, ERR_MEMORY, inExprCharPos, "");
                            goto cleanup;
                        }
                        // Make the matrix
                        if (pplObjMatrix(out, 0, 1, s2, s1) == NULL) {
                            ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                              "Out of memory.");
                            ppl_tbAddExpr(context, inExpr, ERR_MEMORY, inExprCharPos, "");
                            goto cleanup;
                        }
                        gsl_matrix *m = ((pplMatrix *) out->auxil)->m;
                        ppl_unitsDimCpy(out, ((pplObj *) ((list *) item->auxil)->first->data));
                        // Copy each list into a row
                        for (i = 0; i < s2; i++) {
                            long j;
                            pplObj *item = (pplObj *) ppl_listIterate(&li);
                            // Every item must be a list
                            if (item->objType != PPLOBJ_LIST) {
                                ppl_snprintfTrunc(
                                        context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                        "When initialising a matrix from a list of lists, all arguments must be lists. Supplied argument has type <%s>.",
                                        pplObjTypeNames[item->objType]);
                                ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                                goto cleanup;
                            }
                            list *listin2 = (list *) item->auxil;
                            listIterator *li2 = ppl_listIterateInit(listin2);
                            // All the lists must have the same length
                            if (listin2->length != s1) {
                                ppl_snprintfTrunc(
                                        context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                        "When initialising a matrix from a list of lists, the lists must have consistent lengths. Supplied list has length %ld whereas previous lists have had a length %ld.",
                                        (long) listin2->length, s1);
                                ppl_tbAddExpr(context, inExpr, ERR_RANGE, inExprCharPos, "");
                                goto cleanup;
                            }
                            // Each element must be a real number with the same units as the first
                            for (j = 0; j < s1; j++) {
                                pplObj *item2 = (pplObj *) ppl_listIterate(&li2);
                                // Report an element which is not a number
                                if (item2->objType != PPLOBJ_NUM) {
                                    ppl_snprintfTrunc(
                                            context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                            "When initialising a matrix from a list of lists, the elements must all be numerical values; supplied element has type <%s>.",
                                            pplObjTypeNames[item2->objType]);
                                    ppl_tbAddExpr(context, inExpr, ERR_RANGE, inExprCharPos, "");
                                    goto cleanup;
                                }
                                // Report an element with different units
                                if (!ppl_unitsDimEqual(out, item2)) {
                                    ppl_snprintfTrunc(
                                            context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                            "When initialising a matrix from a list of lists, all of the elements must have the same dimensions. Supplied elements have units of <%s> and <%s>.",
                                            ppl_printUnit(context, out, NULL, NULL, 0, 1, 0),
                                            ppl_printUnit(context, item2, NULL, NULL, 1, 1, 0));
                                    ppl_tbAddExpr(context, inExpr, ERR_UNIT, inExprCharPos, "");
                                    goto cleanup;
                                }
                                // Matrices cannot hold complex numbers
                                if (item->flagComplex) {
                                    ppl_snprintfTrunc(
                                            context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                            "Matrices can only hold real numbers; supplied elements are complex.");
                                    ppl_tbAddExpr(context, inExpr, ERR_NUMERICAL, inExprCharPos, "");
                                    goto cleanup;
                                }
                                gsl_matrix_set(m, i, j, item2->real);
                            }
                        }
                        goto cleanup;
                    } else {
                        ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                          "Cannot initialise a matrix from an object of type <%s>.",
                                          pplObjTypeNames[item->objType]);
                        ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                        goto cleanup;
                    }
                } else {
                    ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                      "Cannot initialise a matrix from an object of type <%s>.",
                                      pplObjTypeNames[args[0].objType]);
                    ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                    goto cleanup;
                }
                goto cleanup;
        }
    }

    // Otherwise object being called must be a function object
    if (t != PPLOBJ_FUNC) {
        ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                          "Object of type <%s> cannot be called as a function.", pplObjTypeNames[t]);
        ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
        goto cleanup;
    }
    pplFunc *fn = (pplFunc *) called.auxil;

    // Magic functions take their arguments as unevaluated text: unit(), diff_dx() and int_dx(), told apart by their
    // minimum number of arguments
    if (fn->functionType == PPL_FUNC_MAGIC) {
        if (fn->minArgs == 1) // unit()
        {
            int end, errPos = -1;
            char *u = (char *) context->stack[context->stackPtr - 1].auxil;
            // unit() takes one argument, the text of the units
            if (nArgs != 1) {
                ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                  "The unit() function takes exactly one argument; %d supplied.", nArgs);
                ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                goto cleanup;
            }
            ppl_unitsStringEvaluate(context, u, out, &end, &errPos, context->errStat.errBuff);
            // Report units which could not be parsed
            if (errPos >= 0) {
                ppl_tbAddExpr(context, inExpr, ERR_UNIT, inExprCharPos, "");
                goto cleanup;
            }
            // The whole of the text must be units
            if (end != strlen(u)) {
                ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                  "Unexpected trailing matter after unit string.");
                ppl_tbAddExpr(context, inExpr, ERR_UNIT, inExprCharPos, "");
                goto cleanup;
            }
        } else if (fn->minArgs == 2) // diff_d()
        {
            pplObj v, *step;
            int k;
            for (k = inExprCharPos; ((inExpr->ascii[k] != '(') && (inExpr->ascii[k] != '\0')); k++);
            if (inExpr->ascii[k] == '(') k++;
            for (; ((inExpr->ascii[k] > '\0') && ((unsigned char) inExpr->ascii[k] <= ' ')); k++);
            if (nArgs == 3) step = &v;
            else if (nArgs == 4) step = &args[3];
            // diff_dx(expr, x) or diff_dx(expr, x, step); the first argument is the name of the dummy variable,
            // inserted by the compiler
            else {
                ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                  "The diff_d() function takes two or thee arguments; %d supplied.", nArgs - 1);
                ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                goto cleanup;
            }
            // The compiler passes the dummy variable and the expression as strings
            if (args[0].objType != PPLOBJ_STR) {
                ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                  "Dummy variable not passed to diff_d() as a string");
                ppl_tbAddExpr(context, inExpr, ERR_INTERNAL, inExprCharPos, "");
                goto cleanup;
            }
            // The expression to differentiate
            if (args[1].objType != PPLOBJ_STR) {
                ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                  "Differentiation expression not passed to diff_d() as a string");
                ppl_tbAddExpr(context, inExpr, ERR_INTERNAL, inExprCharPos, "");
                goto cleanup;
            }
            // The point at which to differentiate must be a number
            if (args[2].objType != PPLOBJ_NUM) {
                ppl_snprintfTrunc(
                        context->errStat.errBuff, sizeof(context->errStat.errBuff),
                        "The diff_d() function requires a number as its second argument; supplied argument has type <%s>.",
                        pplObjTypeNames[args[2].objType]);
                ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                goto cleanup;
            }
            // The step, if given, must be a number
            if ((nArgs == 4) && (args[3].objType != PPLOBJ_NUM)) {
                ppl_snprintfTrunc(
                        context->errStat.errBuff, sizeof(context->errStat.errBuff),
                        "The diff_d() function requires a number as its third argument; supplied argument has type <%s>.",
                        pplObjTypeNames[args[3].objType]);
                ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                goto cleanup;
            }
            pplObj *xpos = &args[2];
            memcpy(&v, xpos, sizeof(pplObj));
            v.imag = 0;
            v.real = hypot(xpos->real, xpos->imag) * 1e-6;
            v.flagComplex = 0;
            if (v.real < DBL_MIN * 1e6) v.real = 1e-6;
            ppl_expDifferentiate(context, inExpr, inExprCharPos, (char *) args[1].auxil, k, (char *) args[0].auxil,
                                 xpos, step, out, dollarAllowed, iterDepth);
            if (context->errStat.status) { goto cleanup; }
        } else if (fn->minArgs == 3) // int_d()
        {
            int k;
            for (k = inExprCharPos; ((inExpr->ascii[k] != '(') && (inExpr->ascii[k] != '\0')); k++);
            if (inExpr->ascii[k] == '(') k++;
            for (; ((inExpr->ascii[k] > '\0') && ((unsigned char) inExpr->ascii[k] <= ' ')); k++);
            // int_dx(expr, min, max); the first argument is the name of the dummy variable, inserted by the compiler
            if (nArgs != 4) {
                ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                  "The int_d() function takes two or three arguments; %d supplied.", nArgs - 1);
                ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                goto cleanup;
            }
            // The compiler passes the dummy variable and the expression as strings
            if (args[0].objType != PPLOBJ_STR) {
                ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                  "Dummy variable not passed to int_d() as a string");
                ppl_tbAddExpr(context, inExpr, ERR_INTERNAL, inExprCharPos, "");
                goto cleanup;
            }
            // The expression to integrate
            if (args[1].objType != PPLOBJ_STR) {
                ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                  "Integration expression not passed to diff_d() as a string");
                ppl_tbAddExpr(context, inExpr, ERR_INTERNAL, inExprCharPos, "");
                goto cleanup;
            }
            // The lower limit must be a number
            if (args[2].objType != PPLOBJ_NUM) {
                ppl_snprintfTrunc(
                        context->errStat.errBuff, sizeof(context->errStat.errBuff),
                        "The int_d() function requires a number as its second argument; supplied argument has type <%s>.",
                        pplObjTypeNames[args[2].objType]);
                ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                goto cleanup;
            }
            // The upper limit must be a number
            if (args[3].objType != PPLOBJ_NUM) {
                ppl_snprintfTrunc(
                        context->errStat.errBuff, sizeof(context->errStat.errBuff),
                        "The int_d() function requires a number as its third argument; supplied argument has type <%s>.",
                        pplObjTypeNames[args[3].objType]);
                ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                goto cleanup;
            }
            ppl_expIntegrate(context, inExpr, inExprCharPos, (char *) args[1].auxil, k, (char *) args[0].auxil,
                             &args[2], &args[3], out, dollarAllowed, iterDepth);
            if (context->errStat.status) { goto cleanup; }
        }
    } else {
        // Check the number of arguments. Errors in calling functions give NaN instead if errors are switched off.
        if (fn->minArgs == fn->maxArgs) {
            // The function takes an exact number of arguments
            if (nArgs != fn->maxArgs) {
                // Report the error, or return NaN
                if (context->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
                    ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                      "Function takes exactly %d arguments; %d supplied.", fn->maxArgs, nArgs);
                    ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                    goto cleanup;
                } else {
                    context->stack[context->stackPtr - 1 - nArgs].real = GSL_NAN;
                    goto cleanup;
                }
            }
        } else if (nArgs < fn->minArgs) {
            // Report the error, or return NaN
            if (context->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
                ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                  "Function takes a minimum of %d arguments; %d supplied.", fn->minArgs, nArgs);
                ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                goto cleanup;
            } else {
                context->stack[context->stackPtr - 1 - nArgs].real = GSL_NAN;
                goto cleanup;
            }
        } else if (nArgs > fn->maxArgs) {
            // Report the error, or return NaN
            if (context->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
                ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                  "Function takes a maximum of %d arguments; %d supplied.", fn->maxArgs, nArgs);
                ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                goto cleanup;
            } else {
                context->stack[context->stackPtr - 1 - nArgs].real = GSL_NAN;
                goto cleanup;
            }
        }

        // Check the arguments against the restrictions which the function declares: numbers only, real only,
        // dimensionless only, and finite only
        if (fn->numOnly)
            for (i = 0; i < nArgs; i++)
                // Report an argument which is not a number
                if (args[i].objType != PPLOBJ_NUM) {
                    // Report the error, or return NaN
                    if (context->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
                        ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                          "Function required numeric arguments; argument %d has type <%s>.", i + 1,
                                          pplObjTypeNames[args[i].objType]);
                        ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                        goto cleanup;
                    } else {
                        context->stack[context->stackPtr - 1 - nArgs].real = GSL_NAN;
                        goto cleanup;
                    }
                }

        // Real numbers only
        if (fn->realOnly)
            for (i = 0; i < nArgs; i++)
                // Report a complex argument
                if (args[i].flagComplex) {
                    // Report the error, or return NaN
                    if (context->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
                        ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                          "Function requires real arguments; argument %d is complex.", i + 1);
                        ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                        goto cleanup;
                    } else {
                        context->stack[context->stackPtr - 1 - nArgs].real = GSL_NAN;
                        goto cleanup;
                    }
                }

        // Dimensionless numbers only
        if (fn->dimlessOnly)
            for (i = 0; i < nArgs; i++)
                // Report an argument with units
                if (!args[i].dimensionless) {
                    // Report the error, or return NaN
                    if (context->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
                        ppl_snprintfTrunc(
                                context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                "Function requires dimensionless arguments; argument %d has dimensions of <%s>.", i + 1,
                                ppl_printUnit(context, &args[i], NULL, NULL, 1, 1, 0));
                        ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                        goto cleanup;
                    } else {
                        context->stack[context->stackPtr - 1 - nArgs].real = GSL_NAN;
                        goto cleanup;
                    }
                }

        // Finite numbers only
        if (fn->notNan)
            for (i = 0; i < nArgs; i++)
                // Report an argument which is not finite
                if ((!gsl_finite(args[i].real)) || (!gsl_finite(args[i].imag))) {
                    // Report the error, or return NaN
                    if (context->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
                        ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                          "Function requires finite arguments; argument %d is not finite.", i + 1);
                        ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                        goto cleanup;
                    } else {
                        context->stack[context->stackPtr - 1 - nArgs].real = GSL_NAN;
                        goto cleanup;
                    }
                }

        // A method must be called on the object it belongs to
        if ((fn->functionType == PPL_FUNC_SYSTEM) && (fn->needSelfThis) && (called.self_this == NULL)) {
            // Report the error, or return NaN
            if (context->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
                ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                  "Function is a method which has been detached from the object that owns it.");
                ppl_tbAddExpr(context, inExpr, ERR_TYPE, inExprCharPos, "");
                goto cleanup;
            } else {
                context->stack[context->stackPtr - 1 - nArgs].real = GSL_NAN;
                goto cleanup;
            }
        }

        // Call the function, according to its kind
        switch (fn->functionType) {
            case PPL_FUNC_SYSTEM: {
                int stat = 0, errType = -1;
                ((void (*)(ppl_context *, pplObj *, int, int *, int *, char *)) fn->functionPtr)(
                        context, args, nArgs, &stat, &errType, context->errStat.errBuff);
                // Report an error in the built-in function
                if (stat) {
                    ppl_tbAddExpr(context, inExpr, errType, inExprCharPos, "");
                    goto cleanup;
                }
                break;
            }
            case PPL_FUNC_USERDEF: {
                int checked[FUNC_MAXARGS], j;
                pplFunc *f = fn;
                for (j = 0; j < nArgs; j++) checked[j] = 0;

                while (f != NULL) // Check whether supplied arguments are within the range of this definition
                {
                    int k, l = 1;
                    // A user-defined function may have several definitions, each valid over a range of its arguments:
                    // find the first whose ranges contain the arguments
                    for (k = 0; ((k < nArgs) && l); k++) {
                        // Check the lower limit of this argument, if there is one
                        if (f->minActive[k] != 0) {
                            // Check, once, that the argument is a number with the units of the limit
                            if (!checked[k]) {
                                // Report an argument which is not a number
                                if (args[k].objType != PPLOBJ_NUM) {
                                    ppl_snprintfTrunc(
                                            context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                            "Argument %d supplied to this function is not numeric, but a numeric range is specified for this argument in the function's definition.",
                                            k + 1);
                                    ppl_tbAddExpr(context, inExpr, ERR_NUMERICAL, inExprCharPos, "");
                                    return;
                                } else if (!ppl_unitsDimEqual(f->min + k, args + k)) {
                                    ppl_snprintfTrunc(
                                            context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                            "Argument %d supplied to this function is dimensionally incompatible with the argument's specified min/max range: argument has dimensions of <%s>, meanwhile range has dimensions of <%s>.",
                                            k + 1, ppl_printUnit(context, args + k, NULL, NULL, 0, 1, 0),
                                            ppl_printUnit(context, f->min + k, NULL, NULL, 1, 1, 0));
                                    ppl_tbAddExpr(context, inExpr, ERR_UNIT, inExprCharPos, "");
                                    return;
                                } else if (args[k].flagComplex) {
                                    ppl_snprintfTrunc(
                                            context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                            "Argument %d supplied to this function must be a real number: any arguments which have min/max ranges specified must be real.",
                                            k + 1);
                                    ppl_tbAddExpr(context, inExpr, ERR_NUMERICAL, inExprCharPos, "");
                                    return;
                                } else {
                                    checked[k] = 1;
                                }
                            }
                            // Below the lower limit: try the next definition
                            if (args[k].real < f->min[k].real) {
                                f = f->next;
                                l = 0;
                                continue;
                            }
                        }
                        // Check the upper limit of this argument, if there is one
                        if (f->maxActive[k] != 0) {
                            // Check, once, that the argument is a number with the units of the limit
                            if (!checked[k]) {
                                // Report an argument which is not a number
                                if (args[k].objType != PPLOBJ_NUM) {
                                    ppl_snprintfTrunc(
                                            context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                            "Argument %d supplied to this function is not numeric, but a numeric range is specified for this argument in the function's definition.",
                                            k + 1);
                                    ppl_tbAddExpr(context, inExpr, ERR_NUMERICAL, inExprCharPos, "");
                                    return;
                                } else if (!ppl_unitsDimEqual(f->max + k, args + k)) {
                                    ppl_snprintfTrunc(
                                            context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                            "Argument %d supplied to this function is dimensionally incompatible with the argument's specified min/max range: argument has dimensions of <%s>, meanwhile range has dimensions of <%s>.",
                                            k + 1, ppl_printUnit(context, args + k, NULL, NULL, 0, 1, 0),
                                            ppl_printUnit(context, f->max + k, NULL, NULL, 1, 1, 0));
                                    ppl_tbAddExpr(context, inExpr, ERR_UNIT, inExprCharPos, "");
                                    return;
                                } else if (args[k].flagComplex) {
                                    ppl_snprintfTrunc(
                                            context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                            "Argument %d supplied to this function must be a real number: any arguments which have min/max ranges specified must be real.",
                                            k + 1);
                                    ppl_tbAddExpr(context, inExpr, ERR_UNIT, inExprCharPos, "");
                                    return;
                                } else {
                                    checked[k] = 1;
                                }
                            }
                            // Above the upper limit: try the next definition
                            if (args[k].real > f->max[k].real) {
                                f = f->next;
                                l = 0;
                                continue;
                            }
                        }
                    }
                    if (l) break;
                }

                // No definition covers these arguments
                if (f == NULL) {
                    // Report the error, or return NaN
                    if (context->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
                        ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                          "This function is not defined in the requested region of parameter space.");
                        ppl_tbAddExpr(context, inExpr, ERR_RANGE, inExprCharPos, "");
                        goto cleanup;
                    } else {
                        context->stack[context->stackPtr - 1 - nArgs].real = GSL_NAN;
                        goto cleanup;
                    }
                } else {
                    int k, lastOpAssign = 0, stkp = context->stackPtr, ns_ptr = context->ns_ptr;
                    const int setSelf = (called.self_this != NULL) &&
                            ((called.self_this->objType == PPLOBJ_USER) || (called.self_this->objType == PPLOBJ_MOD));

                    // If function definition is null, result is NAN
                    if (f->functionPtr == NULL) {
                        // A definition with no expression marks a region where the function was explicitly left
                        // undefined: report the error, or return NaN
                        if (context->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
                            ppl_snprintfTrunc(
                                    context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                    "This function is not defined in the requested region of parameter space.");
                            ppl_tbAddExpr(context, inExpr, ERR_RANGE, inExprCharPos, "");
                            goto cleanup;
                        } else {
                            context->stack[context->stackPtr - 1 - nArgs].real = GSL_NAN;
                            goto cleanup;
                        }
                    }

                    // Check that there's enough space on the stack
                    ppl_stackMustHave(context, nArgs + 1);
                    // Evaluating the definition needs room on the stack
                    if (context->stackFull) {
                        strcpy(context->errStat.errBuff, "Stack overflow.");
                        ppl_tbAddExpr(context, inExpr, ERR_MEMORY, inExprCharPos, "");
                        goto cleanup;
                    }
                    // ...and a new namespace, in which the arguments are bound as local variables
                    if (context->ns_ptr > CONTEXT_DEPTH - 2) {
                        strcpy(context->errStat.errBuff, "Stack overflow.");
                        ppl_tbAddExpr(context, inExpr, ERR_MEMORY, inExprCharPos, "");
                        goto cleanup;
                    }

                    // Consider entering a new local namespace if function is within a module
                    if ((called.self_this != NULL) &&
                        ((called.self_this->objType == PPLOBJ_USER) || (called.self_this->objType == PPLOBJ_MOD))) {
                        dict *d = (dict *) called.self_this->auxil;
                        context->namespaces[++context->ns_ptr] = d;
                    }

                    // Insert arguments into namespace dictionary. Any existing variables with the same names as the
                    // arguments are saved in the stack slots above the arguments (stkp + j), to be restored afterwards.
                    for (k = j = 0; j < nArgs; j++) {
                        pplObj *varObj;
                        ppl_contextGetVarPointer(context, fn->argList + k, &varObj, &context->stack[stkp + j]);
                        pplObjCpy(varObj, args + j, 0, varObj->amMalloced, 1);
                        k += strlen(fn->argList + k) + 1;
                    }
                    context->stackPtr += nArgs; // Keep the saved variables on the stack while the function runs

                    // Insert variable 'self' into namespace, if this is a method
                    if (setSelf) {
                        pplObj *varObj;
                        ppl_contextGetVarPointer(context, "self", &varObj, &context->stack[stkp + j]);
                        pplObjCpy(varObj, called.self_this, 0, varObj->amMalloced, 1);
                        context->stackPtr++; // Keep any saved variable called self on the stack too
                    }

                    // Evaluate function
                    pplObj *output = ppl_expEval(context, (pplExpr *) f->functionPtr, &lastOpAssign, dollarAllowed,
                                         iterDepth + 1);

                    // Take arguments out of namespace dictionary
                    for (k = j = 0; j < nArgs; j++) {
                        ppl_contextRestoreVarPointer(context, fn->argList + k, &context->stack[stkp + j]);
                        k += strlen(fn->argList + k) + 1;
                    }

                    // Take self out of namespace dictionary
                    if (setSelf) { ppl_contextRestoreVarPointer(context, "self", &context->stack[stkp + j]); }

                    // Add traceback information if error happened
                    if (context->errStat.status) {
                        strcpy(context->errStat.errBuff, "");
                        ppl_tbAddExpr(context, inExpr, ERR_GENERIC, inExprCharPos, "called function");
                    }

                    // Tidy up
                    if (output != NULL) {
                        // Move the function's result into the slot reserved for it below the arguments, and remove
                        // it from the top of the stack
                        memcpy(args - 1, output, sizeof(pplObj));
                        context->stackPtr--;
                    }
                    context->stackPtr -= nArgs + setSelf; // Release the slots used to save variables
                    context->ns_ptr = ns_ptr;
                }
                break;
            }
            case PPL_FUNC_SUBROUTINE: {
                int j, k, ns_ptr = context->ns_ptr;
                const int shellReturnableOld = context->shellReturnable;
                const int stkLevelOld = context->stackPtr;

                // Check that there's enough space on the stack
                if (context->ns_ptr > CONTEXT_DEPTH - 2) {
                    strcpy(context->errStat.errBuff, "Stack overflow.");
                    ppl_tbAddExpr(context, inExpr, ERR_MEMORY, inExprCharPos, "");
                    goto cleanup;
                }

                // Enter a new namespace
                dict *d = ppl_dictInit(1);
                // Failure to allocate memory is reported as an error
                if (d == NULL) {
                    strcpy(context->errStat.errBuff, "Out of memory.");
                    ppl_tbAddExpr(context, inExpr, ERR_MEMORY, inExprCharPos, "");
                    goto cleanup;
                }
                context->namespaces[++context->ns_ptr] = d;

                // Set returnable flag
                context->shellReturnable = 1;

                // Insert arguments into namespace dictionary
                for (k = j = 0; j < nArgs; j++) {
                    pplObj *varObj, tmp;
                    ppl_contextGetVarPointer(context, fn->argList + k, &varObj, &tmp);
                    pplObjCpy(varObj, args + j, 0, varObj->amMalloced, 1);
                    tmp.amMalloced = 0;
                    ppl_garbageObject(&tmp);
                    k += strlen(fn->argList + k) + 1;
                }

                // Insert variable 'self' into namespace, if this is a method
                if ((called.self_this != NULL) &&
                    ((called.self_this->objType == PPLOBJ_USER) || (called.self_this->objType == PPLOBJ_MOD))) {
                    pplObj *varObj, tmp;
                    ppl_contextGetVarPointer(context, "self", &varObj, &tmp);
                    pplObjCpy(varObj, called.self_this, 0, varObj->amMalloced, 1);
                    tmp.amMalloced = 0;
                    ppl_garbageObject(&tmp);
                }

                // Execute subroutine
                ppl_parserExecute(context, (parserLine *) fn->functionPtr, NULL, 0, iterDepth + 1);

                // Garbage subroutine's namespace
                ppl_garbageNamespace(d);
                context->ns_ptr = ns_ptr;
                ppl_stackClean(context, stkLevelOld); // Discard anything left on the stack

                // Add traceback information if error happened
                if (context->errStat.status) {
                    strcpy(context->errStat.errBuff, "");
                    ppl_tbAddExpr(context, inExpr, ERR_GENERIC, inExprCharPos, "called subroutine");
                    goto cleanup;
                }

                // Output return value
                if (context->shellReturned) pplObjCpy(out, &context->shellReturnVal, 0, 0, 1);
                else pplObjNum(out, 0, 0, 0);
                ppl_garbageObject(&context->shellReturnVal);
                context->shellReturnable = shellReturnableOld;
                context->shellReturned = 0;
                break;
            }
            case PPL_FUNC_FFT: {
                int stat = 0;
                FFTDescriptor *f = (FFTDescriptor *) fn->functionPtr;
                ppl_fft_evaluate(context, "fft", f, args, out, &stat, context->errStat.errBuff);
                // Report an error in evaluating the Fourier transform
                if (stat) {
                    ppl_tbAddExpr(context, inExpr, ERR_NUMERICAL, inExprCharPos, "");
                    goto cleanup;
                }
                break;
            }
            case PPL_FUNC_HISTOGRAM: {
                int stat = 0;
                histogramDescriptor *h = (histogramDescriptor *) fn->functionPtr;
                ppl_histogram_evaluate(context, "histogram", h, args, out, &stat, context->errStat.errBuff);
                // Report an error in evaluating the histogram
                if (stat) {
                    ppl_tbAddExpr(context, inExpr, ERR_NUMERICAL, inExprCharPos, "");
                    goto cleanup;
                }
                break;
            }
            case PPL_FUNC_SPLINE: {
                int stat = 0;
                splineDescriptor *s = (splineDescriptor *) fn->functionPtr;
                ppl_spline_evaluate(context, "interpolation", s, args, out, &stat, context->errStat.errBuff);
                // Report an error in evaluating the interpolated function
                if (stat) {
                    ppl_tbAddExpr(context, inExpr, ERR_NUMERICAL, inExprCharPos, "");
                    goto cleanup;
                }
                break;
            }
            case PPL_FUNC_INTERP2D:
            case PPL_FUNC_BMPDATA: {
                int stat = 0;
                splineDescriptor *s = (splineDescriptor *) fn->functionPtr;
                ppl_interp2d_evaluate(context, "interpolation", s, args, args + 1, fn->functionType == PPL_FUNC_BMPDATA,
                                      out, &stat, context->errStat.errBuff);
                // Report an error in evaluating the interpolated function
                if (stat) {
                    ppl_tbAddExpr(context, inExpr, ERR_NUMERICAL, inExprCharPos, "");
                    goto cleanup;
                }
                break;
            }
            default: {
                ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                  "Call of unsupported function type.");
                ppl_tbAddExpr(context, inExpr, ERR_INTERNAL, inExprCharPos, "");
                goto cleanup;
            }
        }
    }
cleanup:
cast_fail:
    out->self_this = NULL;
    out->refCount = 1;
    for (i = 0; i < nArgs; i++) { ppl_stackPop(context); } // Pop the function's arguments
    ppl_garbageObject(&called);
    return;
}
