// pplObj.c
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

#define _PPLOBJ_C 1

#include <stdlib.h>
#include <string.h>

#include "coreUtils/errorReport.h"
#include "coreUtils/memAlloc.h"
#include "coreUtils/list.h"
#include "coreUtils/dict.h"

#include "userspace/context.h"
#include "userspace/pplObj_fns.h"
#include "userspace/pplObjFunc.h"
#include "userspace/pplObjMethods.h"
#include "userspace/pplObjUnits.h"

// Items with ! should never be displayed because they are internal markers
const char *pplObjTypeNames[] = {"number",    "string",  "boolean", "date",        "color",     "dictionary", "module",
                                 "list",      "vector",  "matrix",  "fileHandle",  "function",  "type",       "null",
                                 "exception", "!global", "!zombie", "!expression", "!bytecode", "instance",   NULL};
const int pplObjTypeOrder[] = {2, 4, 2, 3, 5, 9, 11, 7, 6, 8, 13, 12, 14, 0, 15, 0, 0, 0, 0, 10};
pplObj *pplObjPrototypes;

//! pplObjInit - Create the prototype objects for each of Pyxplot's object types, which hold their methods. Has no
//! effect if called more than once.
//! \param [in] c - The Pyxplot context

void pplObjInit(ppl_context *c) {
    static int initialised = 0; // Only ever run this once
    int i;
    const int n = PPLOBJ_USER + 1;
    if (initialised) return;
    pplType *typeObjs = (pplType *) malloc(n * sizeof(pplType));
    pplObjPrototypes = (pplObj *) malloc(n * sizeof(pplObj));
    if ((typeObjs == NULL) || (pplObjPrototypes == NULL))
        ppl_fatal(&c->errcontext, __FILE__, __LINE__, "Out of memory.");
    pplObjMethodsInit(c);
    for (i = 0; i < n; i++) {
        typeObjs[i].refCount = 1;
        typeObjs[i].id = i;
        typeObjs[i].methods = pplObjMethods[i];
        pplObjPrototypes[i].refCount = 1;
        pplObjType(&pplObjPrototypes[i], 1, 1, &typeObjs[i]);
    }
    initialised = 1;
    return;
}

//! pplObjNum - Initialise an object as a dimensionless number
//! \param [out] in - The object to initialise
//! \param [in] amMalloced - Flag indicating whether the object itself was allocated with malloc()
//! \param [in] real - The real part of the number
//! \param [in] imag - The imaginary part of the number
//! \return Pointer to <in>

pplObj *pplObjNum(pplObj *in, const unsigned char amMalloced, const double real, const double imag) {
    int i;
    in->objType = PPLOBJ_NUM;
    in->real = real;
    in->imag = imag;
    in->dimensionless = 1;
    in->flagComplex = in->tempType = 0;
    in->objPrototype = &pplObjPrototypes[PPLOBJ_NUM];
    in->self_lval = NULL;
    in->self_dval = NULL;
    in->self_this = NULL;
    in->amMalloced = amMalloced;
    in->immutable = 0;
    for (i = 0; i < UNITS_MAX_BASEUNITS; i++) in->exponent[i] = 0;
    return in;
}

//! pplObjStr - Initialise an object as a string. The string is not copied.
//! \param [out] in - The object to initialise
//! \param [in] amMalloced - Flag indicating whether the object itself was allocated with malloc()
//! \param [in] auxilMalloced - Flag indicating whether <str> was allocated with malloc(), and should be freed with
//! the object
//! \param [in] str - The string
//! \return Pointer to <in>

pplObj *pplObjStr(pplObj *in, const unsigned char amMalloced, const unsigned char auxilMalloced, char *str) {
    in->objType = PPLOBJ_ZOM; // In case we get interrupted
    in->auxil = (void *) str;
    in->auxilMalloced = auxilMalloced;
    in->auxilLen = strlen(str) + 1;
    in->objPrototype = &pplObjPrototypes[PPLOBJ_STR];
    in->self_lval = NULL;
    in->self_dval = NULL;
    in->self_this = NULL;
    in->amMalloced = amMalloced;
    in->immutable = 0;
    in->objType = PPLOBJ_STR;
    return in;
}

//! pplObjBool - Initialise an object as a boolean
//! \param [out] in - The object to initialise
//! \param [in] amMalloced - Flag indicating whether the object itself was allocated with malloc()
//! \param [in] stat - The boolean value
//! \return Pointer to <in>

pplObj *pplObjBool(pplObj *in, const unsigned char amMalloced, const int stat) {
    int i;
    in->objType = PPLOBJ_BOOL;
    in->real = stat;
    in->auxil = NULL;
    in->objPrototype = &pplObjPrototypes[PPLOBJ_BOOL];
    in->self_lval = NULL;
    in->self_dval = NULL;
    in->self_this = NULL;
    in->auxilMalloced = 0;
    in->auxilLen = 0;
    in->amMalloced = amMalloced;
    in->immutable = 0;
    in->imag = 0; // Need to set these as booleans use same object comparison logic as numeric values
    in->dimensionless = 1;
    in->flagComplex = in->tempType = 0;
    for (i = 0; i < UNITS_MAX_BASEUNITS; i++) in->exponent[i] = 0;
    return in;
}

//! pplObjDate - Initialise an object as a date
//! \param [out] in - The object to initialise
//! \param [in] amMalloced - Flag indicating whether the object itself was allocated with malloc()
//! \param [in] unixTime - The date, expressed as a unix time
//! \return Pointer to <in>

pplObj *pplObjDate(pplObj *in, const unsigned char amMalloced, const double unixTime) {
    in->objType = PPLOBJ_DATE;
    in->real = unixTime;
    in->auxil = NULL;
    in->objPrototype = &pplObjPrototypes[PPLOBJ_DATE];
    in->self_lval = NULL;
    in->self_dval = NULL;
    in->self_this = NULL;
    in->auxilMalloced = 0;
    in->auxilLen = 0;
    in->amMalloced = amMalloced;
    in->immutable = 0;
    return in;
}

//! pplObjColor - Initialise an object as a color. Color components are clipped to the range 0-1.
//! \param [out] in - The object to initialise
//! \param [in] amMalloced - Flag indicating whether the object itself was allocated with malloc()
//! \param [in] scheme - The color space of the components, one of the SW_COLSPACE_* constants
//! \param [in] c1 - The first color component
//! \param [in] c2 - The second color component
//! \param [in] c3 - The third color component
//! \param [in] c4 - The fourth color component (CMYK only)
//! \return Pointer to <in>

pplObj *pplObjColor(pplObj *in, const unsigned char amMalloced, const int scheme, double c1, double c2, double c3,
                    double c4) {
    if (c1 < 0) c1 = 0;
    if (c1 > 1) c1 = 1;
    if (c2 < 0) c2 = 0;
    if (c2 > 1) c2 = 1;
    if (c3 < 0) c3 = 0;
    if (c3 > 1) c3 = 1;
    if (c4 < 0) c4 = 0;
    if (c4 > 1) c4 = 1;
    in->objType = PPLOBJ_COL;
    in->auxil = NULL;
    in->objPrototype = &pplObjPrototypes[PPLOBJ_COL];
    in->self_lval = NULL;
    in->self_dval = NULL;
    in->self_this = NULL;
    in->auxilMalloced = 0;
    in->auxilLen = 0;
    in->amMalloced = amMalloced;
    in->immutable = 0;
    in->exponent[0] = scheme;
    in->exponent[2] = 0;
    in->exponent[8] = c1;
    in->exponent[9] = c2;
    in->exponent[10] = c3;
    in->exponent[11] = c4;
    return in;
}

//! pplObjDict - Initialise an object as a dictionary
//! \param [out] in - The object to initialise
//! \param [in] amMalloced - Flag indicating whether the object itself was allocated with malloc()
//! \param [in] auxilMalloced - Flag indicating whether the dictionary is allocated with malloc()
//! \param [in] d - The dictionary to wrap, or NULL to create a new empty dictionary
//! \return Pointer to <in>, or NULL if memory allocation failed

pplObj *pplObjDict(pplObj *in, const unsigned char amMalloced, unsigned char auxilMalloced, dict *d) {
    in->objType = PPLOBJ_ZOM;
    if (d == NULL) in->auxil = (void *) ppl_dictInit(auxilMalloced);
    else in->auxil = (void *) d;
    if (in->auxil == NULL) return NULL;
    in->auxilMalloced = auxilMalloced;
    in->auxilLen = 0;
    in->objPrototype = &pplObjPrototypes[PPLOBJ_DICT];
    in->self_lval = NULL;
    in->self_dval = NULL;
    in->self_this = NULL;
    in->amMalloced = amMalloced;
    in->immutable = 0;
    in->objType = PPLOBJ_DICT;
    return in;
}

//! pplObjModule - Initialise an object as a module, with an empty dictionary of members
//! \param [out] in - The object to initialise
//! \param [in] amMalloced - Flag indicating whether the object itself was allocated with malloc()
//! \param [in] auxilMalloced - Flag indicating whether the dictionary of members is allocated with malloc()
//! \param [in] frozen - If true, the module's members cannot be modified by the user
//! \return Pointer to <in>, or NULL if memory allocation failed

pplObj *pplObjModule(pplObj *in, const unsigned char amMalloced, unsigned char auxilMalloced,
                     const unsigned char frozen) {
    in->objType = PPLOBJ_ZOM;
    in->auxil = (void *) ppl_dictInit(auxilMalloced);
    ((dict *) in->auxil)->immutable = frozen;
    if (in->auxil == NULL) return NULL;
    in->auxilMalloced = auxilMalloced;
    in->auxilLen = 0;
    in->objPrototype = &pplObjPrototypes[PPLOBJ_MOD];
    in->self_lval = NULL;
    in->self_dval = NULL;
    in->self_this = NULL;
    in->amMalloced = amMalloced;
    in->immutable = frozen;
    in->objType = PPLOBJ_MOD;
    return in;
}

//! pplObjList - Initialise an object as a list
//! \param [out] in - The object to initialise
//! \param [in] amMalloced - Flag indicating whether the object itself was allocated with malloc()
//! \param [in] auxilMalloced - Flag indicating whether the list is allocated with malloc()
//! \param [in] l - The list to wrap, or NULL to create a new empty list
//! \return Pointer to <in>, or NULL if memory allocation failed

pplObj *pplObjList(pplObj *in, const unsigned char amMalloced, unsigned char auxilMalloced, list *l) {
    in->objType = PPLOBJ_ZOM;
    if (l == NULL) in->auxil = (void *) ppl_listInit(auxilMalloced);
    else in->auxil = (void *) l;
    if (in->auxil == NULL) return NULL;
    in->auxilMalloced = auxilMalloced;
    in->auxilLen = 0;
    in->objPrototype = &pplObjPrototypes[PPLOBJ_LIST];
    in->self_lval = NULL;
    in->self_dval = NULL;
    in->self_this = NULL;
    in->amMalloced = amMalloced;
    in->immutable = 0;
    in->objType = PPLOBJ_LIST;
    return in;
}

//! pplObjVector - Initialise an object as a vector of zeros
//! \param [out] in - The object to initialise
//! \param [in] amMalloced - Flag indicating whether the object itself was allocated with malloc()
//! \param [in] auxilMalloced - Flag indicating whether the vector's storage is allocated with malloc(), or in the
//! current ppl_memAlloc context
//! \param [in] size - The number of elements in the vector
//! \return Pointer to <in>, or NULL if memory allocation failed

pplObj *pplObjVector(pplObj *in, const unsigned char amMalloced, const unsigned char auxilMalloced, const int size) {
    int i;
    pplVector *pv;
    pplVectorRaw *pvr;
    in->objType = PPLOBJ_ZOM;
    if (auxilMalloced) pv = (pplVector *) (in->auxil = malloc(sizeof(pplVector)));
    else pv = (pplVector *) (in->auxil = ppl_memAlloc(sizeof(pplVector)));
    if (pv == NULL) return NULL;
    if (auxilMalloced) pvr = pv->raw = (pplVectorRaw *) malloc(sizeof(pplVectorRaw));
    else pvr = pv->raw = (pplVectorRaw *) ppl_memAlloc(sizeof(pplVectorRaw));
    if (pvr == NULL) {
        if (auxilMalloced) free(pv);
        return NULL;
    }
    if ((pvr->v = gsl_vector_calloc(size)) == NULL) {
        if (auxilMalloced) {
            free(pv);
            free(pvr);
        }
        return NULL;
    }
    pvr->refCount = 1;
    pv->refCount = 1;
    pv->rawm = NULL;
    pv->v = pvr->v;
    in->auxilMalloced = auxilMalloced;
    in->auxilLen = sizeof(pplMatrix);
    in->objPrototype = &pplObjPrototypes[PPLOBJ_VEC];
    in->self_lval = NULL;
    in->self_dval = NULL;
    in->self_this = NULL;
    in->amMalloced = amMalloced;
    in->immutable = 0;
    in->dimensionless = 1;
    for (i = 0; i < UNITS_MAX_BASEUNITS; i++) in->exponent[i] = 0;
    in->objType = PPLOBJ_VEC;
    return in;
}

//! pplObjMatrix - Initialise an object as a matrix of zeros
//! \param [out] in - The object to initialise
//! \param [in] amMalloced - Flag indicating whether the object itself was allocated with malloc()
//! \param [in] auxilMalloced - Flag indicating whether the matrix's storage is allocated with malloc(), or in the
//! current ppl_memAlloc context
//! \param [in] size1 - The number of rows in the matrix
//! \param [in] size2 - The number of columns in the matrix
//! \return Pointer to <in>, or NULL if memory allocation failed

pplObj *pplObjMatrix(pplObj *in, const unsigned char amMalloced, const unsigned char auxilMalloced, const int size1,
                     const int size2) {
    int i;
    pplMatrix *pm;
    pplMatrixRaw *pmr;
    in->objType = PPLOBJ_ZOM;
    if (auxilMalloced) pm = (pplMatrix *) (in->auxil = malloc(sizeof(pplMatrix)));
    else pm = (pplMatrix *) (in->auxil = ppl_memAlloc(sizeof(pplMatrix)));
    if (pm == NULL) return NULL;
    if (auxilMalloced) pmr = pm->raw = (pplMatrixRaw *) malloc(sizeof(pplMatrixRaw));
    else pmr = pm->raw = (pplMatrixRaw *) ppl_memAlloc(sizeof(pplMatrixRaw));
    if (pmr == NULL) {
        if (auxilMalloced) free(pm);
        return NULL;
    }
    if ((pmr->m = gsl_matrix_calloc(size1, size2)) == NULL) {
        if (auxilMalloced) {
            free(pm);
            free(pmr);
        }
        return NULL;
    }
    pmr->refCount = 1;
    pm->refCount = 1;
    pm->sliceNext = 0;
    pm->m = pmr->m;
    in->auxilMalloced = auxilMalloced;
    in->auxilLen = sizeof(pplMatrix);
    in->objPrototype = &pplObjPrototypes[PPLOBJ_MAT];
    in->self_lval = NULL;
    in->self_dval = NULL;
    in->self_this = NULL;
    in->amMalloced = amMalloced;
    in->immutable = 0;
    in->dimensionless = 1;
    for (i = 0; i < UNITS_MAX_BASEUNITS; i++) in->exponent[i] = 0;
    in->objType = PPLOBJ_MAT;
    return in;
}

//! pplObjFile - Initialise an object as a file handle
//! \param [out] in - The object to initialise
//! \param [in] amMalloced - Flag indicating whether the object itself was allocated with malloc()
//! \param [in] auxilMalloced - Flag indicating whether the file descriptor structure is allocated with malloc()
//! \param [in] f - The file handle
//! \param [in] pipe - Zero for a file, one for a pipe opened with popen(), or two for stdin/stdout, which are never
//! closed
//! \return Pointer to <in>, or NULL if memory allocation failed

pplObj *pplObjFile(pplObj *in, const unsigned char amMalloced, const unsigned char auxilMalloced, FILE *f,
                   const int pipe) {
    in->objType = PPLOBJ_ZOM;
    if (auxilMalloced) in->auxil = (void *) malloc(sizeof(pplFile));
    else in->auxil = (void *) ppl_memAlloc(sizeof(pplFile));
    if (in->auxil == NULL) return NULL;
    ((pplFile *) in->auxil)->refCount = 1;
    ((pplFile *) in->auxil)->file = f;
    ((pplFile *) in->auxil)->open = 1;
    ((pplFile *) in->auxil)->pipe = pipe;
    in->auxilMalloced = auxilMalloced;
    in->auxilLen = sizeof(pplFile);
    in->objPrototype = &pplObjPrototypes[PPLOBJ_FILE];
    in->self_lval = NULL;
    in->self_dval = NULL;
    in->self_this = NULL;
    in->amMalloced = amMalloced;
    in->immutable = 0;
    in->objType = PPLOBJ_FILE;
    return in;
}

//! pplObjFunc - Initialise an object as a function
//! \param [out] in - The object to initialise
//! \param [in] amMalloced - Flag indicating whether the object itself was allocated with malloc()
//! \param [in] auxilMalloced - Flag indicating whether the function descriptor <f> was allocated with malloc()
//! \param [in] f - The function descriptor
//! \return Pointer to <in>

pplObj *pplObjFunc(pplObj *in, const unsigned char amMalloced, const unsigned char auxilMalloced, pplFunc *f) {
    in->objType = PPLOBJ_ZOM;
    in->auxilLen = sizeof(pplFunc);
    in->auxil = (void *) f;
    in->auxilMalloced = auxilMalloced;
    in->objPrototype = &pplObjPrototypes[PPLOBJ_FUNC];
    in->self_lval = NULL;
    in->self_dval = NULL;
    in->self_this = NULL;
    in->amMalloced = amMalloced;
    in->immutable = 0;
    in->objType = PPLOBJ_FUNC;
    return in;
}

//! pplObjType - Initialise an object as a type object, which describes one of Pyxplot's object types
//! \param [out] in - The object to initialise
//! \param [in] amMalloced - Flag indicating whether the object itself was allocated with malloc()
//! \param [in] auxilMalloced - Flag indicating whether the type descriptor <t> was allocated with malloc()
//! \param [in] t - The type descriptor
//! \return Pointer to <in>

pplObj *pplObjType(pplObj *in, const unsigned char amMalloced, const unsigned char auxilMalloced, pplType *t) {
    in->objType = PPLOBJ_ZOM;
    in->auxilLen = sizeof(pplType);
    in->auxil = (void *) t;
    in->auxilMalloced = auxilMalloced;
    in->objPrototype = &pplObjPrototypes[PPLOBJ_TYPE];
    in->self_lval = NULL;
    in->self_dval = NULL;
    in->self_this = NULL;
    in->amMalloced = amMalloced;
    in->immutable = 1; // Types are always immutable
    in->objType = PPLOBJ_TYPE;
    return in;
}

//! pplObjNull - Initialise an object as null
//! \param [out] in - The object to initialise
//! \param [in] amMalloced - Flag indicating whether the object itself was allocated with malloc()
//! \return Pointer to <in>

pplObj *pplObjNull(pplObj *in, const unsigned char amMalloced) {
    in->objType = PPLOBJ_NULL;
    in->objPrototype = &pplObjPrototypes[PPLOBJ_NULL];
    in->self_lval = NULL;
    in->self_dval = NULL;
    in->self_this = NULL;
    in->amMalloced = amMalloced;
    in->immutable = 0;
    return in;
}

//! pplObjException - Initialise an object as an exception. The message is not copied.
//! \param [out] in - The object to initialise
//! \param [in] amMalloced - Flag indicating whether the object itself was allocated with malloc()
//! \param [in] auxilMalloced - Flag indicating whether <str> was allocated with malloc()
//! \param [in] str - The exception message
//! \param [in] errCode - The type of error, one of the ERR_* constants
//! \return Pointer to <in>

pplObj *pplObjException(pplObj *in, const unsigned char amMalloced, const unsigned char auxilMalloced, char *str,
                        const int errCode) {
    in->objType = PPLOBJ_ZOM;
    in->auxil = (void *) str;
    in->auxilMalloced = auxilMalloced;
    in->auxilLen = strlen(str) + 1;
    in->real = errCode;
    in->objPrototype = &pplObjPrototypes[PPLOBJ_EXC];
    in->self_lval = NULL;
    in->self_dval = NULL;
    in->self_this = NULL;
    in->amMalloced = amMalloced;
    in->immutable = 0;
    in->objType = PPLOBJ_EXC;
    return in;
}

//! pplObjGlobal - Initialise an object as a placeholder, which marks a name in a local namespace as referring to a
//! global variable
//! \param [out] in - The object to initialise
//! \param [in] amMalloced - Flag indicating whether the object itself was allocated with malloc()
//! \return Pointer to <in>

pplObj *pplObjGlobal(pplObj *in, const unsigned char amMalloced) {
    in->objType = PPLOBJ_GLOB;
    in->objPrototype = &pplObjPrototypes[PPLOBJ_GLOB];
    in->self_lval = NULL;
    in->self_dval = NULL;
    in->self_this = NULL;
    in->amMalloced = amMalloced;
    in->immutable = 0;
    return in;
}

//! pplObjExpression - Initialise an object as a compiled expression
//! \param [out] in - The object to initialise
//! \param [in] amMalloced - Flag indicating whether the object itself was allocated with malloc()
//! \param [in] bytecode - The compiled expression
//! \return Pointer to <in>

pplObj *pplObjExpression(pplObj *in, const unsigned char amMalloced, void *bytecode) {
    in->objType = PPLOBJ_EXP;
    in->auxil = bytecode;
    in->auxilMalloced = 1;
    in->auxilLen = 0;
    in->objPrototype = &pplObjPrototypes[PPLOBJ_EXP];
    in->self_lval = NULL;
    in->self_dval = NULL;
    in->self_this = NULL;
    in->amMalloced = amMalloced;
    in->immutable = 1;
    return in;
}

//! pplObjBytecode - Initialise an object as a compiled command line
//! \param [out] in - The object to initialise
//! \param [in] amMalloced - Flag indicating whether the object itself was allocated with malloc()
//! \param [in] parserline - The compiled command line
//! \return Pointer to <in>

pplObj *pplObjBytecode(pplObj *in, const unsigned char amMalloced, void *parserline) {
    in->objType = PPLOBJ_BYT;
    in->auxil = parserline;
    in->auxilMalloced = 1;
    in->auxilLen = 0;
    in->objPrototype = &pplObjPrototypes[PPLOBJ_BYT];
    in->self_lval = NULL;
    in->self_dval = NULL;
    in->self_this = NULL;
    in->amMalloced = amMalloced;
    in->immutable = 1;
    return in;
}

//! pplObjZom - Initialise an object as a zombie, i.e. an object which has been deleted
//! \param [out] in - The object to initialise
//! \param [in] amMalloced - Flag indicating whether the object itself was allocated with malloc()
//! \return Pointer to <in>

pplObj *pplObjZom(pplObj *in, const unsigned char amMalloced) {
    in->objType = PPLOBJ_ZOM;
    in->objPrototype = &pplObjPrototypes[PPLOBJ_ZOM];
    in->self_lval = NULL;
    in->self_dval = NULL;
    in->self_this = NULL;
    in->amMalloced = amMalloced;
    in->immutable = 0;
    return in;
}

//! pplObjUser - Initialise an object as an instance of a user-defined class, with an empty dictionary of members
//! \param [out] in - The object to initialise
//! \param [in] amMalloced - Flag indicating whether the object itself was allocated with malloc()
//! \param [in] auxilMalloced - Flag indicating whether the dictionary of members is allocated with malloc()
//! \param [in] prototype - The class from which the instance inherits its methods
//! \return Pointer to <in>, or NULL if memory allocation failed

pplObj *pplObjUser(pplObj *in, const unsigned char amMalloced, unsigned char auxilMalloced, pplObj *prototype) {
    in->objType = PPLOBJ_ZOM;
    in->auxil = (void *) ppl_dictInit(auxilMalloced);
    if (in->auxil == NULL) return NULL;
    in->auxilMalloced = auxilMalloced;
    in->auxilLen = 0;
    in->objPrototype = (pplObj *) malloc(sizeof(pplObj));
    if (in->objPrototype == NULL) return NULL;
    in->objPrototype->refCount = 1;
    pplObjCpy(in->objPrototype, prototype, 0, 1, 1);
    in->self_lval = NULL;
    in->self_dval = NULL;
    in->self_this = NULL;
    in->amMalloced = amMalloced;
    in->immutable = 0;
    in->objType = PPLOBJ_USER;
    return in;
}

//! pplObjCpy - Make a copy of an object. Compound objects (lists, dictionaries, vectors, etc.) are not duplicated;
//! instead their reference counts are incremented, so that the copy refers to the same storage.
//! \param [out] out - The object to copy into, or NULL to allocate a new object
//! \param [in] in - The object to copy
//! \param [in] lval - If true, the copy records that it was derived from <in>, so that assignments to it can be
//! propagated back to <in>
//! \param [in] outMalloced - Flag indicating whether <out> is allocated with malloc() (if <out> is NULL, whether the
//! new object should be allocated with malloc() or in the current ppl_memAlloc context)
//! \param [in] useMalloc - Flag indicating whether any storage that needs to be allocated should use malloc()
//! \return Pointer to the copy, or NULL on failure

pplObj *pplObjCpy(pplObj *out, pplObj *in, const unsigned char lval, const unsigned char outMalloced,
                  const unsigned char useMalloc) {
    const int rc = (out == NULL) ? 1 : out->refCount;
    int t;

    if (in == NULL) return NULL;

    if (out == NULL) {
        if (outMalloced) out = (pplObj *) malloc(sizeof(pplObj));
        else out = (pplObj *) ppl_memAlloc(sizeof(pplObj));
        if (out == NULL) return out;
    }

    t = in->objType;
    memcpy(out, in, sizeof(pplObj));
    if (lval) {
        out->self_lval = in;
        __sync_add_and_fetch(&out->self_lval->refCount, 1);
    } else {
        out->self_lval = NULL;
    }
    out->self_dval = NULL;
    out->self_this = NULL;
    out->refCount = rc;
    out->amMalloced = outMalloced;
    out->immutable = in->immutable &&
            ((t == PPLOBJ_EXP) || (t == PPLOBJ_BYT) || (t == PPLOBJ_LIST) || (t == PPLOBJ_VEC) || (t == PPLOBJ_MAT) ||
             (t == PPLOBJ_DICT) || (t == PPLOBJ_MOD) || (t == PPLOBJ_USER) || (t == PPLOBJ_FILE) ||
             (t == PPLOBJ_FUNC) || (t == PPLOBJ_TYPE));

    switch (in->objType) {
        case PPLOBJ_STR:
        case PPLOBJ_EXC: // copy string value
            if (in->auxil == NULL) {
                out->auxilMalloced = 0;
                return out;
            }
            if (useMalloc) out->auxil = (void *) malloc(in->auxilLen);
            else out->auxil = (void *) ppl_memAlloc(in->auxilLen);
            if (out->auxil == NULL) {
                out->objType = PPLOBJ_ZOM;
                return NULL;
            }
            memcpy(out->auxil, in->auxil, in->auxilLen);
            out->auxilMalloced = useMalloc;
            break;
        case PPLOBJ_EXP: {
            // Copying bytecode is difficult. Assume that original will outlive the copy
            out->auxilMalloced = 0;
            break;
        }
        case PPLOBJ_BYT: {
            parserLine *item = (parserLine *) out->auxil;
            if (item != NULL) __sync_add_and_fetch(&item->refCount, 1);
            break;
        }
        case PPLOBJ_USER: {
            pplObj *p = (pplObj *) malloc(sizeof(pplObj)); // Copy prototype pointer object
            if (p == NULL) {
                out->objType = PPLOBJ_ZOM;
                return NULL;
            }
            pplObjCpy(p, out->objPrototype, 0, 1, 1);
            p->refCount = 1;
            out->objPrototype = p;
        }
        case PPLOBJ_DICT: // dictionary -- pass by pointer
        case PPLOBJ_MOD:
            __sync_add_and_fetch(&((dict *) (out->auxil))->refCount, 1);
            break;
        case PPLOBJ_LIST: // list
            __sync_add_and_fetch(&((list *) (out->auxil))->refCount, 1);
            break;
        case PPLOBJ_VEC: // vector
        {
            pplVector *pv = (pplVector *) out->auxil;
            __sync_add_and_fetch(&pv->refCount, 1);
            if (pv->raw != NULL) __sync_add_and_fetch(&pv->raw->refCount, 1);
            if (pv->rawm != NULL) __sync_add_and_fetch(&pv->rawm->refCount, 1);
            break;
        }
        case PPLOBJ_MAT: // matrix
        {
            pplMatrix *pm = (pplMatrix *) out->auxil;
            __sync_add_and_fetch(&pm->refCount, 1);
            __sync_add_and_fetch(&pm->raw->refCount, 1);
            break;
        }
        case PPLOBJ_FILE: // file handle
            __sync_add_and_fetch(&((pplFile *) (out->auxil))->refCount, 1);
            break;
        case PPLOBJ_FUNC: // function pointer
            __sync_add_and_fetch(&((pplFunc *) (out->auxil))->refCount, 1);
            break;
        case PPLOBJ_TYPE: // data type
            __sync_add_and_fetch(&((pplType *) (out->auxil))->refCount, 1);
            break;
    }
    return out;
}

//! pplObjDeepCpy - Make a deep copy of an object, duplicating the storage of compound objects such as lists,
//! dictionaries, vectors and matrices
//! \param [out] out - The object to copy into, or NULL to allocate a new object
//! \param [in] in - The object to copy
//! \param [in] deep - If true, the members of lists and dictionaries are themselves deep-copied; otherwise only the
//! containers are duplicated
//! \param [in] outMalloced - Flag indicating whether <out> is allocated with malloc() (if <out> is NULL, whether the
//! new object should be allocated with malloc() or in the current ppl_memAlloc context)
//! \param [in] useMalloc - Flag indicating whether new storage should be allocated with malloc()
//! \return Pointer to the copy, or NULL on failure

pplObj *pplObjDeepCpy(pplObj *out, pplObj *in, const int deep, const unsigned char outMalloced,
                      unsigned char useMalloc) {
    const int t = in->objType;

    if (in == NULL) return NULL;

    if (out == NULL) {
        if (outMalloced) out = (pplObj *) malloc(sizeof(pplObj));
        else out = (pplObj *) ppl_memAlloc(sizeof(pplObj));
        if (out == NULL) return out;
    }

    switch (t) {
        case PPLOBJ_DICT:
        case PPLOBJ_MOD:
        case PPLOBJ_USER: {
            pplObj *item;
            char *key;
            dict *d;
            dictIterator *di = ppl_dictIterateInit((dict *) in->auxil);
            memcpy(out, in, sizeof(pplObj));
            out->objType = PPLOBJ_ZOM;
            out->self_lval = NULL;
            out->self_dval = NULL;
            out->self_this = NULL;
            out->amMalloced = outMalloced;
            out->auxil = (void *) (d = ppl_dictInit(useMalloc));
            if (out->auxil == NULL) return NULL;
            while ((item = (pplObj *) ppl_dictIterate(&di, &key)) != NULL) {
                pplObj v;
                v.refCount = 1;
                if (!deep) pplObjCpy(&v, item, 0, useMalloc, useMalloc);
                else pplObjDeepCpy(&v, item, 1, useMalloc, useMalloc);
                ppl_dictAppendCpy(d, key, &v, sizeof(pplObj));
            }
            out->objType = t;
            return out;
        }
        case PPLOBJ_LIST: {
            pplObj *item;
            list *l;
            listIterator *li = ppl_listIterateInit((list *) in->auxil);
            memcpy(out, in, sizeof(pplObj));
            out->objType = PPLOBJ_ZOM;
            out->self_lval = NULL;
            out->self_dval = NULL;
            out->self_this = NULL;
            out->amMalloced = outMalloced;
            out->auxil = (void *) (l = ppl_listInit(useMalloc));
            if (out->auxil == NULL) return NULL;
            while ((item = (pplObj *) ppl_listIterate(&li)) != NULL) {
                pplObj v;
                v.refCount = 1;
                if (!deep) pplObjCpy(&v, item, 0, useMalloc, useMalloc);
                else pplObjDeepCpy(&v, item, 1, useMalloc, useMalloc);
                ppl_listAppendCpy(l, &v, sizeof(pplObj));
            }
            out->objType = t;
            return out;
        }
        default:
            return pplObjCpy(out, in, 0, outMalloced, useMalloc);
    }
}
