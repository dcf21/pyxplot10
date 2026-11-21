// moduleRandom.c
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

// The random module, which draws random numbers from various distributions using GSL's random number generator. The
// generator is created on first use with a seed of zero, so that results are repeatable; "set seed" reseeds it. These
// functions are marked as unsafe for parallel evaluation, since they share the generator.

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
#include <gsl/gsl_randist.h>
#include <gsl/gsl_rng.h>
#include <gsl/gsl_sf_erf.h>
#include <gsl/gsl_sf_gamma.h>
#include <gsl/gsl_sf_hyperg.h>

#include "coreUtils/dict.h"

#include "settings/settings.h"

#include "stringTools/asciidouble.h"

#include "userspace/pplObj.h"
#include "userspace/pplObjFunc.h"
#include "userspace/unitsArithmetic.h"

#include "defaultObjs/moduleRandom.h"
#include "defaultObjs/defaultFuncs.h"
#include "defaultObjs/defaultFuncsChecks.h"

static gsl_rng *rndgen = NULL; // Random number generator

//! pplfunc_setRandomSeed - Seed the random number generator used by the random module, creating it if necessary
//! \param [in] i - The seed

void pplfunc_setRandomSeed(const long i) {
    if (rndgen == NULL) rndgen = gsl_rng_alloc(gsl_rng_default);
    gsl_rng_set(rndgen, i);
    return;
}

//! pplfunc_frandom - Implement the function random.random() in the random module, which returns a random number between
//! 0 and 1
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_frandom(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    // Create the random number generator on first use
    if (rndgen == NULL) {
        rndgen = gsl_rng_alloc(gsl_rng_default);
        gsl_rng_set(rndgen, 0);
    }
    OUTPUT.real = gsl_rng_uniform(rndgen);
}

//! pplfunc_frandombin - Implement the function random.binomial(p,n) in the random module, which returns a random sample
//! from a binomial distribution with n independent trials and a success probability p
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_frandombin(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    const char *FunctionDescription = "binomial(p,n)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    // Create the random number generator on first use
    if (rndgen == NULL) {
        rndgen = gsl_rng_alloc(gsl_rng_default);
        gsl_rng_set(rndgen, 0);
    }
    if (ppl_funcCheckInt(&call, &in[1], "n", "function's second argument must be an integer in the range")) return;
    OUTPUT.real = gsl_ran_binomial(rndgen, in[0].real, (unsigned int) in[1].real);
    if (ppl_funcCheckOutput(&call)) return;
}

//! pplfunc_frandomcs - Implement the function random.chisq(nu) in the random module, which returns a random sample from
//! a chi-squared distribution with nu degrees of freedom
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_frandomcs(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    const char *FunctionDescription = "chisq(nu)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    // Create the random number generator on first use
    if (rndgen == NULL) {
        rndgen = gsl_rng_alloc(gsl_rng_default);
        gsl_rng_set(rndgen, 0);
    }
    OUTPUT.real = gsl_ran_chisq(rndgen, in[0].real);
    if (ppl_funcCheckOutput(&call)) return;
}

//! pplfunc_frandomg - Implement the function random.gaussian(sigma) in the random module, which returns a random sample
//! from a Gaussian (normal) distribution of standard deviation sigma
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_frandomg(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    const char *FunctionDescription = "gaussian(sigma)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    // Create the random number generator on first use
    if (rndgen == NULL) {
        rndgen = gsl_rng_alloc(gsl_rng_default);
        gsl_rng_set(rndgen, 0);
    }
    OUTPUT.real = gsl_ran_gaussian(rndgen, in[0].real);
    if (ppl_funcCheckOutput(&call)) return;
}

//! pplfunc_frandomln - Implement the function random.lognormal(zeta,sigma) in the random module, which returns a random
//! sample from the log normal distribution centred on zeta, and of width sigma
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_frandomln(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    const char *FunctionDescription = "lognormal(zeta,sigma)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    // Create the random number generator on first use
    if (rndgen == NULL) {
        rndgen = gsl_rng_alloc(gsl_rng_default);
        gsl_rng_set(rndgen, 0);
    }
    in++;
    if (ppl_funcCheckDimensionless(&call, 1)) return; // THIS IS CORRECT. Only check in[1]
    in--;
    OUTPUT.real = gsl_ran_lognormal(rndgen, in[0].real, in[1].real);
    if (ppl_funcCheckOutput(&call)) return;
    ppl_unitsDimCpy(&OUTPUT, &in[0]);
}

//! pplfunc_frandomp - Implement the function random.poisson(n) in the random module, which returns a random integer
//! from a Poisson distribution with mean n
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_frandomp(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    const char *FunctionDescription = "poisson(n)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    // Create the random number generator on first use
    if (rndgen == NULL) {
        rndgen = gsl_rng_alloc(gsl_rng_default);
        gsl_rng_set(rndgen, 0);
    }
    OUTPUT.real = gsl_ran_poisson(rndgen, in[0].real);
    if (ppl_funcCheckOutput(&call)) return;
}

//! pplfunc_frandomt - Implement the function random.tdist(nu) in the random module, which returns a random sample from
//! a t-distribution with nu degrees of freedom
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_frandomt(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    const char *FunctionDescription = "tdist(nu)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    // Create the random number generator on first use
    if (rndgen == NULL) {
        rndgen = gsl_rng_alloc(gsl_rng_default);
        gsl_rng_set(rndgen, 0);
    }
    OUTPUT.real = gsl_ran_tdist(rndgen, in[0].real);
    if (ppl_funcCheckOutput(&call)) return;
}
