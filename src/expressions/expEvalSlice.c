// expEvalSlice.c
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

// The [] operator of the expression evaluator, which takes items and slices from strings, lists, vectors, matrices,
// dictionaries and modules, e.g. a[3], a[1:4] and d["key"]. Negative indices count from the end. Items can be returned
// as pointers, so that they can be assigned to, e.g. a[3]=1.

#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include <gsl/gsl_math.h>

#include "expressions/expEvalSlice.h"
#include "userspace/stack.h"
#include "stringTools/asciidouble.h"
#include "userspace/context.h"
#include "userspace/garbageCollector.h"
#include "userspace/pplObj_fns.h"
#include "userspace/unitsArithmetic.h"
#include "userspace/unitsDisp.h"

#include "pplConstants.h"
#include "defaultObjs/defaultFuncsChecks.h"


//! ppl_sliceItem - Evaluate the [] operator with a single index, e.g. a[3], which looks up an item in a list,
//! vector, matrix row, dictionary or string. The object and index are taken from the top of the stack, and replaced
//! by the item.
//! \param [in] context - The Pyxplot context
//! \param [in] getPtr - If true, return a pointer to the item so that it can be assigned to, rather than a copy
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void ppl_sliceItem(ppl_context *context, const int getPtr, int *status, int *errType, char *errText) {
    // The object being sliced is on the stack below its index; the result replaces the object. called holds a reference
    // to the object while it is being sliced.
    const int nArgs = 1;
    pplObj *out = &context->stack[context->stackPtr - 1 - nArgs];
    pplObj *args = context->stack + context->stackPtr - nArgs;
    pplObj called;
    int i;
    int t = out->objType;
    // The stack must hold the object and its index
    if (context->stackPtr < nArgs + 1) {
        ppl_funcError(status, errType, errText, ERR_INTERNAL, "Attempt to slice object with few items on the stack.");
        return;
    }
    memcpy(&called, out, sizeof(pplObj));
    pplObjNum(out, 0, 0, 0); // Overwrite sliced object on stack, but don't garbage collect it yet
    out->refCount = 1;

    // Dictionaries, modules and class instances are indexed by string keys
    if ((t == PPLOBJ_DICT) || (t == PPLOBJ_MOD) || (t == PPLOBJ_USER)) {
        dict *din = (dict *) called.auxil;
        char *key;
        pplObj *obj;
        // The key must be a string
        if (args->objType != PPLOBJ_STR) {
            *errType = ERR_TYPE;
            ppl_snprintfTrunc(errText, LSTR_LENGTH, "Dictionary keys must be strings; supplied key has type <%s>.",
                              pplObjTypeNames[args->objType]);
            goto fail;
        }
        key = (char *) args[0].auxil;
        // The key does not exist
        if (!ppl_dictContains(din, key)) {
            // When assigning, create a new item with the key; otherwise the missing key is an error
            if (getPtr && (!din->immutable)) {
                pplObj v;
                v.refCount = 1;
                pplObjNull(&v, 1);
                ppl_dictAppendCpy(din, key, &v, sizeof(pplObj));
            } else {
                ppl_funcError(status, errType, errText, ERR_DICTKEY, "Undefined dictionary key '%s'.", key);
                goto fail;
            }
        }
        obj = (pplObj *) ppl_dictLookup(din, key);
        pplObjCpy(out, obj, 1, 0, 1);
        out->immutable = out->immutable || din->immutable;
        goto cleanup;
    }

    // Other objects are indexed by integers, which must be real, dimensionless and of a sensible size
    if (args->objType != PPLOBJ_NUM) {
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(errText, LSTR_LENGTH,
                          "Item numbers when slicing must be numerical values; supplied index has type <%s>.",
                          pplObjTypeNames[args->objType]);
        goto fail;
    }
    // The index must be dimensionless
    if (!args->dimensionless) {
        *errType = ERR_NUMERICAL;
        ppl_snprintfTrunc(errText, LSTR_LENGTH,
                          "Item numbers when slicing must be dimensionless numbers; supplied index has units of <%s>.",
                          ppl_printUnit(context, args, NULL, NULL, 0, 1, 0));
        goto fail;
    }
    // The index must be real
    if (args->flagComplex) {
        *errType = ERR_NUMERICAL;
        ppl_snprintfTrunc(errText, LSTR_LENGTH,
                          "Item numbers when slicing must be real numbers; supplied index is complex.");
        goto fail;
    }
    // The index must fit in an int
    if ((!gsl_finite(args->real)) || (args->real < INT_MIN) || (args->real > INT_MAX)) {
        *errType = ERR_NUMERICAL;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Item numbers when slicing must be in the range %d to %d.", INT_MIN,
                          INT_MAX);
        goto fail;
    }

    // Fetch the item from each type of object
    switch (t) {
        case PPLOBJ_STR: {
            const char *in = (char *) called.auxil;
            const int inl = ppl_utf8Length(in); // Strings are indexed by character, not byte
            char *outstr;
            int p = args[0].real, start, len;
            if (p < 0) p += inl;
            // The character must exist
            if ((p < 0) || (p >= inl)) {
                ppl_funcError(status, errType, errText, ERR_RANGE, "String index out of range.");
                goto fail;
            }
            // Strings are immutable, character by character
            if (getPtr) {
                ppl_funcError(status, errType, errText, ERR_TYPE, "Cannot assign to a character in a string.");
                goto fail;
            }
            // A character may be several bytes long
            start = ppl_utf8ByteOffset(in, p);
            len = ppl_utf8ByteOffset(in, p + 1) - start;
            // Copy the character, which may occupy several bytes of UTF-8, into a new string
            if ((outstr = (char *) malloc(len + 1)) == NULL) {
                ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
                goto fail;
            }
            memcpy(outstr, in + start, len);
            outstr[len] = '\0';
            pplObjStr(out, 0, 1, outstr);
            break;
        }
        case PPLOBJ_LIST: {
            list *lin = (list *) called.auxil;
            const int linl = lin->length;
            int p = args[0].real;
            pplObj *obj;
            if (p < 0) p += linl;
            // Assigning to the item just past the end of a list appends a new item
            if (getPtr && (p == linl) && (!lin->immutable)) {
                pplObj v;
                v.refCount = 1;
                pplObjNull(&v, 1);
                ppl_listAppendCpy(lin, &v, sizeof(pplObj));
            } else {
                // The item must exist
                if ((p < 0) || (p >= linl)) {
                    ppl_funcError(status, errType, errText, ERR_RANGE, "List index out of range.");
                    goto fail;
                }
            }
            obj = (pplObj *) ppl_listGetItem(lin, p);
            pplObjCpy(out, obj, 1, 0, 1);
            out->immutable = out->immutable || lin->immutable;
            break;
        }
        case PPLOBJ_VEC: {
            gsl_vector *vin = ((pplVector *) called.auxil)->v;
            const int vinl = vin->size;
            int p = args[0].real;
            double *obj;
            if (p < 0) p += vinl;
            // The element must exist
            if ((p < 0) || (p >= vinl)) {
                ppl_funcError(status, errType, errText, ERR_RANGE, "Vector index out of range.");
                goto fail;
            }
            obj = gsl_vector_ptr(vin, p);
            pplObjNum(out, 0, *obj, 0);
            ppl_unitsDimCpy(out, &called);
            if ((out->self_lval = called.self_lval) != NULL) { __sync_add_and_fetch(&out->self_lval->refCount, 1); }
            out->self_dval = obj;
            out->immutable = out->immutable || called.immutable;
            break;
        }
        case PPLOBJ_MAT: {
            pplMatrix *mob = (pplMatrix *) called.auxil;
            gsl_matrix *min = mob->m;
            const int minl = mob->sliceNext ? min->size1 : min->size2;
            int p = args[0].real;
            pplVector *vo;
            if (p < 0) p += minl;
            // The row must exist
            if ((p < 0) || (p >= minl)) {
                ppl_funcError(status, errType, errText, ERR_RANGE, "Matrix index out of range.");
                goto fail;
            }
            vo = (pplVector *) malloc(sizeof(pplVector));
            // A row of a matrix is a vector which views the matrix's storage, so that changing it changes the matrix
            if (vo == NULL) {
                ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
                goto fail;
            }
            vo->refCount = 1;
            vo->raw = NULL;
            vo->rawm = mob->raw;
            __sync_add_and_fetch(&mob->raw->refCount, 1);
            if (mob->sliceNext) vo->view = gsl_matrix_column(min, p);
            else vo->view = gsl_matrix_row(min, p);
            vo->v = &vo->view.vector;
            out->objType = PPLOBJ_ZOM;
            out->refCount = 1;
            out->auxil = (void *) vo;
            out->auxilMalloced = 1;
            out->auxilLen = sizeof(pplVector);
            out->objPrototype = &pplObjPrototypes[PPLOBJ_VEC];
            if ((out->self_lval = called.self_lval) != NULL) { __sync_add_and_fetch(&out->self_lval->refCount, 1); }
            out->self_dval = NULL;
            out->self_this = NULL;
            out->immutable = 0;
            ppl_unitsDimCpy(out, &called);
            out->objType = PPLOBJ_VEC;
            out->immutable = called.immutable;
            break;
        }
        default:
            ppl_funcError(status, errType, errText, ERR_TYPE,
                    "Attempt to slice an object of type <%s>, which cannot be sliced.",
                    pplObjTypeNames[t]);
            goto fail;
    }

// Pop the index, and release the object being sliced
cleanup:
    for (i = 0; i < nArgs; i++) { ppl_stackPop(context); } // Pop the subscripts
    ppl_garbageObject(&called);
    return;
fail:
    *status = 1;
    goto cleanup;
}

//! ppl_sliceRange - Evaluate the [] operator with a range of indices, e.g. a[3:5], which takes a slice of a list,
//! vector or string. The object is taken from the top of the stack, and replaced by the slice.
//! \param [in] context - The Pyxplot context
//! \param [in] minset - If true, a lower limit was given
//! \param [in] min - The index of the first item in the slice, if <minset> is true. Negative values count back
//! from the end.
//! \param [in] maxset - If true, an upper limit was given
//! \param [in] max - The index after the last item in the slice, if <maxset> is true. Negative values count back
//! from the end.
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void ppl_sliceRange(ppl_context *context, const int minset, int min, const int maxset, int max, int *status,
                    int *errType, char *errText) {
    const int nArgs = minset + maxset;
    pplObj *out = &context->stack[context->stackPtr - 1 - nArgs];
    pplObj called;
    int i;
    int t = out->objType;
    // The stack must hold the object and the two ends of the range
    if (context->stackPtr < nArgs + 1) {
        ppl_funcError(status, errType, errText, ERR_INTERNAL, "Attempt to slice object with few items on the stack.");
        return;
    }
    memcpy(&called, out, sizeof(pplObj));
    pplObjNum(out, 0, 0, 0); // Overwrite function object on stack, but don't garbage collect it yet
    out->refCount = 1;

    // Slice each type of object. A missing end of the range means the start or end of the object.
    switch (t) {
        case PPLOBJ_STR: {
            const char *in = (char *) called.auxil;
            const int inl = ppl_utf8Length(in); // Strings are sliced by character, not byte
            int outlen, byteMin;
            char *outstr;
            if (!minset) min = 0;
            else if (min < 0) min += inl;
            if (!maxset) max = inl;
            else if (max < 0) max += inl;
            // The ends of the range must lie within the string
            if ((min < 0) || (min > inl) || (max < 0) || (max > inl)) {
                ppl_funcError(status, errType, errText, ERR_RANGE, "String index out of range.");
                goto fail;
            }
            // Convert the range of characters into a range of bytes
            byteMin = ppl_utf8ByteOffset(in, min);
            outlen = ppl_utf8ByteOffset(in, max) - byteMin;
            if (outlen < 0) outlen = 0;
            // Copy the characters in the range into a new string
            if ((outstr = (char *) malloc(outlen + 1)) == NULL) {
                ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
                goto fail;
            }
            memcpy(outstr, in + byteMin, outlen);
            outstr[outlen] = '\0';
            pplObjStr(out, 0, 1, outstr);
            break;
        }
        case PPLOBJ_LIST: {
            list *lin = (list *) called.auxil;
            list *lout;
            const int inl = lin->length;
            int i;
            listIterator *li;
            pplObj obj, *objptr;
            if (!minset) min = 0;
            else if (min < 0) min += inl;
            if (!maxset) max = inl;
            else if (max < 0) max += inl;
            // The ends of the range must lie within the list
            if ((min < 0) || (min > inl) || (max < 0) || (max > inl)) {
                ppl_funcError(status, errType, errText, ERR_RANGE, "List index out of range.");
                goto fail;
            }
            // The output is a new list
            if (pplObjList(out, 0, 1, NULL) == NULL) {
                ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
                goto fail;
            }
            lout = (list *) out->auxil;
            obj.refCount = 1;
            li = ppl_listIterateInit(lin);
            for (i = 0; ((objptr = (pplObj *) ppl_listIterate(&li)) != NULL); i++)
                // Copy the items in the range into the new list
                if ((i >= min) && (i < max)) {
                    pplObjCpy(&obj, objptr, 0, 1, 1);
                    ppl_listAppendCpy(lout, &obj, sizeof(pplObj));
                }
            break;
        }
        case PPLOBJ_VEC: {
            pplVector *vob = (pplVector *) called.auxil;
            gsl_vector *vecin = vob->v;
            const int vinl = vecin->size;
            pplVector *vo;
            if (!minset) min = 0;
            else if (min < 0) min += vinl;
            if (!maxset) max = vinl;
            else if (max < 0) max += vinl;
            // The ends of the range must lie within the vector
            if ((min < 0) || (min > vinl) || (max < 0) || (max > vinl)) {
                ppl_funcError(status, errType, errText, ERR_RANGE, "Vector index out of range.");
                goto fail;
            }
            // Vectors cannot be empty
            if (max < min + 1) {
                ppl_funcError(status, errType, errText, ERR_RANGE, "Cannot create a vector of zero size.");
                goto fail;
            }
            vo = (pplVector *) malloc(sizeof(pplVector));
            // The slice is a vector which views the original's storage, so that changing it changes the original
            if (vo == NULL) {
                ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
                goto fail;
            }
            vo->refCount = 1;
            if ((vo->raw = vob->raw) != NULL) { __sync_add_and_fetch(&vob->raw->refCount, 1); }
            if ((vo->rawm = vob->rawm) != NULL) { __sync_add_and_fetch(&vob->rawm->refCount, 1); }
            vo->view = gsl_vector_subvector(vecin, min, max - min);
            vo->v = &vo->view.vector;
            out->objType = PPLOBJ_ZOM;
            out->refCount = 1;
            out->auxil = (void *) vo;
            out->auxilMalloced = 1;
            out->auxilLen = sizeof(pplVector);
            out->objPrototype = &pplObjPrototypes[PPLOBJ_VEC];
            out->self_lval = NULL;
            out->self_dval = NULL;
            out->self_this = NULL;
            out->immutable = 0;
            ppl_unitsDimCpy(out, &called);
            out->objType = PPLOBJ_VEC;
            out->immutable = called.immutable;
            break;
        }
        case PPLOBJ_MAT: {
            pplMatrix *mob = (pplMatrix *) called.auxil;
            gsl_matrix *matin = mob->m;
            const int minl = mob->sliceNext ? matin->size1 : matin->size2;
            pplMatrix *mo;
            if (!minset) min = 0;
            else if (min < 0) min += minl;
            if (!maxset) max = minl;
            else if (max < 0) max += minl;
            // The ends of the range must lie within the matrix
            if ((min < 0) || (min > minl) || (max < 0) || (max > minl)) {
                ppl_funcError(status, errType, errText, ERR_RANGE, "Matrix index out of range.");
                goto fail;
            }
            // Matrices cannot be empty
            if (max < min + 1) {
                ppl_funcError(status, errType, errText, ERR_RANGE, "Cannot create a matrix of zero size.");
                goto fail;
            }
            mo = (pplMatrix *) malloc(sizeof(pplMatrix));
            // The slice is a matrix which views the original's storage
            if (mo == NULL) {
                ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
                goto fail;
            }
            mo->refCount = 1;
            mo->raw = mob->raw;
            __sync_add_and_fetch(&mob->raw->refCount, 1);
            if (!mob->sliceNext) mo->view = gsl_matrix_submatrix(matin, min, 0, max - min, matin->size2);
            else mo->view = gsl_matrix_submatrix(matin, 0, min, matin->size1, max - min);
            mo->sliceNext = !mob->sliceNext;
            mo->m = &mo->view.matrix;
            out->objType = PPLOBJ_ZOM;
            out->refCount = 1;
            out->auxil = (void *) mo;
            out->auxilMalloced = 1;
            out->auxilLen = sizeof(pplMatrix);
            out->objPrototype = &pplObjPrototypes[PPLOBJ_MAT];
            out->self_lval = NULL;
            out->self_dval = NULL;
            out->self_this = NULL;
            out->immutable = 0;
            ppl_unitsDimCpy(out, &called);
            out->objType = PPLOBJ_MAT;
            out->immutable = called.immutable;
            break;
        }
        default:
            ppl_funcError(status, errType, errText, ERR_TYPE,
                    "Attempt to slice an object of type <%s>, which cannot be sliced.",
                    pplObjTypeNames[t]);
            goto fail;
    }

// Pop the ends of the range, and release the object being sliced
cleanup:
    for (i = 0; i < nArgs; i++) { ppl_stackPop(context); } // Pop the subscripts
    ppl_garbageObject(&called);
    return;
fail:
    *status = 1;
    goto cleanup;
}
