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

// The implementations of Pyxplot's built-in mathematical, string and utility functions, e.g. sin(), besselJ(), gcd(),
// len(), max() and eval(), together with helper functions which check their arguments and report errors in a consistent
// way. Each function receives its arguments in an array of pplObj, and writes its result into the slot before them
// (OUTPUT). They are registered in the default namespace by defaultVars.c.

#include <stdarg.h>
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
#include "userspace/stack.h"
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
#include "defaultObjs/defaultFuncsChecks.h"
#include "defaultObjs/zetaRiemann.h"

#include "texify.h"

// ----------------------------------------------------------------------------
// Helper functions used by built-in functions to check their arguments and set their results (see
// defaultFuncsChecks.h)
// ----------------------------------------------------------------------------

//! funcFail - Report that a built-in function cannot return a result, either as an error, or (if the user has not
//! asked for explicit errors) by returning NaN
//! \param [in] f - The call to the built-in function
//! \param [in] errType - The type of error, one of the ERR_* constants
//! \param [in] alwaysError - If true, report an error even if the user has not asked for explicit errors
//! \param [in] format - printf-style format string for the error message
static void funcFail(pplFuncCall *f, const int errType, const int alwaysError, const char *format, ...)
        __attribute__((format(printf, 4, 5)));

static void funcFail(pplFuncCall *f, const int errType, const int alwaysError, const char *format, ...) {
    va_list args;
    // Report an error; or, if errors are switched off and the error is not one which is always reported, return NaN
    if (alwaysError || (f->c->set->term_current.ExplicitErrors == SW_ONOFF_ON)) {
        *f->status = 1;
        *f->errType = errType;
        va_start(args, format);
        vsnprintf(f->errText, LSTR_LENGTH, format, args);
        va_end(args);
        f->errText[LSTR_LENGTH - 1] = '\0';
    } else {
        ppl_funcNullOutput(f);
    }
}

//! ppl_funcNullOutput - Set the result of a built-in function to NaN
//! \param [in] f - The call to the built-in function

void ppl_funcNullOutput(pplFuncCall *f) {
    pplObj *out = &f->in[-1];
    out->real = GSL_NAN;
    out->imag = 0;
    out->flagComplex = 0;
}

//! ppl_funcUndefined - Report that a built-in function is not defined at the requested point
//! \param [in] f - The call to the built-in function

void ppl_funcUndefined(pplFuncCall *f) {
    funcFail(f, ERR_RANGE, 0, "The function %s is not defined at the requested point in parameter space.",
             f->description);
}

//! ppl_funcNeedsRealArguments - Report that a built-in function was passed complex arguments, which it cannot accept
//! \param [in] f - The call to the built-in function

void ppl_funcNeedsRealArguments(pplFuncCall *f) {
    funcFail(f, ERR_RANGE, 0, "The function %s only accepts real arguments; the supplied arguments are complex.",
             f->description);
}

//! ppl_funcCheckOutput - Check that the result of a built-in function is finite, and is real unless complex
//! arithmetic is enabled
//! \param [in] f - The call to the built-in function
//! \return Zero if the result is acceptable, or nonzero if it is not (and has been reported)

int ppl_funcCheckOutput(pplFuncCall *f) {
    const pplObj *out = &f->in[-1];
    if ((!gsl_finite(out->real)) || (!gsl_finite(out->imag)) ||
        ((out->flagComplex) && (f->c->set->term_current.ComplexNumbers == SW_ONOFF_OFF))) {
        ppl_funcUndefined(f);
        return 1;
    }
    return 0;
}

//! ppl_funcCheckFinite - Check that the first <nArgs> arguments of a built-in function are finite, and are real
//! unless complex arithmetic is enabled
//! \param [in] f - The call to the built-in function
//! \param [in] nArgs - The number of arguments to check
//! \return Zero if the arguments are acceptable, or nonzero if not (and this has been reported)

int ppl_funcCheckFinite(pplFuncCall *f, const int nArgs) {
    int i;
    // Every argument must be finite, and real unless complex arithmetic is on
    for (i = 0; i < nArgs; i++) {
        const pplObj *x = &f->in[i];
        if (((f->c->set->term_current.ComplexNumbers == SW_ONOFF_OFF) && x->flagComplex) || (!gsl_finite(x->real)) ||
            (!gsl_finite(x->imag))) {
            funcFail(f, ERR_RANGE, 0, "The function %s has received a non-finite input.", f->description);
            return 1;
        }
    }
    return 0;
}

//! ppl_funcCheckDimensionless - Check that the first <nArgs> arguments of a built-in function are dimensionless
//! \param [in] f - The call to the built-in function
//! \param [in] nArgs - The number of arguments to check
//! \return Zero if the arguments are dimensionless, or nonzero if not (and this has been reported)

int ppl_funcCheckDimensionless(pplFuncCall *f, const int nArgs) {
    ppl_context *c = f->c;
    const pplObj *in = f->in;
    int i;
    for (i = 0; (i < nArgs) && in[i].dimensionless; i++);
    if (i == nArgs) return 0;
    if (nArgs == 1)
        funcFail(f, ERR_UNIT, 1,
                 "The %s function can only act upon dimensionless inputs. Supplied input has dimensions of <%s>.",
                 f->description, ppl_printUnit(c, &in[0], NULL, NULL, 1, 1, 0));
    else if (nArgs == 2)
        funcFail(f, ERR_UNIT, 1,
                 "The %s function can only act upon dimensionless inputs. Supplied inputs have dimensions of <%s> and "
                 "<%s>.",
                 f->description, ppl_printUnit(c, &in[0], NULL, NULL, 0, 1, 0),
                 ppl_printUnit(c, &in[1], NULL, NULL, 1, 1, 0));
    else if (nArgs == 3)
        funcFail(f, ERR_UNIT, 1,
                 "The %s function can only act upon dimensionless inputs. Supplied inputs have dimensions of <%s>, "
                 "<%s> and <%s>.",
                 f->description, ppl_printUnit(c, &in[0], NULL, NULL, 0, 1, 0),
                 ppl_printUnit(c, &in[1], NULL, NULL, 1, 1, 0), ppl_printUnit(c, &in[2], NULL, NULL, 2, 1, 0));
    else funcFail(f, ERR_UNIT, 1, "The %s function can only act upon dimensionless inputs.", f->description);
    return 1;
}

//! ppl_funcCheckDimensionsMatch - Check that the first two arguments of a built-in function have the same dimensions
//! \param [in] f - The call to the built-in function
//! \return Zero if the dimensions match, or nonzero if not (and this has been reported)

int ppl_funcCheckDimensionsMatch(pplFuncCall *f) {
    ppl_context *c = f->c;
    const pplObj *in = f->in;
    if ((in[0].dimensionless && in[1].dimensionless) || ppl_unitsDimEqual(&in[0], &in[1])) return 0;
    funcFail(f, ERR_UNIT, 1,
             "The %s function can only act upon inputs with matching dimensions. Supplied inputs have dimensions of "
             "<%s> and <%s>.",
             f->description, ppl_printUnit(c, &in[0], NULL, NULL, 0, 1, 0),
             ppl_printUnit(c, &in[1], NULL, NULL, 1, 1, 0));
    return 1;
}

//! ppl_funcCheckLong - Check that an argument of a built-in function is a real number in the range 0 <= x < LONG_MAX
//! \param [in] f - The call to the built-in function
//! \param [in] x - The argument to check
//! \param [in] var - The name of the argument, used in error messages
//! \param [in] description - What the argument must be, e.g. "function's argument must be an integer"
//! \return Zero if the argument is acceptable, or nonzero if not (and this has been reported)

int ppl_funcCheckLong(pplFuncCall *f, const pplObj *x, const char *var, const char *description) {
    if ((!x->flagComplex) && (x->real >= 0) && (x->real < LONG_MAX)) return 0;
    funcFail(f, ERR_RANGE, 0, "The %s %s in the range 0 <= %s < %ld.", f->description, description, var, LONG_MAX);
    return 1;
}

//! ppl_funcCheckInt - Check that an argument of a built-in function is a real number in the range 0 <= x < INT_MAX
//! \param [in] f - The call to the built-in function
//! \param [in] x - The argument to check
//! \param [in] var - The name of the argument, used in error messages
//! \param [in] description - What the argument must be, e.g. "function's argument must be an integer"
//! \return Zero if the argument is acceptable, or nonzero if not (and this has been reported)

int ppl_funcCheckInt(pplFuncCall *f, const pplObj *x, const char *var, const char *description) {
    if ((!x->flagComplex) && (x->real >= 0) && (x->real < INT_MAX)) return 0;
    funcFail(f, ERR_RANGE, 0, "The %s %s in the range 0 <= %s < %d.", f->description, description, var, INT_MAX);
    return 1;
}

//! ppl_funcCheckSignedInt - Check that an argument of a built-in function is in the range INT_MIN < x < INT_MAX
//! \param [in] f - The call to the built-in function
//! \param [in] x - The argument to check
//! \param [in] var - The name of the argument, used in error messages
//! \param [in] description - What the argument must be, e.g. "function's argument must be an integer"
//! \return Zero if the argument is acceptable, or nonzero if not (and this has been reported)

int ppl_funcCheckSignedInt(pplFuncCall *f, const pplObj *x, const char *var, const char *description) {
    if ((x->real > INT_MIN) && (x->real < INT_MAX)) return 0;
    funcFail(f, ERR_RANGE, 0, "The %s %s in the range %d <= %s < %d.", f->description, description, INT_MIN, var,
             INT_MAX);
    return 1;
}

//! ppl_funcCheckRealDimensionlessFinite - Check that an argument of a built-in function is real, dimensionless and
//! finite
//! \param [in] f - The call to the built-in function
//! \param [in] x - The argument to check
//! \param [in] var - The name of the argument, used in error messages
//! \return Zero if the argument is acceptable, or nonzero if not (and this has been reported)

int ppl_funcCheckRealDimensionlessFinite(pplFuncCall *f, const pplObj *x, const char *var) {
    // The argument must be real
    if (x->flagComplex) {
        funcFail(f, ERR_TYPE, 0, "The %s requires real arguments; argument %s is complex.", f->description, var);
        return 1;
    }
    // The argument must be dimensionless
    if (!x->dimensionless) {
        funcFail(f, ERR_TYPE, 0, "The %s requires dimensionless arguments; argument %s has dimensions of <%s>.",
                 f->description, var, ppl_printUnit(f->c, x, NULL, NULL, 1, 1, 0));
        return 1;
    }
    // The argument must be finite
    if ((!gsl_finite(x->real)) || (!gsl_finite(x->imag))) {
        funcFail(f, ERR_TYPE, 0, "The %s requires finite arguments; argument %s is not finite.", f->description, var);
        return 1;
    }
    return 0;
}

//! ppl_funcCheckUnit - Check that an argument of a built-in function is either dimensionless, or has dimensions of a
//! given power of one base unit, e.g. length
//! \param [in] f - The call to the built-in function
//! \param [in] x - The argument to check
//! \param [in] description - A description of the argument, e.g. "first", used in error messages
//! \param [in] unitName - A description of the dimensions required, e.g. "a length", used in error messages
//! \param [in] unit - The base unit, e.g. UNIT_LENGTH
//! \param [in] power - The power of the base unit required
//! \return Zero if the argument is acceptable, or nonzero if not (and this has been reported)

int ppl_funcCheckUnit(pplFuncCall *f, const pplObj *x, const char *description, const char *unitName, const int unit,
                      const int power) {
    int i;
    if (x->dimensionless) return 0;
    for (i = 0; i < UNITS_MAX_BASEUNITS; i++)
        // The exponent of every base unit must be zero, except the required unit, which must have the required power
        if (x->exponent[i] != power * (i == unit)) {
            funcFail(f, ERR_UNIT, 1,
                     "The %s argument to the %s function must be %s. Supplied input has dimensions of <%s>.",
                     description, f->description, unitName, ppl_printUnit(f->c, x, NULL, NULL, 1, 1, 0));
            return 1;
        }
    return 0;
}

//! ppl_funcStrdup - Copy a string into newly malloced memory, reporting an error if memory runs out
//! \param [in] f - The call to the built-in function
//! \param [in] str - The string to copy
//! \return The copy, or NULL if memory ran out (which has been reported)

char *ppl_funcStrdup(pplFuncCall *f, const char *str) {
    char *out = (char *) malloc(strlen(str) + 1);
    // Failure to allocate memory is reported as an error
    if (out == NULL) {
        *f->status = 1;
        *f->errType = ERR_MEMORY;
        ppl_snprintfTrunc(f->errText, LSTR_LENGTH, "Out of memory.");
        return NULL;
    }
    strcpy(out, str);
    return out;
}

//! ppl_funcSetComplexOutput - Set the result of a built-in function to a GSL complex number, marking it as real if
//! its imaginary part is zero
//! \param [out] out - The result of the built-in function
//! \param [in] z - The value to set

void ppl_funcSetComplexOutput(pplObj *out, const gsl_complex z) {
    out->real = GSL_REAL(z);
    out->imag = GSL_IMAG(z);
    out->flagComplex = !ppl_dblEqual(out->imag, 0);
    if (!out->flagComplex) out->imag = 0.0;
}

//! ppl_funcSetOutputUnit - Give the result of a built-in function the dimensions of a base unit. Angles are left
//! dimensionless if the user has asked for angles to be dimensionless (set unit angle nodimensionless).
//! \param [in] c - The Pyxplot context
//! \param [out] out - The result of the built-in function
//! \param [in] unit - The base unit, e.g. UNIT_ANGLE

void ppl_funcSetOutputUnit(ppl_context *c, pplObj *out, const int unit) {
    out->dimensionless = ((unit == UNIT_ANGLE) && (c->set->term_current.UnitAngleDimless == SW_ONOFF_ON));
    if (!out->dimensionless) out->exponent[unit] = 1;
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
    // Magic functions receive their arguments as text, which the compiler treats specially, e.g. the units in unit(m/s)
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
    f->threadSafe = (id == 1); // unit() is a pure function; int_dx() and diff_dx() evaluate expressions themselves
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
    f->threadSafe = 1; // Functions with side effects are marked otherwise by markThreadUnsafeFuncs() in defaultVars.c
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    // Complex arguments use GSL's complex functions; real ones the real functions
    if ((in[0].flagComplex)) {
        { OUTPUT.real = hypot(in[0].real, in[0].imag); }
    } else {
        { OUTPUT.real = fabs(in[0].real); }
    };
    ppl_unitsDimCpy(&OUTPUT, in);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex z;
    // Complex arguments use GSL's complex functions; real ones the real functions
    if ((in[0].flagComplex)) {
        {
            GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
            z = gsl_complex_arccos(z);
        }
    } else {
        { z = gsl_complex_arccos_real(in[0].real); }
    };
    ppl_funcSetComplexOutput(&OUTPUT, z);
    ppl_funcSetOutputUnit(c, &OUTPUT, UNIT_ANGLE);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex z;
    // Complex arguments use GSL's complex functions; real ones the real functions
    if ((in[0].flagComplex)) {
        {
            GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
            z = gsl_complex_arccosh(z);
        }
    } else {
        { z = gsl_complex_arccosh_real(in[0].real); }
    };
    ppl_funcSetComplexOutput(&OUTPUT, z);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex z;
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    z = gsl_complex_arccot(z);
    ppl_funcSetComplexOutput(&OUTPUT, z);
    ppl_funcSetOutputUnit(c, &OUTPUT, UNIT_ANGLE);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex z;
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    z = gsl_complex_arccoth(z);
    ppl_funcSetComplexOutput(&OUTPUT, z);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex z;
    // Complex arguments use GSL's complex functions; real ones the real functions
    if ((in[0].flagComplex)) {
        {
            GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
            z = gsl_complex_arccsc(z);
        }
    } else {
        { z = gsl_complex_arccsc_real(in[0].real); }
    };
    ppl_funcSetComplexOutput(&OUTPUT, z);
    ppl_funcSetOutputUnit(c, &OUTPUT, UNIT_ANGLE);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex z;
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    z = gsl_complex_arccsch(z);
    ppl_funcSetComplexOutput(&OUTPUT, z);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex zi, z;
    GSL_SET_COMPLEX(&zi, in[0].real, in[0].imag);
    airy_ai(zi, &z, status, errText);
    // Report an overflow, or other failure, in the Airy function
    if (*status) {
        *errType = ERR_NUMERICAL;
        return;
    }
    ppl_funcSetComplexOutput(&OUTPUT, z);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex zi, z;
    GSL_SET_COMPLEX(&zi, in[0].real, in[0].imag);
    airy_ai_diff(zi, &z, status, errText);
    // Report an overflow, or other failure, in the Airy function
    if (*status) {
        *errType = ERR_NUMERICAL;
        return;
    }
    ppl_funcSetComplexOutput(&OUTPUT, z);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex zi, z;
    GSL_SET_COMPLEX(&zi, in[0].real, in[0].imag);
    airy_bi(zi, &z, status, errText);
    // Report an overflow, or other failure, in the Airy function
    if (*status) {
        *errType = ERR_NUMERICAL;
        return;
    }
    ppl_funcSetComplexOutput(&OUTPUT, z);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex zi, z;
    GSL_SET_COMPLEX(&zi, in[0].real, in[0].imag);
    airy_bi_diff(zi, &z, status, errText);
    // Report an overflow, or other failure, in the Airy function
    if (*status) {
        *errType = ERR_NUMERICAL;
        return;
    }
    ppl_funcSetComplexOutput(&OUTPUT, z);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex z;
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    OUTPUT.real = gsl_complex_arg(z);
    ppl_funcSetOutputUnit(c, &OUTPUT, UNIT_ANGLE);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex z;
    // Complex arguments use GSL's complex functions; real ones the real functions
    if ((in[0].flagComplex)) {
        {
            GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
            z = gsl_complex_arcsec(z);
        }
    } else {
        { z = gsl_complex_arcsec_real(in[0].real); }
    };
    ppl_funcSetComplexOutput(&OUTPUT, z);
    ppl_funcSetOutputUnit(c, &OUTPUT, UNIT_ANGLE);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex z;
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    z = gsl_complex_arcsech(z);
    ppl_funcSetComplexOutput(&OUTPUT, z);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex z;
    // Complex arguments use GSL's complex functions; real ones the real functions
    if ((in[0].flagComplex)) {
        {
            GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
            z = gsl_complex_arcsin(z);
        }
    } else {
        { z = gsl_complex_arcsin_real(in[0].real); }
    };
    ppl_funcSetComplexOutput(&OUTPUT, z);
    ppl_funcSetOutputUnit(c, &OUTPUT, UNIT_ANGLE);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex z;
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    z = gsl_complex_arcsinh(z);
    ppl_funcSetComplexOutput(&OUTPUT, z);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex z;
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    z = gsl_complex_arctan(z);
    ppl_funcSetComplexOutput(&OUTPUT, z);
    ppl_funcSetOutputUnit(c, &OUTPUT, UNIT_ANGLE);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex z;
    // Complex arguments use GSL's complex functions; real ones the real functions
    if ((in[0].flagComplex)) {
        {
            GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
            z = gsl_complex_arctanh(z);
        }
    } else {
        { z = gsl_complex_arctanh_real(in[0].real); }
    };
    ppl_funcSetComplexOutput(&OUTPUT, z);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    if (ppl_funcCheckDimensionsMatch(&call)) return;
    OUTPUT.real = atan2(in[0].real, in[1].real);
    ppl_funcSetOutputUnit(c, &OUTPUT, UNIT_ANGLE);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    if (ppl_funcCheckInt(&call, &in[0], "l", "function can only evaluate integer-order Bessel functions")) return;
    // GSL only provides the scaled function exp(-|x|) i_l(x), so the scaling is removed
    OUTPUT.real = gsl_sf_bessel_il_scaled((int) in[0].real, in[1].real) * exp(fabs(in[1].real));
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    if (ppl_funcCheckInt(&call, &in[0], "l", "function can only evaluate integer-order Bessel functions")) return;
    OUTPUT.real = gsl_sf_bessel_In((int) in[0].real, in[1].real);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    if (ppl_funcCheckInt(&call, &in[0], "l", "function can only evaluate integer-order Bessel functions")) return;
    OUTPUT.real = gsl_sf_bessel_jl((int) in[0].real, in[1].real);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    if (ppl_funcCheckInt(&call, &in[0], "l", "function can only evaluate integer-order Bessel functions")) return;
    OUTPUT.real = gsl_sf_bessel_Jn((int) in[0].real, in[1].real);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    if (ppl_funcCheckInt(&call, &in[0], "l", "function can only evaluate integer-order Bessel functions")) return;
    // GSL only provides the scaled function exp(x) k_l(x), so the scaling is removed
    OUTPUT.real = gsl_sf_bessel_kl_scaled((int) in[0].real, in[1].real) * exp(-in[1].real);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    if (ppl_funcCheckInt(&call, &in[0], "l", "function can only evaluate integer-order Bessel functions")) return;
    OUTPUT.real = gsl_sf_bessel_Kn((int) in[0].real, in[1].real);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    if (ppl_funcCheckInt(&call, &in[0], "l", "function can only evaluate integer-order Bessel functions")) return;
    OUTPUT.real = gsl_sf_bessel_yl((int) in[0].real, in[1].real);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    if (ppl_funcCheckInt(&call, &in[0], "l", "function can only evaluate integer-order Bessel functions")) return;
    OUTPUT.real = gsl_sf_bessel_Yn((int) in[0].real, in[1].real);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    OUTPUT.real = gsl_sf_beta(in[0].real, in[1].real);
    if (ppl_funcCheckOutput(&call)) return;
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
    // call(f, a) calls the function f with the arguments in the list a
    char *FunctionDescription = "call(f,a)";
    const int stkLevelOld = c->stackPtr;
    list *argList;
    pplExpr dummy;
    pplFunc *fi;
    pplObj *item;
    listIterator *li;

    // The first argument must be a function
    if (in[0].objType != PPLOBJ_FUNC) {
        *status = 1;
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(
                errText, LSTR_LENGTH,
                "The %s function requires a function object as its first argument. Supplied object is of type <%s>.",
                FunctionDescription, pplObjTypeNames[in[0].objType]);
        return;
    }
    fi = (pplFunc *) in[0].auxil;
    // Magic functions, such as diff_d, cannot be called in this way
    if ((fi == NULL) || (fi->functionType == PPL_FUNC_MAGIC)) {
        *status = 1;
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(
                errText, LSTR_LENGTH,
                "The %s function requires a function object as its first argument. Integration and differentiation operators are not suitable functions.",
                FunctionDescription);
        return;
    }

    // The second argument must be a list of arguments
    if (in[1].objType != PPLOBJ_LIST) {
        *status = 1;
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(
                errText, LSTR_LENGTH,
                "The %s function requires a list object as its second argument. Supplied object is of type <%s>.",
                FunctionDescription, pplObjTypeNames[in[1].objType]);
        return;
    }
    argList = (list *) in[1].auxil;
    li = ppl_listIterateInit(argList);

    // Calling the function needs room on the stack for its arguments
    ppl_stackMustHave(c, argList->length);
    // Report a full stack
    if (c->stackFull) {
        *status = 1;
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "%s", "Stack overflow.");
        return;
    }

    // Dummy expression object with dummy line number information
    dummy.srcLineN = 0;
    dummy.srcId = 0;
    dummy.srcFname = "<dummy>";
    dummy.ascii = "<call(f,a) function>";

    // Push the function to call, followed by its arguments
    ppl_stackPushCopy(c, &in[0], 0);

    // Push arguments
    while ((item = (pplObj *) ppl_listIterate(&li)) != NULL) {
        ppl_stackPushCopy(c, item, 0); // Each item of the list is an argument
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

    // Return the function's result, and discard it from the stack
    pplObjCpy(&OUTPUT, &c->stack[c->stackPtr - 1], 0, 0, 1);
    ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack before giving up
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    OUTPUT.real = ceil(in[0].real);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    char *out;
    if (ppl_funcCheckInt(&call, &in[0], "s", "function can only evaluate integer ASCII codes")) return;
    out = (char *) malloc(2);
    // Failure to allocate memory is reported as an error
    if (out == NULL) {
        *status = 1;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Out of memory.");
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    // Copy only the value and physical units of the argument; copying the whole object would also copy its
    // memory-management fields, and it would then be freed twice
    OUTPUT.real = in[0].real;
    OUTPUT.imag = -in[0].imag;
    OUTPUT.flagComplex = in[0].flagComplex;
    ppl_unitsDimCpy(&OUTPUT, in);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex z;
    if (ppl_funcCheckUnit(&call, &in[0], "first", "an angle", UNIT_ANGLE, 1)) return;
    // Complex arguments use GSL's complex functions; real ones the real functions
    if ((in[0].flagComplex)) {
        {
            GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
            z = gsl_complex_cos(z);
            ppl_funcSetComplexOutput(&OUTPUT, z);
        }
    } else {
        { OUTPUT.real = cos(in[0].real); }
    }
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex z;
    if (ppl_funcCheckUnit(&call, &in[0], "first", "an angle", UNIT_ANGLE, 1)) return;
    // Complex arguments use GSL's complex functions; real ones the real functions
    if ((in[0].flagComplex)) {
        {
            GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
            z = gsl_complex_cosh(z);
            ppl_funcSetComplexOutput(&OUTPUT, z);
        }
    } else {
        { OUTPUT.real = cosh(in[0].real); }
    }
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex z;
    if (ppl_funcCheckUnit(&call, &in[0], "first", "an angle", UNIT_ANGLE, 1)) return;
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    z = gsl_complex_cot(z);
    ppl_funcSetComplexOutput(&OUTPUT, z);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex z;
    if (ppl_funcCheckUnit(&call, &in[0], "first", "an angle", UNIT_ANGLE, 1)) return;
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    z = gsl_complex_coth(z);
    ppl_funcSetComplexOutput(&OUTPUT, z);
    if (ppl_funcCheckOutput(&call)) return;
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
    // Both arguments must be vectors of three elements
    char *FunctionDescription = "cross(a,b)";
    int t1 = in[0].objType;
    int t2 = in[1].objType;
    gsl_vector *va, *vb, *vo;
    // The first argument must be a vector
    if (t1 != PPLOBJ_VEC) {
        *status = 1;
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(errText, LSTR_LENGTH,
                          "First argument to %s must be a vector. Supplied argument had type <%s>.",
                          FunctionDescription, pplObjTypeNames[t1]);
        return;
    }
    // The second argument must be a vector
    if (t2 != PPLOBJ_VEC) {
        *status = 1;
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(errText, LSTR_LENGTH,
                          "Second argument to %s must be a vector. Supplied argument had type <%s>.",
                          FunctionDescription, pplObjTypeNames[t2]);
        return;
    }
    va = ((pplVector *) in[0].auxil)->v;
    vb = ((pplVector *) in[1].auxil)->v;
    // The first vector must have three elements
    if (va->size != 3) {
        *status = 1;
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(errText, LSTR_LENGTH,
                          "%s can only act on three-component vectors. Supplied vector has %ld components.",
                          FunctionDescription, (long) va->size);
        return;
    }
    // The second vector must have three elements
    if (vb->size != 3) {
        *status = 1;
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(errText, LSTR_LENGTH,
                          "%s can only act on three-component vectors. Supplied vector has %ld components.",
                          FunctionDescription, (long) vb->size);
        return;
    }
    // The output is a new vector of three elements
    if (pplObjVector(&OUTPUT, 0, 1, 3) == NULL) {
        *status = 1;
        *errType = ERR_MEMORY;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Out of memory.");
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex z;
    if (ppl_funcCheckUnit(&call, &in[0], "first", "an angle", UNIT_ANGLE, 1)) return;
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    z = gsl_complex_csc(z);
    ppl_funcSetComplexOutput(&OUTPUT, z);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex z;
    if (ppl_funcCheckUnit(&call, &in[0], "first", "an angle", UNIT_ANGLE, 1)) return;
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    z = gsl_complex_csch(z);
    ppl_funcSetComplexOutput(&OUTPUT, z);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    if (ppl_funcCheckUnit(&call, &in[0], "first", "an angle", UNIT_ANGLE, 1)) return;
    OUTPUT.real = ppl_degs(in[0].real);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    OUTPUT.real = gsl_sf_ellint_Kcomp(in[0].real, GSL_PREC_DOUBLE);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    OUTPUT.real = gsl_sf_ellint_Ecomp(in[0].real, GSL_PREC_DOUBLE);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    OUTPUT.real = gsl_sf_ellint_Pcomp(in[0].real, in[1].real, GSL_PREC_DOUBLE);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    OUTPUT.real = gsl_sf_erf(in[0].real);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    OUTPUT.real = gsl_sf_erfc(in[0].real);
    if (ppl_funcCheckOutput(&call)) return;
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
    // eval(s) compiles and evaluates the expression in the string s
    const int stkLevelOld = c->stackPtr;
    int explen = -1, errPos = -1, lastOpAssign;
    char *exp;
    pplExpr *e;
    pplObj *output;
    // The argument must be a string
    if (in[0].objType != PPLOBJ_STR) {
        *status = 1;
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "The eval() function requires a string argument.");
        return;
    }
    exp = (char *) in[0].auxil;
    explen = strlen(exp);
    ppl_error_setstreaminfo(&c->errcontext, 1, "evaluated expression");
    ppl_expCompile(c, c->errcontext.error_input_linenumber, c->errcontext.error_input_sourceId,
                   c->errcontext.error_input_filename, exp, &explen, 1, 1, 1, &e, &errPos, errType, errText);
    // The string could not be compiled
    if (errPos >= 0) {
        pplExpr_free(e);
        e = NULL;
        *status = 1;
        return;
    }
    // The expression must use the whole of the string
    if (explen < strlen(exp)) {
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "%s", "Unexpected trailing matter at the end of expression.");
        *status = 1;
        *errType = ERR_SYNTAX;
        pplExpr_free(e);
        e = NULL;
        return;
    }
    output = ppl_expEval(c, e, &lastOpAssign, 1, 1);
    pplExpr_free(e);
    e = NULL;
    // The expression could not be evaluated
    if (c->errStat.status) {
        *status = 1;
        *errType = ERR_GENERIC;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "%s", "Error in evaluated expression");
        return;
    }
    pplObjCpy(&OUTPUT, output, 0, 0, 1);
    ppl_stackClean(c, stkLevelOld); // Discard the expression's value, now that it has been copied
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex z;
    if (ppl_funcCheckUnit(&call, &in[0], "first", "dimensionless or an angle", UNIT_ANGLE, 1)) return;
    // Complex arguments use GSL's complex functions; real ones the real functions
    if ((in[0].flagComplex)) {
        {
            GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
            z = gsl_complex_exp(z);
            ppl_funcSetComplexOutput(&OUTPUT, z);
        }
    } else {
        { OUTPUT.real = exp(in[0].real); }
    }
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    if (ppl_funcCheckUnit(&call, &in[0], "first", "dimensionless or an angle", UNIT_ANGLE, 1)) return;
    OUTPUT.real = expm1(in[0].real);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    if (ppl_funcCheckSignedInt(&call, &in[0], "n", "function's first argument must be")) return;
    OUTPUT.real = gsl_sf_expint_En((int) in[0].real, in[1].real);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    if (ppl_funcCheckInt(&call, &in[0], "x", "function's argument must be")) return;
    // The output is a list of the factors
    if (pplObjList(&OUTPUT, 0, 1, NULL) == NULL) {
        *status = 1;
        *errType = ERR_MEMORY;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Out of memory.");
        return;
    }
    v.refCount = 1;
    l = (list *) OUTPUT.auxil;
    // Add every number up to n/2 which divides n, and then n itself
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    OUTPUT.real = floor(in[0].real);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    OUTPUT.real = gsl_sf_gamma(in[0].real);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    int i, out;
    // Every argument must be a positive integer; non-positive ones are set to -1 so that the check reports them
    for (i = 0; i < nArgs; i++) {
        if (round(in[i].real) < 1) in[i].real = -1;
        if (ppl_funcCheckInt(&call, &in[i], "x", "function's requires arguments to be")) return;
    }
    out = ppl_gcd((int) round(in[0].real), (int) round(in[1].real));
    for (i = 2; i < nArgs; i++) out = ppl_gcd(out, (int) round(in[i].real));
    OUTPUT.real = out;
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    if (in[0].real >= 0) OUTPUT.real = 1.0;
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    OUTPUT.real = gsl_sf_hyperg_0F1(in[0].real, in[1].real);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    OUTPUT.real = gsl_sf_hyperg_1F1(in[0].real, in[1].real, in[2].real);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    OUTPUT.real = gsl_sf_hyperg_2F0(in[0].real, in[1].real, in[2].real);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    OUTPUT.real = gsl_sf_hyperg_2F1(in[0].real, in[1].real, in[2].real, in[3].real);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    OUTPUT.real = gsl_sf_hyperg_U(in[0].real, in[1].real, in[2].real);
    if (ppl_funcCheckOutput(&call)) return;
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
    // The arguments are copied into a buffer of doubles for gsl_hypot
    char *FunctionDescription = "hypot(...)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    int i;
    double acc = 0, *buffer;
    // With no arguments, the result is zero
    if (nArgs < 1) {
        pplObjNum(&OUTPUT, 0, 0, 0);
        return;
    }
    for (i = 1; i < nArgs; i++) // Check matching dimensions
        // All the arguments must have the same units
        if (!ppl_unitsDimEqual(&in[0], &in[i])) {
            *status = 1;
            *errType = ERR_UNIT;
            ppl_snprintfTrunc(
                    errText, LSTR_LENGTH,
                    "The %s function can only act upon inputs with matching dimensions. Input 1 has dimensions of <%s>, but input %d has dimensions of <%s>.",
                    FunctionDescription, ppl_printUnit(c, &in[0], NULL, NULL, 0, 1, 0), i + 1,
                    ppl_printUnit(c, &in[i], NULL, NULL, 1, 1, 0));
            return;
        }
    buffer = (double *) malloc(nArgs * sizeof(double));
    // Failure to allocate memory is reported as an error
    if (buffer == NULL) {
        *status = 1;
        *errType = ERR_MEMORY;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Out of memory.");
        return;
    }
    for (i = 0; i < nArgs; i++) buffer[i] = hypot(in[i].real, in[i].imag);
    qsort((void *) buffer, nArgs, sizeof(double), ppl_dblSort);
    for (i = 0; i < nArgs; i++) acc += gsl_pow_2(buffer[i]);
    free(buffer);
    buffer = NULL;
    OUTPUT.real = sqrt(acc);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    // With complex arithmetic off, there is no imaginary part to return
    if (c->set->term_current.ComplexNumbers == SW_ONOFF_OFF) {
        // Report an error, or return NaN if errors are switched off
        if (c->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
            *status = 1;
            *errText = ERR_NUMERICAL;
            ppl_snprintfTrunc(
                    errText, LSTR_LENGTH,
                    "The function %s can only be used when complex arithmetic is enabled; type 'set numerics complex' first.",
                    FunctionDescription);
            return;
        } else {
            {
                ppl_funcNullOutput(&call);
                return;
            }
        }
    }
    OUTPUT.real = in[0].imag;
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    {
        double t1, t2, t3;
        // GSL computes sn, cn and dn together; this function returns cn
        if (gsl_sf_elljac_e(in[0].real, in[1].real, &t1, &t2, &t3) != GSL_SUCCESS) {
            OUTPUT.real = GSL_NAN;
        } else {
            OUTPUT.real = t2;
        }
    }
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    {
        double t1, t2, t3;
        // GSL computes sn, cn and dn together; this function returns dn
        if (gsl_sf_elljac_e(in[0].real, in[1].real, &t1, &t2, &t3) != GSL_SUCCESS) {
            OUTPUT.real = GSL_NAN;
        } else {
            OUTPUT.real = t3;
        }
    }
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    {
        double t1, t2, t3;
        // GSL computes sn, cn and dn together; this function returns sn
        if (gsl_sf_elljac_e(in[0].real, in[1].real, &t1, &t2, &t3) != GSL_SUCCESS) {
            OUTPUT.real = GSL_NAN;
        } else {
            OUTPUT.real = t1;
        }
    }
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    OUTPUT.real = gsl_sf_lambert_W0(in[0].real);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    OUTPUT.real = gsl_sf_lambert_Wm1(in[0].real);
    if (ppl_funcCheckOutput(&call)) return;
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
    // The lowest common multiple is built up one argument at a time, as lcm(a,b) = a*b/gcd(a,b), and must not overflow
    // an int
    char *FunctionDescription = "lcm(x,...)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    int i, gcd, lcm_i, zero = 0;
    double lcm;
    // Every argument must be an integer; if any is zero or negative, the result is zero
    for (i = 0; i < nArgs; i++) {
        if (round(in[i].real) < 1) zero = 1;
        if (ppl_funcCheckInt(&call, &in[i], "x", "function's requires arguments to be")) return;
    }
    // The lowest common multiple of anything with zero is zero
    if (zero) {
        OUTPUT.real = 0;
        return;
    }
    gcd = ppl_gcd((int) round(in[0].real), (int) round(in[1].real));
    lcm = 1.0 / ((double) gcd) * round(in[0].real) * round(in[1].real);
    // Report an overflow, or return NaN if errors are switched off
    if (lcm > INT_MAX) {
        // Report the overflow
        if (c->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
            *status = 1;
            *errType = ERR_OVERFLOW;
            ppl_snprintfTrunc(errText, LSTR_LENGTH, "Integer overflow in the %s function.", FunctionDescription);
            return;
        } else {
            {
                ppl_funcNullOutput(&call);
                return;
            }
        }
    }
    lcm_i = (int) round(lcm);
    // Combine each further argument with the result so far
    for (i = 2; i < nArgs; i++) {
        gcd = ppl_gcd(lcm_i, (int) round(in[i].real));
        lcm = 1.0 / ((double) gcd) * lcm * round(in[i].real);
        // Report an overflow, or return NaN if errors are switched off
        if (lcm > INT_MAX) {
            // Report the overflow
            if (c->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
                *status = 1;
                *errType = ERR_OVERFLOW;
                ppl_snprintfTrunc(errText, LSTR_LENGTH, "Integer overflow in the %s function.", FunctionDescription);
                return;
            } else {
                {
                    ppl_funcNullOutput(&call);
                    return;
                }
            }
        }
        lcm_i = (int) round(lcm);
    }
    OUTPUT.real = lcm;
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    if (ppl_funcCheckSignedInt(&call, &in[1], "y", "function's second argument must be")) return;
    OUTPUT.real = ldexp(in[0].real, (int) in[1].real);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    if (ppl_funcCheckInt(&call, &in[0], "l", "function's first argument must be")) return;
    OUTPUT.real = gsl_sf_legendre_Pl((int) in[0].real, in[1].real);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    if (ppl_funcCheckInt(&call, &in[0], "l", "function's first argument must be")) return;
    OUTPUT.real = gsl_sf_legendre_Ql((int) in[0].real, in[1].real);
    if (ppl_funcCheckOutput(&call)) return;
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
    else if ((t == PPLOBJ_STR) || (t == PPLOBJ_EXC))
        OUTPUT.real = ppl_utf8Length((char *) in->auxil); // Length in characters, as for the len() method
    else if (t == PPLOBJ_VEC) OUTPUT.real = ((pplVector *) in->auxil)->v->size;
    else if (t == PPLOBJ_MAT) OUTPUT.real = ((pplMatrix *) in->auxil)->m->size1;
    // Other types have no length
    else {
        *status = 1;
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(errText, LSTR_LENGTH,
                          "Object of type <%s> is not a compound object and has no property of length",
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex z;
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    z = gsl_complex_log(z);
    ppl_funcSetComplexOutput(&OUTPUT, z);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex z;
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    z = gsl_complex_log10(z);
    ppl_funcSetComplexOutput(&OUTPUT, z);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex z;
    pplObj base;
    GSL_SET_COMPLEX(&z, in[1].real, in[1].imag);
    z = gsl_complex_log(z);
    ppl_funcSetComplexOutput(&OUTPUT, z);
    base = OUTPUT;
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    z = gsl_complex_log(z);
    ppl_funcSetComplexOutput(&OUTPUT, z);
    ppl_uaDiv(c, &OUTPUT, &base, &OUTPUT, status, errType, errText);
    if (ppl_funcCheckOutput(&call)) return;
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
    // lrange() makes a vector of values in geometric progression; start, end and step are the first value, the limit
    // and the factor between values, and n the number of values
    char *FunctionDescription = "lrange([f],l,[s])";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    double start, end, step, n;
    int i;
    if (nArgs > 1) {
        if (ppl_funcCheckDimensionsMatch(&call)) return;
    }
    // The limit and the step must have the same units
    if (nArgs > 2) {
        in++;
        if (ppl_funcCheckDimensionsMatch(&call)) return;
        in--;
    }

    // With one argument, count from 1 by factors of 2
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
    // The step must give a sensible number of values
    if ((!gsl_finite(n)) || (n > INT_MAX)) {
        *status = 1;
        *errType = ERR_NUMERICAL;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "%s", "Invalid step size.");
        return;
    }
    if (n < 0) n = 0;
    // The output is a vector of n values
    if (pplObjVector(&OUTPUT, 0, 1, n) == NULL) {
        *status = 1;
        *errType = ERR_MEMORY;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "%s", "Out of memory.");
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
    // With no arguments, the result is null
    if (nArgs == 0) {
        pplObjNull(&OUTPUT, 0);
        return;
    }
    // A single list: find its greatest item; otherwise find the greatest argument, a vector's greatest element, and so
    // on
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
        // Find the greatest argument
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
    // With no arguments, the result is null
    if (nArgs == 0) {
        pplObjNull(&OUTPUT, 0);
        return;
    }
    // A single list: find its least item; otherwise find the least argument, a vector's least element, and so on
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
        // Find the least argument
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    // If x is so much larger than y that x/y cannot be represented exactly, the remainder would be meaningless
    if (in[0].real * ppl_machineEpsilon * 10 > in[1].real) {
        // Report an error, or return NaN if errors are switched off
        if (c->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
            *status = 1;
            *errType = ERR_NUMERICAL;
            ppl_snprintfTrunc(
                    errText, LSTR_LENGTH,
                    "Loss of accuracy in the function %s; the remainder of this division is lost in floating-point rounding.",
                    FunctionDescription);
            return;
        } else {
            {
                ppl_funcNullOutput(&call);
                return;
            }
        }
    }
    OUTPUT.real = fmod(in[0].real, in[1].real);
    if (ppl_funcCheckOutput(&call)) return;
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
    // The arguments are the filename, and optionally the mode, e.g. "w"
    if ((nArgs < 1) || (in[0].objType != PPLOBJ_STR)) {
        *status = 1;
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "The open() function requires string arguments.");
        return;
    }
    // The mode must be a string
    if ((nArgs > 1) && (in[1].objType != PPLOBJ_STR)) {
        *status = 1;
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "The open() function requires string arguments.");
        return;
    }
    if (nArgs > 1) mode = (char *) in[1].auxil;
    else mode = "r";
    f = fopen((char *) in[0].auxil, mode);
    // Report a failure to open the file
    if (f == NULL) {
        *status = 1;
        *errType = ERR_FILE;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "%s", strerror(errno));
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
    // The argument must be a string
    if (t != PPLOBJ_STR) {
        *status = 1;
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Argument to %s must be a string. Supplied argument had type <%s>.",
                          FunctionDescription, pplObjTypeNames[t]);
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    char *out;
    int n = (int) ceil(fabs(log10(in[0].real)));
    int i = (int) in[0].real;
    if (in[0].real < 0) in[0].real = GSL_NAN;
    if (n < 2) n = 2;
    if (n > 64) in[0].real = GSL_NAN;
    if (ppl_funcCheckInt(&call, &in[0], "n", "function's input must be an integer")) return;
    out = (char *) malloc(n + 8);
    // Failure to allocate memory is reported as an error
    if (out == NULL) {
        *status = 1;
        *errType = ERR_MEMORY;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Out of memory.");
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    if (ppl_funcCheckInt(&call, &in[0], "x", "function's argument must be an integer in the range")) return;
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
        // Trial division by every number of the form 6k-1 and 6k+1 up to the square root, after the small primes
        // checked above
        for (n = 11; n <= m; n += 6) {
            if ((x % n) == 0) return;
            if ((x % (n + 2)) == 0) return;
        }
        OUTPUT.real = 1;
    }
    if (ppl_funcCheckOutput(&call)) return;
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
    // primeFactors(x) lists the prime factors of x, with repeats
    pplObj v;
    list *l;
    int i, n = (int) in[0].real;
    char *FunctionDescription = "primeFactors(x)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    if (ppl_funcCheckInt(&call, &in[0], "x", "function's argument must be")) return;
    // The argument must be a positive integer
    if (in[0].real < 1) {
        // Report an error, or return NaN if errors are switched off
        if (c->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
            *status = 1;
            *errType = ERR_RANGE;
            ppl_snprintfTrunc(errText, LSTR_LENGTH, "The %s function's argument must be in the range 1 <= x < %d.",
                              FunctionDescription, INT_MAX);
            return;
        } else {
            {
                ppl_funcNullOutput(&call);
                return;
            }
        }
    }
    // The output is a list of the factors
    if (pplObjList(&OUTPUT, 0, 1, NULL) == NULL) {
        *status = 1;
        *errType = ERR_MEMORY;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Out of memory.");
        return;
    }
    v.refCount = 1;
    l = (list *) OUTPUT.auxil;
    // Try dividing by each prime up to the square root of what remains of n
    for (i = 2; i <= sqrt(n); i++) {
        int prime = 1;
        if (i < 53)
            prime = (i == 2) || (i == 3) || (i == 5) || (i == 7) || (i == 11) || (i == 13) || (i == 17) || (i == 19) ||
                    (i == 23) || (i == 29) || (i == 31) || (i == 37) || (i == 41) || (i == 43) || (i == 47);
        else if (((i % 2) == 0) || ((i % 3) == 0) || ((i % 5) == 0) || ((i % 7) == 0)) prime = 0;
        // Test larger numbers for primality by trial division
        else {
            int t, tm = sqrt(i);
            // Trial division by numbers of the form 6k-1 and 6k+1
            for (t = 11; ((t <= tm) && prime); t += 6) {
                if ((i % t) == 0) prime = 0;
                if ((i % (t + 2)) == 0) prime = 0;
            }
            prime = 1;
        }
        if (!prime) continue;
        // Divide out the prime as many times as it goes
        while (n % i == 0) {
            pplObjNum(&v, 0, i, 0);
            ppl_listAppendCpy(l, &v, sizeof(v));
            if ((n /= i) == 1) break;
        }
    }
    // Whatever remains is itself prime
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    if (ppl_funcCheckUnit(&call, &in[0], "first", "an angle", UNIT_ANGLE, 1)) return;
    // Dimensionless angles are taken to be in degrees; angles with units are already in radians
    if (in[0].dimensionless) {
        OUTPUT.real = ppl_rads(in[0].real);
    } else {
        OUTPUT.real = in[0].real;
    }
    if (ppl_funcCheckOutput(&call)) return;
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
    // raise(e, s) raises the exception e with the message s
    if (nArgs != 2) {
        *status = 1;
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "The %s function expects two input arguments.", FunctionDescription);
        return;
    }
    // The first argument must be an exception
    if (in[0].objType != PPLOBJ_EXC) {
        *status = 1;
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(
                errText, LSTR_LENGTH,
                "The first argument to the %s function should be an exception object; supplied object is of type <%s>.",
                FunctionDescription, pplObjTypeNames[in[0].objType]);
        return;
    }
    // The second argument must be a string
    if (in[1].objType != PPLOBJ_STR) {
        *status = 1;
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(
                errText, LSTR_LENGTH,
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
    // range() makes a vector of values in arithmetic progression; start, end and step are the first value, the limit
    // and the step, and n the number of values
    char *FunctionDescription = "range([f],l,[s])";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    double start, end, step, n;
    int i;
    if (nArgs > 1) {
        if (ppl_funcCheckDimensionsMatch(&call)) return;
    }
    // The limit and the step must have the same units
    if (nArgs > 2) {
        in++;
        if (ppl_funcCheckDimensionsMatch(&call)) return;
        in--;
    }

    // With one argument, count from 0 in steps of 1
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
    // The step must give a sensible number of values
    if ((!gsl_finite(n)) || (n > INT_MAX)) {
        *status = 1;
        *errType = ERR_NUMERICAL;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "%s", "Invalid step size.");
        return;
    }
    if (n < 0) n = 0;
    // The output is a vector of n values
    if (pplObjVector(&OUTPUT, 0, 1, n) == NULL) {
        *status = 1;
        *errType = ERR_MEMORY;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "%s", "Out of memory.");
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    OUTPUT.real = in[0].real;
    if (ppl_funcCheckOutput(&call)) return;
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
    // The Roman numerals for the hundreds, tens and units digits
    char *FunctionDescription = "romanNumeral(n)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    char *h[] = {"", "C", "CC", "CCC", "CD", "D", "DC", "DCC", "DCCC", "CM"};
    char *t[] = {"", "X", "XX", "XXX", "XL", "L", "LX", "LXX", "LXXX", "XC"};
    char *o[] = {"", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX"};
    char *out;
    int n, p = 0;
    if (ppl_funcCheckInt(&call, &in[0], "n", "function's input must be an integer")) return;
    // The number must be between 1 and 10000
    if ((in[0].real < 1) || (in[0].real > 10000)) {
        *status = 1;
        *errType = ERR_RANGE;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Argument to %s must be in the range 0<n<=10000.", FunctionDescription);
        return;
    }
    out = (char *) malloc(32);
    // Failure to allocate memory is reported as an error
    if (out == NULL) {
        *status = 1;
        *errType = ERR_MEMORY;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Out of memory.");
        return;
    }
    n = (int) in[0].real;
    // One M for each thousand
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
    // root(z, n) is the nth root of z; negative real numbers have real odd roots
    pplObj x1, x2;
    double tmpdbl;
    char *FunctionDescription = "root(z,n)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    unsigned char negated = 0;
    in++;
    if (ppl_funcCheckDimensionless(&call, 1)) return; // THIS IS CORRECT. Only check in[1]
    in--;
    // n must be a real integer of at least 2
    if ((in[1].flagComplex) || (in[1].real < 2) || (in[1].real >= INT_MAX)) {
        // Report an error, or return NaN if errors are switched off
        if (c->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
            *status = 1;
            ppl_snprintfTrunc(errText, LSTR_LENGTH, "The %s %s in the range 2 <= n < %d.", FunctionDescription,
                              "function's second argument must be an integer in the range", INT_MAX);
            return;
        } else {
            {
                ppl_funcNullOutput(&call);
                return;
            }
        }
    }
    x1 = in[0];
    // Take the root of the negated number, and fix the sign afterwards
    if (x1.real < 0.0) {
        negated = 1;
        x1.real = -x1.real;
        if (x1.imag != 0.0) x1.imag = -x1.imag;
    }
    pplObjNum(&x2, 0, 1.0 / floor(in[1].real), 0);
    ppl_uaPow(c, &x1, &x2, &OUTPUT, status, errType, errText);
    if (*status) return;
    // Odd roots of negative numbers are negative; even roots are complex
    if (negated) {
        // An odd root
        if (fmod(floor(in[1].real), 2) == 1) {
            OUTPUT.real = -OUTPUT.real;
            if (OUTPUT.imag != 0.0) OUTPUT.imag = -OUTPUT.imag;
        } else {
            // An even root of a negative number is undefined without complex arithmetic
            if (c->set->term_current.ComplexNumbers == SW_ONOFF_OFF) {
                {
                    ppl_funcUndefined(&call);
                    return;
                }
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
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    OUTPUT.real = round(in[0].real);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex z;
    if (ppl_funcCheckUnit(&call, &in[0], "first", "an angle", UNIT_ANGLE, 1)) return;
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    z = gsl_complex_sec(z);
    ppl_funcSetComplexOutput(&OUTPUT, z);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex z;
    if (ppl_funcCheckUnit(&call, &in[0], "first", "an angle", UNIT_ANGLE, 1)) return;
    GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
    z = gsl_complex_sech(z);
    ppl_funcSetComplexOutput(&OUTPUT, z);
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex z;
    if (ppl_funcCheckUnit(&call, &in[0], "first", "an angle", UNIT_ANGLE, 1)) return;
    // Complex arguments use GSL's complex functions; real ones the real functions
    if ((in[0].flagComplex)) {
        {
            GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
            z = gsl_complex_sin(z);
            ppl_funcSetComplexOutput(&OUTPUT, z);
        }
    } else {
        { OUTPUT.real = sin(in[0].real); }
    }
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex z;
    if (ppl_funcCheckUnit(&call, &in[0], "first", "an angle", UNIT_ANGLE, 1)) return;
    // sinc(0) is 1, the limit of sin(x)/x
    if ((in[0].real == 0) && (in[0].imag == 0)) {
        OUTPUT.real = 1.0;
    } else {
        // Complex arguments use GSL's complex functions; real ones the real functions
        if ((in[0].flagComplex)) {
            {
                GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
                z = gsl_complex_sin(z);
                ppl_funcSetComplexOutput(&OUTPUT, z);
                ppl_uaDiv(c, &OUTPUT, &in[0], &OUTPUT, status, errType, errText);
            }
        } else {
            { OUTPUT.real = sin(in[0].real) / in[0].real; }
        }
    }
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex z;
    if (ppl_funcCheckUnit(&call, &in[0], "first", "an angle", UNIT_ANGLE, 1)) return;
    // Complex arguments use GSL's complex functions; real ones the real functions
    if ((in[0].flagComplex)) {
        {
            GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
            z = gsl_complex_sinh(z);
            ppl_funcSetComplexOutput(&OUTPUT, z);
        }
    } else {
        { OUTPUT.real = sinh(in[0].real); }
    }
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    int i;
    gsl_complex z;
    // Complex arguments use GSL's complex functions; real ones the real functions
    if ((in[0].flagComplex)) {
        {
            GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
            z = gsl_complex_sqrt(z);
        }
    } else {
        { z = gsl_complex_sqrt_real(in[0].real); }
    }
    ppl_funcSetComplexOutput(&OUTPUT, z);
    if (ppl_funcCheckOutput(&call)) return;
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
    // With no arguments, the sum is zero
    if (nArgs == 0) {
        pplObjNum(&OUTPUT, 0, 0, 0);
        return;
    }
    // A single list: add up its items; a single dictionary: add up its values; otherwise add up the arguments
    if ((nArgs == 1) && (in[0].objType == PPLOBJ_LIST)) {
        int first = 1;
        pplObj acc, acc2, *item;
        listIterator *li = ppl_listIterateInit((list *) in[0].auxil);
        // Add each item to the running total
        while ((item = (pplObj *) ppl_listIterate(&li)) != NULL) {
            // The first item starts the running total
            if (first) {
                pplObjCpy(&acc2, item, 0, 0, 1);
                memcpy(&acc, &acc2, sizeof(pplObj));
                first = 0;
            } else {
                ppl_opAdd(c, &acc, item, &acc2, 1, status, errType, errText);
                // Give up if the addition failed
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
        // Add each value to the running total
        while ((item = (pplObj *) ppl_dictIterate(&di, &key)) != NULL) {
            // The first value starts the running total
            if (first) {
                pplObjCpy(&acc2, item, 0, 0, 1);
                memcpy(&acc, &acc2, sizeof(pplObj));
                first = 0;
            } else {
                ppl_opAdd(c, &acc, item, &acc2, 1, status, errType, errText);
                // Give up if the addition failed
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
        // Add each further argument to the running total
        for (i = 1; i < nArgs; i++) {
            ppl_opAdd(c, &acc, &in[i], &acc2, 1, status, errType, errText);
            // Give up if the addition failed
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex z;
    if (ppl_funcCheckUnit(&call, &in[0], "first", "an angle", UNIT_ANGLE, 1)) return;
    // Complex arguments use GSL's complex functions; real ones the real functions
    if ((in[0].flagComplex)) {
        {
            GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
            z = gsl_complex_tan(z);
            ppl_funcSetComplexOutput(&OUTPUT, z);
        }
    } else {
        { OUTPUT.real = tan(in[0].real); }
    }
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex z;
    if (ppl_funcCheckUnit(&call, &in[0], "first", "an angle", UNIT_ANGLE, 1)) return;
    // Complex arguments use GSL's complex functions; real ones the real functions
    if ((in[0].flagComplex)) {
        {
            GSL_SET_COMPLEX(&z, in[0].real, in[0].imag);
            z = gsl_complex_tanh(z);
            ppl_funcSetComplexOutput(&OUTPUT, z);
        }
    } else {
        { OUTPUT.real = tanh(in[0].real); }
    }
    if (ppl_funcCheckOutput(&call)) return;
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
    // texify(e) converts the expression in the string e into LaTeX
    char *FunctionDescription = "texify(e)";
    char *instr = (char *) in[0].auxil;
    char *outstr;
    int inlen = 0;
    // Exactly one argument is needed
    if (nArgs != 1) {
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "The %s function takes exactly one argument; %d supplied.",
                          FunctionDescription, nArgs);
        *errType = ERR_TYPE;
        *status = 1;
        return;
    }
    // The argument must be a string
    if (in[0].objType != PPLOBJ_STR) {
        ppl_snprintfTrunc(errText, LSTR_LENGTH,
                          "The %s requires a single string argument; supplied argument had type <%s>.",
                          FunctionDescription, pplObjTypeNames[in[0].objType]);
        *errType = ERR_TYPE;
        *status = 1;
        return;
    }
    outstr = (char *) malloc(LSTR_LENGTH);
    // Failure to allocate memory is reported as an error
    if (outstr == NULL) {
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Out of memory.");
        *errType = ERR_MEMORY;
        *status = 1;
        return;
    }
    ppl_texify_generic(c, instr, -1, &inlen, outstr, LSTR_LENGTH, NULL, NULL);
    pplObjStr(&OUTPUT, 0, 1, outstr);
    // The whole string must be a valid expression
    if (inlen < strlen(instr)) {
        ppl_snprintfTrunc(errText, LSTR_LENGTH,
                          "Unexpected trailing matter at the end of texified expression (character position %d).",
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
    // Exactly one argument is needed
    if (nArgs != 1) {
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "The %s function takes exactly one argument; %d supplied.",
                          FunctionDescription, nArgs);
        *errType = ERR_TYPE;
        *status = 1;
        return;
    }
    // The argument must be a string
    if (in[0].objType != PPLOBJ_STR) {
        ppl_snprintfTrunc(errText, LSTR_LENGTH,
                          "The %s requires a single string argument; supplied argument had type <%s>.",
                          FunctionDescription, pplObjTypeNames[in[0].objType]);
        *errType = ERR_TYPE;
        *status = 1;
        return;
    }
    outstr = (char *) malloc(LSTR_LENGTH);
    // Failure to allocate memory is reported as an error
    if (outstr == NULL) {
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Out of memory.");
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    if (ppl_funcCheckDimensionsMatch(&call)) return;
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
    // zernike(n,m,r,phi) evaluates the Zernike polynomial Z_n^m at the polar position (r,phi) in the unit disk
    int i;
    char *FunctionDescription = "zernike(n,m,r,phi)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    if (ppl_funcCheckDimensionless(&call, 3)) return;
    if (ppl_funcCheckInt(&call, &in[0], "n", "function can only evaluate Zernike polynomials")) return;
    if (ppl_funcCheckSignedInt(&call, &in[1], "m", "function can only evaluate Zernike polynomials")) return;
    if (ppl_funcCheckUnit(&call, &in[3], "fourth", "an angle", UNIT_ANGLE, 1)) return;
    {
        int n, m, ms, sgn = 1;
        double r;
        n = in[0].real;
        ms = in[1].real;
        m = abs(ms);
        r = in[2].real;
        // m may not exceed n
        if (m > n) {
            // Report an error, or return NaN if errors are switched off
            if (c->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
                *status = 1;
                *errType = ERR_RANGE;
                ppl_snprintfTrunc(errText, LSTR_LENGTH, "The function %s is only defined for -n<=m<=n.",
                                  FunctionDescription);
                return;
            } else {
                {
                    ppl_funcNullOutput(&call);
                    return;
                }
            }
        }
        // Outside the unit disk, the polynomial is undefined
        if ((r < 0) || (r > 1)) {
            OUTPUT.real = GSL_NAN; // Defined only within the unit disk
        } else {
            if ((n % 2) != (m % 2)) return; // Defined to be zero
            // Sum the series for the radial polynomial R_n^m(r)
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
    if (ppl_funcCheckOutput(&call)) return;
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
    // zernikeR(n,m,r) evaluates the radial Zernike polynomial R_n^m(r)
    int i;
    char *FunctionDescription = "zernikeR(n,m,r)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    if (ppl_funcCheckInt(&call, &in[0], "n", "function can only evaluate Zernike polynomials")) return;
    if (ppl_funcCheckSignedInt(&call, &in[1], "m", "function can only evaluate Zernike polynomials")) return;
    {
        int n, m, ms, sgn = 1;
        double r;
        n = in[0].real;
        ms = in[1].real;
        m = abs(ms);
        r = in[2].real;
        // m may not exceed n
        if (m > n) {
            // Report an error, or return NaN if errors are switched off
            if (c->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
                *status = 1;
                *errType = ERR_RANGE;
                ppl_snprintfTrunc(errText, LSTR_LENGTH, "The function %s is only defined for -n<=m<=n.",
                                  FunctionDescription);
                return;
            } else {
                {
                    ppl_funcNullOutput(&call);
                    return;
                }
            }
        }
        // Outside the unit disk, the polynomial is undefined
        if ((r < 0) || (r > 1)) {
            OUTPUT.real = GSL_NAN; // Defined only within the unit disk
        } else {
            if ((n % 2) != (m % 2)) return; // Defined to be zero

            // Sum the series for the radial polynomial
            for (i = 0; i < (1 + (n - m) / 2); i++) {
                OUTPUT.real += sgn * gsl_sf_fact(n - i) /
                        (gsl_sf_fact(i) * gsl_sf_fact((n + m) / 2 - i) * gsl_sf_fact((n - m) / 2 - i)) *
                        pow(r, n - 2 * i);
                sgn *= -1;
            }
        }
    }
    if (ppl_funcCheckOutput(&call)) return;
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
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    gsl_complex zi, z;
    // Complex arguments use GSL's complex functions; real ones the real functions
    if ((in[0].flagComplex)) {
        {
            GSL_SET_COMPLEX(&zi, in[0].real, in[0].imag);
            riemann_zeta_complex(zi, &z, status, errText);
            if (*status) return;
            ppl_funcSetComplexOutput(&OUTPUT, z);
        }
    } else {
        { OUTPUT.real = gsl_sf_zeta(in[0].real); }
    }
    if (ppl_funcCheckOutput(&call)) return;
}
