// expEvalOps.c
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

// The arithmetic operators (+, -, *, /, %, **) of the expression evaluator, for every combination of types they accept:
// numbers with units, strings (concatenation), dates (adding time intervals), colors (mixing), vectors, matrices
// (including matrix products), lists and dictionaries. Operations on plain numbers are passed to unitsArithmetic.c;
// other types are cast to numbers where that makes sense, or reported as errors.

#include <stdlib.h>
#include <string.h>

#include <gsl/gsl_blas.h>
#include <gsl/gsl_errno.h>
#include <gsl/gsl_math.h>
#include <gsl/gsl_matrix.h>
#include <gsl/gsl_vector.h>
#define GSL_RANGE_CHECK_OFF 1

#include "expressions/expEval.h"
#include "expressions/expEvalOps.h"
#include "settings/settingTypes.h"
#include "stringTools/asciidouble.h"
#include "userspace/context.h"
#include "userspace/garbageCollector.h"
#include "userspace/pplObj_fns.h"
#include "userspace/pplObjCmp.h"
#include "userspace/unitsArithmetic.h"
#include "userspace/unitsDisp.h"

#include "pplConstants.h"
#include "defaultObjs/defaultFuncsChecks.h"


//! addSubArrays - Add or subtract two vectors, or two matrices, which must have the same units and size
//! \param [in] context - The Pyxplot context
//! \param [in] a - The left operand
//! \param [in] b - The right operand, of the same type as <a>
//! \param [out] o - The result
//! \param [in] sub - Zero to add the operands, or one to subtract them
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, if an error occurred
//! \param [out] errText - Buffer of LSTR_LENGTH bytes into which to write an error message

static void addSubArrays(ppl_context *context, const pplObj *a, const pplObj *b, pplObj *o, const int sub,
                         int *status, int *errType, char *errText) {
    const char *const verb = sub ? "subtract" : "add";
    // The operands can only be combined if they have the same units
    if (!ppl_unitsDimEqual(a, b)) {
        ppl_unitsConflictError(context, a, b, verb, status, errType, errText);
        return;
    }
    // Vectors must have the same length
    if (a->objType == PPLOBJ_VEC) {
        const gsl_vector *v1 = ((pplVector *) (a->auxil))->v;
        const gsl_vector *v2 = ((pplVector *) (b->auxil))->v;
        if (v1->size != v2->size) {
            ppl_funcError(status, errType, errText, ERR_RANGE,
                          "Can only %s vectors of a common size. Left operand has length of %ld, while right operand has length of %ld.",
                          verb, (long) v1->size, (long) v2->size);
            return;
        }
        // The output is a new vector
        if (pplObjVector(o, 0, 1, v1->size) == NULL) {
            ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
            return;
        }
        gsl_vector *vo = ((pplVector *) (o->auxil))->v;
        for (size_t i = 0; i < v1->size; i++) {
            const double x = gsl_vector_get(v1, i), y = gsl_vector_get(v2, i);
            gsl_vector_set(vo, i, sub ? (x - y) : (x + y));
        }
    }
    // Matrices must have the same size
    else {
        const gsl_matrix *m1 = ((pplMatrix *) (a->auxil))->m;
        const gsl_matrix *m2 = ((pplMatrix *) (b->auxil))->m;
        if ((m1->size1 != m2->size1) || (m1->size2 != m2->size2)) {
            ppl_funcError(status, errType, errText, ERR_RANGE,
                          "Can only %s matrices of a common size. Left operand has size of %ldx%ld, while right operand has size of %ldx%ld.",
                          verb, (long) m1->size1, (long) m1->size2, (long) m2->size1, (long) m2->size2);
            return;
        }
        // The output is a new matrix
        if (pplObjMatrix(o, 0, 1, m1->size1, m2->size2) == NULL) {
            ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
            return;
        }
        gsl_matrix *mo = ((pplMatrix *) (o->auxil))->m;
        for (size_t i = 0; i < m1->size1; i++)
            for (size_t j = 0; j < m1->size2; j++) {
                const double x = gsl_matrix_get(m1, i, j), y = gsl_matrix_get(m2, i, j);
                gsl_matrix_set(mo, i, j, sub ? (x - y) : (x + y));
            }
    }
    ppl_unitsDimCpy(o, a);
}

//! ppl_opAdd - Evaluate the + operator, which adds numbers, vectors and matrices, concatenates strings and lists,
//! merges dictionaries, and adds time intervals to dates
//! \param [in] context - The Pyxplot context
//! \param [in] a - The first operand
//! \param [in] b - The second operand
//! \param [out] o - The result (may be the same object as <a>)
//! \param [in] invertible - Unused. Callers pass one for binary operators, and zero for in-place assignment
//! operators such as +=
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void ppl_opAdd(ppl_context *context, pplObj *a, pplObj *b, pplObj *o, const int invertible, int *status, int *errType,
               char *errText) {
    const int t1 = a->objType;
    const int t2 = b->objType;

    // Choose what addition means from the types of the two operands
    if ((t1 == PPLOBJ_NUM) && (t2 == PPLOBJ_NUM)) {
        goto am_numeric;
    } else if ((t1 == PPLOBJ_STR) && (t2 == PPLOBJ_STR)) // adding strings: concatenate
    {
        const int l1 = strlen((char *) a->auxil);
        const int l2 = strlen((char *) b->auxil);
        int l = l1 + l2 + 1;
        char *tmp = (char *) malloc(l);
        // Failure to allocate memory is reported as an error
        if (tmp == NULL) {
            ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
            return;
        }
        strcpy(tmp, (char *) a->auxil);
        strcpy(tmp + l1, (char *) b->auxil);
        pplObjStr(o, 0, 1, tmp);
    } else if ((t1 == PPLOBJ_COL) && (t2 == PPLOBJ_COL)) // adding colors
    {
        double r1, g1, b1, r2, g2, b2;
        // Adding colors mixes them additively: convert both to RGB, and add their components
        pplcol_toRGB(a, &r1, &g1, &b1);
        // Convert the second color to RGB
        pplcol_toRGB(b, &r2, &g2, &b2);
        r1 += r2;
        g1 += g2;
        b1 += b2;
        pplObjColor(o, 0, SW_COLSPACE_RGB, r1, g1, b1, 0);
    } else if (((t1 == PPLOBJ_DATE) && (t2 == PPLOBJ_NUM)) ||
               ((t1 == PPLOBJ_NUM) && (t2 == PPLOBJ_DATE))) // adding time interval to date
    {
        int i;
        pplObj *num = (t1 == PPLOBJ_NUM) ? a : b;
        for (i = 0; i < UNITS_MAX_BASEUNITS; i++)
            // The number added to a date must have units of time
            if (num->exponent[i] != (i == UNIT_TIME)) {
                ppl_funcError(status, errType, errText, ERR_UNIT,
                        "Can only add quantities with units of time to dates. Attempt to add a quantity with units of <%s>.",
                        ppl_printUnit(context, num, NULL, NULL, 1, 1, 0));
                return;
            }
        pplObjDate(o, 0, a->real + b->real);
    } else if ((t1 == PPLOBJ_VEC) && (t2 == PPLOBJ_VEC)) // adding vectors
    {
        addSubArrays(context, a, b, o, 0, status, errType, errText);
    } else if ((t1 == PPLOBJ_LIST) && (t2 == PPLOBJ_LIST)) // adding lists
    {
        pplObj *item, buff;
        buff.refCount = 1; // Each copy appended to the list holds one reference
        // Adding lists concatenates them into a new list
        if (pplObjList(o, 0, 1, NULL) == NULL) {
            ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
            return;
        }
        list *l = (list *) o->auxil;
        listIterator *li = ppl_listIterateInit((list *) a->auxil);
        // Copy the items of the first list
        while ((item = (pplObj *) ppl_listIterate(&li)) != NULL) {
            pplObjCpy(&buff, item, 0, 1, 1);
            ppl_listAppendCpy(l, (void *) &buff, sizeof(pplObj));
        }
        li = ppl_listIterateInit((list *) b->auxil);
        // Copy the items of the second list
        while ((item = (pplObj *) ppl_listIterate(&li)) != NULL) {
            pplObjCpy(&buff, item, 0, 1, 1);
            ppl_listAppendCpy(l, (void *) &buff, sizeof(pplObj));
        }
    } else if ((t1 == PPLOBJ_LIST) && (t2 == PPLOBJ_VEC)) // adding vector to list
    {
        int i;
        pplObj *item, buff;
        buff.refCount = 1; // Each copy appended to the list holds one reference
        gsl_vector *bv = ((pplVector *) (b->auxil))->v;
        // The output is a new list
        if (pplObjList(o, 0, 1, NULL) == NULL) {
            ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
            return;
        }
        list *l = (list *) o->auxil;
        listIterator *li = ppl_listIterateInit((list *) a->auxil);
        // Copy the items of the list
        while ((item = (pplObj *) ppl_listIterate(&li)) != NULL) {
            pplObjCpy(&buff, item, 0, 1, 1);
            ppl_listAppendCpy(l, (void *) &buff, sizeof(pplObj));
        }
        pplObjNum(&buff, 1, 0, 0);
        ppl_unitsDimCpy(&buff, b);
        // Append the elements of the vector, as numbers with its units
        for (i = 0; i < bv->size; i++) {
            buff.real = gsl_vector_get(bv, i);
            ppl_listAppendCpy(l, (void *) &buff, sizeof(pplObj));
        }
    } else if ((t1 == PPLOBJ_VEC) && (t2 == PPLOBJ_LIST)) // adding list to vector
    {
        int i;
        pplObj *item, buff;
        buff.refCount = 1; // Each copy appended to the list holds one reference
        gsl_vector *av = ((pplVector *) (a->auxil))->v;
        // The output is a new list
        if (pplObjList(o, 0, 1, NULL) == NULL) {
            ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
            return;
        }
        list *l = (list *) o->auxil;
        pplObjNum(&buff, 1, 0, 0);
        ppl_unitsDimCpy(&buff, a);
        // Copy the elements of the vector, as numbers with its units
        for (i = 0; i < av->size; i++) {
            buff.real = gsl_vector_get(av, i);
            ppl_listAppendCpy(l, (void *) &buff, sizeof(pplObj));
        }
        listIterator *li = ppl_listIterateInit((list *) b->auxil);
        // Append the items of the list
        while ((item = (pplObj *) ppl_listIterate(&li)) != NULL) {
            pplObjCpy(&buff, item, 0, 1, 1);
            ppl_listAppendCpy(l, (void *) &buff, sizeof(pplObj));
        }
    } else if (((t1 == PPLOBJ_DICT) && (t2 == PPLOBJ_DICT)) || // adding dictionaries
               ((t1 == PPLOBJ_MOD) && (t2 == PPLOBJ_MOD))) // adding modules
    {
        pplObj *item, buff;
        buff.refCount = 1; // Each copy appended to the list holds one reference
        char *key;
        // Adding dictionaries or modules merges them; items of the second override those of the first
        if (((t1 == PPLOBJ_DICT) ? pplObjDict(o, 0, 1, NULL) : pplObjModule(o, 0, 1, 0)) == NULL) {
            ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
            return;
        }
        dict *d = (dict *) o->auxil;
        dictIterator *di = ppl_dictIterateInit((dict *) a->auxil);
        // Copy the items of the first dictionary
        while ((item = (pplObj *) ppl_dictIterate(&di, &key)) != NULL) {
            pplObjCpy(&buff, item, 0, 1, 1);
            ppl_dictAppendCpy(d, key, (void *) &buff, sizeof(pplObj));
        }
        di = ppl_dictIterateInit((dict *) b->auxil);
        // Copy the items of the second dictionary
        while ((item = (pplObj *) ppl_dictIterate(&di, &key)) != NULL) {
            pplObjCpy(&buff, item, 0, 1, 1);
            ppl_dictAppendCpy(d, key, (void *) &buff, sizeof(pplObj));
        }
    } else if ((t1 == PPLOBJ_MAT) && (t2 == PPLOBJ_MAT)) // adding matrices
    {
        addSubArrays(context, a, b, o, 0, status, errType, errText);
    } else // adding numbers
    {
        // Any other types must be cast to numbers, e.g. booleans
        if (ppl_castToNumber(context, a, errText, LSTR_LENGTH)) {
            *status = 1;
            *errType = ERR_TYPE;
            goto cast_fail;
        }
        // Cast the second operand
        if (ppl_castToNumber(context, b, errText, LSTR_LENGTH)) {
            *status = 1;
            *errType = ERR_TYPE;
            goto cast_fail;
        }
    am_numeric:
        o->objType =
                PPLOBJ_NUM; // Do this by hand rather than calling pplObjNum to save time (some field will be set by function call below)
        o->objPrototype = &pplObjPrototypes[PPLOBJ_NUM];
        o->self_lval = NULL;
        o->self_dval = NULL;
        o->self_this = NULL;
        o->amMalloced = 0;
        o->immutable = 0;
        ppl_uaAdd(context, a, b, o, status, errType, errText);
    }
cast_fail:
    return;
}

//! ppl_opSub - Evaluate the - operator, which subtracts numbers, vectors and matrices, and computes the interval
//! between dates or subtracts time intervals from dates
//! \param [in] context - The Pyxplot context
//! \param [in] a - The first operand
//! \param [in] b - The second operand
//! \param [out] o - The result (may be the same object as <a>)
//! \param [in] invertible - Unused. Callers pass one for binary operators, and zero for in-place assignment
//! operators such as +=
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void ppl_opSub(ppl_context *context, pplObj *a, pplObj *b, pplObj *o, const int invertible, int *status, int *errType,
               char *errText) {
    const int t1 = a->objType;
    const int t2 = b->objType;

    // Choose what subtraction means from the types of the two operands
    if ((t1 == PPLOBJ_NUM) && (t2 == PPLOBJ_NUM)) {
        goto am_numeric;
    } else if ((t1 == PPLOBJ_DATE) && (t2 == PPLOBJ_DATE)) // subtracting dates: return time interval
    {
        pplObjNum(o, 0, a->real - b->real, 0);
        o->dimensionless = 0;
        o->exponent[UNIT_TIME] = 1;
    } else if ((t1 == PPLOBJ_DATE) && (t2 == PPLOBJ_NUM)) // subtracting time interval from date
    {
        int i;
        for (i = 0; i < UNITS_MAX_BASEUNITS; i++)
            // The number subtracted from a date must have units of time
            if (b->exponent[i] != (i == UNIT_TIME)) {
                ppl_funcError(status, errType, errText, ERR_UNIT,
                        "Can only subtract quantities with units of time from dates. Attempt to subtract a quantity with units of <%s>.",
                        ppl_printUnit(context, b, NULL, NULL, 1, 1, 0));
                return;
            }
        pplObjDate(o, 0, a->real - b->real);
    } else if ((t1 == PPLOBJ_COL) && (t2 == PPLOBJ_COL)) // subtracting colors
    {
        double r1, g1, b1, r2, g2, b2;
        // Subtracting colors: convert both to RGB, and subtract their components
        pplcol_toRGB(a, &r1, &g1, &b1);
        // Convert the second color to RGB
        pplcol_toRGB(b, &r2, &g2, &b2);
        r1 -= r2;
        g1 -= g2;
        b1 -= b2;
        pplObjColor(o, 0, SW_COLSPACE_RGB, r1, g1, b1, 0);
    } else if ((t1 == PPLOBJ_VEC) && (t2 == PPLOBJ_VEC)) // subtracting vectors
    {
        addSubArrays(context, a, b, o, 1, status, errType, errText);
    } else if ((t1 == PPLOBJ_MAT) && (t2 == PPLOBJ_MAT)) // subtracting matrices
    {
        addSubArrays(context, a, b, o, 1, status, errType, errText);
    } else // subtracting numbers
    {
        // Any other types must be cast to numbers, e.g. booleans
        if (ppl_castToNumber(context, a, errText, LSTR_LENGTH)) {
            *status = 1;
            *errType = ERR_TYPE;
            goto cast_fail;
        }
        // Cast the second operand
        if (ppl_castToNumber(context, b, errText, LSTR_LENGTH)) {
            *status = 1;
            *errType = ERR_TYPE;
            goto cast_fail;
        }
    am_numeric:
        o->objType =
                PPLOBJ_NUM; // Do this by hand rather than calling pplObjNum to save time (some field will be set by function call below)
        o->objPrototype = &pplObjPrototypes[PPLOBJ_NUM];
        o->self_lval = NULL;
        o->self_dval = NULL;
        o->self_this = NULL;
        o->amMalloced = 0;
        o->immutable = 0;
        ppl_uaSub(context, a, b, o, status, errType, errText);
    }
cast_fail:
    return;
}

//! ppl_opMul - Evaluate the * operator, which multiplies numbers, vectors and matrices, and scales colors
//! \param [in] context - The Pyxplot context
//! \param [in] a - The first operand
//! \param [in] b - The second operand
//! \param [out] o - The result (may be the same object as <a>)
//! \param [in] invertible - Unused. Callers pass one for binary operators, and zero for in-place assignment
//! operators such as +=
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void ppl_opMul(ppl_context *context, pplObj *a, pplObj *b, pplObj *o, const int invertible, int *status, int *errType,
               char *errText) {
    const int t1 = a->objType;
    const int t2 = b->objType;

    // Choose what multiplication means from the types of the two operands
    if ((t1 == PPLOBJ_NUM) && (t2 == PPLOBJ_NUM)) {
        goto am_numeric;
    } else if (((t1 == PPLOBJ_COL) && (t2 == PPLOBJ_NUM)) ||
               ((t1 == PPLOBJ_NUM) && (t2 == PPLOBJ_COL))) // multiplying colors by numbers
    {
        double r1, g1, b1;
        pplObj *c = (t1 == PPLOBJ_COL) ? a : b;
        pplObj *n = (t1 == PPLOBJ_COL) ? b : a;
        const double m = n->real;
        // Colors can only be scaled by plain positive numbers
        if ((!n->dimensionless) || (n->flagComplex) || (!gsl_finite(m)) || (m < 0)) {
            ppl_funcError(status, errType, errText, ERR_RANGE,
                    "Can only multiply colors by dimensionless, real, positive numbers.");
            return;
        }
        // Convert the color to RGB, and scale its components
        pplcol_toRGB(c, &r1, &g1, &b1);
        r1 *= m;
        g1 *= m;
        b1 *= m;
        pplObjColor(o, 0, SW_COLSPACE_RGB, r1, g1, b1, 0);
    } else if ((t1 == PPLOBJ_VEC) && (t2 == PPLOBJ_VEC)) // multiplying vectors (scalar dot product)
    {
        gsl_vector *v1 = ((pplVector *) (a->auxil))->v;
        gsl_vector *v2 = ((pplVector *) (b->auxil))->v;
        int i;
        double acc = 0;
        // The product of two vectors is their dot product, which needs vectors of the same length
        if (v1->size != v2->size) {
            ppl_funcError(status, errType, errText, ERR_RANGE,
                    "Can only form dot-product of vectors of a common size. Left operand has length of %ld, while right operand has length of %ld.",
                    (long) v1->size, (long) v2->size);
            return;
        }
        a->real = a->imag = b->real = b->imag = 0;
        a->flagComplex = b->flagComplex = 0;
        ppl_uaMul(context, a, b, o, status, errType, errText);
        for (i = 0; i < v1->size; i++) acc += gsl_vector_get(v1, i) * gsl_vector_get(v2, i);
        o->real = acc;
        o->imag = 0;
        o->flagComplex = 0;
    } else if (((t1 == PPLOBJ_VEC) && (t2 == PPLOBJ_NUM)) || // multiplying vector by number
               ((t1 == PPLOBJ_NUM) && (t2 == PPLOBJ_VEC))) // multiplying number by vector
    {
        int i;
        pplObj *num = (t1 == PPLOBJ_NUM) ? a : b;
        pplObj *vec = (t1 == PPLOBJ_VEC) ? a : b;
        gsl_vector *v = ((pplVector *) (vec->auxil))->v;
        gsl_vector *vo = NULL;
        // Multiplying a vector by a number: the number must be real
        if (num->flagComplex) {
            ppl_funcError(status, errType, errText, ERR_NUMERICAL,
                    "Vectors can only contain real numbers, and cannot be multiplied by complex numbers.");
            return;
        }
        vec->real = 1;
        vec->imag = 0;
        vec->flagComplex = 0;
        // The output is a new vector, each element multiplied by the number
        if (pplObjVector(o, 0, 1, v->size) == NULL) {
            ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
            return;
        }
        ppl_uaMul(context, a, b, o, status, errType, errText);
        vo = ((pplVector *) (o->auxil))->v;
        for (i = 0; i < v->size; i++) gsl_vector_set(vo, i, gsl_vector_get(v, i) * o->real);
        o->real = 0;
    } else if (((t1 == PPLOBJ_MAT) && (t2 == PPLOBJ_NUM)) || // multiplying matrix by number
               ((t1 == PPLOBJ_NUM) && (t2 == PPLOBJ_MAT))) // multiplying number by matrix
    {
        int i, j;
        pplObj *num = (t1 == PPLOBJ_NUM) ? a : b;
        pplObj *mat = (t1 == PPLOBJ_MAT) ? a : b;
        gsl_matrix *m = ((pplMatrix *) (mat->auxil))->m;
        gsl_matrix *mo = NULL;
        // Multiplying a matrix by a number: the number must be real
        if (num->flagComplex) {
            ppl_funcError(status, errType, errText, ERR_NUMERICAL,
                    "Matrices can only contain real numbers, and cannot be multiplied by complex numbers.");
            return;
        }
        mat->real = 1;
        mat->imag = 0;
        mat->flagComplex = 0;
        // The output is a new matrix, each element multiplied by the number
        if (pplObjMatrix(o, 0, 1, m->size1, m->size2) == NULL) {
            ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
            return;
        }
        ppl_uaMul(context, a, b, o, status, errType, errText);
        mo = ((pplMatrix *) (o->auxil))->m;
        for (i = 0; i < m->size1; i++)
            for (j = 0; j < m->size2; j++) gsl_matrix_set(mo, i, j, gsl_matrix_get(m, i, j) * o->real);
        o->real = 0;
    } else if ((t1 == PPLOBJ_MAT) && (t2 == PPLOBJ_VEC)) // matrix-vector multiplication
    {
        int gslerr = 0;
        gsl_matrix *m = ((pplMatrix *) (a->auxil))->m;
        gsl_vector *v = ((pplVector *) (b->auxil))->v;
        gsl_vector *vo = NULL;
        // A matrix times a vector needs as many columns in the matrix as there are elements in the vector
        if (m->size2 != v->size) {
            ppl_funcError(status, errType, errText, ERR_NUMERICAL,
                    "Matrices can only be multiplied by vectors when the number of matrix columns (%ld) equals the number of vector rows (%ld).",
                    (long) m->size2, (long) v->size);
            return;
        }
        // The output is a vector with one element per row of the matrix
        if (pplObjVector(o, 0, 1, m->size1) == NULL) {
            ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
            return;
        }
        a->real = b->real = 1;
        a->imag = b->imag = 0;
        a->flagComplex = b->flagComplex = 0;
        ppl_uaMul(context, a, b, o, status, errType, errText);
        vo = ((pplVector *) (o->auxil))->v;
        // Compute the product with BLAS
        if ((gslerr = gsl_blas_dgemv(CblasNoTrans, 1, m, v, 0, vo)) != 0) {
            ppl_funcError(status, errType, errText, ERR_NUMERICAL, "%s", gsl_strerror(gslerr));
            return;
        }
    } else if ((t1 == PPLOBJ_MAT) && (t2 == PPLOBJ_MAT)) // matrix-matrix multiplication
    {
        int gslerr = 0;
        gsl_matrix *m1 = ((pplMatrix *) (a->auxil))->m;
        gsl_matrix *m2 = ((pplMatrix *) (b->auxil))->m;
        gsl_matrix *mo = NULL;
        // A matrix product needs as many columns in the left matrix as rows in the right
        if (m1->size2 != m2->size1) {
            ppl_funcError(status, errType, errText, ERR_NUMERICAL,
                    "Matrices can only be multiplied when the number of matrix columns (%ld) in the left matrix equals the number of rows (%ld) in the right matrix.",
                    (long) m1->size2, (long) m2->size1);
            return;
        }
        // The output is a new matrix
        if (pplObjMatrix(o, 0, 1, m1->size1, m2->size2) == NULL) {
            ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
            return;
        }
        a->real = b->real = 1;
        a->imag = b->imag = 0;
        a->flagComplex = b->flagComplex = 0;
        ppl_uaMul(context, a, b, o, status, errType, errText);
        mo = ((pplMatrix *) (o->auxil))->m;
        // Compute the product with BLAS
        if ((gslerr = gsl_blas_dgemm(CblasNoTrans, CblasNoTrans, 1, m1, m2, 0, mo)) != 0) {
            ppl_funcError(status, errType, errText, ERR_NUMERICAL, "%s", gsl_strerror(gslerr));
            return;
        }
    } else // multiplying numbers
    {
        // Any other types must be cast to numbers, e.g. booleans
        if (ppl_castToNumber(context, a, errText, LSTR_LENGTH)) {
            *status = 1;
            *errType = ERR_TYPE;
            goto cast_fail;
        }
        // Cast the second operand
        if (ppl_castToNumber(context, b, errText, LSTR_LENGTH)) {
            *status = 1;
            *errType = ERR_TYPE;
            goto cast_fail;
        }
    am_numeric:
        o->objType =
                PPLOBJ_NUM; // Do this by hand rather than calling pplObjNum to save time (some field will be set by function call below)
        o->objPrototype = &pplObjPrototypes[PPLOBJ_NUM];
        o->self_lval = NULL;
        o->self_dval = NULL;
        o->self_this = NULL;
        o->amMalloced = 0;
        o->immutable = 0;
        ppl_uaMul(context, a, b, o, status, errType, errText);
    }
cast_fail:
    return;
}

//! ppl_opDiv - Evaluate the / operator, which divides numbers, vectors and matrices by numbers, and scales colors
//! \param [in] context - The Pyxplot context
//! \param [in] a - The numerator
//! \param [in] b - The denominator
//! \param [out] o - The result (may be the same object as <a>)
//! \param [in] invertible - Unused. Callers pass one for binary operators, and zero for in-place assignment
//! operators such as +=
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void ppl_opDiv(ppl_context *context, pplObj *a, pplObj *b, pplObj *o, const int invertible, int *status, int *errType,
               char *errText) {
    const int t1 = a->objType;
    const int t2 = b->objType;

    // Choose what division means from the types of the two operands
    if ((t1 == PPLOBJ_NUM) && (t2 == PPLOBJ_NUM)) {
        goto am_numeric;
    } else if ((t1 == PPLOBJ_COL) && (t2 == PPLOBJ_NUM)) // dividing colors by numbers
    {
        double r1, g1, b1;
        const double m = b->real;
        // Colors can only be divided by plain positive numbers
        if ((!b->dimensionless) || (b->flagComplex) || (!gsl_finite(m)) || (m < 0)) {
            ppl_funcError(status, errType, errText, ERR_RANGE,
                    "Can only divide colors by dimensionless, real, positive numbers.");
            return;
        }
        // Convert the color to RGB, and divide its components
        pplcol_toRGB(a, &r1, &g1, &b1);
        r1 /= m;
        g1 /= m;
        b1 /= m;
        pplObjColor(o, 0, SW_COLSPACE_RGB, r1, g1, b1, 0);
    } else if ((t1 == PPLOBJ_VEC) && (t2 == PPLOBJ_NUM)) // dividing vector by number
    {
        int i;
        gsl_vector *v = ((pplVector *) (a->auxil))->v;
        gsl_vector *vo = NULL;
        // Dividing a vector by a number: the number must be real
        if (b->flagComplex) {
            ppl_funcError(status, errType, errText, ERR_NUMERICAL,
                    "Vectors can only contain real numbers, and cannot be divided by complex numbers.");
            return;
        }
        a->real = 1;
        a->imag = 0;
        a->flagComplex = 0;
        // The output is a new vector, each element divided by the number
        if (pplObjVector(o, 0, 1, v->size) == NULL) {
            ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
            return;
        }
        ppl_uaDiv(context, a, b, o, status, errType, errText);
        vo = ((pplVector *) (o->auxil))->v;
        for (i = 0; i < v->size; i++) gsl_vector_set(vo, i, gsl_vector_get(v, i) * o->real);
        o->real = 0;
    } else if ((t1 == PPLOBJ_MAT) && (t2 == PPLOBJ_NUM)) // dividing matrix by number
    {
        int i, j;
        gsl_matrix *m = ((pplMatrix *) (a->auxil))->m;
        gsl_matrix *mo = NULL;
        // Dividing a matrix by a number: the number must be real
        if (b->flagComplex) {
            ppl_funcError(status, errType, errText, ERR_NUMERICAL,
                    "Matrices can only contain real numbers, and cannot be divided by complex numbers.");
            return;
        }
        a->real = 1;
        a->imag = 0;
        a->flagComplex = 0;
        // The output is a new matrix, each element divided by the number
        if (pplObjMatrix(o, 0, 1, m->size1, m->size2) == NULL) {
            ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
            return;
        }
        ppl_uaDiv(context, a, b, o, status, errType, errText);
        mo = ((pplMatrix *) (o->auxil))->m;
        for (i = 0; i < m->size1; i++)
            for (j = 0; j < m->size2; j++) gsl_matrix_set(mo, i, j, gsl_matrix_get(m, i, j) * o->real);
        o->real = 0;
    } else // dividing numbers
    {
        // Any other types must be cast to numbers, e.g. booleans
        if (ppl_castToNumber(context, a, errText, LSTR_LENGTH)) {
            *status = 1;
            *errType = ERR_TYPE;
            goto cast_fail;
        }
        // Cast the second operand
        if (ppl_castToNumber(context, b, errText, LSTR_LENGTH)) {
            *status = 1;
            *errType = ERR_TYPE;
            goto cast_fail;
        }
    am_numeric:
        o->objType =
                PPLOBJ_NUM; // Do this by hand rather than calling pplObjNum to save time (some field will be set by function call below)
        o->objPrototype = &pplObjPrototypes[PPLOBJ_NUM];
        o->self_lval = NULL;
        o->self_dval = NULL;
        o->self_this = NULL;
        o->amMalloced = 0;
        o->immutable = 0;
        ppl_uaDiv(context, a, b, o, status, errType, errText);
    }
cast_fail:
    return;
}
