// defaultFuncs.c
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

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <string.h>

#include <gsl/gsl_cdf.h>
#include <gsl/gsl_complex.h>
#include <gsl/gsl_complex_math.h>
#include <gsl/gsl_const_mksa.h>
#include <gsl/gsl_const_num.h>
#include <gsl/gsl_math.h>
#include <gsl/gsl_sf_bessel.h>
#include <gsl/gsl_sf_ellint.h>
#include <gsl/gsl_sf_elljac.h>
#include <gsl/gsl_sf_erf.h>
#include <gsl/gsl_sf_expint.h>
#include <gsl/gsl_sf_gamma.h>
#include <gsl/gsl_sf_hyperg.h>
#include <gsl/gsl_sf_lambert.h>
#include <gsl/gsl_sf_legendre.h>
#include <gsl/gsl_sf_zeta.h>
#define GSL_RANGE_CHECK_OFF 1

#include "coreUtils/dict.h"
#include "mathsTools/dcfmath.h"
#include "settings/settings.h"
#include "stringTools/asciidouble.h"

#include "expressions/expCompile_fns.h"
#include "expressions/expEval.h"
#include "expressions/expEvalOps.h"
#include "expressions/fnCall.h"
#include "expressions/traceback_fns.h"
#include "userspace/garbageCollector.h"
#include "userspace/pplObj_fns.h"
#include "userspace/pplObjCmp.h"
#include "userspace/pplObjFunc.h"
#include "userspace/unitsArithmetic.h"

#include "defaultObjs/airyFuncs.h"
#include "defaultObjs/defaultFuncs.h"
#include "defaultObjs/defaultFuncsMacros.h"
#include "defaultObjs/zetaRiemann.h"

#include "texify.h"

#define STACK_POP \
    { \
        c->stackPtr--; \
        ppl_garbageObject(&c->stack[c->stackPtr]); \
        if (c->stack[c->stackPtr].refCount != 0) { fprintf(stderr, "Stack forward reference detected."); } \
    }

//! ppl_addMagicFunction - Add a "magic" function, such as diff_d or int_d, to a namespace. Magic functions are
//! handled specially by the expression evaluator, because their arguments are not evaluated in the usual way.
//! \param [in|out] n - The namespace to add the function to
//! \param [in] name - The name of the function
//! \param [in] id - The number which identifies the magic function to the expression evaluator
//! \param [in] shortdesc - A short description of the function's syntax, e.g. "int_dx(e,min,max)"
//! \param [in] latex - A template for rendering calls to the function in LaTeX
//! \param [in] desc - A description of the function, displayed by the help command

void ppl_addMagicFunction(dict *n, const char *name, const int id, char *shortdesc, char *latex, char *desc) {
    pplObj v;
    pplFunc *f = malloc(sizeof(pplFunc));
    if (f == NULL) return;
    f->functionType = PPL_FUNC_MAGIC;
    f->refCount = 1;
    f->minArgs = id;
    f->maxArgs = 0;
    f->functionPtr = NULL;
    f->argList = NULL;
    f->minActive = NULL;
    f->maxActive = NULL;
    f->numOnly = 0;
    f->notNan = 0;
    f->realOnly = 0;
    f->dimlessOnly = 0;
    f->next = NULL;
    f->LaTeX = latex;
    f->descriptionShort = shortdesc;
    f->description = desc;
    f->needSelfThis = 0;
    v.refCount = 1;
    if (pplObjFunc(&v, 1, 1, f) != NULL) ppl_dictAppendCpy(n, name, (void *) &v, sizeof(v));
    return;
}

//! ppl_addSystemFunc - Add a function implemented in C to a namespace
//! \param [in|out] n - The namespace to add the function to
//! \param [in] name - The name of the function
//! \param [in] minArgs - The minimum number of arguments the function accepts
//! \param [in] maxArgs - The maximum number of arguments the function accepts
//! \param [in] numOnly - If true, all arguments must be numbers
//! \param [in] notNan - If true, all arguments must be finite
//! \param [in] realOnly - If true, all arguments must be real
//! \param [in] dimlessOnly - If true, all arguments must be dimensionless
//! \param [in] fn - Pointer to the C function which implements the function
//! \param [in] shortdesc - A short description of the function's syntax, e.g. "abs(z)"
//! \param [in] latex - A template for rendering calls to the function in LaTeX
//! \param [in] desc - A description of the function, displayed by the help command
//! \return The descriptor of the new function, or NULL if memory allocation failed

pplFunc *ppl_addSystemFunc(dict *n, const char *name, const int minArgs, const int maxArgs, const int numOnly,
                           const int notNan, const int realOnly, const int dimlessOnly, void *fn, char *shortdesc,
                           char *latex, char *desc) {
    pplObj v;
    pplFunc *f = malloc(sizeof(pplFunc));
    if (f == NULL) return NULL;
    f->functionType = PPL_FUNC_SYSTEM;
    f->refCount = 1;
    f->minArgs = minArgs;
    f->maxArgs = maxArgs;
    f->functionPtr = fn;
    f->argList = NULL;
    f->minActive = NULL;
    f->maxActive = NULL;
    f->numOnly = numOnly;
    f->notNan = notNan;
    f->realOnly = realOnly;
    f->dimlessOnly = dimlessOnly;
    f->next = NULL;
    f->LaTeX = latex;
    f->descriptionShort = shortdesc;
    f->description = desc;
    f->needSelfThis = 0;
    v.refCount = 1;
    if (pplObjFunc(&v, 1, 1, f) != NULL) ppl_dictAppendCpy(n, name, (void *) &v, sizeof(v));
    return f;
}

//! ppl_addSystemMethod - Add a method implemented in C to the dictionary of methods of an object type. This is the
//! same as ppl_addSystemFunc(), except that the method is passed the object it was called on.
//! \param [in|out] n - The dictionary of methods to add the method to
//! \param [in] name - The name of the method
//! \param [in] minArgs - The minimum number of arguments the method accepts
//! \param [in] maxArgs - The maximum number of arguments the method accepts
//! \param [in] numOnly - If true, all arguments must be numbers
//! \param [in] notNan - If true, all arguments must be finite
//! \param [in] realOnly - If true, all arguments must be real
//! \param [in] dimlessOnly - If true, all arguments must be dimensionless
//! \param [in] fn - Pointer to the C function which implements the method
//! \param [in] shortdesc - A short description of the method's syntax, e.g. "find(x)"
//! \param [in] latex - A template for rendering calls to the method in LaTeX
//! \param [in] desc - A description of the method, displayed by the help command

void ppl_addSystemMethod(dict *n, char *name, const int minArgs, const int maxArgs, const int numOnly, const int notNan,
                         const int realOnly, const int dimlessOnly, void *fn, char *shortdesc, char *latex,
                         char *desc) {
    pplFunc *f = ppl_addSystemFunc(n, name, minArgs, maxArgs, numOnly, notNan, realOnly, dimlessOnly, fn, shortdesc,
                                   latex, desc);
    f->needSelfThis = 1;
}

//! pplfunc_abs - Implement the function abs(z) in the default namespace, which returns the absolute magnitude of z
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_abs(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "abs(z)";
    IF_1COMPLEX {
        OUTPUT.real = hypot(in[0].real, in[0].imag);
    }
    ELSE_REAL {
        OUTPUT.real = fabs(in[0].real);
    }
    ENDIF;
    ppl_unitsDimCpy(&OUTPUT, in);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_acos - Implement the function acos(z) in the default namespace, which returns the arccosine of z
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_acos(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "acos(z)";
    gsl_complex z;
    IF_1COMPLEX {
        GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
        z = gsl_complex_arccos(z);
    }
    ELSE_REAL {
        z = gsl_complex_arccos_real(in[0].real);
    }
    ENDIF;
    CLEANUP_GSLCOMPLEX;
    CLEANUP_APPLYUNIT(UNIT_ANGLE);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_acosh - Implement the function acosh(z) in the default namespace, which returns the hyperbolic arccosine of
//! z
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_acosh(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "acosh(z)";
    gsl_complex z;
    IF_1COMPLEX {
        GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
        z = gsl_complex_arccosh(z);
    }
    ELSE_REAL {
        z = gsl_complex_arccosh_real(in[0].real);
    }
    ENDIF;
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_acot - Implement the function acot(z) in the default namespace, which returns the arccotangent of z
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_acot(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "acot(z)";
    gsl_complex z;
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    z = gsl_complex_arccot(z);
    CLEANUP_GSLCOMPLEX;
    CLEANUP_APPLYUNIT(UNIT_ANGLE);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_acoth - Implement the function acoth(z) in the default namespace, which returns the hyperbolic arccotangent
//! of z
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_acoth(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "acoth(z)";
    gsl_complex z;
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    z = gsl_complex_arccoth(z);
    CLEANUP_GSLCOMPLEX;
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_acsc - Implement the function acsc(z) in the default namespace, which returns the arccosecant of z
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_acsc(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "acsc(z)";
    gsl_complex z;
    IF_1COMPLEX {
        GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
        z = gsl_complex_arccsc(z);
    }
    ELSE_REAL {
        z = gsl_complex_arccsc_real(in[0].real);
    }
    ENDIF;
    CLEANUP_GSLCOMPLEX;
    CLEANUP_APPLYUNIT(UNIT_ANGLE);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_acsch - Implement the function acsch(z) in the default namespace, which returns the hyperbolic arccosecant
//! of z
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_acsch(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "acsch(z)";
    gsl_complex z;
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    z = gsl_complex_arccsch(z);
    CLEANUP_GSLCOMPLEX;
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_airy_ai - Implement the function airy_ai(z) in the default namespace, which returns the Airy function Ai
//! evaluated at z
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_airy_ai(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "airy_ai(z)";
    gsl_complex zi, z;
    GSL_SET_COMPLEX(&zi, in[0].real, in[0].imag);
    airy_ai(zi, &z, status, errText);
    if (*status) {
        *errType = ERR_NUMERICAL;
        return;
    }
    CLEANUP_GSLCOMPLEX;
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_airy_ai_diff - Implement the function airy_ai_diff(z) in the default namespace, which returns the first
//! derivative of the Airy function Ai evaluated at z
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_airy_ai_diff(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "airy_ai_diff(z)";
    gsl_complex zi, z;
    GSL_SET_COMPLEX(&zi, in[0].real, in[0].imag);
    airy_ai_diff(zi, &z, status, errText);
    if (*status) {
        *errType = ERR_NUMERICAL;
        return;
    }
    CLEANUP_GSLCOMPLEX;
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_airy_bi - Implement the function airy_bi(z) in the default namespace, which returns the Airy function Bi
//! evaluated at z
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_airy_bi(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "airy_bi(z)";
    gsl_complex zi, z;
    GSL_SET_COMPLEX(&zi, in[0].real, in[0].imag);
    airy_bi(zi, &z, status, errText);
    if (*status) {
        *errType = ERR_NUMERICAL;
        return;
    }
    CLEANUP_GSLCOMPLEX;
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_airy_bi_diff - Implement the function airy_bi_diff(z) in the default namespace, which returns the first
//! derivative of the Airy function Bi evaluated at z
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_airy_bi_diff(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "airy_bi_diff(z)";
    gsl_complex zi, z;
    GSL_SET_COMPLEX(&zi, in[0].real, in[0].imag);
    airy_bi_diff(zi, &z, status, errText);
    if (*status) {
        *errType = ERR_NUMERICAL;
        return;
    }
    CLEANUP_GSLCOMPLEX;
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_arg - Implement the function arg(z) in the default namespace, which returns the argument of the complex
//! number z
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_arg(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "arg(z)";
    gsl_complex z;
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    OUTPUT.real = gsl_complex_arg(z);
    CLEANUP_APPLYUNIT(UNIT_ANGLE);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_asec - Implement the function asec(z) in the default namespace, which returns the arcsecant of z
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_asec(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "asec(z)";
    gsl_complex z;
    IF_1COMPLEX {
        GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
        z = gsl_complex_arcsec(z);
    }
    ELSE_REAL {
        z = gsl_complex_arcsec_real(in[0].real);
    }
    ENDIF;
    CLEANUP_GSLCOMPLEX;
    CLEANUP_APPLYUNIT(UNIT_ANGLE);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_asech - Implement the function asech(z) in the default namespace, which returns the hyperbolic arcsecant of
//! z
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_asech(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "asech(z)";
    gsl_complex z;
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    z = gsl_complex_arcsech(z);
    CLEANUP_GSLCOMPLEX;
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_asin - Implement the function asin(z) in the default namespace, which returns the arcsine of z
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_asin(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "asin(z)";
    gsl_complex z;
    IF_1COMPLEX {
        GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
        z = gsl_complex_arcsin(z);
    }
    ELSE_REAL {
        z = gsl_complex_arcsin_real(in[0].real);
    }
    ENDIF;
    CLEANUP_GSLCOMPLEX;
    CLEANUP_APPLYUNIT(UNIT_ANGLE);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_asinh - Implement the function asinh(z) in the default namespace, which returns the hyperbolic arcsine of z
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_asinh(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "asinh(z)";
    gsl_complex z;
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    z = gsl_complex_arcsinh(z);
    CLEANUP_GSLCOMPLEX;
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_atan - Implement the function atan(z) in the default namespace, which returns the arctangent of z
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_atan(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "atan(z)";
    gsl_complex z;
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    z = gsl_complex_arctan(z);
    CLEANUP_GSLCOMPLEX;
    CLEANUP_APPLYUNIT(UNIT_ANGLE);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_atanh - Implement the function atanh(z) in the default namespace, which returns the hyperbolic arctangent of
//! z
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_atanh(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "atanh(z)";
    gsl_complex z;
    IF_1COMPLEX {
        GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
        z = gsl_complex_arctanh(z);
    }
    ELSE_REAL {
        z = gsl_complex_arctanh_real(in[0].real);
    }
    ENDIF;
    CLEANUP_GSLCOMPLEX;
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_atan2 - Implement the function atan2(x,y) in the default namespace, which returns the arctangent of x/y.
//! Unlike atan(y/x), atan2(x,y) takes account of the signs of both x and y to remove the degeneracy between (1,1) and
//! (-1,-1)
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_atan2(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "atan2(x,y)";
    CHECK_2INPUT_DIMMATCH;
    OUTPUT.real = atan2(in[0].real, in[1].real);
    CLEANUP_APPLYUNIT(UNIT_ANGLE);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_besseli - Implement the function besseli(l,x) in the default namespace, which evaluates the lth regular
//! modified spherical Bessel function at x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_besseli(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "besseli(l,x)";
    CHECK_NEEDINT(in[0], "l", "function can only evaluate integer-order Bessel functions");
    OUTPUT.real = gsl_sf_bessel_il_scaled((int) in[0].real, in[1].real);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_besselI - Implement the function besselI(l,x) in the default namespace, which evaluates the lth regular
//! modified cylindrical Bessel function at x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_besselI(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "besselI(l,x)";
    CHECK_NEEDINT(in[0], "l", "function can only evaluate integer-order Bessel functions");
    OUTPUT.real = gsl_sf_bessel_In((int) in[0].real, in[1].real);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_besselj - Implement the function besselj(l,x) in the default namespace, which evaluates the lth regular
//! spherical Bessel function at x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_besselj(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "besselj(l,x)";
    CHECK_NEEDINT(in[0], "l", "function can only evaluate integer-order Bessel functions");
    OUTPUT.real = gsl_sf_bessel_jl((int) in[0].real, in[1].real);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_besselJ - Implement the function besselJ(l,x) in the default namespace, which evaluates the lth regular
//! cylindrical Bessel function at x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_besselJ(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "besselJ(l,x)";
    CHECK_NEEDINT(in[0], "l", "function can only evaluate integer-order Bessel functions");
    OUTPUT.real = gsl_sf_bessel_Jn((int) in[0].real, in[1].real);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_besselk - Implement the function besselk(l,x) in the default namespace, which evaluates the lth irregular
//! modified spherical Bessel function at x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_besselk(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "besselk(l,x)";
    CHECK_NEEDINT(in[0], "l", "function can only evaluate integer-order Bessel functions");
    OUTPUT.real = gsl_sf_bessel_kl_scaled((int) in[0].real, in[1].real);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_besselK - Implement the function besselK(l,x) in the default namespace, which evaluates the lth irregular
//! modified cylindrical Bessel function at x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_besselK(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "besselK(l,x)";
    CHECK_NEEDINT(in[0], "l", "function can only evaluate integer-order Bessel functions");
    OUTPUT.real = gsl_sf_bessel_Kn((int) in[0].real, in[1].real);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_bessely - Implement the function bessely(l,x) in the default namespace, which evaluates the lth irregular
//! spherical Bessel function at x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_bessely(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "bessely(l,x)";
    CHECK_NEEDINT(in[0], "l", "function can only evaluate integer-order Bessel functions");
    OUTPUT.real = gsl_sf_bessel_yl((int) in[0].real, in[1].real);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_besselY - Implement the function besselY(l,x) in the default namespace, which evaluates the lth irregular
//! cylindrical Bessel function at x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_besselY(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "besselY(l,x)";
    CHECK_NEEDINT(in[0], "l", "function can only evaluate integer-order Bessel functions");
    OUTPUT.real = gsl_sf_bessel_Yn((int) in[0].real, in[1].real);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_beta - Implement the function beta(a,b) in the default namespace, which evaluates the beta function B(a,b)
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_beta(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "beta(a,b)";
    OUTPUT.real = gsl_sf_beta(in[0].real, in[1].real);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_call - Implement the function call(f,a) in the default namespace, which calls the function f with the
//! arguments contained in the list a
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_call(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "call(f,a)";
    const int stkLevelOld = c->stackPtr;
    list *argList;
    pplExpr dummy;
    pplFunc *fi;
    pplObj *item;
    listIterator *li;

    if (in[0].objType != PPLOBJ_FUNC) {
        *status = 1;
        *errType = ERR_TYPE;
        sprintf(errText,
                "The %s function requires a function object as its first argument. Supplied object is of type <%s>.",
                FunctionDescription, pplObjTypeNames[in[0].objType]);
        return;
    }
    fi = (pplFunc *) in[0].auxil;
    if ((fi == NULL) || (fi->functionType == PPL_FUNC_MAGIC)) {
        *status = 1;
        *errType = ERR_TYPE;
        sprintf(errText,
                "The %s function requires a function object as its first argument. Integration and differentiation operators are not suitable functions.",
                FunctionDescription);
        return;
    }

    if (in[1].objType != PPLOBJ_LIST) {
        *status = 1;
        *errType = ERR_TYPE;
        sprintf(errText,
                "The %s function requires a list object as its second argument. Supplied object is of type <%s>.",
                FunctionDescription, pplObjTypeNames[in[1].objType]);
        return;
    }
    argList = (list *) in[1].auxil;
    li = ppl_listIterateInit(argList);

    STACK_MUSTHAVE(c, argList->length);
    if (c->stackFull) {
        *status = 1;
        *errType = ERR_TYPE;
        strcpy(errText, "Stack overflow.");
        return;
    }

    // Dummy expression object with dummy line number information
    dummy.srcLineN = 0;
    dummy.srcId = 0;
    dummy.srcFname = "<dummy>";
    dummy.ascii = "<call(f,a) function>";

    // Push function object
    pplObjCpy(&c->stack[c->stackPtr], &in[0], 0, 0, 1);
    c->stack[c->stackPtr].refCount = 1;
    c->stackPtr++;

    // Push arguments
    while ((item = (pplObj *) ppl_listIterate(&li)) != NULL) {
        pplObjCpy(&c->stack[c->stackPtr], item, 0, 0, 1);
        c->stack[c->stackPtr].refCount = 1;
        c->stackPtr++;
    }

    // Call function
    c->errStat.errMsgExpr[0] = '\0';
    ppl_fnCall(c, &dummy, 0, argList->length, 1, 1);

    // Propagate error if function failed
    if (c->errStat.status) {
        *status = 1;
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Error inside function supplied to the %s function: %s",
                          FunctionDescription, c->errStat.errMsgExpr);
        return;
    }

    pplObjCpy(&OUTPUT, &c->stack[c->stackPtr - 1], 0, 0, 1);
    while (c->stackPtr > stkLevelOld) { STACK_POP; }
    return;
}

//! pplfunc_ceil - Implement the function ceil(x) in the default namespace, which returns the smallest integer value
//! greater than or equal to x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_ceil(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "ceil(x)";
    OUTPUT.real = ceil(in[0].real);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_chr - Implement the function chr(x) in the default namespace, which returns the character with ASCII code x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_chr(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "chr(x)";
    char *out;
    CHECK_NEEDINT(in[0], "s", "function can only evaluate integer ASCII codes");
    out = (char *) malloc(2);
    if (out == NULL) {
        *status = 1;
        sprintf(errText, "Out of memory.");
        return;
    }
    out[0] = ((int) in[0].real) & 255;
    out[1] = '\0';
    pplObjStr(&OUTPUT, 0, 1, out);
}

//! pplfunc_classOf - Implement the function classOf(x) in the default namespace, which returns the class prototype of
//! the object x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_classOf(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObjCpy(&OUTPUT, in[0].objPrototype, 0, 0, 1);
}

//! pplfunc_cmp - Implement the function cmp(a,b) in the default namespace, which returns 1 if a>b, -1 if a<b and zero
//! if a=b
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_cmp(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    void *a = &in[0];
    void *b = &in[1];
    OUTPUT.real = pplObjCmpQuiet(&a, &b);
}

//! pplfunc_cmyk - Implement the function cmyk(c,m,y,k) in the default namespace, which returns a color with specified
//! CMYK components in the range 0-1
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_cmyk(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObjColor(&OUTPUT, 0, SW_COLSPACE_CMYK, in[0].real, in[1].real, in[2].real, in[3].real);
}

//! pplfunc_conjugate - Implement the function conjugate(z) in the default namespace, which returns the complex
//! conjugate of z
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_conjugate(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "conjugate(z)";
    memcpy(&OUTPUT, in, sizeof(pplObj));
    OUTPUT.imag *= -1;
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_copy - Implement the function copy(x) in the default namespace, which returns a copy of the data structure
//! x. Nested data structures are not copied; see deepcopy(x) for this
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_copy(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObjDeepCpy(&OUTPUT, &in[0], 0, 0, 1);
}

//! pplfunc_cos - Implement the function cos(x) in the default namespace, which returns the cosine of x. If x is
//! dimensionless, it is assumed to be measured in radians
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_cos(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "cos(x)";
    int i;
    gsl_complex z;
    CHECK_DIMLESS_OR_HAS_UNIT(in[0], "first", "an angle", UNIT_ANGLE, 1);
    IF_1COMPLEX {
        GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
        z = gsl_complex_cos(z);
        CLEANUP_GSLCOMPLEX;
    }
    ELSE_REAL {
        OUTPUT.real = cos(in[0].real);
    }
    ENDIF
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_cosh - Implement the function cosh(x) in the default namespace, which returns the hyperbolic cosine of x. x
//! may either be a dimensionless number or may have units of angle
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_cosh(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "cosh(x)";
    int i;
    gsl_complex z;
    CHECK_DIMLESS_OR_HAS_UNIT(in[0], "first", "an angle", UNIT_ANGLE, 1);
    IF_1COMPLEX {
        GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
        z = gsl_complex_cosh(z);
        CLEANUP_GSLCOMPLEX;
    }
    ELSE_REAL {
        OUTPUT.real = cosh(in[0].real);
    }
    ENDIF
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_cot - Implement the function cot(x) in the default namespace, which returns the cotangent of x. If x is
//! dimensionless, it is assumed to be measured in radians
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_cot(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "cot(x)";
    int i;
    gsl_complex z;
    CHECK_DIMLESS_OR_HAS_UNIT(in[0], "first", "an angle", UNIT_ANGLE, 1);
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    z = gsl_complex_cot(z);
    CLEANUP_GSLCOMPLEX;
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_coth - Implement the function coth(x) in the default namespace, which returns the hyperbolic cotangent of x.
//! x may either be a dimensionless number or may have units of angle
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_coth(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "coth(x)";
    int i;
    gsl_complex z;
    CHECK_DIMLESS_OR_HAS_UNIT(in[0], "first", "an angle", UNIT_ANGLE, 1);
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    z = gsl_complex_coth(z);
    CLEANUP_GSLCOMPLEX;
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_cross - Implement the function cross(a,b) in the default namespace, which returns the vector cross product
//! of the three-component vectors a and b
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_cross(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "cross(a,b)";
    int t1 = in[0].objType;
    int t2 = in[1].objType;
    gsl_vector *va, *vb, *vo;
    if (t1 != PPLOBJ_VEC) {
        *status = 1;
        *errType = ERR_TYPE;
        sprintf(errText, "First argument to %s must be a vector. Supplied argument had type <%s>.", FunctionDescription,
                pplObjTypeNames[t1]);
        return;
    }
    if (t2 != PPLOBJ_VEC) {
        *status = 1;
        *errType = ERR_TYPE;
        sprintf(errText, "Second argument to %s must be a vector. Supplied argument had type <%s>.",
                FunctionDescription, pplObjTypeNames[t2]);
        return;
    }
    va = ((pplVector *) in[0].auxil)->v;
    vb = ((pplVector *) in[1].auxil)->v;
    if (va->size != 3) {
        *status = 1;
        *errType = ERR_TYPE;
        sprintf(errText, "%s can only act on three-component vectors. Supplied vector has %ld components.",
                FunctionDescription, (long) va->size);
        return;
    }
    if (vb->size != 3) {
        *status = 1;
        *errType = ERR_TYPE;
        sprintf(errText, "%s can only act on three-component vectors. Supplied vector has %ld components.",
                FunctionDescription, (long) vb->size);
        return;
    }
    if (pplObjVector(&OUTPUT, 0, 1, 3) == NULL) {
        *status = 1;
        *errType = ERR_MEMORY;
        sprintf(errText, "Out of memory.");
        return;
    }
    vo = ((pplVector *) (OUTPUT.auxil))->v;
    gsl_vector_set(vo, 0,
                   gsl_vector_get(va, 1) * gsl_vector_get(vb, 2) - gsl_vector_get(vb, 1) * gsl_vector_get(va, 2));
    gsl_vector_set(vo, 1,
                   gsl_vector_get(va, 2) * gsl_vector_get(vb, 0) - gsl_vector_get(vb, 2) * gsl_vector_get(va, 0));
    gsl_vector_set(vo, 2,
                   gsl_vector_get(va, 0) * gsl_vector_get(vb, 1) - gsl_vector_get(vb, 0) * gsl_vector_get(va, 1));
    in[0].real = in[1].real = 1;
    in[0].imag = in[1].imag = 0;
    in[0].flagComplex = in[1].flagComplex = 0;
    ppl_uaMul(c, &in[0], &in[1], &OUTPUT, status, errType, errText);
}

//! pplfunc_csc - Implement the function csc(x) in the default namespace, which returns the cosecant of x. If x is
//! dimensionless, it is assumed to be measured in radians
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_csc(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "csc(x)";
    int i;
    gsl_complex z;
    CHECK_DIMLESS_OR_HAS_UNIT(in[0], "first", "an angle", UNIT_ANGLE, 1);
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    z = gsl_complex_csc(z);
    CLEANUP_GSLCOMPLEX;
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_csch - Implement the function csch(x) in the default namespace, which returns the hyperbolic cosecant of x.
//! x may either be a dimensionless number or may have units of angle
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_csch(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "csch(x)";
    int i;
    gsl_complex z;
    CHECK_DIMLESS_OR_HAS_UNIT(in[0], "first", "an angle", UNIT_ANGLE, 1);
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    z = gsl_complex_csch(z);
    CLEANUP_GSLCOMPLEX;
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_deepcopy - Implement the function deepcopy(x) in the default namespace, which returns a deep copy of the
//! data structure x, copying also any nested data structures
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_deepcopy(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObjDeepCpy(&OUTPUT, &in[0], 1, 0, 1);
}

//! pplfunc_degrees - Implement the function degrees(x) in the default namespace, which converts angles measured in
//! radians into degrees
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_degrees(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "degrees(x)";
    int i;
    CHECK_DIMLESS_OR_HAS_UNIT(in[0], "first", "an angle", UNIT_ANGLE, 1);
    OUTPUT.real = ppl_degs(in[0].real);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_ellK - Implement the function ellipticintK(k) in the default namespace, which evaluates the complete
//! elliptic integral K(k)
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_ellK(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "ellipticintK(k)";
    OUTPUT.real = gsl_sf_ellint_Kcomp(in[0].real, GSL_PREC_DOUBLE);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_ellE - Implement the function ellipticintE(k) in the default namespace, which evaluates the complete
//! elliptic integral E(k)
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_ellE(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "ellipticintE(k)";
    OUTPUT.real = gsl_sf_ellint_Ecomp(in[0].real, GSL_PREC_DOUBLE);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_ellP - Implement the function ellipticintP(k,n) in the default namespace, which evaluates the complete
//! elliptic integral P(k,n)
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_ellP(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "ellipticintP(k,n)";
    OUTPUT.real = gsl_sf_ellint_Pcomp(in[0].real, in[1].real, GSL_PREC_DOUBLE);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_erf - Implement the function erf(x) in the default namespace, which evaluates the error function at x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_erf(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "erf(x)";
    OUTPUT.real = gsl_sf_erf(in[0].real);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_erfc - Implement the function erfc(x) in the default namespace, which evaluates the complimentary error
//! function at x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_erfc(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "erfc(x)";
    OUTPUT.real = gsl_sf_erfc(in[0].real);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_eval - Implement the function eval(s) in the default namespace, which evaluates the string expression s and
//! returns the result
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_eval(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    const int stkLevelOld = c->stackPtr;
    int explen = -1, errPos = -1, lastOpAssign;
    char *exp;
    pplExpr *e;
    pplObj *output;
    if (in[0].objType != PPLOBJ_STR) {
        *status = 1;
        *errType = ERR_TYPE;
        sprintf(errText, "The eval() function requires a string argument.");
        return;
    }
    exp = (char *) in[0].auxil;
    explen = strlen(exp);
    ppl_error_setstreaminfo(&c->errcontext, 1, "evaluated expression");
    ppl_expCompile(c, c->errcontext.error_input_linenumber, c->errcontext.error_input_sourceId,
                   c->errcontext.error_input_filename, exp, &explen, 1, 1, 1, &e, &errPos, errType, errText);
    if (errPos >= 0) {
        pplExpr_free(e);
        e = NULL;
        *status = 1;
        return;
    }
    if (explen < strlen(exp)) {
        strcpy(errText, "Unexpected trailing matter at the end of expression.");
        *status = 1;
        *errType = ERR_SYNTAX;
        pplExpr_free(e);
        e = NULL;
        return;
    }
    output = ppl_expEval(c, e, &lastOpAssign, 1, 1);
    pplExpr_free(e);
    e = NULL;
    if (c->errStat.status) {
        *status = 1;
        *errType = ERR_GENERIC;
        strcpy(errText, "Error in evaluated expression");
        return;
    }
    pplObjCpy(&OUTPUT, output, 0, 0, 1);
    while (c->stackPtr > stkLevelOld) {
        c->stackPtr--;
        if (c->stack[c->stackPtr].objType != PPLOBJ_NUM) // optimisation: Don't waste time garbage collecting numbers
        {
            ppl_garbageObject(&c->stack[c->stackPtr]);
            if (c->stack[c->stackPtr].refCount != 0) {
                ppl_error(&c->errcontext, ERR_INTERNAL, -1, -1, "Stack forward reference detected.");
            }
        }
    }
    return;
}

//! pplfunc_exp - Implement the function exp(x) in the default namespace, which returns e to the power of x. x may
//! either be a dimensionless number or may have units of angle
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_exp(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "exp(z)";
    int i;
    gsl_complex z;
    CHECK_DIMLESS_OR_HAS_UNIT(in[0], "first", "dimensionless or an angle", UNIT_ANGLE, 1);
    IF_1COMPLEX {
        GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
        z = gsl_complex_exp(z);
        CLEANUP_GSLCOMPLEX;
    }
    ELSE_REAL {
        OUTPUT.real = exp(in[0].real);
    }
    ENDIF
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_expm1 - Implement the function expm1(x) in the default namespace, which accurately evaluates exp(x)-1
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_expm1(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "expm1(x)";
    int i;
    CHECK_DIMLESS_OR_HAS_UNIT(in[0], "first", "dimensionless or an angle", UNIT_ANGLE, 1);
    OUTPUT.real = expm1(in[0].real);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_expint - Implement the function expint(n,x) in the default namespace, which evaluates the integral of
//! exp(-xt)/t**n between one and infinity
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_expint(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "expint(n,x)";
    CHECK_NEEDSINT(in[0], "n", "function's first argument must be");
    OUTPUT.real = gsl_sf_expint_En((int) in[0].real, in[1].real);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_factors - Implement the function factors(x) in the default namespace, which returns a list of the factors of
//! the integer x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_factors(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObj v;
    list *l;
    int i, n = (int) in[0].real;
    char *FunctionDescription = "factors(x)";
    CHECK_NEEDINT(in[0], "x", "function's argument must be");
    if (pplObjList(&OUTPUT, 0, 1, NULL) == NULL) {
        *status = 1;
        *errType = ERR_MEMORY;
        sprintf(errText, "Out of memory.");
        return;
    }
    v.refCount = 1;
    l = (list *) OUTPUT.auxil;
    for (i = 1; i <= n / 2; i++) {
        if ((n % i) != 0) continue;
        pplObjNum(&v, 0, i, 0);
        ppl_listAppendCpy(l, &v, sizeof(v));
    }
    pplObjNum(&v, 0, n, 0);
    ppl_listAppendCpy(l, &v, sizeof(v));
    return;
}

//! pplfunc_finite - Implement the function finite(x) in the default namespace, which returns true if x is a finite
//! number, and false otherwise
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_finite(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObjBool(&OUTPUT, 0, 0);
    if ((c->set->term_current.ComplexNumbers == SW_ONOFF_OFF) && (in[0].flagComplex)) return;
    if ((!gsl_finite(in[0].real)) || (!gsl_finite(in[0].imag))) return;
    OUTPUT.real = 1;
}

//! pplfunc_floor - Implement the function floor(x) in the default namespace, which returns the largest integer value
//! smaller than or equal to x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_floor(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "floor(x)";
    OUTPUT.real = floor(in[0].real);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_gamma - Implement the function gamma(x) in the default namespace, which evaluates the gamma function at x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_gamma(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "gamma(x)";
    OUTPUT.real = gsl_sf_gamma(in[0].real);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_gcd - Implement the function gcd(x,...) in the default namespace, which returns the greatest common divisor
//! (a.k.a. highest common factor) of its arguments, which should be dimensionless non-zero integers
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_gcd(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "gcd(x,...)";
    int i, out;
    for (i = 0; i < nArgs; i++) {
        if (round(in[i].real) < 1) in[i].real = -1;
        CHECK_NEEDINT(in[i], "x", "function's requires arguments to be");
    }
    out = ppl_gcd((int) round(in[0].real), (int) round(in[1].real));
    for (i = 2; i < nArgs; i++) out = ppl_gcd(out, (int) round(in[i].real));
    OUTPUT.real = out;
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_globals - Implement the function globals() in the default namespace, which returns a dictionary of all
//! currently-defined global variables
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_globals(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObjDict(&OUTPUT, 0, 1, c->namespaces[1]);
    __sync_add_and_fetch(&c->namespaces[1]->refCount, 1);
}

//! pplfunc_gray - Implement the function gray(x) in the default namespace, which returns a shade of gray with
//! brightness x in the range 0-1
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_gray(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObjColor(&OUTPUT, 0, SW_COLSPACE_RGB, in[0].real, in[0].real, in[0].real, 0);
}

//! pplfunc_heaviside - Implement the function heaviside(x) in the default namespace, which returns the Heaviside
//! function, defined to be one for x>=0 and zero otherwise
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_heaviside(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "heaviside(x)";
    if (in[0].real >= 0) OUTPUT.real = 1.0;
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_hsb - Implement the function hsb(h,s,b) in the default namespace, which returns a color with specified hue,
//! saturation and brightness in the range 0-1
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_hsb(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObjColor(&OUTPUT, 0, SW_COLSPACE_HSB, in[0].real, in[1].real, in[2].real, 0);
}

//! pplfunc_hyperg_0F1 - Implement the function hyperg_0F1(c,x) in the default namespace, which evaluates the
//! hypergeometric function 0F1(c,x)
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_hyperg_0F1(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "hyperg_0F1(c,x)";
    OUTPUT.real = gsl_sf_hyperg_0F1(in[0].real, in[1].real);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_hyperg_1F1 - Implement the function hyperg_1F1(a,b,x) in the default namespace, which evaluates the
//! confluent hypergeometric function 1F1(a,b,x)
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_hyperg_1F1(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "hyperg_1F1(a,b,x)";
    OUTPUT.real = gsl_sf_hyperg_1F1(in[0].real, in[1].real, in[2].real);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_hyperg_2F0 - Implement the function hyperg_2F0(a,b,x) in the default namespace, which evaluates the
//! hypergeometric function 2F0(a,b,x)
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_hyperg_2F0(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "hyperg_2F0(a,b,x)";
    OUTPUT.real = gsl_sf_hyperg_2F0(in[0].real, in[1].real, in[2].real);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_hyperg_2F1 - Implement the function hyperg_2F1(a,b,c,x) in the default namespace, which evaluates the Gauss
//! hypergeometric function 2F1(a,b,c,x)
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_hyperg_2F1(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "hyperg_2F1(a,b,c,x)";
    OUTPUT.real = gsl_sf_hyperg_2F1(in[0].real, in[1].real, in[2].real, in[3].real);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_hyperg_U - Implement the function hyperg_U(a,b,x) in the default namespace, which evaluates the confluent
//! hypergeometric function U(m,n,x)
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_hyperg_U(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "hyperg_U(a,b,x)";
    OUTPUT.real = gsl_sf_hyperg_U(in[0].real, in[1].real, in[2].real);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_hypot - Implement the function hypot(x,...) in the default namespace, which returns the quadrature sum of
//! its arguments, i.e. the square root of the sum of their squares
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_hypot(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "hypot(...)";
    int i;
    double acc = 0, *buffer;
    if (nArgs < 1) {
        pplObjNum(&OUTPUT, 0, 0, 0);
        return;
    }
    for (i = 1; i < nArgs; i++) // Check matching dimensions
        if (!ppl_unitsDimEqual(&in[0], &in[i])) {
            *status = 1;
            *errType = ERR_UNIT;
            sprintf(errText,
                    "The %s function can only act upon inputs with matching dimensions. Input 1 has dimensions of <%s>, but input %d has dimensions of <%s>.",
                    FunctionDescription, ppl_printUnit(c, &in[0], NULL, NULL, 0, 1, 0), i + 1,
                    ppl_printUnit(c, &in[i], NULL, NULL, 1, 1, 0));
            return;
        }
    buffer = (double *) malloc(nArgs * sizeof(double));
    if (buffer == NULL) {
        *status = 1;
        *errType = ERR_MEMORY;
        sprintf(errText, "Out of memory.");
        return;
    }
    for (i = 0; i < nArgs; i++) buffer[i] = hypot(in[i].real, in[i].imag);
    qsort((void *) buffer, nArgs, sizeof(double), ppl_dblSort);
    for (i = 0; i < nArgs; i++) acc += gsl_pow_2(buffer[i]);
    free(buffer);
    buffer = NULL;
    OUTPUT.real = sqrt(acc);
    CHECK_OUTPUT_OKAY;
    ppl_unitsDimCpy(&OUTPUT, in);
}

//! pplfunc_imag - Implement the function Im(z) in the default namespace, which returns the magnitude of the imaginary
//! part of z
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_imag(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "Im(z)";
    if (c->set->term_current.ComplexNumbers == SW_ONOFF_OFF) {
        if (c->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
            *status = 1;
            *errText = ERR_NUMERICAL;
            sprintf(errText,
                    "The function %s can only be used when complex arithmetic is enabled; type 'set numerics complex' first.",
                    FunctionDescription);
            return;
        } else {
            NULL_OUTPUT;
        }
    }
    OUTPUT.real = in[0].imag;
    CHECK_OUTPUT_OKAY;
    ppl_unitsDimCpy(&OUTPUT, in);
}

//! pplfunc_jacobi_cn - Implement the function jacobi_cn(u,m) in the default namespace, which returns the Jacobi
//! elliptic function cn(u,m)
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_jacobi_cn(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "jacobi_cn(u,m)";
    {
        double t1, t2, t3;
        if (gsl_sf_elljac_e(in[0].real, in[1].real, &t1, &t2, &t3) != GSL_SUCCESS) {
            OUTPUT.real = GSL_NAN;
        } else {
            OUTPUT.real = t2;
        }
    }
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_jacobi_dn - Implement the function jacobi_dn(u,m) in the default namespace, which returns the Jacobi
//! elliptic function dn(u,m)
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_jacobi_dn(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "jacobi_dn(u,m)";
    {
        double t1, t2, t3;
        if (gsl_sf_elljac_e(in[0].real, in[1].real, &t1, &t2, &t3) != GSL_SUCCESS) {
            OUTPUT.real = GSL_NAN;
        } else {
            OUTPUT.real = t3;
        }
    }
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_jacobi_sn - Implement the function jacobi_sn(u,m) in the default namespace, which returns the Jacobi
//! elliptic function sn(u,m)
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_jacobi_sn(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "jacobi_sn(u,m)";
    {
        double t1, t2, t3;
        if (gsl_sf_elljac_e(in[0].real, in[1].real, &t1, &t2, &t3) != GSL_SUCCESS) {
            OUTPUT.real = GSL_NAN;
        } else {
            OUTPUT.real = t1;
        }
    }
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_lambert_W0 - Implement the function lambert_W0(x) in the default namespace, which returns the principal
//! branch of the Lambert W function
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_lambert_W0(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "lambert_W0(x)";
    OUTPUT.real = gsl_sf_lambert_W0(in[0].real);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_lambert_W1 - Implement the function lambert_W1(x) in the default namespace, which returns the secondary
//! branch of the Lambert W function
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_lambert_W1(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "lambert_W1(x)";
    OUTPUT.real = gsl_sf_lambert_Wm1(in[0].real);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_lcm - Implement the function lcm(x,...) in the default namespace, which returns the lowest common multiple
//! of its arguments, which should be dimensionless positive integers
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_lcm(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "lcm(x,...)";
    int i, gcd, lcm_i, zero = 0;
    double lcm;
    for (i = 0; i < nArgs; i++) {
        if (round(in[i].real) < 1) zero = 1;
        CHECK_NEEDINT(in[i], "x", "function's requires arguments to be");
    }
    if (zero) {
        OUTPUT.real = 0;
        return;
    }
    gcd = ppl_gcd((int) round(in[0].real), (int) round(in[1].real));
    lcm = 1.0 / ((double) gcd) * round(in[0].real) * round(in[1].real);
    if (lcm > INT_MAX) {
        if (c->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
            *status = 1;
            *errType = ERR_OVERFLOW;
            sprintf(errText, "Integer overflow in the %s function.", FunctionDescription);
            return;
        } else {
            NULL_OUTPUT;
        }
    }
    lcm_i = (int) round(lcm);
    for (i = 2; i < nArgs; i++) {
        gcd = ppl_gcd(lcm_i, (int) round(in[i].real));
        lcm = 1.0 / ((double) gcd) * lcm * round(in[i].real);
        if (lcm > INT_MAX) {
            if (c->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
                *status = 1;
                *errType = ERR_OVERFLOW;
                sprintf(errText, "Integer overflow in the %s function.", FunctionDescription);
                return;
            } else {
                NULL_OUTPUT;
            }
        }
        lcm_i = (int) round(lcm);
    }
    OUTPUT.real = lcm;
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_ldexp - Implement the function ldexp(x,y) in the default namespace, which returns x times 2 to the power of
//! an integer y
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_ldexp(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "ldexp(x,y)";
    CHECK_NEEDSINT(in[1], "y", "function's second argument must be");
    OUTPUT.real = ldexp(in[0].real, (int) in[1].real);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_legendreP - Implement the function legendreP(l,x) in the default namespace, which evaluates the lth Legendre
//! polynomial at x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_legendreP(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "legendreP(l,x)";
    CHECK_NEEDINT(in[0], "l", "function's first argument must be");
    OUTPUT.real = gsl_sf_legendre_Pl((int) in[0].real, in[1].real);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_legendreQ - Implement the function legendreQ(l,x) in the default namespace, which evaluates the lth Legendre
//! function at x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_legendreQ(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "legendreQ(l,x)";
    CHECK_NEEDINT(in[0], "l", "function's first argument must be");
    OUTPUT.real = gsl_sf_legendre_Ql((int) in[0].real, in[1].real);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_len - Implement the function len(x) in the default namespace, which returns the length of the object x. The
//! may be the length of a string, or the number of entries in a compound data type
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_len(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    int t = in->objType;
    if ((t == PPLOBJ_DICT) || (t == PPLOBJ_MOD) || (t == PPLOBJ_USER)) OUTPUT.real = ((dict *) in->auxil)->length;
    else if (t == PPLOBJ_LIST) OUTPUT.real = ((list *) in->auxil)->length;
    else if ((t == PPLOBJ_STR) || (t == PPLOBJ_EXC)) OUTPUT.real = strlen((char *) in->auxil);
    else if (t == PPLOBJ_VEC) OUTPUT.real = ((pplVector *) in->auxil)->v->size;
    else if (t == PPLOBJ_MAT) OUTPUT.real = ((pplMatrix *) in->auxil)->m->size1;
    else {
        *status = 1;
        *errType = ERR_TYPE;
        sprintf(errText, "Object of type <%s> is not a compound object and has no property of length",
                pplObjTypeNames[t]);
        return;
    }
    return;
}

//! pplfunc_locals - Implement the function locals() in the default namespace, which returns a dictionary of all
//! currently-defined local variables
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_locals(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObjDict(&OUTPUT, 0, 1, c->namespaces[c->ns_ptr]);
    __sync_add_and_fetch(&c->namespaces[c->ns_ptr]->refCount, 1);
}

//! pplfunc_log - Implement the function log(x) in the default namespace, which returns the natural logarithm of x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_log(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "log(z)";
    gsl_complex z;
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    z = gsl_complex_log(z);
    CLEANUP_GSLCOMPLEX;
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_log10 - Implement the function log10(x) in the default namespace, which returns the logarithm of x to base
//! 10
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_log10(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "log10(z)";
    gsl_complex z;
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    z = gsl_complex_log10(z);
    CLEANUP_GSLCOMPLEX;
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_logn - Implement the function logn(x,n) in the default namespace, which returns the logarithm of x to base n
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_logn(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "logn(z,n)";
    gsl_complex z;
    pplObj base;
    GSL_SET_COMPLEX(&z, in[1].real, in[1].imag);
    z = gsl_complex_log(z);
    CLEANUP_GSLCOMPLEX;
    base = OUTPUT;
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    z = gsl_complex_log(z);
    CLEANUP_GSLCOMPLEX;
    ppl_uaDiv(c, &OUTPUT, &base, &OUTPUT, status, errType, errText);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_lrange - Implement the function lrange([f],l,[s]) in the default namespace, which returns a vector of
//! numbers between f and l with uniform multiplicative spacing s
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_lrange(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "lrange([f],l,[s])";
    double start, end, step, n;
    int i;
    if (nArgs > 1) { CHECK_2INPUT_DIMMATCH; }
    if (nArgs > 2) {
        in++;
        CHECK_2INPUT_DIMMATCH;
        in--;
    }

    if (nArgs == 1) {
        start = 1;
        end = in[0].real;
        step = 2;
    } else if (nArgs == 2) {
        start = in[0].real;
        end = in[1].real;
        step = 2;
    } else {
        start = in[0].real;
        end = in[1].real;
        step = in[2].real;
    }
    n = ceil(log(end / start) / log(step));
    if ((!gsl_finite(n)) || (n > INT_MAX)) {
        *status = 1;
        *errType = ERR_NUMERICAL;
        strcpy(errText, "Invalid step size.");
        return;
    }
    if (n < 0) n = 0;
    if (pplObjVector(&OUTPUT, 0, 1, n) == NULL) {
        *status = 1;
        *errType = ERR_MEMORY;
        strcpy(errText, "Out of memory.");
        return;
    }
    for (i = 0; i < n; i++) gsl_vector_set(((pplVector *) OUTPUT.auxil)->v, i, start * pow(step, i));
    ppl_unitsDimCpy(&OUTPUT, &in[0]);
}

//! pplfunc_max - Implement the function max(x,...) in the default namespace, which returns the greatest of its
//! arguments
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_max(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    if (nArgs == 0) {
        pplObjNull(&OUTPUT, 0);
        return;
    }
    if ((nArgs == 1) && (in[0].objType == PPLOBJ_LIST)) {
        pplObj *item, *best = NULL;
        listIterator *li = ppl_listIterateInit((list *) in[0].auxil);
        while ((item = (pplObj *) ppl_listIterate(&li)) != NULL) {
            if ((best == NULL) || (pplObjCmpQuiet((void *) &item, (void *) &best) == 1)) best = item;
        }
        if (best == NULL) pplObjNull(&OUTPUT, 0);
        else pplObjCpy(&OUTPUT, best, 0, 0, 1);
    } else if ((nArgs == 1) &&
               ((in[0].objType == PPLOBJ_DICT) || (in[0].objType == PPLOBJ_MOD) || (in[0].objType == PPLOBJ_USER))) {
        char *key;
        pplObj *item, *best = NULL;
        dictIterator *di = ppl_dictIterateInit((dict *) in[0].auxil);
        while ((item = (pplObj *) ppl_dictIterate(&di, &key)) != NULL) {
            if ((best == NULL) || (pplObjCmpQuiet((void *) &item, (void *) &best) == 1)) best = item;
        }
        if (best == NULL) pplObjNull(&OUTPUT, 0);
        else pplObjCpy(&OUTPUT, best, 0, 0, 1);
    } else if ((nArgs == 1) && (in[0].objType == PPLOBJ_VEC)) {
        int i;
        double *best = NULL;
        gsl_vector *v = ((pplVector *) (in[0].auxil))->v;
        for (i = 0; i < v->size; i++)
            if ((best == NULL) || (gsl_vector_get(v, i) > *best)) best = gsl_vector_ptr(v, i);
        if (best == NULL) pplObjNull(&OUTPUT, 0);
        else pplObjNum(&OUTPUT, 0, *best, 0);
        ppl_unitsDimCpy(&OUTPUT, &in[0]);
    } else if ((nArgs == 1) && (in[0].objType == PPLOBJ_MAT)) {
        int i, j;
        double *best = NULL;
        gsl_matrix *m = ((pplMatrix *) (in[0].auxil))->m;
        for (i = 0; i < m->size1; i++)
            for (j = 0; j < m->size2; j++)
                if ((best == NULL) || (gsl_matrix_get(m, i, j) > *best)) best = gsl_matrix_ptr(m, i, j);
        if (best == NULL) pplObjNull(&OUTPUT, 0);
        else pplObjNum(&OUTPUT, 0, *best, 0);
        ppl_unitsDimCpy(&OUTPUT, &in[0]);
    } else {
        int i;
        pplObj *best = in;
        for (i = 1; i < nArgs; i++) {
            pplObj *x = &in[i];
            if (pplObjCmpQuiet((void *) &x, (void *) &best) == 1) best = &in[i];
        }
        if (best == NULL) pplObjNull(&OUTPUT, 0);
        else pplObjCpy(&OUTPUT, best, 0, 0, 1);
    }
}

//! pplfunc_min - Implement the function min(x,...) in the default namespace, which returns the least of its arguments
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_min(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    if (nArgs == 0) {
        pplObjNull(&OUTPUT, 0);
        return;
    }
    if ((nArgs == 1) && (in[0].objType == PPLOBJ_LIST)) {
        pplObj *item, *best = NULL;
        listIterator *li = ppl_listIterateInit((list *) in[0].auxil);
        while ((item = (pplObj *) ppl_listIterate(&li)) != NULL) {
            if ((best == NULL) || (pplObjCmpQuiet((void *) &item, (void *) &best) == -1)) best = item;
        }
        if (best == NULL) pplObjNull(&OUTPUT, 0);
        else pplObjCpy(&OUTPUT, best, 0, 0, 1);
    } else if ((nArgs == 1) &&
               ((in[0].objType == PPLOBJ_DICT) || (in[0].objType == PPLOBJ_MOD) || (in[0].objType == PPLOBJ_USER))) {
        char *key;
        pplObj *item, *best = NULL;
        dictIterator *di = ppl_dictIterateInit((dict *) in[0].auxil);
        while ((item = (pplObj *) ppl_dictIterate(&di, &key)) != NULL) {
            if ((best == NULL) || (pplObjCmpQuiet((void *) &item, (void *) &best) == -1)) best = item;
        }
        if (best == NULL) pplObjNull(&OUTPUT, 0);
        else pplObjCpy(&OUTPUT, best, 0, 0, 1);
    } else if ((nArgs == 1) && (in[0].objType == PPLOBJ_VEC)) {
        int i;
        double *best = NULL;
        gsl_vector *v = ((pplVector *) (in[0].auxil))->v;
        for (i = 0; i < v->size; i++)
            if ((best == NULL) || (gsl_vector_get(v, i) < *best)) best = gsl_vector_ptr(v, i);
        if (best == NULL) pplObjNull(&OUTPUT, 0);
        else pplObjNum(&OUTPUT, 0, *best, 0);
        ppl_unitsDimCpy(&OUTPUT, &in[0]);
    } else if ((nArgs == 1) && (in[0].objType == PPLOBJ_MAT)) {
        int i, j;
        double *best = NULL;
        gsl_matrix *m = ((pplMatrix *) (in[0].auxil))->m;
        for (i = 0; i < m->size1; i++)
            for (j = 0; j < m->size2; j++)
                if ((best == NULL) || (gsl_matrix_get(m, i, j) < *best)) best = gsl_matrix_ptr(m, i, j);
        if (best == NULL) pplObjNull(&OUTPUT, 0);
        else pplObjNum(&OUTPUT, 0, *best, 0);
        ppl_unitsDimCpy(&OUTPUT, &in[0]);
    } else {
        int i;
        pplObj *best = in;
        for (i = 1; i < nArgs; i++) {
            pplObj *x = &in[i];
            if (pplObjCmpQuiet((void *) &x, (void *) &best) == -1) best = &in[i];
        }
        if (best == NULL) pplObjNull(&OUTPUT, 0);
        else pplObjCpy(&OUTPUT, best, 0, 0, 1);
    }
}

//! pplfunc_mod - Implement the function mod(x,y) in the default namespace, which returns the remainder of x/y
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_mod(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "mod(x,y)";
    if (in[0].real * ppl_machineEpsilon * 10 > in[1].real) {
        if (c->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
            *status = 1;
            *errType = ERR_NUMERICAL;
            sprintf(errText,
                    "Loss of accuracy in the function %s; the remainder of this division is lost in floating-point rounding.",
                    FunctionDescription);
            return;
        } else {
            NULL_OUTPUT;
        }
    }
    OUTPUT.real = fmod(in[0].real, in[1].real);
    CHECK_OUTPUT_OKAY;
    ppl_unitsDimCpy(&OUTPUT, &in[0]);
}

//! pplfunc_open - Implement the function open(x[,y]) in the default namespace, which opens the file x with access mode
//! y, and returns a file object
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_open(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    FILE *f;
    char *mode;
    if ((nArgs < 1) || (in[0].objType != PPLOBJ_STR)) {
        *status = 1;
        *errType = ERR_TYPE;
        sprintf(errText, "The open() function requires string arguments.");
        return;
    }
    if ((nArgs > 1) && (in[1].objType != PPLOBJ_STR)) {
        *status = 1;
        *errType = ERR_TYPE;
        sprintf(errText, "The open() function requires string arguments.");
        return;
    }
    if (nArgs > 1) mode = (char *) in[1].auxil;
    else mode = "r";
    f = fopen((char *) in[0].auxil, mode);
    if (f == NULL) {
        *status = 1;
        *errType = ERR_FILE;
        strcpy(errText, strerror(errno));
        return;
    }
    pplObjFile(&OUTPUT, 0, 1, f, 0);
}

//! pplfunc_ord - Implement the function ord(s) in the default namespace, which returns the ASCII code of the first
//! character of the string s
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_ord(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "ord(s)";
    int t = in[0].objType;
    if (t != PPLOBJ_STR) {
        *status = 1;
        *errType = ERR_TYPE;
        sprintf(errText, "Argument to %s must be a string. Supplied argument had type <%s>.", FunctionDescription,
                pplObjTypeNames[t]);
        return;
    }
    OUTPUT.real = (double) ((char *) in[0].auxil)[0];
}

//! pplfunc_ordinal - Implement the function ordinal(n) in the default namespace, which returns the ordinal string, e.g.
//! '1st', '2nd', '3rd' for the positive integer n
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_ordinal(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "ordinal(n)";
    char *out;
    int n = (int) ceil(fabs(log10(in[0].real)));
    int i = (int) in[0].real;
    if (in[0].real < 0) in[0].real = GSL_NAN;
    if (n < 2) n = 2;
    if (n > 64) in[0].real = GSL_NAN;
    CHECK_NEEDINT(in[0], "n", "function's input must be an integer");
    out = (char *) malloc(n + 8);
    if (out == NULL) {
        *status = 1;
        *errType = ERR_MEMORY;
        sprintf(errText, "Out of memory.");
        return;
    }
    if (((i % 100) < 21) && ((i % 100) > 3)) sprintf(out, "%dth", i);
    else if ((i % 10) == 1) sprintf(out, "%dst", i);
    else if ((i % 10) == 2) sprintf(out, "%dnd", i);
    else if ((i % 10) == 3) sprintf(out, "%drd", i);
    else sprintf(out, "%dth", i);
    pplObjStr(&OUTPUT, 0, 1, out);
}

//! pplfunc_pow - Implement the function pow(x,y) in the default namespace, which returns x to the power of y
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_pow(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    ppl_uaPow(c, &in[0], &in[1], &OUTPUT, status, errType, errText);
}

//! pplfunc_prime - Implement the function prime(x) in the default namespace, which returns true if floor(x) is a prime
//! number; false otherwise
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_prime(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "prime(x)";
    CHECK_NEEDINT(in[0], "x", "function's argument must be an integer in the range");
    pplObjBool(&OUTPUT, 0, 0);
    {
        long x = floor(in[0].real), m, n;
        if (x < 53) // Hardcode primes less than 53
        {
            if ((x == 2) || (x == 3) || (x == 5) || (x == 7) || (x == 11) || (x == 13) || (x == 17) || (x == 19) ||
                (x == 23) || (x == 29) || (x == 31) || (x == 37) || (x == 41) || (x == 43) || (x == 47)) {
                OUTPUT.real = 1;
                return;
            } else return;
        }
        if (((x % 2) == 0) || ((x % 3) == 0) || ((x % 5) == 0) || ((x % 7) == 0)) return;
        m = sqrt(x);
        for (n = 11; n <= m; n += 6) {
            if ((x % n) == 0) return;
            if ((x % (n + 2)) == 0) return;
        }
        OUTPUT.real = 1;
    }
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_primefactors - Implement the function primeFactors(x) in the default namespace, which returns a list of the
//! prime factors of the integer x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_primefactors(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObj v;
    list *l;
    int i, n = (int) in[0].real;
    char *FunctionDescription = "primeFactors(x)";
    CHECK_NEEDINT(in[0], "x", "function's argument must be");
    if (in[0].real < 1) {
        if (c->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
            *status = 1;
            *errType = ERR_RANGE;
            sprintf(errText, "The %s function's argument must be in the range 1 <= x < %d.", FunctionDescription,
                    INT_MAX);
            return;
        } else {
            NULL_OUTPUT;
        }
    }
    if (pplObjList(&OUTPUT, 0, 1, NULL) == NULL) {
        *status = 1;
        *errType = ERR_MEMORY;
        sprintf(errText, "Out of memory.");
        return;
    }
    v.refCount = 1;
    l = (list *) OUTPUT.auxil;
    for (i = 2; i <= sqrt(n); i++) {
        int prime = 1;
        if (i < 53)
            prime = (i == 2) || (i == 3) || (i == 5) || (i == 7) || (i == 11) || (i == 13) || (i == 17) || (i == 19) ||
                    (i == 23) || (i == 29) || (i == 31) || (i == 37) || (i == 41) || (i == 43) || (i == 47);
        else if (((i % 2) == 0) || ((i % 3) == 0) || ((i % 5) == 0) || ((i % 7) == 0)) prime = 0;
        else {
            int t, tm = sqrt(i);
            for (t = 11; ((t <= tm) && prime); t += 6) {
                if ((i % t) == 0) prime = 0;
                if ((i % (t + 2)) == 0) prime = 0;
            }
            prime = 1;
        }
        if (!prime) continue;
        while (n % i == 0) {
            pplObjNum(&v, 0, i, 0);
            ppl_listAppendCpy(l, &v, sizeof(v));
            if ((n /= i) == 1) break;
        }
    }
    if (n != 1) {
        pplObjNum(&v, 0, n, 0);
        ppl_listAppendCpy(l, &v, sizeof(v));
    }
    return;
}

//! pplfunc_radians - Implement the function radians(x) in the default namespace, which converts angles measured in
//! degrees into radians
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_radians(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "radians(x)";
    int i;
    CHECK_DIMLESS_OR_HAS_UNIT(in[0], "first", "an angle", UNIT_ANGLE, 1);
    if (in[0].dimensionless) {
        OUTPUT.real = ppl_rads(in[0].real);
    } else {
        OUTPUT.real = in[0].real;
    }
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_raise - Implement the function raise(e,s) in the default namespace, which raises the exception e, with error
//! string s
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_raise(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "raise(e,s)";
    if (nArgs != 2) {
        *status = 1;
        *errType = ERR_TYPE;
        sprintf(errText, "The %s function expects two input arguments.", FunctionDescription);
        return;
    }
    if (in[0].objType != PPLOBJ_EXC) {
        *status = 1;
        *errType = ERR_TYPE;
        sprintf(errText,
                "The first argument to the %s function should be an exception object; supplied object is of type <%s>.",
                FunctionDescription, pplObjTypeNames[in[0].objType]);
        return;
    }
    if (in[1].objType != PPLOBJ_STR) {
        *status = 1;
        *errType = ERR_TYPE;
        sprintf(errText,
                "The second argument to the %s function should be a string object; supplied object is of type <%s>.",
                FunctionDescription, pplObjTypeNames[in[1].objType]);
        return;
    }
    *status = 1;
    *errType = (int) round(in[0].real);
    strncpy(errText, in[1].auxil, FNAME_LENGTH);
    errText[FNAME_LENGTH - 1] = '\0';
    return;
}

//! pplfunc_range - Implement the function range([f],l,[s]) in the default namespace, which returns a vector of
//! uniformly-spaced numbers between f and l, with stepsize s
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_range(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "range([f],l,[s])";
    double start, end, step, n;
    int i;
    if (nArgs > 1) { CHECK_2INPUT_DIMMATCH; }
    if (nArgs > 2) {
        in++;
        CHECK_2INPUT_DIMMATCH;
        in--;
    }

    if (nArgs == 1) {
        start = 0;
        end = in[0].real;
        step = 1;
    } else if (nArgs == 2) {
        start = in[0].real;
        end = in[1].real;
        step = 1;
    } else {
        start = in[0].real;
        end = in[1].real;
        step = in[2].real;
    }
    n = ceil((end - start) / step);
    if ((!gsl_finite(n)) || (n > INT_MAX)) {
        *status = 1;
        *errType = ERR_NUMERICAL;
        strcpy(errText, "Invalid step size.");
        return;
    }
    if (n < 0) n = 0;
    if (pplObjVector(&OUTPUT, 0, 1, n) == NULL) {
        *status = 1;
        *errType = ERR_MEMORY;
        strcpy(errText, "Out of memory.");
        return;
    }
    for (i = 0; i < n; i++) gsl_vector_set(((pplVector *) OUTPUT.auxil)->v, i, start + i * step);
    ppl_unitsDimCpy(&OUTPUT, &in[0]);
}

//! pplfunc_real - Implement the function Re(z) in the default namespace, which returns the magnitude of the real part
//! of z
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_real(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "Re(z)";
    OUTPUT.real = in[0].real;
    CHECK_OUTPUT_OKAY;
    ppl_unitsDimCpy(&OUTPUT, &in[0]);
}

//! pplfunc_rgb - Implement the function rgb(r,g,b) in the default namespace, which returns a color with specified RGB
//! components in the range 0-1
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_rgb(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObjColor(&OUTPUT, 0, SW_COLSPACE_RGB, in[0].real, in[1].real, in[2].real, 0);
}

//! pplfunc_romanNum - Implement the function romanNumeral(n) in the default namespace, which returns the Roman numeral
//! representing the number n
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_romanNum(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "romanNumeral(n)";
    char *h[] = {"", "C", "CC", "CCC", "CD", "D", "DC", "DCC", "DCCC", "CM"};
    char *t[] = {"", "X", "XX", "XXX", "XL", "L", "LX", "LXX", "LXXX", "XC"};
    char *o[] = {"", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX"};
    char *out;
    int n, p = 0;
    CHECK_NEEDINT(in[0], "n", "function's input must be an integer");
    if ((in[0].real < 1) || (in[0].real > 10000)) {
        *status = 1;
        *errType = ERR_RANGE;
        sprintf(errText, "Argument to %s must be in the range 0<n<=10000.", FunctionDescription);
        return;
    }
    out = (char *) malloc(32);
    if (out == NULL) {
        *status = 1;
        *errType = ERR_MEMORY;
        sprintf(errText, "Out of memory.");
        return;
    }
    n = (int) in[0].real;
    while (n >= 1000) {
        out[p++] = 'M';
        n -= 1000;
    }
    strcpy(out + p, h[n / 100]);
    p += strlen(out + p);
    n = n % 100;
    strcpy(out + p, t[n / 10]);
    p += strlen(out + p);
    n = n % 10;
    strcpy(out + p, o[n]);
    p += strlen(out + p);
    out[p] = '\0';
    pplObjStr(&OUTPUT, 0, 1, out);
}

//! pplfunc_root - Implement the function root(z,n) in the default namespace, which returns the nth root of z
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_root(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObj x1, x2;
    double tmpdbl;
    char *FunctionDescription = "root(z,n)";
    unsigned char negated = 0;
    in++;
    CHECK_1INPUT_DIMLESS; // THIS IS CORRECT. Only check in[1]
    in--;
    if ((in[1].flagComplex) || (in[1].real < 2) || (in[1].real >= INT_MAX)) {
        if (c->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
            *status = 1;
            sprintf(errText, "The %s %s in the range 2 <= n < %d.", FunctionDescription,
                    "function's second argument must be an integer in the range", INT_MAX);
            return;
        } else {
            NULL_OUTPUT;
        }
    }
    x1 = in[0];
    if (x1.real < 0.0) {
        negated = 1;
        x1.real = -x1.real;
        if (x1.imag != 0.0) x1.imag = -x1.imag;
    }
    pplObjNum(&x2, 0, 1.0 / floor(in[1].real), 0);
    ppl_uaPow(c, &x1, &x2, &OUTPUT, status, errType, errText);
    if (*status) return;
    if (negated) {
        if (fmod(floor(in[1].real), 2) == 1) {
            OUTPUT.real = -OUTPUT.real;
            if (OUTPUT.imag != 0.0) OUTPUT.imag = -OUTPUT.imag;
        } else {
            if (c->set->term_current.ComplexNumbers == SW_ONOFF_OFF) {
                QUERY_OUT_OF_RANGE;
            } else {
                tmpdbl = OUTPUT.imag;
                OUTPUT.imag = OUTPUT.real;
                OUTPUT.real = -tmpdbl;
                OUTPUT.flagComplex = !ppl_dblEqual(OUTPUT.imag, 0);
                if (!OUTPUT.flagComplex)
                    OUTPUT.imag = 0.0; // Enforce that real numbers have positive zero imaginary components
            }
        }
    }
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_round - Implement the function round(x) in the default namespace, which returns the nearest integer value to
//! x. If x is exactly half way between integers, it is rounded away from zero
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_round(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "round(x)";
    OUTPUT.real = round(in[0].real);
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_sec - Implement the function sec(x) in the default namespace, which returns the secant of x. If x is
//! dimensionless, it is assumed to be measured in radians
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_sec(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "sec(z)";
    int i;
    gsl_complex z;
    CHECK_DIMLESS_OR_HAS_UNIT(in[0], "first", "an angle", UNIT_ANGLE, 1);
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    z = gsl_complex_sec(z);
    CLEANUP_GSLCOMPLEX;
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_sech - Implement the function sech(x) in the default namespace, which returns the hyperbolic secant of x. x
//! may either be a dimensionless number or may have units of angle
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_sech(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "sech(z)";
    int i;
    gsl_complex z;
    CHECK_DIMLESS_OR_HAS_UNIT(in[0], "first", "an angle", UNIT_ANGLE, 1);
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    z = gsl_complex_sech(z);
    CLEANUP_GSLCOMPLEX;
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_sgn - Implement the function sgn(x) in the default namespace, which returns 1 if x is greater than zero, -1
//! if x is less than zero, and 0 if x equals zero
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_sgn(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    OUTPUT.real = ppl_sgn(in[0].real);
}

//! pplfunc_sin - Implement the function sin(x) in the default namespace, which returns the sine of x. If x is
//! dimensionless, it is assumed to be measured in radians
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_sin(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "sin(z)";
    int i;
    gsl_complex z;
    CHECK_DIMLESS_OR_HAS_UNIT(in[0], "first", "an angle", UNIT_ANGLE, 1);
    IF_1COMPLEX {
        GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
        z = gsl_complex_sin(z);
        CLEANUP_GSLCOMPLEX;
    }
    ELSE_REAL {
        OUTPUT.real = sin(in[0].real);
    }
    ENDIF
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_sinc - Implement the function sinc(x) in the default namespace, which returns the function sin(pi*x)/(pi*x).
//! If x is dimensionless, it is assumed to be measured in radians. The output is dimensionless
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_sinc(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "sinc(z)";
    int i;
    gsl_complex z;
    CHECK_DIMLESS_OR_HAS_UNIT(in[0], "first", "an angle", UNIT_ANGLE, 1);
    if ((in[0].real == 0) && (in[0].imag == 0)) {
        OUTPUT.real = 1.0;
    } else {
        IF_1COMPLEX {
            GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
            z = gsl_complex_sin(z);
            CLEANUP_GSLCOMPLEX;
            ppl_uaDiv(c, &OUTPUT, &in[0], &OUTPUT, status, errType, errText);
        }
        ELSE_REAL {
            OUTPUT.real = sin(in[0].real) / in[0].real;
        }
        ENDIF
    }
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_sinh - Implement the function sinh(x) in the default namespace, which returns the hyperbolic sine of x. x
//! may either be a dimensionless number or may have units of angle
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_sinh(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "sinh(z)";
    int i;
    gsl_complex z;
    CHECK_DIMLESS_OR_HAS_UNIT(in[0], "first", "an angle", UNIT_ANGLE, 1);
    IF_1COMPLEX {
        GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
        z = gsl_complex_sinh(z);
        CLEANUP_GSLCOMPLEX;
    }
    ELSE_REAL {
        OUTPUT.real = sinh(in[0].real);
    }
    ENDIF
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_sqrt - Implement the function sqrt(x) in the default namespace, which returns the square root of x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_sqrt(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "sqrt(z)";
    int i;
    gsl_complex z;
    IF_1COMPLEX {
        GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
        z = gsl_complex_sqrt(z);
    }
    ELSE_REAL {
        z = gsl_complex_sqrt_real(in[0].real);
    }
    ENDIF
    CLEANUP_GSLCOMPLEX;
    CHECK_OUTPUT_OKAY;
    OUTPUT.dimensionless = in[0].dimensionless;
    OUTPUT.tempType = in[0].tempType;
    for (i = 0; i < UNITS_MAX_BASEUNITS; i++) OUTPUT.exponent[i] = in[0].exponent[i] / 2;
}

//! pplfunc_sum - Implement the function sum(...) in the default namespace, which returns the sum of its arguments
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_sum(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    if (nArgs == 0) {
        pplObjNum(&OUTPUT, 0, 0, 0);
        return;
    }
    if ((nArgs == 1) && (in[0].objType == PPLOBJ_LIST)) {
        int first = 1;
        pplObj acc, acc2, *item;
        listIterator *li = ppl_listIterateInit((list *) in[0].auxil);
        while ((item = (pplObj *) ppl_listIterate(&li)) != NULL) {
            if (first) {
                pplObjCpy(&acc2, item, 0, 0, 1);
                memcpy(&acc, &acc2, sizeof(pplObj));
                first = 0;
            } else {
                ppl_opAdd(c, &acc, item, &acc2, 1, status, errType, errText);
                if (*status) {
                    ppl_garbageObject(&acc);
                    return;
                }
                memcpy(&acc, &acc2, sizeof(pplObj));
            }
        }
        if (first) pplObjNum(&OUTPUT, 0, 0, 0);
        else memcpy(&OUTPUT, &acc, sizeof(pplObj));
    } else if ((nArgs == 1) &&
               ((in[0].objType == PPLOBJ_DICT) || (in[0].objType == PPLOBJ_MOD) || (in[0].objType == PPLOBJ_USER))) {
        int first = 1;
        pplObj acc, acc2, *item;
        char *key;
        dictIterator *di = ppl_dictIterateInit((dict *) in[0].auxil);
        while ((item = (pplObj *) ppl_dictIterate(&di, &key)) != NULL) {
            if (first) {
                pplObjCpy(&acc2, item, 0, 0, 1);
                memcpy(&acc, &acc2, sizeof(pplObj));
                first = 0;
            } else {
                ppl_opAdd(c, &acc, item, &acc2, 1, status, errType, errText);
                if (*status) {
                    ppl_garbageObject(&acc);
                    return;
                }
                memcpy(&acc, &acc2, sizeof(pplObj));
            }
        }
        if (first) pplObjNum(&OUTPUT, 0, 0, 0);
        else memcpy(&OUTPUT, &acc, sizeof(pplObj));
    } else if ((nArgs == 1) && (in[0].objType == PPLOBJ_VEC)) {
        int i;
        double acc = 0;
        gsl_vector *v = ((pplVector *) (in[0].auxil))->v;
        for (i = 0; i < v->size; i++) acc += gsl_vector_get(v, i);
        pplObjNum(&OUTPUT, 0, acc, 0);
    } else if ((nArgs == 1) && (in[0].objType == PPLOBJ_MAT)) {
        int i, j;
        double acc = 0;
        gsl_matrix *m = ((pplMatrix *) (in[0].auxil))->m;
        for (i = 0; i < m->size1; i++)
            for (j = 0; j < m->size2; j++) acc += gsl_matrix_get(m, i, j);
        pplObjNum(&OUTPUT, 0, acc, 0);
    } else {
        int i;
        pplObj acc, acc2;
        pplObjCpy(&acc2, &in[0], 0, 0, 1);
        memcpy(&acc, &acc2, sizeof(pplObj));
        for (i = 1; i < nArgs; i++) {
            ppl_opAdd(c, &acc, &in[i], &acc2, 1, status, errType, errText);
            if (*status) {
                ppl_garbageObject(&acc);
                return;
            }
            memcpy(&acc, &acc2, sizeof(pplObj));
        }
        memcpy(&OUTPUT, &acc, sizeof(pplObj));
    }
}

//! pplfunc_tan - Implement the function tan(x) in the default namespace, which returns the tangent of x. If x is
//! dimensionless, it is assumed to be measured in radians
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_tan(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "tan(z)";
    int i;
    gsl_complex z;
    CHECK_DIMLESS_OR_HAS_UNIT(in[0], "first", "an angle", UNIT_ANGLE, 1);
    IF_1COMPLEX {
        GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
        z = gsl_complex_tan(z);
        CLEANUP_GSLCOMPLEX;
    }
    ELSE_REAL {
        OUTPUT.real = tan(in[0].real);
    }
    ENDIF
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_tanh - Implement the function tanh(x) in the default namespace, which returns the hyperbolic tangent of x. x
//! may either be a dimensionless number or may have units of angle
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_tanh(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "tanh(z)";
    int i;
    gsl_complex z;
    CHECK_DIMLESS_OR_HAS_UNIT(in[0], "first", "an angle", UNIT_ANGLE, 1);
    IF_1COMPLEX {
        GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
        z = gsl_complex_tanh(z);
        CLEANUP_GSLCOMPLEX;
    }
    ELSE_REAL {
        OUTPUT.real = tanh(in[0].real);
    }
    ENDIF
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_texify - Implement the function texify(str) in the default namespace, which converts an algebraic
//! expression into a LaTeX string
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_texify(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "texify(e)";
    char *instr = (char *) in[0].auxil;
    char *outstr;
    int inlen = 0;
    if (nArgs != 1) {
        sprintf(errText, "The %s function takes exactly one argument; %d supplied.", FunctionDescription, nArgs);
        *errType = ERR_TYPE;
        *status = 1;
        return;
    }
    if (in[0].objType != PPLOBJ_STR) {
        sprintf(errText, "The %s requires a single string argument; supplied argument had type <%s>.",
                FunctionDescription, pplObjTypeNames[in[0].objType]);
        *errType = ERR_TYPE;
        *status = 1;
        return;
    }
    outstr = (char *) malloc(LSTR_LENGTH);
    if (outstr == NULL) {
        sprintf(errText, "Out of memory.");
        *errType = ERR_MEMORY;
        *status = 1;
        return;
    }
    ppl_texify_generic(c, instr, -1, &inlen, outstr, LSTR_LENGTH, NULL, NULL);
    pplObjStr(&OUTPUT, 0, 1, outstr);
    if (inlen < strlen(instr)) {
        sprintf(errText, "Unexpected trailing matter at the end of texified expression (character position %d).",
                inlen);
        *errType = ERR_SYNTAX;
        *status = 1;
        return;
    }
    return;
}

//! pplfunc_texifyText - Implement the function texifyText(str) in the default namespace, which converts a text
//! string into a LaTeX string, escaping any characters which LaTeX treats as special
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_texifyText(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "texifyText(s)";
    char *instr = (char *) in[0].auxil;
    char *outstr;
    if (nArgs != 1) {
        sprintf(errText, "The %s function takes exactly one argument; %d supplied.", FunctionDescription, nArgs);
        *errType = ERR_TYPE;
        *status = 1;
        return;
    }
    if (in[0].objType != PPLOBJ_STR) {
        sprintf(errText, "The %s requires a single string argument; supplied argument had type <%s>.",
                FunctionDescription, pplObjTypeNames[in[0].objType]);
        *errType = ERR_TYPE;
        *status = 1;
        return;
    }
    outstr = (char *) malloc(LSTR_LENGTH);
    if (outstr == NULL) {
        sprintf(errText, "Out of memory.");
        *errType = ERR_MEMORY;
        *status = 1;
        return;
    }
    ppl_texify_string(instr, outstr, -1, LSTR_LENGTH);
    pplObjStr(&OUTPUT, 0, 1, outstr);
    return;
}

//! pplfunc_tophat - Implement the function tophat(x,sigma) in the default namespace, which returns one if |x| <=
//! |sigma|, and zero otherwise
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_tophat(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "tophat(x,sigma)";
    CHECK_2INPUT_DIMMATCH;
    if (fabs(in[0].real) <= fabs(in[1].real)) OUTPUT.real = 1.0;
}

//! pplfunc_typeOf - Implement the function typeOf(x) in the default namespace, which returns the type of the object x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_typeOf(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObjCpy(&OUTPUT, &pplObjPrototypes[in[0].objType], 0, 0, 1);
    OUTPUT.self_lval = NULL;
}

//! pplfunc_zernike - Implement the function zernike(n,m,r,phi) in the default namespace, which evaluates the (n,m)th
//! Zernike polynomial at radius r and position angle phi
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_zernike(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    int i;
    char *FunctionDescription = "zernike(n,m,r,phi)";
    CHECK_3INPUT_DIMLESS;
    CHECK_NEEDINT(in[0], "n", "function can only evaluate Zernike polynomials");
    CHECK_NEEDSINT(in[1], "m", "function can only evaluate Zernike polynomials");
    CHECK_DIMLESS_OR_HAS_UNIT(in[3], "fourth", "an angle", UNIT_ANGLE, 1);
    {
        int n, m, ms, sgn = 1;
        double r;
        n = in[0].real;
        ms = in[1].real;
        m = abs(ms);
        r = in[2].real;
        if (m > n) {
            if (c->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
                *status = 1;
                *errType = ERR_RANGE;
                sprintf(errText, "The function %s is only defined for -n<=m<=n.", FunctionDescription);
                return;
            } else {
                NULL_OUTPUT;
            }
        }
        if ((r < 0) || (r > 1)) {
            OUTPUT.real = GSL_NAN; // Defined only within the unit disk
        } else {
            if ((n % 2) != (m % 2)) return; // Defined to be zero
            for (i = 0; i < (1 + (n - m) / 2); i++) {
                OUTPUT.real += sgn * gsl_sf_fact(n - i) /
                        (gsl_sf_fact(i) * gsl_sf_fact((n + m) / 2 - i) * gsl_sf_fact((n - m) / 2 - i)) *
                        pow(r, n - 2 * i);
                sgn *= -1;
            }
            if (ms > 0) OUTPUT.real *= cos(m * in[3].real);
            else if (ms < 0) OUTPUT.real *= sin(m * in[3].real);
        }
    }
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_zernikeR - Implement the function zernikeR(n,m,r) in the default namespace, which evaluates the (n,m)th
//! radial Zernike polynomial at radius r
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_zernikeR(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    int i;
    char *FunctionDescription = "zernikeR(n,m,r)";
    CHECK_NEEDINT(in[0], "n", "function can only evaluate Zernike polynomials");
    CHECK_NEEDSINT(in[1], "m", "function can only evaluate Zernike polynomials");
    {
        int n, m, ms, sgn = 1;
        double r;
        n = in[0].real;
        ms = in[1].real;
        m = abs(ms);
        r = in[2].real;
        if (m > n) {
            if (c->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
                *status = 1;
                *errType = ERR_RANGE;
                sprintf(errText, "The function %s is only defined for -n<=m<=n.", FunctionDescription);
                return;
            } else {
                NULL_OUTPUT;
            }
        }
        if ((r < 0) || (r > 1)) {
            OUTPUT.real = GSL_NAN; // Defined only within the unit disk
        } else {
            if ((n % 2) != (m % 2)) return; // Defined to be zero

            for (i = 0; i < (1 + (n - m) / 2); i++) {
                OUTPUT.real += sgn * gsl_sf_fact(n - i) /
                        (gsl_sf_fact(i) * gsl_sf_fact((n + m) / 2 - i) * gsl_sf_fact((n - m) / 2 - i)) *
                        pow(r, n - 2 * i);
                sgn *= -1;
            }
        }
    }
    CHECK_OUTPUT_OKAY;
}

//! pplfunc_zeta - Implement the function zeta(x) in the default namespace, which evaluates the Riemann zeta function at
//! x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_zeta(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "zeta(z)";
    gsl_complex zi, z;
    IF_1COMPLEX {
        GSL_SET_COMPLEX(&zi, in[0].real, in[0].imag);
        riemann_zeta_complex(zi, &z, status, errText);
        if (*status) return;
        CLEANUP_GSLCOMPLEX;
    }
    ELSE_REAL {
        OUTPUT.real = gsl_sf_zeta(in[0].real);
    }
    ENDIF
    CHECK_OUTPUT_OKAY;
}
