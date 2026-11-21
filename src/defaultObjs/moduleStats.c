// moduleStats.c
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

// The stats module of statistical distributions: the probability density functions and cumulative distribution
// functions of the binomial, chi-squared, Gaussian, lognormal, Poisson and Student's t distributions, and the inverses of
// the cumulative distribution functions of the continuous ones, mostly evaluated with GSL.

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

#include "coreUtils/dict.h"

#include "settings/settings.h"

#include "stringTools/asciidouble.h"

#include "userspace/pplObj.h"
#include "userspace/pplObjFunc.h"
#include "userspace/unitsArithmetic.h"

#include "defaultObjs/moduleStats.h"
#include "defaultObjs/defaultFuncs.h"
#include "defaultObjs/defaultFuncsChecks.h"

//! pplfunc_binomialPDF - Implement the function stats.binomialPDF(k,p,n) in the stats module, which evaulates the
//! probability of getting k successes out of n trials in a binomial distribution with success probability p
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_binomialPDF(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "binomialPDF(k,p,n)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    if (ppl_funcCheckInt(&call, &in[0], "k", "function's first argument must be")) return;
    if (ppl_funcCheckInt(&call, &in[2], "n", "function's third argument must be")) return;
    OUTPUT.real = gsl_ran_binomial_pdf((unsigned int) in[0].real, in[1].real, (unsigned int) in[2].real);
    if (ppl_funcCheckOutput(&call)) return;
}

//! pplfunc_binomialCDF - Implement the function stats.binomialCDF(k,p,n) in the stats module, which evaulates the
//! probability of getting fewer than or exactly k successes out of n trials in a binomial distribution with success
//! probability p
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_binomialCDF(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "binomialCDF(k,p,n)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    if (ppl_funcCheckInt(&call, &in[0], "k", "function's first argument must be")) return;
    if (ppl_funcCheckInt(&call, &in[2], "n", "function's third argument must be")) return;
    OUTPUT.real = gsl_cdf_binomial_P((unsigned int) in[0].real, in[1].real, (unsigned int) in[2].real);
    if (ppl_funcCheckOutput(&call)) return;
}

//! pplfunc_chisqPDF - Implement the function stats.chisqPDF(x,nu) in the stats module, which returns the probability
//! density at x in a chi-squared distribution with nu degrees of freedom
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_chisqPDF(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "chisqPDF(x,nu)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    OUTPUT.real = gsl_ran_chisq_pdf(in[0].real, in[1].real);
    if (ppl_funcCheckOutput(&call)) return;
}

//! pplfunc_chisqCDF - Implement the function stats.chisqCDF(x,nu) in the stats module, which returns the cumulative
//! probability density at x in a chi-squared distribution with nu degrees of freedom
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_chisqCDF(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "chisqCDF(x,nu)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    OUTPUT.real = gsl_cdf_chisq_P(in[0].real, in[1].real);
    if (ppl_funcCheckOutput(&call)) return;
}

//! pplfunc_chisqCDFi - Implement the function stats.chisqCDFi(P,nu) in the stats module, which returns the point x at
//! which the cumulative probability density in a chi-squared distribution with nu degrees of freedom is P
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_chisqCDFi(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "chisqCDFi(P,nu)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    OUTPUT.real = gsl_cdf_chisq_Pinv(in[0].real, in[1].real);
    if (ppl_funcCheckOutput(&call)) return;
}

//! pplfunc_gaussianPDF - Implement the function stats.gaussianPDF(x,sigma) in the stats module, which evaluates the
//! Gaussian probability density function of standard deviation sigma at x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_gaussianPDF(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "gaussianPDF(x,sigma)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    if (ppl_funcCheckDimensionsMatch(&call)) return;
    OUTPUT.real = gsl_ran_gaussian_pdf(in[0].real, in[1].real);
    if (ppl_funcCheckOutput(&call)) return;
    ppl_unitsDimInverse(&OUTPUT, &in[0]);
}

//! pplfunc_gaussianCDF - Implement the function stats.gaussianCDF(x,sigma) in the stats module, which evaluates the
//! Gaussian cumulative distribution function of standard deviation sigma at x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_gaussianCDF(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "gaussianCDF(x,sigma)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    if (ppl_funcCheckDimensionsMatch(&call)) return;
    OUTPUT.real = gsl_cdf_gaussian_P(in[0].real, in[1].real);
    if (ppl_funcCheckOutput(&call)) return;
}

//! pplfunc_gaussianCDFi - Implement the function stats.gaussianCDFi(x,sigma) in the stats module, which evaluates the
//! inverse Gaussian cumulative distribution function of standard deviation sigma at x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_gaussianCDFi(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "gaussianCDFi(x,sigma)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    if (ppl_funcCheckDimensionless(&call, 1)) return;
    OUTPUT.real = gsl_cdf_gaussian_Pinv(in[0].real, in[1].real);
    if (ppl_funcCheckOutput(&call)) return;
    ppl_unitsDimCpy(&OUTPUT, &in[1]);
}

//! pplfunc_lognormalPDF - Implement the function stats.lognormalPDF(x,zeta,sigma) in the stats module, which evaluates
//! the log normal probability density function of standard deviation sigma at x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_lognormalPDF(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "lognormalPDF(x,zeta,sigma)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    in += 2;
    if (ppl_funcCheckDimensionless(&call, 1)) return;
    in -= 2;
    if (ppl_funcCheckDimensionsMatch(&call)) return;
    OUTPUT.real = gsl_ran_lognormal_pdf(in[0].real, in[1].real, in[2].real);
    if (ppl_funcCheckOutput(&call)) return;
    ppl_unitsDimInverse(&OUTPUT, &in[0]);
}

//! pplfunc_lognormalCDF - Implement the function stats.lognormalCDF(x,zeta,sigma) in the stats module, which evaluates
//! the log normal cumulative distribution function of standard deviation sigma at x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_lognormalCDF(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "lognormalCDF(x,zeta,sigma)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    in += 2;
    if (ppl_funcCheckDimensionless(&call, 1)) return;
    in -= 2;
    if (ppl_funcCheckDimensionsMatch(&call)) return;
    OUTPUT.real = gsl_cdf_lognormal_P(in[0].real, in[1].real, in[2].real);
    if (ppl_funcCheckOutput(&call)) return;
}

//! pplfunc_lognormalCDFi - Implement the function stats.lognormalCDFi(x,zeta,sigma) in the stats module, which
//! evaluates the inverse log normal cumulative distribution function of standard deviation sigma at x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_lognormalCDFi(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "lognormalCDFi(x,zeta,sigma)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    in += 2;
    if (ppl_funcCheckDimensionless(&call, 1)) return;
    in -= 2;
    if (ppl_funcCheckDimensionsMatch(&call)) return;
    OUTPUT.real = gsl_cdf_lognormal_Pinv(in[0].real, in[1].real, in[2].real);
    if (ppl_funcCheckOutput(&call)) return;
    ppl_unitsDimCpy(&OUTPUT, &in[1]);
}

//! pplfunc_poissonPDF - Implement the function stats.poissonPDF(x,mu) in the stats module, which returns the
//! probability of getting x from a Poisson distribution with mean mu
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_poissonPDF(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "poissonPDF(x,mu)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    OUTPUT.real = gsl_ran_poisson_pdf(in[0].real, in[1].real);
    if (ppl_funcCheckOutput(&call)) return;
}

//! pplfunc_poissonCDF - Implement the function stats.poissonCDF(x,mu) in the stats module, which returns the
//! probability of getting <= x from a Poisson distribution with mean mu
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_poissonCDF(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "poissonCDF(x,mu)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    OUTPUT.real = gsl_cdf_poisson_P(in[0].real, in[1].real);
    if (ppl_funcCheckOutput(&call)) return;
}

//! pplfunc_tdistPDF - Implement the function stats.tdistPDF(x,nu) in the stats module, which returns the probability
//! density at x in a t-distribution with nu degrees of freedom
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_tdistPDF(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "tdistPDF(x,nu)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    OUTPUT.real = gsl_ran_tdist_pdf(in[0].real, in[1].real);
    if (ppl_funcCheckOutput(&call)) return;
}

//! pplfunc_tdistCDF - Implement the function stats.tdistCDF(x,nu) in the stats module, which returns the cumulative
//! probability density at x in a t-distribution with nu degrees of freedom
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_tdistCDF(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "tdistCDF(x,nu)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    OUTPUT.real = gsl_cdf_tdist_P(in[0].real, in[1].real);
    if (ppl_funcCheckOutput(&call)) return;
}

//! pplfunc_tdistCDFi - Implement the function stats.tdistCDFi(P,nu) in the stats module, which returns the point x at
//! which the cumulative probability density in a t-distribution with nu degrees of freedom is P
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_tdistCDFi(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "tdistCDFi(P,nu)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    OUTPUT.real = gsl_cdf_tdist_Pinv(in[0].real, in[1].real);
    if (ppl_funcCheckOutput(&call)) return;
}
