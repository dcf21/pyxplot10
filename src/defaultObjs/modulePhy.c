// modulePhy.c
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
#include <limits.h>
#include <math.h>

#include <gsl/gsl_cdf.h>
#include <gsl/gsl_complex.h>
#include <gsl/gsl_complex_math.h>
#include <gsl/gsl_const_mksa.h>
#include <gsl/gsl_const_num.h>
#include <gsl/gsl_math.h>

#include "coreUtils/dict.h"

#include "settings/settings.h"

#include "stringTools/asciidouble.h"

#include "userspace/pplObj_fns.h"
#include "userspace/pplObjFunc.h"
#include "userspace/unitsArithmetic.h"

#include "defaultObjs/modulePhy.h"
#include "defaultObjs/defaultFuncs.h"
#include "defaultObjs/defaultFuncsChecks.h"

//! pplfunc_planck_Bv - Implement the function phy.Bv(nu,T) in the phy module, which returns the power emitted by a
//! blackbody of temperature T per unit area, per unit solid angle, per unit frequency
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_planck_Bv(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "Bv(nu,T)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    pplObj kelvin;

    if (ppl_funcCheckUnit(&call, &in[0], "first", "a frequency", UNIT_TIME, -1)) return;
    if (ppl_funcCheckUnit(&call, &in[1], "second", "a temperature", UNIT_TEMPERATURE, 1)) return;
    pplObjNum(&kelvin, 0, 1, 0);
    kelvin.exponent[UNIT_TEMPERATURE] = 1;
    kelvin.tempType = 1;
    ppl_uaDiv(c, &in[1], &kelvin, &kelvin, status, errType, errText); // Convert in[1] into kelvin
    if (*status) kelvin.real = GSL_NAN;
    OUTPUT.dimensionless = 0;
    OUTPUT.exponent[UNIT_MASS] = 1;
    OUTPUT.exponent[UNIT_TIME] = -2;
    OUTPUT.exponent[UNIT_ANGLE] = -2;
    OUTPUT.real = 2 * GSL_CONST_MKSA_PLANCKS_CONSTANT_H / pow(GSL_CONST_MKSA_SPEED_OF_LIGHT, 2) * pow(in[0].real, 3) /
            expm1(GSL_CONST_MKSA_PLANCKS_CONSTANT_H * in[0].real / GSL_CONST_MKSA_BOLTZMANN / kelvin.real);
    if (ppl_funcCheckOutput(&call)) return;
}

//! pplfunc_planck_Bvmax - Implement the function phy.Bvmax(T) in the phy module, which returns the frequency of the
//! maximum of the function Bv(nu,T)
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_planck_Bvmax(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "Bvmax(T)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    pplObj kelvin;

    if (ppl_funcCheckUnit(&call, &in[0], "first", "a temperature", UNIT_TEMPERATURE, 1)) return;
    pplObjNum(&kelvin, 0, 1, 0);
    kelvin.exponent[UNIT_TEMPERATURE] = 1;
    kelvin.tempType = 1;
    ppl_uaDiv(c, &in[0], &kelvin, &kelvin, status, errType, errText); // Convert in into kelvin
    if (*status) kelvin.real = GSL_NAN;
    OUTPUT.dimensionless = 0;
    OUTPUT.exponent[UNIT_TIME] = -1;
    OUTPUT.real = 2.821439 * GSL_CONST_MKSA_BOLTZMANN / GSL_CONST_MKSA_PLANCKS_CONSTANT_H *
            kelvin.real; // Wien displacement law
    if (ppl_funcCheckOutput(&call)) return;
}
