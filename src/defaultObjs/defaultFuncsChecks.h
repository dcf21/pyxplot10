// defaultFuncsChecks.h
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

// Helper functions used by Pyxplot's built-in functions to check their arguments and set their results.
//
// Every built-in function has the signature
//   void fn(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText)
// where in[0] to in[nArgs-1] are its arguments, and its result is written into in[-1] (OUTPUT). Errors are reported
// by setting *status, *errType and errText. A built-in function which uses these helpers starts by bundling these
// together, along with a description of the function for error messages:
//   pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
// and then calls e.g.
//   if (ppl_funcCheckOutput(&call)) return;
//
// Where a check fails, the helper either reports an error or, if the user has not asked for explicit errors (set
// explicit errors), sets the result to NaN. In either case, it returns nonzero and the built-in function returns.

#ifndef _PPL_DEFAULT_FUNCTIONS_CHECKS_H
#define _PPL_DEFAULT_FUNCTIONS_CHECKS_H 1

#include <gsl/gsl_complex.h>

#include "settings/settings.h"
#include "settings/settingTypes.h"
#include "userspace/context.h"
#include "userspace/pplObj.h"
#include "userspace/pplObjUnits.h"
#include "userspace/unitsDisp.h"

// The result of a built-in function, which is written into the stack slot below its arguments
#define OUTPUT in[-1]

//! pplFuncCall - The details of a call to a built-in function, needed to check its arguments and report errors
typedef struct pplFuncCall {
    ppl_context *c; // The Pyxplot context
    pplObj *in; // The arguments, in[0] to in[nArgs-1]; the result is written into in[-1]
    int *status; // Set to one if an error occurs
    int *errType; // Set to the type of error, one of the ERR_* constants
    char *errText; // Buffer of LSTR_LENGTH bytes into which to write an error message
    const char *description; // A description of the function, e.g. "sin(x)", used in error messages
} pplFuncCall;

void ppl_funcNullOutput(const pplFuncCall *f);
void ppl_funcUndefined(pplFuncCall *f);
void ppl_funcNeedsRealArguments(pplFuncCall *f);
int ppl_funcCheckOutput(pplFuncCall *f);
int ppl_funcCheckFinite(pplFuncCall *f, int nArgs);
int ppl_funcCheckDimensionless(pplFuncCall *f, int nArgs);
int ppl_funcCheckDimensionsMatch(pplFuncCall *f);
int ppl_funcCheckLong(pplFuncCall *f, const pplObj *x, const char *var, const char *description);
int ppl_funcCheckInt(pplFuncCall *f, const pplObj *x, const char *var, const char *description);
int ppl_funcCheckSignedInt(pplFuncCall *f, const pplObj *x, const char *var, const char *description);
int ppl_funcCheckRealDimensionlessFinite(pplFuncCall *f, const pplObj *x, const char *var);
int ppl_funcCheckUnit(pplFuncCall *f, const pplObj *x, const char *description, const char *unitName, int unit,
                      int power);
void ppl_funcError(int *status, int *errType, char *errText, int type, const char *format, ...)
        __attribute__((format(printf, 5, 6)));
char *ppl_funcStrdup(const pplFuncCall *f, const char *str);
void ppl_funcSetComplexOutput(pplObj *out, gsl_complex z);
void ppl_funcSetOutputUnit(ppl_context *c, pplObj *out, int unit);

#endif
