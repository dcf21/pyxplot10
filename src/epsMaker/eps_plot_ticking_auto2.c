// eps_plot_ticking_auto2.c
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

// One of the algorithms for placing ticks automatically along an axis, chosen by eps_plot_ticking.c. Method 2, which
// was Pyxplot 0.7's algorithm, generates candidate tick schemes at round multiples of powers of ten (or, on logarithmic
// axes, at mantissas such as 1, 2 and 5 times powers of the base), and picks the one which places the most ticks
// without exceeding the number which fit on the axis. Minor ticks must coincide with every major tick. If it fails,
// ticks are simply spaced evenly along the axis.

// METHOD 2: Pyxplot 0.7 automatic ticking.

#define _PPL_EPS_PLOT_TICKING_AUTO2_C 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <gsl/gsl_math.h>

#include "coreUtils/errorReport.h"
#include "coreUtils/memAlloc.h"
#include "mathsTools/dcfmath.h"
#include "stringTools/asciidouble.h"
#include "settings/axes_fns.h"
#include "settings/settings.h"
#include "settings/settingTypes.h"
#include "userspace/context.h"
#include "userspace/unitsDisp.h"

#include "epsMaker/eps_plot_canvas.h"
#include "epsMaker/eps_plot_ticking.h"
#include "epsMaker/eps_plot_ticking_auto.h"
#include "epsMaker/eps_plot_ticking_auto2.h"

#define MAX_FACTORS     32 /* Maximum number of factors of LogBase which we consider */
#define FACTOR_MULTIPLY 2.0 /* Factorise LogBase**2, so that 0.00,0.25,0.50,0.75,1.00 is a valid factorisation */
#define TICKS_MAXIMUM   256 /* Maximum number of ticks allowed along any given axis */

typedef struct TickScheme {
    int mantissa[TICKS_MAXIMUM];
    int Nmantissas;
    double ticksep;
    double offset;
} TickScheme;

//! TickScheme2_sprintf - Write a list of tick positions, for debugging
//! \param [out] out - Character buffer into which to write the list
//! \param [in] OutLen - The size of the buffer <out>
//! \param [in] UnitMultiplier - The factor by which values on the axis are multiplied to express them in the units
//! shown on the axis
//! \param [in] TL - The list of tick positions
//! \param [in] TL_len - The number of tick positions in <TL>

static void TickScheme2_sprintf(char *out, const int OutLen, const double UnitMultiplier, const double *TL,
                                const double TL_len) {
    int i = 0, j = 0, k = 0;
    out[j++] = '(';
    // Write each tick, separated by commas, stopping if the buffer is full
    for (i = 0; ((i < TL_len) && (j < OutLen - 8)); i++) {
        if (k != 0) out[j++] = ',';
        snprintf(out + j, OutLen - j - 8, "%g", TL[i] / UnitMultiplier);
        out[OutLen - 7] = '\0';
        j += strlen(out + j);
        k++;
    }
    out[j++] = ')';
    out[j++] = '.';
    out[j++] = '\0';
    return;
}

//! GenerateLogTickSchemes - Generate the possible schemes for placing ticks on a logarithmic axis, e.g. every decade,
//! or at 1, 2 and 5 times each power of ten
//! \param [out] TickSchemes - Array into which to write the tick schemes
//! \param [in|out] NTickSchemes - The number of tick schemes, which is increased by the number generated
//! \param [in] LogBase - The base of the logarithmic axis

static void GenerateLogTickSchemes(TickScheme *TickSchemes, int *NTickSchemes, const int LogBase) {
    // Make schemes which divide each power of the base into 1, 2, 3, ... parts, until rounding makes two mantissas
    // equal
    unsigned char stopping;
    int i, divisor;

    // Try each number of divisions
    for (stopping = 0, divisor = 1; (!stopping); divisor++) {
        if (*NTickSchemes > TICKS_MAXIMUM) return;
        // Compute each mantissa, rounded to an integer
        for (i = 0; i < divisor; i++) {
            TickSchemes[*NTickSchemes].mantissa[i] = floor(pow(LogBase, ((double) i) / divisor) + 0.5);
            if ((i > 0) && (TickSchemes[*NTickSchemes].mantissa[i - 1] == TickSchemes[*NTickSchemes].mantissa[i]))
                stopping = 1;
        }
        // Accept the scheme if all its mantissas differ
        if (!stopping) {
            TickSchemes[*NTickSchemes].Nmantissas = divisor;
            TickSchemes[*NTickSchemes].ticksep = 1.0;
            TickSchemes[*NTickSchemes].offset = 0.0;
            (*NTickSchemes)++;
        }
    }
    // Add a scheme with ticks at every integer multiple of each power of the base
    if (*NTickSchemes < TICKS_MAXIMUM) {
        // The mantissas are 1, 2, ..., LogBase-1
        for (i = 0; ((i < LogBase - 1) && (i < TICKS_MAXIMUM)); i++) TickSchemes[*NTickSchemes].mantissa[i] = i + 1;
        TickSchemes[*NTickSchemes].Nmantissas = i;
        TickSchemes[*NTickSchemes].ticksep = 1.0;
        TickSchemes[*NTickSchemes].offset = 0.0;
        (*NTickSchemes)++;
    }
    return;
}

//! GenerateTickSchemes - Generate the possible schemes for placing ticks on an axis, dividing each order of magnitude
//! by each of the factors of ten
//! \param [out] TickSchemes - Array into which to write the tick schemes
//! \param [out] NTickSchemes - Set to the number of tick schemes generated
//! \param [in] OoM - The order of magnitude of the range of the axis
//! \param [in] IsLog - Flag indicating whether the axis is logarithmic
//! \param [in] LogBase - The base of the logarithmic axis
//! \param [in] FactorsTen - Array of the factors of ten
//! \param [in] NFactors - The number of entries in <FactorsTen>

static void GenerateTickSchemes(TickScheme *TickSchemes, int *NTickSchemes, const double OoM, const unsigned char IsLog,
                                const int LogBase, const int *FactorsTen, const int NFactors) {
    int LevelDescend = 1;
    *NTickSchemes = 0;
    while (pow(10.0, LevelDescend - 1) < (10.0 * TICKS_MAXIMUM)) {
        const double OoMscan = OoM / pow(10.0, LevelDescend);
        // On logarithmic axes, at the scale of one power of the base, use the special schemes; otherwise use steps
        // which are factors of a power of ten
        if (IsLog && (OoMscan > 0.09) && (OoMscan < 0.11)) {
            GenerateLogTickSchemes(TickSchemes, NTickSchemes, LogBase);
        } else {
            // Make a scheme for each factor
            for (int i = NFactors - 1; i >= 0; i--) {
                double t;
                TickSchemes[*NTickSchemes].mantissa[0] = 1;
                TickSchemes[*NTickSchemes].Nmantissas = 1;
                TickSchemes[*NTickSchemes].ticksep = t = FactorsTen[i] * OoMscan / pow(10.0, FACTOR_MULTIPLY - 1);
                TickSchemes[*NTickSchemes].offset = 0.0;
                if ((!IsLog) || (ppl_dblEqual(t, floor(t + 0.5))))
                    (*NTickSchemes)++; // Fractional steps (i.e. sqrts) not allowed
            }
        }
        LevelDescend++;
    }
    return;
}

//! eps_plot_ticking_auto2 - Place ticks automatically on an axis without a format expression, trying each possible tick
//! scheme and choosing the one which gives the closest to the ideal number of ticks
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] axis - The axis
//! \param [in] UnitMultiplier - The factor by which values on the axis are multiplied to express them in the units
//! shown on the axis
//! \param [in] AutoTicks - Array of two flags indicating whether major and minor ticks are to be placed automatically
//! \param [in] linkedto - The axis this axis is linked to, or NULL

void eps_plot_ticking_auto2(EPSComm *x, pplset_axis *axis, const double UnitMultiplier, const unsigned char *AutoTicks,
                            const pplset_axis *linkedto) {
    char msg[PPL_MSG_LENGTH];
    int i, j, ContextRough = -1, NFactorsTen;
    unsigned char IsLog = (axis->LogFinal == SW_BOOL_TRUE);
    int FactorsTen[MAX_FACTORS];
    double OoM, outer_min, outer_max;
    double *TLP, *TL_best = NULL, *TL_trial = NULL;
    char **TLS;
    int NTickSchemes;
    TickScheme *TickSchemes = NULL;

    // Log which algorithm is in use
    if (DEBUG) ppl_log(&x->c->errcontext, "Using eps_plot_ticking_auto2()");

    // Make temporary rough workspace
    const int OutContext = ppl_memAlloc_GetMemContext();
    ContextRough = ppl_memAlloc_DescendIntoNewContext();

    // This ticking scheme does not work on axes with non-linear mappings
    if (axis->AxisLinearInterpolation != NULL) goto FAIL;

    // Work out factors of log base of axis. In fact, work out factors of log base ** 2, so that ten divides by four.
    const int LogBase = IsLog ? axis->tics.logBase : 10;
    eps_plot_ticking_factorise(pow(10.0, FACTOR_MULTIPLY), FactorsTen, MAX_FACTORS, axis->PhysicalLengthMinor,
                               &NFactorsTen);

    // Work out order of magnitude of axis range
    double axis_min = axis->MinFinal * UnitMultiplier;
    double axis_max = axis->MaxFinal * UnitMultiplier;
    // Make sure the range is the right way around, and not of zero width
    if (axis_min > axis_max) {
        const double temp = axis_min;
        axis_min = axis_max;
        axis_max = temp;
    } else if (axis_min == axis_max) {
        goto FAIL;
    }
    if (IsLog && (axis_max < 3 * axis_min)) IsLog = 0;
    // Find the order of magnitude of the axis's range, and round its ends outwards to it, working with logarithms on
    // logarithmic axes
    if (!IsLog) {
        OoM = pow(10.0, ceil(log10(axis_max - axis_min)));
        outer_min = floor(axis_min / OoM) * OoM;
        outer_max = ceil(axis_max / OoM) * OoM;
    } else {
        const double axis_min_l = log(axis_min) / log(LogBase);
        const double axis_max_l = log(axis_max) / log(LogBase);
        OoM = pow(10.0, ceil(log10(axis_max_l - axis_min_l)));
        outer_min = floor(axis_min_l / OoM) * OoM;
        outer_max = ceil(axis_max_l / OoM) * OoM;
    }

    // Malloc list of accepted ticks
    if (AutoTicks[1]) {
        axis->TickListPositions = (double *) ppl_memAlloc_incontext((TICKS_MAXIMUM + 1) * sizeof(double), OutContext);
        axis->TickListStrings = (char **) ppl_memAlloc_incontext((TICKS_MAXIMUM + 1) * sizeof(char *), OutContext);
    }
    // Allocate the lists of minor ticks
    if (AutoTicks[0]) {
        axis->MTickListPositions = (double *) ppl_memAlloc_incontext((TICKS_MAXIMUM + 1) * sizeof(double), OutContext);
        axis->MTickListStrings = (char **) ppl_memAlloc_incontext((TICKS_MAXIMUM + 1) * sizeof(char *), OutContext);
    }
    if (((AutoTicks[1]) && ((axis->TickListPositions == NULL) || (axis->TickListStrings == NULL))) ||
        ((AutoTicks[0]) && ((axis->MTickListPositions == NULL) || (axis->MTickListStrings == NULL))))
        goto FAIL;

    // Malloc temporary lists of ticks
    TL_best = (double *) malloc((TICKS_MAXIMUM + 1) * sizeof(double));
    TL_trial = (double *) malloc((TICKS_MAXIMUM + 1) * sizeof(double));
    if ((TL_best == NULL) || (TL_trial == NULL)) goto FAIL;
    int TL_best_len = 0;

    // Malloc list of tick schemes
    TickSchemes = (TickScheme *) malloc((TICKS_MAXIMUM + 1) * sizeof(TickScheme));
    if (TickSchemes == NULL) goto FAIL;

    // Deal with MAJOR ticks. Then MINOR ticks.
    for (int major = 1; major >= 0; major--)
        // Place the major ticks, then the minor ticks
        if (AutoTicks[major]) {
            // Fill the list of major ticks, or minor ticks
            if (major) {
                TLP = axis->TickListPositions;
                TLS = axis->TickListStrings;
            } else {
                TLP = axis->MTickListPositions;
                TLS = axis->MTickListStrings;
            }

            // How many ticks can we fit onto this axis?
            int number_ticks = (major ? axis->PhysicalLengthMajor : axis->PhysicalLengthMinor) + 1;
            if (number_ticks < 2) number_ticks = 2; // Minimum of two ticks along any given axis
            if (number_ticks > TICKS_MAXIMUM)
                number_ticks = TICKS_MAXIMUM; // Maximum number of ticks along any given axis

            // Generate list of potential tick schemes
            GenerateTickSchemes(TickSchemes, &NTickSchemes, OoM, IsLog, LogBase, FactorsTen, NFactorsTen);

            // Try each tick scheme in turn
            for (i = 0; i < NTickSchemes; i++) {
                const double ts_min = outer_min + TickSchemes[i].offset;
                int TL_trial_len = 0;
                // Generate the ticks of this scheme which lie within the axis's range
                for (j = 0; j < ((outer_max - outer_min) / TickSchemes[i].ticksep + 1.5); j++) {
                    if (TL_trial_len >= TICKS_MAXIMUM) break;
                    // Each mantissa within each step
                    for (int k = 0; k < TickSchemes[i].Nmantissas; k++) {
                        if (TL_trial_len >= TICKS_MAXIMUM) break;
                        double x = ts_min + j * TickSchemes[i].ticksep;
                        if (fabs(x) < 1e-6 * TickSchemes[i].ticksep) x = 0;
                        if (IsLog) x = pow(LogBase, x);
                        x *= TickSchemes[i].mantissa[k];
                        if ((x < axis_min) || (x > axis_max)) continue;
                        TL_trial[TL_trial_len] = x;
                        TL_trial_len++;
                    }
                }

                // If minor ticks, make sure that this tick scheme overlays major ticks
                int OverlayMatch = 1;
                // Minor tick schemes must have ticks at every major tick, which are then removed from the list of minor
                // ticks
                if (!major) {
                    // Log the scheme in debugging mode
                    if (DEBUG) {
                        ppl_snprintfTrunc(msg, sizeof(msg), "Trying minor tick scheme: ");
                        const int tmp = strlen(msg);
                        TickScheme2_sprintf(msg + tmp, sizeof(msg) - tmp, UnitMultiplier, TL_trial, TL_trial_len);
                        ppl_log(&x->c->errcontext, msg);
                    }
                    // Check each major tick
                    for (j = 0; axis->TickListStrings[j] != NULL; j++) {
                        OverlayMatch = 0;
                        // Find a minor tick at the same position, and remove it
                        for (int k = 0; k < TL_trial_len; k++) {
                            const double pos = pplaxis_GetPosition(TL_trial[k] / UnitMultiplier, axis, 0, 0);
                            if (fabs(pos - axis->TickListPositions[j]) > 1e-4) continue;
                            for (k++; k < TL_trial_len; k++) TL_trial[k - 1] = TL_trial[k];
                            TL_trial_len--;
                            OverlayMatch = 1;
                            break;
                        }
                        if (!OverlayMatch) break;
                    }
                    // Log the failure in debugging mode
                    if (DEBUG && !OverlayMatch) {
                        ppl_snprintfTrunc(msg, sizeof(msg), "FAIL: Did not overlay major tick at %g (axis pos %.3f)",
                                          pplaxis_InvGetPosition(axis->TickListPositions[j], axis),
                                          axis->TickListPositions[j]);
                        ppl_log(&x->c->errcontext, msg);
                    }
                    if (!OverlayMatch) continue;
                    if (DEBUG) ppl_log(&x->c->errcontext, "PASS");
                }

                // See whether this tick scheme is better than previous
                if ((IsLog) && (TickSchemes[i].Nmantissas >= LogBase - 1) && (!major)) {
                    // Too many ticks; since schemes are tried from coarsest to finest, stop looking
                    if (TL_trial_len > 3 * number_ticks) {
                        // Log the rejection in debugging mode
                        if (DEBUG) {
                            ppl_snprintfTrunc(
                                    msg, sizeof(msg),
                                    "Minor tick scheme with %d mantissas rejected because it has too many ticks (%d when we wanted ** %d)",
                                    TickSchemes[i].Nmantissas, TL_trial_len, number_ticks);
                            ppl_log(&x->c->errcontext, msg);
                        }
                        break;
                    }
                    if (DEBUG)
                        ppl_log(&x->c->errcontext, "Accepted minor tick scheme which marks every mantissa value");
                    TL_best_len = TL_trial_len;
                    for (j = 0; j < TL_trial_len; j++) TL_best[j] = TL_trial[j];
                } else if ((IsLog) && (TickSchemes[i].Nmantissas > 1) && (TL_trial_len > number_ticks)) {
                    // Log the rejection in debugging mode
                    if (DEBUG) {
                        ppl_snprintfTrunc(
                                msg, sizeof(msg),
                                "Tick scheme with %d mantissas rejected because it has too many ticks (%d when we wanted %d)",
                                TickSchemes[i].Nmantissas, TL_trial_len, number_ticks);
                        ppl_log(&x->c->errcontext, msg);
                    }
                    continue;
                } else if (TL_trial_len > number_ticks) break;
                // Keep the scheme if it has more ticks than the best so far
                else if (TL_trial_len > TL_best_len) {
                    for (j = 0; j < TL_trial_len; j++) TL_best[j] = TL_trial[j];
                    TL_best_len = TL_trial_len;
                }
            }

            // Commit list of ticks
            for (i = j = 0; i < TL_best_len; i++) {
                const double xtmp = TL_best[i];
                TLP[j] = pplaxis_GetPosition(xtmp / UnitMultiplier, axis, 0, 0);
                if (!gsl_finite(TLP[j])) continue;
                // Label major ticks, either automatically or with the axis's format; minor ticks have no labels
                if (major) {
                    if (axis->format == NULL) TickLabelAutoGen(x, &TLS[j], xtmp, LogBase, OutContext);
                    else
                        TickLabelFromFormat(x, &TLS[j], axis->format, xtmp / UnitMultiplier, &axis->DataUnit, axis->xyz,
                                            OutContext);
                } else {
                    TLS[j] = "";
                }
                j++;
            }
            TLS[j] = NULL;
        }

    // Finished
    goto CLEANUP;

// If the algorithm failed, space ticks evenly along the axis
FAIL:
    if (DEBUG) ppl_log(&x->c->errcontext, "eps_plot_ticking_auto2() has failed");

    // A very simple way of putting ticks on axes when clever logic fails
    int N = 1 + axis->PhysicalLengthMajor; // Estimate how many ticks we want
    if (N < 3) N = 3;
    if (N > 100) N = 100;

    // Allocate the lists of ticks
    axis->TickListPositions = (double *) ppl_memAlloc_incontext((N + 1) * sizeof(double), OutContext);
    axis->TickListStrings = (char **) ppl_memAlloc_incontext((N + 1) * sizeof(char *), OutContext);
    // Failure to allocate memory is reported as an error
    if ((axis->TickListPositions == NULL) || (axis->TickListStrings == NULL)) {
        ppl_error(&x->c->errcontext, ERR_MEMORY, -1, -1, "Out of memory (s).");
        axis->TickListPositions = NULL;
        axis->TickListStrings = NULL;
        goto CLEANUP;
    }
    // Place each tick, and label it
    for (i = 0; i < N; i++) {
        double X = ((double) i) / (N - 1);
        axis->TickListPositions[i] = X;
        X = pplaxis_InvGetPosition(X, axis);
        if (axis->format == NULL)
            TickLabelAutoGen(x, &axis->TickListStrings[i], X * UnitMultiplier, axis->tics.logBase, OutContext);
        else TickLabelFromFormat(x, &axis->TickListStrings[i], axis->format, X, &axis->DataUnit, axis->xyz, OutContext);
        // Failure to allocate memory is reported as an error
        if (axis->TickListStrings[i] == NULL) {
            ppl_error(&x->c->errcontext, ERR_MEMORY, -1, -1, "Out of memory (t).");
            axis->TickListPositions = NULL;
            axis->TickListStrings = NULL;
            goto CLEANUP;
        }
    }
    axis->TickListStrings[i] = NULL; // null terminate list

// Free the temporary workspace
CLEANUP:
    // Delete temporary lists of ticks and tick schemes
    free(TL_best);
    TL_best = NULL;
    free(TL_trial);
    TL_trial = NULL;
    free(TickSchemes);
    TickSchemes = NULL;

    // Delete rough workspace
    if (ContextRough > 0) ppl_memAlloc_AscendOutOfContext(ContextRough);
    return;
}
