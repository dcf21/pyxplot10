// fit.c
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

// Implements the fit command, which fits a function to data read from a file by varying its free parameters to maximise
// the likelihood, i.e. to minimise the sum of the squared residuals, weighted by the error bars if supplied. It uses
// GSL's Nelder-Mead simplex minimiser, then computes the Hessian matrix of the log-likelihood by numerical
// differentiation to estimate the covariance matrix and the uncertainties of the parameters. If no error bars are
// supplied, their size is first estimated by maximising the Bayesian evidence. On large data sets, the residuals are
// summed in parallel with OpenMP when the function is safe to call from several threads.

#define _FIT_C 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <gsl/gsl_deriv.h>
#include <gsl/gsl_errno.h>
#include <gsl/gsl_linalg.h>
#include <gsl/gsl_math.h>
#include <gsl/gsl_matrix.h>
#include <gsl/gsl_multimin.h>
#include <gsl/gsl_permutation.h>
#include <gsl/gsl_vector.h>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "commands/fit.h"
#include "userspace/stack.h"
#include "coreUtils/errorReport.h"
#include "coreUtils/memAlloc.h"
#include "expressions/expCompile_fns.h"
#include "expressions/expEval.h"
#include "expressions/expThreadSafe.h"
#include "expressions/fnCall.h"
#include "expressions/traceback_fns.h"
#include "parser/cmdList.h"
#include "parser/parser.h"
#include "settings/settings.h"
#include "settings/settingTypes.h"
#include "stringTools/asciidouble.h"
#include "stringTools/strConstants.h"
#include "userspace/context.h"
#include "userspace/contextVarDef.h"
#include "userspace/garbageCollector.h"
#include "userspace/pplObj_fns.h"
#include "userspace/pplObjFunc.h"
#include "userspace/unitsArithmetic.h"
#include "userspace/unitsDisp.h"

#include "datafile.h"

// Structure used for passing data around
typedef struct fitComm {
    ppl_context *c;
    parserLine *pl;
    int NArgs; // The number of arguments taken by the function that we're fitting
    int NExpect; // The total number of columns we're reading from datafile; equals NArgs, plus the target value for f(), plus possibly the error on each target
    int NFitVars; // The number of variables listed after via ....
    int NParams; // The total number of free parameters in the fitting problems; either equals NFitVars, or twice this if complex arithmetic is enabled
    long int NDataPoints; // The number of data points read from the supplied datafile
    pplObj **
            varObj; // Pointers to the values of the variables which we're fitting the values of in the user's variable space
    const gsl_vector *paramVals; // Trial parameter values to be tried by fitResidual in this iteration
    const gsl_vector *bestFitParamVals; // The best fit parameter values (only set after first round of minimisation)
    gsl_vector
            *paramValsHessian; // Internal variable used by GetHessian() to vary parameter values when differentiating
    pplObj *firstVals; // The first values found in each column of the supplied datafile. These determine the physical units associated with each column.
    double *dataTable; // Two-dimensional table of the data read from the datafile.
    unsigned char
            flagYErrorBars; // If true, the user has specified errorbars for each target value. If false, we have no idea of uncertainty.
    char *errtext, *functionName; // String workspaces
    pplObj *functionObj;
    unsigned char goneNaN; // Used by the minimiser to keep track of when the function being minimised has returned NAN.
    double sigmaData; // The assumed errorbar (uniform for all datapoints) on the supplied target values if errorbars are not supplied. We fit this.
    int diff1, diff2; // The numbers of the free parameters currently being differentiated inside GetHessian()
    double diff1step,
            diff2step; // The step size which GetHessian() recommends using for each of the parameters being differentiated
    int parallel; // True if the function may be evaluated at the data points on several threads at once
} fitComm;

// The residuals of the data points are summed in blocks of this many points, and the sums of the blocks are then
// added together in order. This order is the same whether the blocks are summed serially or on several threads at
// once, so the result does not depend on the number of threads. For fits to no more points than this, the residuals
// are simply summed in order.
#define FIT_BLOCK_POINTS 4096

// Fits to at least this many data points (four blocks) are evaluated on several threads at once, if possible. Each
// thread works on whole blocks; fits to fewer points would give little work to share.
#define FIT_PARALLEL_MIN_POINTS (4 * FIT_BLOCK_POINTS)


//! matrixPrint - Write a GSL matrix as a nested list, e.g. [[1,2],[3,4]]
//! \param [in] c - The Pyxplot context
//! \param [in] m - The matrix to print
//! \param [in] size - The number of rows and columns in the matrix
//! \param [out] out - Character buffer into which to write the matrix
//! \param [in] outLen - The size of the buffer <out>
//! \return Pointer to <out>

static char *matrixPrint(ppl_context *c, const gsl_matrix *m, const size_t size, char *out, const size_t outLen) {
    size_t i, j;
    int p = 0;
    out[0] = '\0';
    ppl_strAppendf(out, outLen, &p, "[ [");
    // Write each row in square brackets
    for (i = 0; i < size; i++) {
        // Write each element, separated by commas
        for (j = 0; j < size; j++) {
            ppl_strAppendf(out, outLen, &p, "%s%s", (j > 0) ? "," : "",
                           ppl_numericDisplay(gsl_matrix_get(m, i, j), c->numdispBuff[0],
                                              c->set->term_current.SignificantFigures, 0));
        }
        ppl_strAppendf(out, outLen, &p, (i + 1 < size) ? "] , [" : "] ]"); // Start a new row, or finish
    }
    if (size == 0) ppl_strAppendf(out, outLen, &p, "]");
    return out;
}

//! fitPoint - Evaluate the function being fitted at one data point, and compute the point's contribution to the
//! mismatch between the function and the data
//! \param [in] c - The context in which to evaluate the function (the user's context, or a worker context)
//! \param [in] p - The fitComm structure describing the fit
//! \param [in] fn - The function object to call
//! \param [in] dummy - A dummy expression, used to report errors
//! \param [in] j - The number of the data point
//! \param [out] residual - Set to the point's contribution to the negative log-likelihood of the data
//! \param [out] errText - Buffer of LSTR_LENGTH bytes into which to write an error message, if the function fails
//! \return Zero on success, or one if the function could not be evaluated

static int fitPoint(ppl_context *c, const fitComm *p, pplObj *fn, pplExpr *dummy, const long j, double *residual,
                    char *errText) {
    const int stkLevelOld = c->stackPtr; // Stack: note its level, so that everything pushed can be discarded
    const double *row = p->dataTable + j * p->NExpect; // The arguments, target value and (perhaps) its uncertainty
    const pplObj *out;
    int k;

    // Stack: push the function to call, followed by its arguments
    ppl_stackPushCopy(c, fn, 1);
    // Push the function's arguments onto the stack, with the units of the corresponding data columns
    for (k = 0; k < p->NArgs; k++) {
        pplObjNum(&c->stack[c->stackPtr], 0, row[k], 0);
        ppl_unitsDimCpy(&c->stack[c->stackPtr], &p->firstVals[k]);
        c->stack[c->stackPtr].refCount = 1;
        c->stackPtr++;
    }

    // Call the function, which replaces it and its arguments on the stack with its value
    c->errStat.errMsgExpr[0] = '\0';
    ppl_fnCall(c, dummy, 0, 1, 1, 1);
    out = &c->stack[c->stackPtr - 1];

    // Report a failure to evaluate the function
    if (c->errStat.status) {
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "%s", c->errStat.errMsgExpr);
        ppl_tbClear(c);
        ppl_stackClean(c, stkLevelOld); // Stack: discard anything left by the failed call
        return 1;
    }
    // The function must return a number
    if (out->objType != PPLOBJ_NUM) {
        ppl_snprintfTrunc(errText, LSTR_LENGTH,
                          "The supplied function to fit produces a value which is not a number but has type <%s>.",
                          pplObjTypeNames[out->objType]);
        ppl_stackClean(c, stkLevelOld); // Stack: discard the function's value
        return 1;
    }
    // The function must return values with the same units as the data it is fitted to
    if (!ppl_unitsDimEqual(out, p->firstVals + p->NArgs)) {
        ppl_snprintfTrunc(
                errText, LSTR_LENGTH,
                "The supplied function to fit produces a value which is dimensionally incompatible with its target value. The function produces a result with dimensions of <%s>, while its target value has dimensions of <%s>.",
                ppl_printUnit(c, out, NULL, NULL, 0, 1, 0),
                ppl_printUnit(c, p->firstVals + p->NArgs, NULL, NULL, 1, 1, 0));
        ppl_stackClean(c, stkLevelOld); // Stack: discard the function's value
        return 1;
    }

    // Squared deviation of the function's value from the target value, divided by 2 sigma squared
    *residual = pow(out->real - row[k], 2) + pow(out->imag, 2);
    if (p->flagYErrorBars) *residual /= 2 * pow(row[k + 1], 2);
    else *residual /= 2 * pow(p->sigmaData, 2);

    ppl_stackClean(c, stkLevelOld); // Stack: discard the function's value, now that it has been used
    return 0;
}

//! fitResidualParallel - Sum the residuals of the data points in each block, on several threads at once
//! \param [in] p - The fitComm structure describing the fit
//! \param [in] nBlocks - The number of blocks of data points
//! \param [out] blockSums - Set to the sum of the residuals in each block
//! \param [out] failIndex - Set to the number of the first data point at which the function could not be evaluated,
//! or to the number of data points if there was none
//! \param [out] errText - Buffer of LSTR_LENGTH bytes, into which is written the error message for that data point

static void fitResidualParallel(fitComm *p, const long nBlocks, double *blockSums, long *failIndex, char *errText) {
    *failIndex = p->NDataPoints;
#ifdef _OPENMP
#pragma omp parallel
    {
        // Each thread calls the function in a worker context of its own, with private copies of the shared variables
        // it reads. These are made after the trial values of the free parameters have been set, so hold their values.
        ppl_context *w = ppl_contextWorkerNew(p->c);
        char *myErrText = (char *) malloc(LSTR_LENGTH);
        long myFailIndex = p->NDataPoints, b;
        pplObj ownFn, *fn = p->functionObj;
        pplExpr dummy;
        dummy.srcLineN = 0;
        dummy.srcId = 0;
        dummy.srcFname = "<dummy>";
        dummy.ascii = NULL;

        // Each thread uses its own copy of the function and its own stack
        if ((w != NULL) && (myErrText != NULL)) {
            if (ppl_contextWorkerPrivatiseFunc(w, p->c, p->functionObj, &ownFn) == 0) fn = &ownFn;
            ppl_stackMustHave(w, p->NArgs + 1);
        }

#pragma omp for schedule(dynamic, 1)
        // Each thread sums the residuals of whole blocks of data points, so that the total is the same however the work
        // is divided
        for (b = 0; b < nBlocks; b++) {
            const long start = b * FIT_BLOCK_POINTS;
            const long end =
                    ((start + FIT_BLOCK_POINTS < p->NDataPoints) ? (start + FIT_BLOCK_POINTS) : p->NDataPoints);
            double sum = 0.0, residual;
            long j;
            // Report a thread which could not be set up as a failure at the start of its first block
            if ((w == NULL) || (myErrText == NULL) || w->stackFull) {
                if (start < myFailIndex) { // Report the problem as if it occurred at the start of this block
                    myFailIndex = start;
                    if (myErrText != NULL) strcpy(myErrText, "Out of memory, or stack overflow, in the fit command.");
                }
                continue;
            }
            if (start >= myFailIndex) continue; // A failure earlier in the data has already been found
            // Sum the residuals of the data points in this block
            for (j = start; j < end; j++) {
                // Note where the first failure occurred, so that the earliest failure is the one reported
                if (fitPoint(w, p, fn, &dummy, j, &residual, myErrText)) {
                    myFailIndex = j;
                    break;
                }
                sum += residual;
            }
            blockSums[b] = sum;
        }

        // The error to report is the one at the first failing data point, as when the points are summed serially
#pragma omp critical(fitResidualFailure)
        // Keep the error from whichever thread failed earliest in the data
        if (myFailIndex < *failIndex) {
            *failIndex = myFailIndex;
            ppl_snprintfTrunc(errText, LSTR_LENGTH, "%s",
                              (myErrText != NULL) ? myErrText : "Out of memory in the fit command.");
        }
        free(myErrText);
        ppl_contextWorkerFree(w); // Frees the worker's private copy of the function too
    }
#endif
}

//! fitResidual - Compute the mismatch between the function being fitted and the data, for the current values of
//! the free parameters, as the negative log-likelihood of the data
//! \param [in] p - The fitComm structure describing the fit
//! \return The mismatch, or NaN if the function could not be evaluated

static double fitResidual(fitComm *p) {
    // Variables for summing the residuals
    ppl_context *c = p->c;
    char *errText = p->errtext; // A buffer of LSTR_LENGTH bytes
    const long nBlocks = (p->NDataPoints + FIT_BLOCK_POINTS - 1) / FIT_BLOCK_POINTS;
    double accumulator, *blockSums;
    long b, failIndex;
    int i;

    // Set free parameter values
    for (i = 0; i < p->NFitVars; i++)
        if (c->set->term_current.ComplexNumbers == SW_ONOFF_OFF)
            p->varObj[i]->real =
                    gsl_vector_get(p->paramVals, i); // This is setting variables in the user's variable space
        // With complex arithmetic, each variable has a real and an imaginary component
        else {
            p->varObj[i]->real = gsl_vector_get(p->paramVals, 2 * i);
            p->varObj[i]->imag = gsl_vector_get(p->paramVals, 2 * i + 1);
        }

    // Check there's enough space on the stack
    ppl_stackMustHave(c, p->NArgs);
    // Report a stack overflow
    if (c->stackFull) {
        strcpy(errText, "stack overflow in the fit command.");
        return GSL_NAN;
    }

    // The sum of the residuals in each block of data points
    blockSums = (double *) malloc(((nBlocks > 0) ? nBlocks : 1) * sizeof(double));
    // Failure to allocate memory is reported as an error
    if (blockSums == NULL) {
        strcpy(errText, "Out of memory in the fit command.");
        return GSL_NAN;
    }

    // Sum the residuals in parallel, or one block after another
    if (p->parallel) {
        fitResidualParallel(p, nBlocks, blockSums, &failIndex, errText);
    } else {
        // Sum the residuals of the data points serially, block by block
        pplExpr dummy; // Dummy expression object with dummy line number information
        dummy.srcLineN = 0;
        dummy.srcId = 0;
        dummy.srcFname = "<dummy>";
        dummy.ascii = NULL;
        failIndex = p->NDataPoints;
        // Sum each block, stopping at the first failure
        for (b = 0; (b < nBlocks) && (failIndex == p->NDataPoints); b++) {
            const long start = b * FIT_BLOCK_POINTS;
            const long end =
                    ((start + FIT_BLOCK_POINTS < p->NDataPoints) ? (start + FIT_BLOCK_POINTS) : p->NDataPoints);
            double sum = 0.0, residual;
            long j;
            // Sum the residuals of the data points in this block
            for (j = start; j < end; j++) {
                // Note where the failure occurred
                if (fitPoint(c, p, p->functionObj, &dummy, j, &residual, errText)) {
                    failIndex = j;
                    break;
                }
                sum += residual;
            }
            blockSums[b] = sum;
        }
    }

    // Return NaN if any data point could not be evaluated
    if (failIndex < p->NDataPoints) {
        free(blockSums);
        return GSL_NAN; // errText describes the failure
    }

    // Add up the sums of the blocks, in order
    accumulator = 0.0;
    for (b = 0; b < nBlocks; b++) accumulator += blockSums[b];
    free(blockSums);
    return accumulator;
}

// Slave routines called by the differentiation operation when working out the Hessian matrix

//! GetHessian_diff2 - Slave routine for GetHessian_diff1(), which evaluates the residual as a function of the
//! second of the two free parameters being differentiated with respect to
//! \param [in] x - The trial value of the second free parameter
//! \param [in] p_void - Pointer to the fitComm structure describing the fit
//! \return The residual

static double GetHessian_diff2(const double x, void *p_void) {
    double tmp, output;
    fitComm *p = (fitComm *) p_void;
    tmp = gsl_vector_get(p->paramValsHessian, p->diff2); // Replace old parameter with x
    gsl_vector_set(p->paramValsHessian, p->diff2, x);
    output = fitResidual(p); // Evaluate residual
    gsl_vector_set(p->paramValsHessian, p->diff2, tmp); // Restore old parameter value
    return output;
}

//! GetHessian_diff1 - Slave routine for GetHessian(), which evaluates the derivative of the residual with respect
//! to the second of two free parameters, as a function of the first
//! \param [in] x - The trial value of the first free parameter
//! \param [in] p_void - Pointer to the fitComm structure describing the fit
//! \return The derivative of the residual with respect to the second free parameter

static double GetHessian_diff1(const double x, void *p_void) {
    double tmp, output, output_error;
    gsl_function fn;
    fitComm *p = (fitComm *) p_void;

    // Differentiate the residual with respect to the second parameter, with the first set to x
    fn.function = &GetHessian_diff2;
    fn.params = p_void;

    tmp = gsl_vector_get(p->paramValsHessian, p->diff1); // Replace old parameter with x
    gsl_vector_set(p->paramValsHessian, p->diff1, x);
    gsl_deriv_central(&fn, gsl_vector_get(p->paramValsHessian, p->diff2), p->diff2step, &output,
                      &output_error); // Differentiate residual a second time
    gsl_vector_set(p->paramValsHessian, p->diff1, tmp); // Restore old parameter value
    return output;
}

//! GetHessian - Compute the Hessian matrix of second derivatives of the residual with respect to the free
//! parameters, at their best-fitting values, using numerical differentiation
//! \param [in] p - The fitComm structure describing the fit
//! \return The Hessian matrix, allocated with gsl_matrix_alloc(), or NULL if it could not be computed

static gsl_matrix *GetHessian(fitComm *p) {
    // Variables for computing the Hessian matrix
    gsl_matrix *out;
    int i, j;
    double output, output_error;
    gsl_function fn;

    // Allocate a vector for passing our position in free-parameter space to fitResidual()
    p->paramValsHessian = gsl_vector_alloc(p->NParams);
    for (i = 0; i < p->NParams; i++) gsl_vector_set(p->paramValsHessian, i, gsl_vector_get(p->bestFitParamVals, i));
    p->paramVals = p->paramValsHessian;

    out = gsl_matrix_alloc(p->NParams, p->NParams); // Allocate memory for Hessian matrix
    if (out == NULL) return NULL;

    // Each element is a second derivative of the residual, found by nesting two numerical differentiations
    fn.function = &GetHessian_diff1;
    fn.params = (void *) p;

    // Loop over the elements of the Hessian, differentiating with steps of a millionth of each parameter
    for (i = 0; i < p->NParams; i++)
        for (j = 0; j < p->NParams; j++) // Loop over elements of Hessian matrix
        {
            p->diff1 = i;
            p->diff1step = ((output = fabs(gsl_vector_get(p->paramVals, i)) * 1e-6) < 1e-100) ? 1e-6 : output;
            p->diff2 = j;
            p->diff2step = ((output = fabs(gsl_vector_get(p->paramVals, j)) * 1e-6) < 1e-100) ? 1e-6 : output;
            gsl_deriv_central(&fn, gsl_vector_get(p->paramVals, p->diff1), p->diff1step, &output, &output_error);
            gsl_matrix_set(out, i, j, -output); // Minus sign here since fitResidual returns the negative of log(P)
        }

    // Free the vector of parameter values
    gsl_vector_free(p->paramValsHessian);
    p->paramValsHessian = NULL;
    return out;
}

// Slave routines called by minimisers

//! ResidualMinimiserSlave - Cost function passed to the GSL minimiser when fitting a function to data
//! \param [in] x - The trial values of the free parameters, in the minimiser's coordinate system
//! \param [in] p_void - Pointer to the fitComm structure describing the fit
//! \return The residual for these parameter values

static double ResidualMinimiserSlave(const gsl_vector *x, void *p_void) {
    fitComm *p = (fitComm *) p_void;
    p->paramVals = x;
    return fitResidual(p);
}

//! FitsigmaData - Cost function passed to the GSL minimiser when estimating the uncertainty in the data, for fits
//! where no error bars were supplied. It returns minus the log of the Bayesian evidence of the fit, as a function
//! of the assumed uncertainty.
//! \param [in] x - The trial value of the uncertainty, in the minimiser's coordinate system
//! \param [in] p_void - Pointer to the fitComm structure describing the fit
//! \return Minus the log of the evidence

static double FitsigmaData(const gsl_vector *x, void *p_void) {
    double term1, term2, term3;
    int sgn;
    gsl_matrix *hessian;
    gsl_permutation *perm;

    // Evaluate the negative log-evidence for the best-fit parameters, assuming error bars of size sigmaData on every
    // data point
    fitComm *p = (fitComm *) p_void;
    p->sigmaData = gsl_vector_get(x, 0);
    p->paramVals = p->bestFitParamVals;
    term1 = fitResidual(p); // Likelihood for the best-fit set of parameters, without the Gaussian normalisation factor
    term2 = -(p->NDataPoints * log(1.0 / sqrt(2 * M_PI) / p->sigmaData)); // Gaussian normalisation factor

    // term3 is the Occam Factor, which equals the determinant of -H
    hessian = GetHessian(p);
    gsl_matrix_scale(hessian, -1.0); // Want the determinant of -H
    // Generate the LU decomposition of the Hessian matrix
    perm = gsl_permutation_alloc(p->NParams);
    gsl_linalg_LU_decomp(hessian, perm, &sgn); // Hessian matrix is overwritten here, but we don't need it again
    // Calculate the determinant of the Hessian matrix
    term3 = gsl_linalg_LU_lndet(hessian) * 0.5; // -log(1/sqrt(det))
    gsl_matrix_free(hessian);
    hessian = NULL;
    gsl_permutation_free(perm);
    perm = NULL;

    // Return the negative log of the evidence
    return term1 + term2 +
            term3; // We return the negative of the log-likelihood, which GSL then minimises by varying the assume errorbar size
}

// Top-level routine for managing the GSL minimiser

//! FitMinimiseIterate - Run the GSL simplex minimiser repeatedly, in a sequence of coordinate systems, until the
//! solution converges
//! \param [in|out] commlink - The fitComm structure describing the fit; the solution is stored in it
//! \param [in] slave - The cost function to minimise
//! \param [in] FittingsigmaData - If true, the uncertainty in the data is being fitted, rather than the free
//! parameters of the function
//! \return Zero on success, or one if an error occurred

static int FitMinimiseIterate(fitComm *commlink, double (*slave)(const gsl_vector *, void *),
                              const unsigned char FittingsigmaData) {
    ppl_context *c = commlink->c;
    size_t iter = 0, iter2 = 0;
    int i, status = 0, NParams;
    double size = 0, sizelast = 0, sizelast2 = 0;
    const gsl_multimin_fminimizer_type *T =
            gsl_multimin_fminimizer_nmsimplex; // We don't use nmsimplex2 here because it was new in gsl 1.12
    gsl_multimin_fminimizer *s;
    gsl_vector *x, *ss;
    gsl_multimin_function fn;

    // When fitting the size of the error bars, there is only one parameter to vary
    if (!FittingsigmaData) NParams = commlink->NParams;
    else NParams = 1;
    x = gsl_vector_alloc(NParams);
    ss = gsl_vector_alloc(NParams);

    // Describe the function to minimise to GSL
    fn.n = NParams;
    fn.f = slave;
    fn.params = (void *) commlink;

    // Restart the minimiser several times, until the result stops improving
    iter2 = 0;
    // Each pass of the minimiser
    do {
        iter2++;
        sizelast2 = size;

        // Start from the current values of the parameters, or from the current estimate of the size of the error bars
        if (!FittingsigmaData) {
            if (c->set->term_current.ComplexNumbers == SW_ONOFF_OFF)
                for (i = 0; i < commlink->NFitVars; i++) gsl_vector_set(x, i, commlink->varObj[i]->real);
            else
                // Each variable takes two elements of the vector
                for (i = 0; i < commlink->NFitVars; i++) {
                    gsl_vector_set(x, 2 * i, commlink->varObj[i]->real);
                    gsl_vector_set(x, 2 * i + 1, commlink->varObj[i]->imag);
                }
        } else {
            gsl_vector_set(x, 0, commlink->sigmaData);
        }

        // The initial step size is 10% of each parameter
        for (i = 0; i < NParams; i++) {
            if (fabs(gsl_vector_get(x, i)) > 1e-6) gsl_vector_set(ss, i, 0.1 * gsl_vector_get(x, i));
            else gsl_vector_set(ss, i, 0.1); // Avoid having a stepsize of zero
        }

        // Create the minimiser
        s = gsl_multimin_fminimizer_alloc(T, fn.n);
        gsl_multimin_fminimizer_set(s, &fn, x, ss);

        // If initial value we are giving the minimiser produces an algebraic error, it's not worth continuing
        (*slave)(x, (void *) commlink);
        // Give up if the function could not be evaluated at the starting point
        if (commlink->errtext[0] != '\0') {
            gsl_vector_free(x);
            x = NULL;
            gsl_vector_free(ss);
            ss = NULL;
            gsl_multimin_fminimizer_free(s);
            s = NULL;
            return 1;
        }

        // Iterate the minimiser until the simplex stops shrinking
        iter = 0;
        commlink->goneNaN = 0;
        // Each batch of iterations
        do {
            iter++;
            for (i = 0; i < 2 + NParams * 2;
                 i++) // When you're minimising over many parameters simultaneously sometimes nothing happens for a long time
            {
                status = gsl_multimin_fminimizer_iterate(s);
                if (status) break;
            }
            if (status) break;
            sizelast = size;
            size = gsl_multimin_fminimizer_minimum(s);
        } while ((iter < 10) ||
                 ((size < sizelast) &&
                  (iter < 50))); // Iterate 10 times, and then see whether size carries on getting smaller

        // Free the minimiser before the next pass
        gsl_multimin_fminimizer_free(s);
        s = NULL;
    } while ((iter2 < 3) ||
             ((commlink->goneNaN == 0) && (!status) && (size < sizelast2) &&
              (iter2 < 20))); // Iterate 2 times, and then see whether size carries on getting smaller

    // Too many passes means the minimiser failed to converge
    if (iter2 >= 20) status = 1;

    // Report a failure to converge
    if (status) {
        ppl_snprintfTrunc(commlink->errtext, LSTR_LENGTH, "Failed to converge. GSL returned error: %s",
                          gsl_strerror(status));
        gsl_vector_free(x);
        x = NULL;
        gsl_vector_free(ss);
        ss = NULL;
        return 1;
    }
    sizelast =
            (*slave)(x, (void *) commlink); // Calling minimiser slave now sets all fitting variables to desired values

    // Free the minimiser's vectors
    gsl_vector_free(x);
    x = NULL;
    gsl_vector_free(ss);
    ss = NULL;
    return 0;
}

//! ppl_directive_fit - Implement the fit command, which fits a function to data by maximum likelihood, and reports
//! the best-fitting values of the free parameters, their uncertainties and correlations
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppl_directive_fit(ppl_context *c, parserLine *pl, parserOutput *in, const int interactive, const int iterDepth) {
    // Variables for reading the data, fitting it and reporting the results
    char msg[PPL_MSG_LENGTH];
    pplObj *stk = in->stk;
    int status = 0, Nargs, Nvars, NExpect, Nusing;
    char *fnName, *scratchPad;
    long int i, j, k, NDataPoints;
    int sigmaPos; // Position in scratchPad when building the names of fitted variables
    int contextLocalVec, contextDataTab, errCount = DATAFILE_NERRS;
    char *fitVars[USING_ITEMS_MAX];
    pplObj unit[USING_ITEMS_MAX];
    int minSet[USING_ITEMS_MAX], maxSet[USING_ITEMS_MAX];
    double min[USING_ITEMS_MAX], max[USING_ITEMS_MAX];
    pplObj firstVals[USING_ITEMS_MAX], *varObj[USING_ITEMS_MAX];
    dataTable *data;
    dataBlock *blk;
    parserLine *spool = NULL, **dataSpool = &spool;
    double *localDataTable, val;
    gsl_vector *bestFitParamVals;
    gsl_matrix *hessian, *hessian_lu, *hessian_inv;
    int sgn;
    double stdDev[2 * USING_ITEMS_MAX], tmp1, tmp2, tmp3;
    fitComm dataComm; // Structure which we fill with variables we need to pass to the minimiser
    pplFunc *funcDef;
    pplObj *funcObj;
    gsl_permutation *perm;

    // Get list of fitting variables
    {
        int pos = PARSE_fit_fit_variables;
        Nvars = 0;
        // Walk the linked list of the fitting variables on the parser stack
        while (stk[pos].objType == PPLOBJ_NUM) {
            pos = (int) round(stk[pos].real);
            if (pos <= 0) break;
            // There is a fixed limit on the number of fitting variables
            if (Nvars >= USING_ITEMS_MAX - 4) {
                ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                                  "Too many fitting variables; a maximum of %d may be specified.", USING_ITEMS_MAX - 4);
                ppl_tbAddLine(c, pl, ERR_SYNTAX, in->stkCharPos[pos + PARSE_fit_fit_variable_fit_variables], "");
                return;
            }
            fitVars[Nvars] = (char *) stk[pos + PARSE_fit_fit_variable_fit_variables].auxil;
            Nvars++;
        }
        // There must be at least one fitting variable
        if (Nvars < 1) {
            ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                              "Too few fitting variables; a minimum of one must be specified.");
            ppl_tbAddLine(c, pl, ERR_SYNTAX, 0, "");
            return;
        }
    }

    // Get pointers to fitting variables
    {
        int i;
        // Look up each fitting variable, creating it if it does not exist
        for (i = 0; i < Nvars; i++) {
            pplObj dummyTemp;
            ppl_contextGetVarPointer(c, fitVars[i], &varObj[i], &dummyTemp);
            // If the variable already had a numerical value, use it, with its units, as the starting guess
            if (dummyTemp.objType == PPLOBJ_NUM) {
                pplObjCpy(varObj[i], &dummyTemp, 0, 1, 1);
            } else {
                pplObjNum(varObj[i], 0, 1, 0);
            }
            dummyTemp.amMalloced = 0;
            ppl_garbageObject(&dummyTemp);
        }
    }

    // Get name of function to fit
    fnName = (char *) stk[PARSE_fit_fit_function].auxil;
    ppl_contextVarLookup(c, fnName, &funcObj, 0);
    // The function to fit must exist
    if (funcObj == NULL) {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "No such function as '%s()'.", fnName);
        ppl_tbAddLine(c, pl, ERR_NAMESPACE, in->stkCharPos[PARSE_fit_fit_function], "");
        return;
    }
    // The object named must be a function
    if (funcObj->objType != PPLOBJ_FUNC) {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                          "Object '%s' is not a function, but has type <%s>.", fnName,
                          pplObjTypeNames[funcObj->objType]);
        ppl_tbAddLine(c, pl, ERR_TYPE, in->stkCharPos[PARSE_fit_fit_function], "");
        return;
    }
    funcDef = (pplFunc *) funcObj->auxil;
    // Magic functions such as diff_d() cannot be fitted directly
    if (funcDef->functionType == PPL_FUNC_MAGIC) {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                          "Function %s() needs wrapping in a user-defined function before it can be fit.", fnName);
        ppl_tbAddLine(c, pl, ERR_TYPE, in->stkCharPos[PARSE_fit_fit_function], "");
        return;
    }
    __sync_add_and_fetch(&funcDef->refCount, 1);
    Nargs = funcDef->minArgs;

    // Count length of using list
    {
        const int hadNonNullUsingItem = 0;
        int pos = PARSE_fit_using_list;
        const int o = PARSE_fit_using_item_using_list;

        // Count the using items, which select the columns of the data
        Nusing = 0;
        // Walk the linked list of using items on the parser stack
        while (stk[pos].objType == PPLOBJ_NUM) {
            pos = (int) round(stk[pos].real);
            if (pos <= 0) break;
            // There is a fixed limit on the number of using items
            if (Nusing >= USING_ITEMS_MAX) {
                ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                                  "Too many using items; maximum of %d are allowed.", USING_ITEMS_MAX);
                ppl_tbAddLine(c, pl, ERR_RANGE, in->stkCharPos[pos + o], "");
                goto cleanup;
            }
            Nusing++;
        }
        if ((!hadNonNullUsingItem) && (Nusing == 1))
            Nusing = 0; // If we've only had one using item, and it was blank, this is a parser abberation
    }

    // Work out how many columns of data we're going to read
    if (Nusing != Nargs + 2) NExpect = Nargs + 1;
    else NExpect = Nargs + 2; // Only expect weights to go with fitting data if using list has exactly the right length

    // Read ranges
    {
        int i, pos = PARSE_fit_0range_list, nr = 0;
        const int o1 = PARSE_fit_min_0range_list;
        const int o2 = PARSE_fit_max_0range_list;
        for (i = 0; i < USING_ITEMS_MAX; i++) minSet[i] = maxSet[i] = 0;
        // Walk the linked list of ranges on the parser stack
        while (stk[pos].objType == PPLOBJ_NUM) {
            pos = (int) round(stk[pos].real);
            if (pos <= 0) break;
            // There is a fixed limit on the number of ranges
            if (nr >= USING_ITEMS_MAX) {
                sprintf(c->errStat.errBuff, "Too many ranges specified; a maximum of %d are allowed.", USING_ITEMS_MAX);
                ppl_tbAddLine(c, pl, ERR_SYNTAX, 0, "");
                goto cleanup;
            }
            if (ppldata_readRange(c, pl, in, pos, o1, o2, nr, NULL, unit, min, minSet, max, maxSet)) goto cleanup;
            nr++;
        }
    }

    // Allocate a new memory context for the data file we're about to read
    contextLocalVec = ppl_memAlloc_DescendIntoNewContext();
    contextDataTab = ppl_memAlloc_DescendIntoNewContext();

    // Read data from file
    ppldata_fromCmd(c, &data, pl, in, 0, NULL, dataSpool, PARSE_TABLE_fit_, 0, NExpect, 0, min, minSet, max, maxSet,
                    unit, 0, &status, c->errStat.errBuff, &errCount, iterDepth);

    // Exit on error
    if ((status) || (data == NULL)) {
        ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "");
        ppl_memAlloc_AscendOutOfContext(contextLocalVec);
        goto cleanup;
    }

    // Check that the firstEntries above have the same units as any supplied ranges
    for (j = 0; j < NExpect; j++)
        // Only columns with a range need checking
        if (minSet[j] || maxSet[j]) {
            // The range must have the same units as the data
            if (!ppl_unitsDimEqual(&unit[j], data->firstEntries + j)) {
                ppl_snprintfTrunc(
                        c->errStat.errBuff, sizeof(c->errStat.errBuff),
                        "The minimum and maximum limits specified in range %ld in the fit command have conflicting physical dimensions with the data returned from the data file. The limits have units of <%s>, whilst the data have units of <%s>.",
                        j + 1, ppl_printUnit(c, unit + j, NULL, NULL, 0, 1, 0),
                        ppl_printUnit(c, data->firstEntries + j, NULL, NULL, 1, 1, 0));
                ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
                goto cleanup;
            }
        }

    // Work out how many data points we have within the specified ranges
    NDataPoints = 0;
    blk = data->first;
    // Count the data points which fall within the ranges
    while (blk != NULL) {
        int j;
        // Loop over the rows in each block
        for (j = 0; j < blk->blockPosition; j++) {
            int inRange = 1, k;
            // Skip rows with any value outside its column's range
            for (k = 0; k < NExpect; k++) {
                val = blk->data_real[k + NExpect * j];
                // This value is outside the range
                if ((minSet[k] && (val < min[k])) || (maxSet[k] && (val > max[k]))) {
                    inRange = 0;
                    break;
                } // Check that value is within range
            }
            if (inRange) NDataPoints++;
        }
        blk = blk->next;
    }

    // Copy data into a new table and apply the specified ranges to it
    localDataTable = (double *) ppl_memAlloc_incontext(NDataPoints * NExpect * sizeof(double), contextLocalVec);
    // Failure to allocate memory is reported as an error
    if (localDataTable == NULL) {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "Out of memory.");
        ppl_tbAddLine(c, pl, ERR_MEMORY, 0, "");
        goto cleanup;
    }
    i = 0;
    blk = data->first;
    // Copy the data points which fall within the ranges into a single array
    while (blk != NULL) {
        int j;
        // Loop over the rows in each block
        for (j = 0; j < blk->blockPosition; j++) {
            int inRange = 1, k;
            // Skip rows with any value outside its column's range
            for (k = 0; k < NExpect; k++) {
                val = blk->data_real[k + NExpect * j];
                // This value is outside the range
                if ((minSet[k] && (val < min[k])) || (maxSet[k] && (val > max[k]))) {
                    inRange = 0;
                    break;
                } // Check that value is within range
                localDataTable[i * NExpect + k] = val;
            }
            if (inRange) i++;
        }
        blk = blk->next;
    }

    // Make a copy of the physical units of all of the columns of data
    for (j = 0; j < NExpect; j++) firstVals[j] = data->firstEntries[j];

    // Free original data table which is no longer needed
    ppl_memAlloc_AscendOutOfContext(contextDataTab);
    data = NULL;

    // Populate dataComm
    dataComm.c = c;
    dataComm.pl = pl;
    dataComm.functionName = fnName;
    dataComm.functionObj = funcObj;
    dataComm.NFitVars = Nvars;
    dataComm.NArgs = Nargs;
    dataComm.NExpect = NExpect;
    dataComm.NParams = dataComm.NFitVars * ((c->set->term_current.ComplexNumbers == SW_ONOFF_OFF) ? 1 : 2);
    dataComm.NDataPoints = NDataPoints;
    dataComm.varObj = varObj;
    dataComm.paramVals = NULL;
    dataComm.bestFitParamVals = NULL;
    dataComm.firstVals = firstVals;
    dataComm.dataTable = localDataTable;
    dataComm.flagYErrorBars = (NExpect == Nargs + 2);
    dataComm.sigmaData = 1.0;
#ifdef _OPENMP
    dataComm.parallel =
            (NDataPoints >= FIT_PARALLEL_MIN_POINTS) && (omp_get_max_threads() > 1) && ppl_funcThreadSafe(c, funcObj);
#else
    dataComm.parallel = 0;
#endif
    dataComm.errtext = (char *) ppl_memAlloc_incontext(LSTR_LENGTH, contextLocalVec);
    scratchPad = (char *) ppl_memAlloc_incontext(LSTR_LENGTH, contextLocalVec);
    // Failure to allocate memory is reported as an error
    if ((scratchPad == NULL) || (dataComm.errtext == NULL)) {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "Out of memory.");
        ppl_tbAddLine(c, pl, ERR_MEMORY, 0, "");
        goto cleanup;
    }
    dataComm.errtext[0] = '\0';

    // Set up a minimiser
    status = FitMinimiseIterate(&dataComm, &ResidualMinimiserSlave, 0);
    // Report a failure to fit
    if (status) {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "%s", dataComm.errtext);
        ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
        goto cleanup;
    }

    // Display the results of the minimiser
    ppl_report(&c->errcontext, "\n# Best fit parameters were:\n# -------------------------\n");
    // Print the value of each variable, in LaTeX if the numeric display is set to latex
    for (j = 0; j < dataComm.NFitVars; j++) {
        if (c->set->term_current.NumDisplay != SW_DISPLAY_L)
            ppl_snprintfTrunc(msg, sizeof(msg), "%s = %s", fitVars[j], ppl_unitsNumericDisplay(c, varObj[j], 0, 0, 0));
        else
            ppl_snprintfTrunc(msg, sizeof(msg), "$%s = %s", fitVars[j],
                              ppl_unitsNumericDisplay(c, varObj[j], 0, 0, 0) + 1);
        ppl_report(&c->errcontext, msg);
    }

    // If 'withouterrors' is specified, stop now
    if (stk[PARSE_fit_withouterrors].objType == PPLOBJ_STR) {
        ppl_memAlloc_AscendOutOfContext(contextLocalVec);
        goto cleanup;
    }

    // Store best-fit position
    bestFitParamVals = gsl_vector_alloc(dataComm.NParams);
    for (i = 0; i < dataComm.NFitVars; i++)
        if (c->set->term_current.ComplexNumbers == SW_ONOFF_OFF) gsl_vector_set(bestFitParamVals, i, varObj[i]->real);
        // With complex arithmetic, each variable has a real and an imaginary component
        else {
            gsl_vector_set(bestFitParamVals, 2 * i, varObj[i]->real);
            gsl_vector_set(bestFitParamVals, 2 * i + 1, varObj[i]->imag);
        }
    dataComm.bestFitParamVals = bestFitParamVals;

    // Estimate the size of the errorbars on the supplied data if no errorbars were supplied (this doesn't affect best fit position, but does affect error estimates)
    if (!dataComm.flagYErrorBars) {
        ppl_snprintfTrunc(
                msg, sizeof(msg),
                "\n# Estimating the size of the error bars on supplied data.\n# This may take a while.\n# The fit command can be made to run very substantially faster if the 'withouterrors' option is set.");
        ppl_report(&c->errcontext, msg);
        status = FitMinimiseIterate(&dataComm, &FitsigmaData, 1);
        // Report a failure to estimate the size of the error bars
        if (status) {
            ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "%s", dataComm.errtext);
            ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
            gsl_vector_free(bestFitParamVals);
            bestFitParamVals = NULL;
            goto cleanup;
        }
        firstVals[Nargs].real = dataComm.sigmaData;
        firstVals[Nargs].imag = 0.0;
        firstVals[Nargs].flagComplex = 0;
        ppl_snprintfTrunc(msg, sizeof(msg),
                          "\n# Estimate of error bars on supplied data, based on their fit to model function = %s",
                          ppl_unitsNumericDisplay(c, firstVals + Nargs, 0, 0, 0));
        ppl_report(&c->errcontext, msg);
    }

    // Calculate and print the Hessian matrix
    hessian = GetHessian(&dataComm);
    matrixPrint(c, hessian, dataComm.NParams, scratchPad, LSTR_LENGTH);
    ppl_snprintfTrunc(
            msg, sizeof(msg),
            "\n# Hessian matrix of log-probability distribution:\n# -----------------------------------------------\n\nhessian = %s",
            scratchPad);
    ppl_report(&c->errcontext, msg);

    // Set variables in user's variable space to best-fit values
    for (i = 0; i < dataComm.NFitVars; i++)
        if (c->set->term_current.ComplexNumbers == SW_ONOFF_OFF)
            varObj[i]->real = gsl_vector_get(dataComm.bestFitParamVals, i);
        // With complex arithmetic, each variable has a real and an imaginary component
        else {
            varObj[i]->real = gsl_vector_get(dataComm.bestFitParamVals, 2 * i);
            varObj[i]->imag = gsl_vector_get(dataComm.bestFitParamVals, 2 * i + 1);
        }

    // Calculate and print the covariance matrix
    hessian_lu = gsl_matrix_alloc(dataComm.NParams, dataComm.NParams);
    hessian_inv = gsl_matrix_alloc(dataComm.NParams, dataComm.NParams);
    gsl_matrix_memcpy(hessian_lu, hessian);
    gsl_matrix_scale(hessian_lu, -1.0); // Want the inverse of -H
    perm = gsl_permutation_alloc(dataComm.NParams);
    gsl_linalg_LU_decomp(hessian_lu, perm, &sgn);
    gsl_linalg_LU_invert(hessian_lu, perm, hessian_inv);
    matrixPrint(c, hessian_inv, dataComm.NParams, scratchPad, LSTR_LENGTH);
    ppl_snprintfTrunc(
            msg, sizeof(msg),
            "\n# Covariance matrix of probability distribution:\n# ----------------------------------------------\n\ncovariance = %s",
            scratchPad);
    ppl_report(&c->errcontext, msg);

    // Calculate the standard deviation of each parameter
    for (i = 0; i < dataComm.NParams; i++) {
        // A variance which is not positive means the fit has failed; warn the user
        if ((gsl_matrix_get(hessian_inv, i, i) <= 0.0) || (!gsl_finite(gsl_matrix_get(hessian_inv, i, i)))) {
            ppl_snprintfTrunc(
                    msg, sizeof(msg),
                    "One of the calculated variances for the fitted parameters is negative. This strongly suggests that the fitting process has failed.");
            ppl_warning(&c->errcontext, ERR_NUMERICAL, msg);
            stdDev[i] = 1e-100;
        } else {
            stdDev[i] = sqrt(gsl_matrix_get(hessian_inv, i, i));
        }
    }

    // Calculate the correlation matrix
    for (i = 0; i < dataComm.NParams; i++)
        for (j = 0; j < dataComm.NParams; j++)
            gsl_matrix_set(hessian_inv, i, j, gsl_matrix_get(hessian_inv, i, j) / stdDev[i] / stdDev[j]);
    matrixPrint(c, hessian_inv, dataComm.NParams, scratchPad, LSTR_LENGTH);
    ppl_snprintfTrunc(
            msg, sizeof(msg),
            "\n# Correlation matrix of probability distribution:\n# ----------------------------------------------\n\ncorrelation = %s",
            scratchPad);
    ppl_report(&c->errcontext, msg);

    // Print a list of standard deviations
    ppl_report(&c->errcontext,
               "\n# Uncertainties in best-fit parameters are:\n# -----------------------------------------\n");
    // Print the uncertainty in each variable, with its units
    for (i = 0; i < dataComm.NFitVars; i++) {
        pplObj dummyTemp = *(varObj[i]);
        // Real variables have one uncertainty; complex ones have one for each component
        if (c->set->term_current.ComplexNumbers == SW_ONOFF_OFF) {
            dummyTemp.real = stdDev[i];
            dummyTemp.imag = 0.0;
            dummyTemp.flagComplex = 0; // Apply appropriate unit to standard deviation, which is currently just a double
            // Plain text, or LaTeX
            if (c->set->term_current.NumDisplay != SW_DISPLAY_L) {
                ppl_snprintfTrunc(scratchPad, LSTR_LENGTH, "sigma_%s", fitVars[i]);
                ppl_snprintfTrunc(msg, sizeof(msg), "%22s = %s", scratchPad,
                                  ppl_unitsNumericDisplay(c, &dummyTemp, 0, 0, 0));
            } else {
                ppl_snprintfTrunc(scratchPad, LSTR_LENGTH, "$\\sigma_\\textrm{");
                sigmaPos = (int) strlen(scratchPad);
                // Escape underscores in the variable name for LaTeX
                for (k = 0; fitVars[i][k] != '\0'; k++) {
                    if (fitVars[i][k] == '_') ppl_strAppendChar(scratchPad, LSTR_LENGTH, &sigmaPos, '\\');
                    ppl_strAppendChar(scratchPad, LSTR_LENGTH, &sigmaPos, fitVars[i][k]);
                }
                ppl_snprintfTrunc(msg, sizeof(msg), "%33s} = %s", scratchPad,
                                  ppl_unitsNumericDisplay(c, &dummyTemp, 0, 0, 0) + 1);
            }
            ppl_report(&c->errcontext, msg);
        } else {
            // Plain text, or LaTeX
            if (c->set->term_current.NumDisplay != SW_DISPLAY_L) {
                dummyTemp.real = stdDev[2 * i];
                dummyTemp.imag = 0.0;
                dummyTemp.flagComplex = 0;
                ppl_snprintfTrunc(scratchPad, LSTR_LENGTH, "sigma_%s_real", fitVars[i]);
                ppl_snprintfTrunc(msg, sizeof(msg), "%27s = %s", scratchPad,
                                  ppl_unitsNumericDisplay(c, &dummyTemp, 0, 0, 0));
                ppl_report(&c->errcontext, msg);
                dummyTemp.real = stdDev[2 * i + 1];
                dummyTemp.imag = 0.0;
                dummyTemp.flagComplex = 0;
                ppl_snprintfTrunc(scratchPad, LSTR_LENGTH, "sigma_%s_imag", fitVars[i]);
                ppl_snprintfTrunc(msg, sizeof(msg), "%27s = %s", scratchPad,
                                  ppl_unitsNumericDisplay(c, &dummyTemp, 0, 0, 0));
                ppl_report(&c->errcontext, msg);
            } else {
                dummyTemp.real = stdDev[2 * i];
                dummyTemp.imag = 0.0;
                dummyTemp.flagComplex = 0;
                ppl_snprintfTrunc(scratchPad, LSTR_LENGTH, "$\\sigma_\\textrm{");
                sigmaPos = (int) strlen(scratchPad);
                // Escape underscores in the variable name for LaTeX
                for (k = 0; fitVars[i][k] != '\0'; k++) {
                    if (fitVars[i][k] == '_') ppl_strAppendChar(scratchPad, LSTR_LENGTH, &sigmaPos, '\\');
                    ppl_strAppendChar(scratchPad, LSTR_LENGTH, &sigmaPos, fitVars[i][k]);
                }
                ppl_snprintfTrunc(msg, sizeof(msg), "%38s,real} = %s", scratchPad,
                                  ppl_unitsNumericDisplay(c, &dummyTemp, 0, 0, 0) + 1);
                ppl_report(&c->errcontext, msg);
                dummyTemp.real = stdDev[2 * i + 1];
                dummyTemp.imag = 0.0;
                dummyTemp.flagComplex = 0;
                ppl_snprintfTrunc(msg, sizeof(msg), "%38s,imag} = %s", scratchPad,
                                  ppl_unitsNumericDisplay(c, &dummyTemp, 0, 0, 0) + 1);
                ppl_report(&c->errcontext, msg);
            }
        }
    }

    // Print summary information
    ppl_report(&c->errcontext, "\n# Summary:\n# --------\n");
    // Print each variable in the form (value +/- uncertainty) unit
    for (i = 0; i < dataComm.NFitVars; i++) {
        pplObj dummyTemp;
        char *cptr = ppl_printUnit(c, varObj[i], &tmp1, &tmp2, 0, 1,
                                   0); // Work out what unit the best-fit value is best displayed in
        if (fabs(varObj[i]->real) > 1e-200)
            tmp3 = tmp1 /
                    varObj[i]
                            ->real; // Set tmp3 to be the multiplicative size of this unit relative to its SI counterpart
        else if (fabs(varObj[i]->imag) > 1e-200)
            tmp3 = tmp2 / varObj[i]->imag; // Can't do this if magnitude of best-fit value is zero, though...
        // If the value is zero, use the unit in which one would be displayed
        else {
            dummyTemp = *(varObj[i]);
            dummyTemp.real = 1.0;
            dummyTemp.imag = 0.0;
            dummyTemp.flagComplex = 0; // If best-fit value is zero, use the unit we would use for unity instead.
            cptr = ppl_printUnit(c, &dummyTemp, &tmp1, &tmp2, 0, 1, 0);
            tmp3 = tmp1;
        }
        pplObjNum(&dummyTemp, 0, tmp1, tmp2);
        dummyTemp.refCount = 1;
        // Real variables, or complex ones
        if (c->set->term_current.ComplexNumbers == SW_ONOFF_OFF) {
            if (c->set->term_current.NumDisplay != SW_DISPLAY_L)
                ppl_snprintfTrunc(msg, sizeof(msg), "%16s = (%s +/- %s) %s", fitVars[i],
                                  ppl_unitsNumericDisplay(c, &dummyTemp, 1, 0, 0),
                                  ppl_numericDisplay(stdDev[i] * tmp3, c->numdispBuff[0],
                                                     c->set->term_current.SignificantFigures, 0),
                                  cptr);
            // LaTeX
            else {
                scratchPad[0] = '$';
                scratchPad[1] = '\0';
                sigmaPos = 1;
                // Escape underscores in the variable name for LaTeX
                for (k = 0; fitVars[i][k] != '\0'; k++) {
                    if (fitVars[i][k] == '_') ppl_strAppendChar(scratchPad, LSTR_LENGTH, &sigmaPos, '\\');
                    ppl_strAppendChar(scratchPad, LSTR_LENGTH, &sigmaPos, fitVars[i][k]);
                }
                ppl_snprintfTrunc(msg, sizeof(msg), "%17s = (%s", scratchPad,
                                  ppl_unitsNumericDisplay(c, &dummyTemp, 1, 0, 0) + 1);
                j = strlen(msg) - 1; // Remove final $
                ppl_snprintfTrunc(msg + j, sizeof(msg) - (j), "\\pm%s)%s$",
                                  ppl_numericDisplay(stdDev[i] * tmp3, c->numdispBuff[0],
                                                     c->set->term_current.SignificantFigures, 0),
                                  cptr);
            }
        } else {
            if (c->set->term_current.NumDisplay == SW_DISPLAY_T)
                ppl_snprintfTrunc(msg, sizeof(msg), "%16s = (%s +/- %s +/- %s*sqrt(-1))%s", fitVars[i],
                                  ppl_unitsNumericDisplay(c, &dummyTemp, 1, 0, 0),
                                  ppl_numericDisplay(stdDev[2 * i] * tmp3, c->numdispBuff[0],
                                                     c->set->term_current.SignificantFigures, 0),
                                  ppl_numericDisplay(stdDev[2 * i + 1] * tmp3, c->numdispBuff[2],
                                                     c->set->term_current.SignificantFigures, 0),
                                  cptr);
            // LaTeX
            else if (c->set->term_current.NumDisplay == SW_DISPLAY_L) {
                scratchPad[0] = '$';
                scratchPad[1] = '\0';
                sigmaPos = 1;
                // Escape underscores in the variable name for LaTeX
                for (k = 0; fitVars[i][k] != '\0'; k++) {
                    if (fitVars[i][k] == '_') ppl_strAppendChar(scratchPad, LSTR_LENGTH, &sigmaPos, '\\');
                    ppl_strAppendChar(scratchPad, LSTR_LENGTH, &sigmaPos, fitVars[i][k]);
                }
                ppl_snprintfTrunc(msg, sizeof(msg), "%17s = (%s", scratchPad,
                                  ppl_unitsNumericDisplay(c, &dummyTemp, 1, 0, 0) + 1);
                j = strlen(msg) - 1; // Remove final $
                ppl_snprintfTrunc(msg + j, sizeof(msg) - (j), "\\pm %s\\pm %si)%s$",
                                  ppl_numericDisplay(stdDev[2 * i] * tmp3, c->numdispBuff[0],
                                                     c->set->term_current.SignificantFigures, 0),
                                  ppl_numericDisplay(stdDev[2 * i + 1] * tmp3, c->numdispBuff[2],
                                                     c->set->term_current.SignificantFigures, 0),
                                  cptr);
            } else
                ppl_snprintfTrunc(msg, sizeof(msg), "%16s = (%s +/- %s +/- %si) %s", fitVars[i],
                                  ppl_unitsNumericDisplay(c, &dummyTemp, 1, 0, 0),
                                  ppl_numericDisplay(stdDev[2 * i] * tmp3, c->numdispBuff[0],
                                                     c->set->term_current.SignificantFigures, 0),
                                  ppl_numericDisplay(stdDev[2 * i + 1] * tmp3, c->numdispBuff[2],
                                                     c->set->term_current.SignificantFigures, 0),
                                  cptr);
        }
        ppl_report(&c->errcontext, msg);
    }

    // We're finished... can now free dataTable
    gsl_vector_free(bestFitParamVals);
    bestFitParamVals = NULL;
    gsl_matrix_free(hessian_inv);
    hessian_inv = NULL;
    gsl_matrix_free(hessian_lu);
    hessian_lu = NULL;
    gsl_matrix_free(hessian);
    hessian = NULL;
    gsl_permutation_free(perm);
    perm = NULL;
    ppl_memAlloc_AscendOutOfContext(contextLocalVec);

// Release the function, whose reference count was raised while it was being fitted
cleanup:
    __sync_sub_and_fetch(&funcDef->refCount, 1);
    return;
}
