// eqnsolve.c
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

// Implements the solve, minimise and maximise commands. Each varies a set of free variables using GSL's Nelder-Mead
// simplex minimiser: solve minimises the sum of the squared differences between the logarithms of the two sides of each
// equation, while minimise and maximise minimise the expression, or minus it. The minimiser is restarted several times,
// alternately working with the variables normalised and in a logarithmic space, until the result stops improving. With
// complex arithmetic on, the real and imaginary parts of each variable are varied separately.

#define _EQNSOLVE_C 1

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

#include <gsl/gsl_errno.h>
#include <gsl/gsl_multimin.h>

#include "commands/eqnsolve.h"
#include "coreUtils/errorReport.h"
#include "expressions/expCompile.h"
#include "expressions/expEval.h"
#include "expressions/traceback_fns.h"
#include "mathsTools/dcfmath.h"
#include "parser/cmdList.h"
#include "parser/parser.h"
#include "settings/settingTypes.h"
#include "stringTools/asciidouble.h"
#include "userspace/context.h"
#include "userspace/contextVarDef.h"
#include "userspace/garbageCollector.h"
#include "userspace/pplObj_fns.h"
#include "userspace/pplObjPrint.h"
#include "userspace/pplObjUnits.h"
#include "userspace/unitsDisp.h"
#include "userspace/unitsArithmetic.h"
#include "stringTools/strConstants.h"

//! ppl_optimise_RealToLog - Transform a free parameter into the coordinate system in which the minimiser searches.
//! On some iterations the parameter is normalised by its initial magnitude; on others it is mapped into a
//! logarithmic space, to help find solutions which are many orders of magnitude different from the initial guess.
//! \param [in] in - The value of the free parameter
//! \param [in] iter - The number of the current pass of the minimiser, which selects the transformation
//! \param [out] norm - Set to the normalisation used, on passes where the parameter is normalised
//! \return The transformed value of the free parameter

double ppl_optimise_RealToLog(const double in, const int iter, double *norm) {
    double a = -2;

    // On the first pass and from the fourth onwards, divide the parameter by its magnitude
    if ((iter == 1) || (iter >= 4)) {
        *norm = ppl_max(1e-200, fabs(in));
        return in / (*norm);
    }
    if (iter >= 3) a = -500;

    // Otherwise take its logarithm, reflecting values below exp(a) so that the mapping stays smooth and invertible for
    // zero and negative values
    if (in >= exp(a)) return log(in);
    else return 2 * a - log(2 * exp(a) - in);
}

//! ppl_optimise_LogToReal - Transform a free parameter from the coordinate system in which the minimiser searches
//! back into its true value. This is the inverse of ppl_optimise_RealToLog().
//! \param [in] in - The transformed value of the free parameter
//! \param [in] iter - The number of the current pass of the minimiser, which selects the transformation
//! \param [in] norm - The normalisation returned by ppl_optimise_RealToLog()
//! \return The true value of the free parameter

double ppl_optimise_LogToReal(const double in, const int iter, const double *norm) {
    double a = -2;

    // Undo the normalisation used on the first pass and from the fourth onwards
    if ((iter == 1) || (iter >= 4)) return in * (*norm);
    if (iter >= 3) a = -500;

    // Otherwise undo the logarithmic mapping, including its reflection below a
    if (in >= a) return exp(in);
    else return 2 * exp(a) - exp(2 * a - in);
}

// Structure used to communicate between top-level optimiser, and the
// evaluation slave called by GSL
typedef struct MMComm {
    ppl_context *context;
    char *inLine;
    int iterDepth;
    int expr1pos[EQNSOLVE_MAXDIMS];
    int expr2pos[EQNSOLVE_MAXDIMS];
    pplExpr *expr1[EQNSOLVE_MAXDIMS];
    pplExpr *expr2[EQNSOLVE_MAXDIMS];
    char *fitvarname[EQNSOLVE_MAXDIMS]; // Name of nth fit variable
    pplObj *fitvar[EQNSOLVE_MAXDIMS];
    double norm[EQNSOLVE_MAXDIMS];
    int Nfitvars, Nexprs, GoneNaN, iter2;
    double sign;
    pplObj first[EQNSOLVE_MAXDIMS];
    unsigned char isFirst[EQNSOLVE_MAXDIMS];
    double WorstScore;
    int warnPos, warnExprId, warnExprNo; // One final algebra error is allowed to produce a warning
    char warntext[LSTR_LENGTH];
} MMComm;

#define WORSTSCORE_INIT -(DBL_MAX / 1e10)

//! printParameterValues - Write a list of the current values of the free parameters, for use in error messages
//! \param [in] data - The state of the minimiser
//! \param [out] output - Character buffer into which to write the list
//! \return Pointer to <output>

static char *printParameterValues(MMComm *data, char *output) {
    // <output> must be at least LSTR_LENGTH bytes long
    int i = 0, j = 0;

    // Write each variable as name=value, in brackets if there are several
    output[0] = '\0';
    if (data->Nfitvars > 1) ppl_strAppendf(output, LSTR_LENGTH, &j, "( ");
    // Append each variable, followed by "; "
    for (i = 0; i < data->Nfitvars; i++) {
        ppl_strAppendf(output, LSTR_LENGTH, &j, "%s=", data->fitvarname[i]);
        pplObjPrint(data->context, data->fitvar[i], data->fitvarname[i], output + j, LSTR_LENGTH - j, 0, 1);
        j += strlen(output + j);
        ppl_strAppendf(output, LSTR_LENGTH, &j, "; ");
    }
    // Tidy up the end of the list
    if (j > 2) {
        // Replace the final "; " with a closing bracket
        j -= 2;
        output[j] = '\0';
        if (data->Nfitvars > 1) ppl_strAppendf(output, LSTR_LENGTH, &j, " )");
    } else ppl_snprintfTrunc(output, LSTR_LENGTH, "()");

    // Return the static buffer
    return output;
}

//! twinLog - A logarithm which is extended smoothly to values below 1e-200 (including zero and negative values), so
//! that it can be used to compare values on a logarithmic scale without failing
//! \param [in] x - The value
//! \return log(x) for x > 1e-200, or a reflection of the logarithm about x = 1e-200 otherwise

static double twinLog(const double x) {
    return (x > 1e-200) ? log(x) : (2 * log(1e-200) - log(2e-200 - x));
}

//! MultiMinSlave - Cost function passed to the GSL minimiser by the solve, minimise and maximise commands. It sets
//! the free parameters to the values requested, and evaluates the expressions being solved or optimised.
//! \param [in] x - The trial values of the free parameters, in the minimiser's coordinate system
//! \param [in] params - Pointer to the MMComm structure describing the problem
//! \return The value of the cost function, or NaN if the expressions could not be evaluated

static double MultiMinSlave(const gsl_vector *x, void *params) {
    // The function which GSL minimises: it sets the free variables to the position given by the minimiser, evaluates
    // the expressions, and returns a score
    char msg[LSTR_LENGTH];
    pplObj output1, output2;
    int lastOpAssign;
    unsigned char squareroot = 0;
    double accumulator = 0.0;
    MMComm *data = (MMComm *) params;
    ppl_context *c = data->context;

    if (c->errStat.status) return GSL_NAN; // We've previously had a serious error... so don't do any more work

    // Set the free variables from the minimiser's position vector; with complex arithmetic, each variable has a real
    // and an imaginary component
    if (c->set->term_current.ComplexNumbers == SW_ONOFF_OFF) {
        for (int i = 0; i < data->Nfitvars; i++)
            data->fitvar[i]->real = ppl_optimise_LogToReal(gsl_vector_get(x, i), data->iter2, &data->norm[i]);
    } else {
        // Each variable takes two elements of the vector
        for (int i = 0; i < data->Nfitvars; i++) {
            data->fitvar[i]->real = ppl_optimise_LogToReal(gsl_vector_get(x, 2 * i), data->iter2, &data->norm[2 * i]);
            data->fitvar[i]->imag =
                    ppl_optimise_LogToReal(gsl_vector_get(x, 2 * i + 1), data->iter2, &data->norm[2 * i + 1]);
            data->fitvar[i]->flagComplex = !ppl_dblEqual(data->fitvar[i]->imag, 0);
            if (!data->fitvar[i]->flagComplex)
                data->fitvar[i]->imag = 0.0; // Enforce that real numbers have positive zero imaginary components
        }
    }

    // Evaluate each expression, or each side of each equation
    for (int i = 0; i < data->Nexprs; i++) {
        pplObj *tmp = ppl_expEval(c, data->expr1[i], &lastOpAssign, 0, data->iterDepth + 1);

        // If a numerical error happened; ignore it for now, but return NAN
        if (c->errStat.status) {
            data->warnExprId = i;
            data->warnExprNo = 1;
            data->warnPos = c->errStat.errPosExpr;
            ppl_snprintfTrunc(data->warntext, LSTR_LENGTH, "An algebraic error was encountered at %s: %s",
                              printParameterValues(data, msg), c->errStat.errMsgExpr);
            ppl_tbClear(c);
            return GSL_NAN;
        }

        // Check that expression evaluates a number
        if ((tmp == NULL) || (tmp->objType != PPLOBJ_NUM)) {
            strcpy(c->errStat.errBuff, "Supplied expression does not evaluate to a numeric quantity.");
            ppl_tbAdd(c, data->expr1[i]->srcLineN, data->expr1[i]->srcId, data->expr1[i]->srcFname, 0, ERR_TYPE,
                      data->expr1pos[i], data->inLine, "");
            ppl_garbageObject(&c->stack[--c->stackPtr]); // trash and pop tmp from stack
            return GSL_NAN;
        }
        memcpy(&output1, tmp, sizeof(pplObj));
        ppl_garbageObject(&c->stack[--c->stackPtr]); // trash and pop tmp from stack

        // For an equation, evaluate the right-hand side and score how far the two sides are apart; for an expression to
        // minimise, the score is its value
        if (data->expr2[i] != NULL) {
            tmp = ppl_expEval(c, data->expr2[i], &lastOpAssign, 0, data->iterDepth + 1);

            // If a numerical error happened; ignore it for now, but return NAN
            if (c->errStat.status) {
                data->warnExprId = i;
                data->warnExprNo = 2;
                data->warnPos = c->errStat.errPosExpr;
                ppl_snprintfTrunc(data->warntext, LSTR_LENGTH, "An algebraic error was encountered at %s: %s",
                                  printParameterValues(data, msg), c->errStat.errMsgExpr);
                ppl_tbClear(c);
                return GSL_NAN;
            }

            // Check that expression evaluates a number
            if ((tmp == NULL) || (tmp->objType != PPLOBJ_NUM)) {
                strcpy(c->errStat.errBuff, "Supplied expression does not evaluate to a numeric quantity.");
                ppl_tbAdd(c, data->expr2[i]->srcLineN, data->expr2[i]->srcId, data->expr2[i]->srcFname, 0, ERR_TYPE,
                          data->expr2pos[i], data->inLine, "");
                ppl_garbageObject(&c->stack[--c->stackPtr]); // trash and pop tmp from stack
                return GSL_NAN;
            }
            memcpy(&output2, tmp, sizeof(pplObj));
            ppl_garbageObject(&c->stack[--c->stackPtr]); // trash and pop tmp from stack

            // The two sides must have the same units
            if (!ppl_unitsDimEqual(&output1, &output2)) {
                ppl_snprintfTrunc(
                        c->errStat.errBuff, sizeof(c->errStat.errBuff),
                        "The two sides of the equation which is being solved are not dimensionally compatible. The left side has dimensions of <%s> while the right side has dimensions of <%s>.",
                        ppl_printUnit(c, &output1, NULL, NULL, 0, 1, 0),
                        ppl_printUnit(c, &output2, NULL, NULL, 1, 1, 0));
                ppl_tbAdd(c, data->expr1[i]->srcLineN, data->expr1[i]->srcId, data->expr1[i]->srcFname, 0, ERR_UNIT,
                          data->expr1pos[i], data->inLine, "");
                return GSL_NAN;
            }

            // Add the squared difference between the logarithms of the two sides, for both the real and imaginary parts
            accumulator += pow(twinLog(output1.real) - twinLog(output2.real),
                               2); // Minimise sum of square deviations of many equations
            accumulator += pow(twinLog(output1.imag) - twinLog(output2.imag), 2);
            squareroot = 1;
        } else {
            // Remember the units of the expression the first time, and check they stay the same afterwards
            if (data->isFirst[i]) {
                memcpy(&data->first[i], &output1, sizeof(pplObj));
                data->isFirst[i] = 0;
            } else {
                // The units of the expression must not change as the variables are varied
                if (!ppl_unitsDimEqual(&data->first[i], &output1)) {
                    ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                                      "The function being minimised or maximised does not have consistent units.");
                    ppl_tbAdd(c, data->expr1[i]->srcLineN, data->expr1[i]->srcId, data->expr1[i]->srcFname, 0, ERR_UNIT,
                              data->expr1pos[i], data->inLine, "");
                    return GSL_NAN;
                }
            }
            accumulator += output1.real; // ... or just the raw values in minimise and maximise commands
        }
    }
    if (squareroot) accumulator = sqrt(accumulator);
    double output = accumulator * data->sign;
    // If the score is not finite, return a score which is worse than any seen so far, which steers the minimiser away
    if ((!gsl_finite(output)) && (data->WorstScore > WORSTSCORE_INIT)) {
        data->WorstScore *= 1.1;
        return data->WorstScore;
    }
    // If there is no score to compare with, note that the minimiser has failed
    if (!gsl_finite(output)) {
        data->GoneNaN = 1;
    } else {
        if (data->WorstScore < output) data->WorstScore = output;
    }
    return output;
}

//! multiMinIterate - Run the GSL simplex minimiser repeatedly, in a sequence of coordinate systems, until the
//! solution converges
//! \param [in|out] commlink - The MMComm structure describing the problem; the solution is stored in it
//! \param [in] pl - The compiled command line, used to report errors

static void multiMinIterate(MMComm *commlink, const parserLine *pl) {
    // Variables for driving the GSL minimiser
    ppl_context *c = commlink->context;
    size_t iter = 0, iter2 = 0;
    int i, status = 0;
    double size = 0, sizelast = 0, sizelast2 = 0;
    const gsl_multimin_fminimizer_type *T =
            gsl_multimin_fminimizer_nmsimplex; // We don't use nmsimplex2 here because it was new in gsl 1.12
    gsl_multimin_fminimizer *s = NULL;
    gsl_multimin_function fn;

    // With complex arithmetic, each variable has two components to vary
    int Nparams = commlink->Nfitvars * ((c->set->term_current.ComplexNumbers == SW_ONOFF_OFF) ? 1 : 2);

    // Describe the function to minimise to GSL
    fn.n = Nparams;
    fn.f = &MultiMinSlave;
    fn.params = (void *) commlink;

    // The position of the minimiser, and its initial step sizes
    gsl_vector *x = gsl_vector_alloc(Nparams);
    gsl_vector *ss = gsl_vector_alloc(Nparams);

    // Restart the minimiser several times, transforming the variables differently on each pass, until the result stops
    // improving
    iter2 = 0;
    // Each pass of the minimiser
    do {
        iter2++;
        commlink->iter2 = iter2;
        sizelast2 = size;

        // Transfer starting guess values from fitting variables into gsl_vector x
        if (c->set->term_current.ComplexNumbers == SW_ONOFF_OFF) {
            for (i = 0; i < commlink->Nfitvars; i++)
                gsl_vector_set(x, i, ppl_optimise_RealToLog(commlink->fitvar[i]->real, iter2, &commlink->norm[i]));
        } else {
            // Each variable takes two elements of the vector
            for (i = 0; i < commlink->Nfitvars; i++) {
                gsl_vector_set(x, 2 * i,
                               ppl_optimise_RealToLog(commlink->fitvar[i]->real, iter2, &commlink->norm[2 * i]));
                gsl_vector_set(x, 2 * i + 1,
                               ppl_optimise_RealToLog(commlink->fitvar[i]->imag, iter2, &commlink->norm[2 * i + 1]));
            }
        }

        // The initial step size is 5% of each parameter
        for (i = 0; i < Nparams; i++) {
            if (fabs(gsl_vector_get(x, i)) > 1e-6) gsl_vector_set(ss, i, 0.05 * gsl_vector_get(x, i));
            else gsl_vector_set(ss, i, 0.05); // Avoid having a stepsize of zero
        }

        // Create the minimiser, starting from the current values of the variables
        s = gsl_multimin_fminimizer_alloc(T, fn.n);
        gsl_multimin_fminimizer_set(s, &fn, x, ss);

        // If initial value we are giving the minimiser produces an algebraic error, it's not worth continuing
        MultiMinSlave(x, (void *) commlink);
        if (c->errStat.status) goto cleanup;
        // Report a numerical error in the expression, pointing to where it occurred
        if (commlink->warnPos >= 0) {
            const int i = commlink->warnExprId;
            const int j = commlink->warnExprNo;
            ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "%s", commlink->warntext);
            if (j == 1)
                ppl_tbAdd(c, commlink->expr1[i]->srcLineN, commlink->expr1[i]->srcId, commlink->expr1[i]->srcFname, 1,
                          ERR_NUMERICAL, commlink->expr1pos[i] + commlink->warnPos, commlink->inLine, "");
            else
                ppl_tbAdd(c, commlink->expr2[i]->srcLineN, commlink->expr2[i]->srcId, commlink->expr2[i]->srcFname, 1,
                          ERR_NUMERICAL, commlink->expr2pos[i] + commlink->warnPos, commlink->inLine, "");
            goto cleanup;
        }

        // Iterate the minimiser until the simplex stops shrinking
        iter = 0;
        commlink->GoneNaN = 0;
        commlink->WorstScore = WORSTSCORE_INIT;
        // Each batch of iterations
        do {
            iter++;
            // When you're minimising over many parameters simultaneously sometimes nothing happens for a long time
            for (i = 0; i < 2 + Nparams * 2; i++) {
                status = gsl_multimin_fminimizer_iterate(s);
                if (status) break;
            }
            if (status || c->errStat.status) break;
            sizelast = size;
            size = gsl_multimin_fminimizer_minimum(s);
        } while ((iter < 10) ||
                 ((size < sizelast) &&
                  (iter < 50))); // Iterate 10 times, and then see whether size carries on getting smaller

        // Transfer best-fit values from s->x into fitting variables
        if (c->set->term_current.ComplexNumbers == SW_ONOFF_OFF) {
            for (i = 0; i < commlink->Nfitvars; i++)
                commlink->fitvar[i]->real = ppl_optimise_LogToReal(gsl_vector_get(s->x, i), iter2, &commlink->norm[i]);
        } else {
            // Each variable takes two elements of the vector
            for (i = 0; i < commlink->Nfitvars; i++) {
                commlink->fitvar[i]->real =
                        ppl_optimise_LogToReal(gsl_vector_get(s->x, 2 * i), iter2, &commlink->norm[2 * i]);
                commlink->fitvar[i]->imag =
                        ppl_optimise_LogToReal(gsl_vector_get(s->x, 2 * i + 1), iter2, &commlink->norm[2 * i + 1]);
            }
        }

        // Free the minimiser before the next pass
        gsl_multimin_fminimizer_free(s);
        s = NULL;
    } while ((iter2 < 4) ||
             ((commlink->GoneNaN == 0) && (!status) && (size < sizelast2) &&
              (iter2 < 20))); // Iterate 2 times, and then see whether size carries on getting smaller

    // Too many passes means the minimiser failed to converge
    if (iter2 >= 20) status = 1;
    // Report a failure to converge
    if (status) {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "Failed to converge. GSL returned error: %s",
                          gsl_strerror(status));
        ppl_tbAdd(c, pl->srcLineN, pl->srcId, pl->srcFname, 0, ERR_NUMERICAL, 0, pl->linetxt, "");
    }

// Free the minimiser, if a pass was abandoned, and the vectors
cleanup:
    if (s != NULL) gsl_multimin_fminimizer_free(s);
    s = NULL;
    gsl_vector_free(x);
    x = NULL;
    gsl_vector_free(ss);
    ss = NULL;
    return;
}

//! minOrMax - Implement the minimise and maximise commands, which vary free parameters to find the minimum or
//! maximum of an expression
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion
//! \param [in] sign - The factor by which to multiply the expression before minimising it: 1 to minimise it, or -1
//! to maximise it

static void minOrMax(ppl_context *c, parserLine *pl, const parserOutput *in, const int interactive, const int iterDepth,
                     const double sign) {
    pplObj *stk = in->stk;
    MMComm commlink;
    pplObj dummyTemp;

    // Set up the data passed to the minimiser; there is one expression to minimise or maximise
    commlink.context = c;
    commlink.inLine = pl->linetxt;
    commlink.iterDepth = iterDepth;
    commlink.Nfitvars = 0;
    commlink.Nexprs = 1;

    // Fetch the names of the variables which we are fitting
    pplObj *pos = &stk[PARSE_minimise_fit_variables];
    // Walk the linked list of the free variables on the parser stack
    while ((pos->objType == PPLOBJ_NUM) && (pos->real > 0)) {
        pos = &stk[(int) round(pos->real)];
        char *varName = (char *) (pos + PARSE_minimise_fit_variable_fit_variables)->auxil;
        ppl_contextGetVarPointer(c, varName, &commlink.fitvar[commlink.Nfitvars], &dummyTemp);
        // If the variable already had a numerical value, use it, with its units, as the starting guess
        if (dummyTemp.objType == PPLOBJ_NUM) {
            ppl_unitsDimCpy(commlink.fitvar[commlink.Nfitvars], &dummyTemp);
            commlink.fitvar[commlink.Nfitvars]->real = dummyTemp.real;
            commlink.fitvar[commlink.Nfitvars]->imag = dummyTemp.imag;
            commlink.fitvar[commlink.Nfitvars]->flagComplex = dummyTemp.flagComplex;
        }
        dummyTemp.amMalloced = 0;
        ppl_garbageObject(&dummyTemp);
        commlink.fitvarname[commlink.Nfitvars] = varName;
        commlink.Nfitvars += 1;
        // There is a fixed limit on the number of free variables
        if (commlink.Nfitvars >= EQNSOLVE_MAXDIMS) {
            ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                              "Too many via variables; the maximum allowed number is %d.", EQNSOLVE_MAXDIMS);
            ppl_tbAdd(c, pl->srcLineN, pl->srcId, pl->srcFname, 0, ERR_OVERFLOW, 0, pl->linetxt, "");
            return;
        }
    }

    // The expression to minimise or maximise
    commlink.expr1[0] = (pplExpr *) stk[PARSE_minimise_expression].auxil;
    commlink.expr1pos[0] = in->stkCharPos[PARSE_minimise_expression];
    commlink.expr2[0] = NULL;
    commlink.sign = sign;
    commlink.isFirst[0] = 1;
    commlink.warnPos = -1;
    commlink.GoneNaN = 0;
    commlink.WorstScore = WORSTSCORE_INIT;
    pplObjNum(&commlink.first[0], 0, 0, 0);

    // Run the minimiser
    multiMinIterate(&commlink, pl);

    // If the minimiser failed, set all the free variables to NaN
    if ((c->errStat.status) || (commlink.GoneNaN == 1)) {
        // Set each variable to NaN
        for (int i = 0; i < commlink.Nfitvars; i++) {
            commlink.fitvar[i]->real = GSL_NAN;
            commlink.fitvar[i]->imag = 0;
            commlink.fitvar[i]->flagComplex = 0;
        } // We didn't produce a sensible answer
        return;
    }

    // Pass on any warning
    if (commlink.warnPos >= 0) ppl_warning(&c->errcontext, ERR_NUMERICAL, commlink.warntext);
    return;
}

//! ppl_directive_solve - Implement the solve command, which varies free parameters to solve a set of simultaneous
//! equations
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppl_directive_solve(ppl_context *c, parserLine *pl, const parserOutput *in, const int interactive,
                         const int iterDepth) {
    pplObj *stk = in->stk;
    MMComm commlink;
    pplObj dummyTemp;

    // Set up the data passed to the minimiser
    commlink.context = c;
    commlink.inLine = pl->linetxt;
    commlink.iterDepth = iterDepth;
    commlink.Nfitvars = 0;
    commlink.Nexprs = 0;

    // Fetch the names of the variables which we are fitting
    int pos = PARSE_solve_fit_variables;
    // Walk the linked list of the free variables on the parser stack
    while ((stk[pos].objType == PPLOBJ_NUM) && (stk[pos].real > 0)) {
        pos = (int) round(stk[pos].real);
        char *varName = (char *) stk[pos + PARSE_solve_fit_variable_fit_variables].auxil;
        ppl_contextGetVarPointer(c, varName, &commlink.fitvar[commlink.Nfitvars], &dummyTemp);
        // If the variable already had a numerical value, use it, with its units, as the starting guess
        if (dummyTemp.objType == PPLOBJ_NUM) {
            ppl_unitsDimCpy(commlink.fitvar[commlink.Nfitvars], &dummyTemp);
            commlink.fitvar[commlink.Nfitvars]->real = dummyTemp.real;
            commlink.fitvar[commlink.Nfitvars]->imag = dummyTemp.imag;
            commlink.fitvar[commlink.Nfitvars]->flagComplex = dummyTemp.flagComplex;
        }
        dummyTemp.amMalloced = 0;
        ppl_garbageObject(&dummyTemp);
        commlink.fitvarname[commlink.Nfitvars] = varName;
        commlink.Nfitvars += 1;
        // There is a fixed limit on the number of free variables
        if (commlink.Nfitvars >= EQNSOLVE_MAXDIMS) {
            ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                              "Too many via variables; the maximum allowed number is %d.", EQNSOLVE_MAXDIMS);
            ppl_tbAdd(c, pl->srcLineN, pl->srcId, pl->srcFname, 0, ERR_OVERFLOW,
                      in->stkCharPos[pos + PARSE_solve_fit_variable_fit_variables], pl->linetxt, "");
            return;
        }
    }


    // Fetch a list of the equations to solve
    pos = PARSE_solve_expressions;
    // Walk the linked list of the equations on the parser stack, storing the expressions on each side
    while ((stk[pos].objType == PPLOBJ_NUM) && (stk[pos].real > 0)) {
        pos = (int) round(stk[pos].real);
        commlink.expr1[commlink.Nexprs] = (pplExpr *) stk[pos + PARSE_solve_left_expression_expressions].auxil;
        commlink.expr1pos[commlink.Nexprs] = in->stkCharPos[pos + PARSE_solve_left_expression_expressions];
        commlink.expr2[commlink.Nexprs] = (pplExpr *) stk[pos + PARSE_solve_right_expression_expressions].auxil;
        commlink.expr2pos[commlink.Nexprs] = in->stkCharPos[pos + PARSE_solve_right_expression_expressions];
        commlink.Nexprs += 1;
        // There is a fixed limit on the number of equations
        if (commlink.Nexprs >= EQNSOLVE_MAXDIMS) {
            ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                              "Too many simultaneous equations to solve; the maximum allowed number is %d.",
                              EQNSOLVE_MAXDIMS);
            ppl_tbAdd(c, pl->srcLineN, pl->srcId, pl->srcFname, 0, ERR_OVERFLOW,
                      in->stkCharPos[pos + PARSE_solve_left_expression_expressions], pl->linetxt, "");
            return;
        }
    }

    // There must be at least one equation
    if (commlink.Nexprs < 1) {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "No equations supplied to solve.");
        ppl_tbAdd(c, pl->srcLineN, pl->srcId, pl->srcFname, 0, ERR_NUMERICAL, 0, pl->linetxt, "");
        return;
    }

    // Minimise the mismatch between the two sides of the equations
    commlink.sign = 1.0;
    commlink.warnPos = -1;
    commlink.GoneNaN = 0;
    commlink.WorstScore = WORSTSCORE_INIT;
    // Each equation has not yet been evaluated
    for (int i = 0; i < commlink.Nexprs; i++) {
        commlink.isFirst[i] = 1;
        pplObjNum(&commlink.first[i], 0, 0, 0);
    }

    // Run the minimiser
    multiMinIterate(&commlink, pl);

    // If the minimiser failed, set all the free variables to NaN
    if ((c->errStat.status) || (commlink.GoneNaN == 1)) {
        // Set each variable to NaN
        for (int i = 0; i < commlink.Nfitvars; i++) {
            commlink.fitvar[i]->real = GSL_NAN;
            commlink.fitvar[i]->imag = 0;
            commlink.fitvar[i]->flagComplex = 0;
        } // We didn't produce a sensible answer
        return;
    }

    // Pass on any warning
    if (commlink.warnPos >= 0) ppl_warning(&c->errcontext, ERR_NUMERICAL, commlink.warntext);
    return;
}

//! ppl_directive_minimise - Implement the minimise command, which varies free parameters to find the minimum of an
//! expression
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppl_directive_minimise(ppl_context *c, parserLine *pl, parserOutput *in, const int interactive,
                            const int iterDepth) {
    minOrMax(c, pl, in, interactive, iterDepth, 1.0);
}

//! ppl_directive_maximise - Implement the maximise command, which varies free parameters to find the maximum of an
//! expression
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppl_directive_maximise(ppl_context *c, parserLine *pl, parserOutput *in, const int interactive,
                            const int iterDepth) {
    minOrMax(c, pl, in, interactive, iterDepth, -1.0);
}
