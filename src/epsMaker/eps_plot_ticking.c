// eps_plot_ticking.c
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

// Decides the final range of each axis of a graph, and where its ticks go. The range is the one set by the user, or
// else the range of the data plotted against the axis, rounded outwards to round numbers, with defaults for axes with
// no data. Ticks may be listed by the user, given as a start, step and end, or placed automatically by one of three
// algorithms (eps_plot_ticking_auto*.c). Also generates tick labels, automatically in LaTeX or using the axis's format
// expression, and appends the axis's physical unit to its label.

#define _PPL_EPS_PLOT_TICKING_C 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <math.h>

#include <gsl/gsl_math.h>

#include "coreUtils/errorReport.h"
#include "userspace/stack.h"
#include "coreUtils/memAlloc.h"
#include "expressions/expEval.h"
#include "expressions/traceback_fns.h"
#include "mathsTools/dcfmath.h"
#include "stringTools/asciidouble.h"
#include "settings/axes_fns.h"
#include "settings/settings.h"
#include "settings/settingTypes.h"
#include "userspace/context.h"
#include "userspace/contextVarDef.h"
#include "userspace/garbageCollector.h"
#include "userspace/pplObj_fns.h"
#include "userspace/unitsArithmetic.h"
#include "userspace/unitsDisp.h"

#include "epsMaker/eps_core.h"
#include "epsMaker/eps_plot_canvas.h"
#include "epsMaker/eps_plot_ticking.h"
#include "epsMaker/eps_plot_ticking_auto.h"
#include "epsMaker/eps_plot_ticking_auto2.h"
#include "epsMaker/eps_plot_ticking_auto3.h"

//! eps_plot_ticking - Decide the positions and labels of an axis's major and minor ticks, either automatically or from
//! the user's settings, and compose its final label including any units
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] axis - The axis
//! \param [in] AxisUnitStyle - The style in which units are shown in the axis label, one of the SW_AXISUNITSTY_*
//! constants
//! \param [in] linkedto - The axis this axis is linked to, whose ticks should be copied, or NULL

void eps_plot_ticking(EPSComm *x, pplset_axis *axis, const int AxisUnitStyle, pplset_axis *linkedto) {
    // Variables for finding the range and units of the axis
    char msg[PPL_MSG_LENGTH];
    int i, j, N;
    const double logmin = 1e-10;
    double UnitMultiplier = 1.0;
    char *UnitString = NULL;
    unsigned char AutoTicks[2] = {0, 0};

    // Axes which are not in use have no range or ticks
    if (!axis->FinalActive) {
        axis->RangeFinalised = 0;
        return;
    } // Axis is not in use

    // First of all, work out what axis range to use
    if (!axis->RangeFinalised) {
        // Work out axis range. The range may be worked out more than once while a graph is drawn (a provisional range
        // is needed to sample functions), but any warnings about it are only given once.
        unsigned char MinSet = 1;
        const int giveWarnings = !axis->RangeWarningsGiven;
        axis->RangeWarningsGiven = 1;

        // The minimum is the one set by the user, or else the smallest value plotted against the axis
        if (axis->HardMinSet) axis->MinFinal = axis->HardMin;
        else if (axis->MinUsedSet) axis->MinFinal = axis->MinUsed;
        else MinSet = 0;

        // The maximum is the one set by the user, or the largest value plotted, or a default relative to the minimum
        if (axis->HardMaxSet) axis->MaxFinal = axis->HardMax;
        else if (axis->MaxUsedSet) axis->MaxFinal = axis->MaxUsed;
        else if (MinSet) axis->MaxFinal = (axis->LogFinal == SW_BOOL_TRUE) ? axis->MinFinal * 100 : axis->MinFinal + 20;
        else axis->MaxFinal = (axis->LogFinal == SW_BOOL_TRUE) ? 10.0 : 10.0;

        // Check that log axes do not venture too close to zero
        if ((axis->LogFinal == SW_BOOL_TRUE) && (axis->MaxFinal <= 1e-200)) {
            axis->MaxFinal = logmin;
            ppl_snprintfTrunc(msg, sizeof(msg),
                              "Maximum of range for logarithmic axis %c%d is zero or negative; defaulting to 1e-10.",
                              "xyzc"[axis->xyz], axis->axis_n);
            if (giveWarnings) ppl_warning(&x->c->errcontext, ERR_NUMERICAL, msg);
        }
        if (!MinSet) axis->MinFinal = (axis->LogFinal == SW_BOOL_TRUE) ? (axis->MaxFinal / 100) : (axis->MaxFinal - 20);
        // Logarithmic axes may not include zero or negative values
        if ((axis->LogFinal == SW_BOOL_TRUE) && (axis->MinFinal <= 1e-200)) {
            axis->MinFinal = logmin;
            ppl_snprintfTrunc(msg, sizeof(msg),
                              "Minimum of range for logarithmic axis %c%d is zero or negative; defaulting to 1e-10.",
                              "xyzc"[axis->xyz], axis->axis_n);
            if (giveWarnings) ppl_warning(&x->c->errcontext, ERR_NUMERICAL, msg);
        }

        // If there's no spread of data on the axis, make a spread up
        if ((fabs(axis->MinFinal - axis->MaxFinal) <= fabs(1e-14 * axis->MinFinal)) ||
            (fabs(axis->MinFinal - axis->MaxFinal) <= fabs(1e-14 * axis->MaxFinal))) {
            // Warn if the user set equal limits
            if (axis->HardMinSet && axis->HardMaxSet) {
                ppl_snprintfTrunc(
                        msg, sizeof(msg),
                        "Specified minimum and maximum range limits for axis %c%d are equal; reverting to alternative limits.",
                        "xyzc"[axis->xyz], axis->axis_n);
                if (giveWarnings) ppl_warning(&x->c->errcontext, ERR_NUMERICAL, msg);
            }
            // Widen the range
            if (axis->LogFinal != SW_BOOL_TRUE) {
                const double step = ppl_max(1.0, 1e-3 * fabs(axis->MinFinal));
                axis->MinFinal -= step;
                axis->MaxFinal += step;
            } else {
                if (axis->MinFinal > 1e-300) axis->MinFinal /= 10.0;
                if (axis->MaxFinal < 1e300) axis->MaxFinal *= 10.0;
            }
        }
    }

    // Finalise the physical unit to be associated with data on this axis
    if (!axis->DataUnitSet) {
        // The units of the axis are those of its range, if set by the user, or else dimensionless
        if (axis->HardUnitSet) {
            axis->DataUnitSet = 1;
            axis->DataUnit = axis->HardUnit;
        } else {
            pplObjNum(&axis->DataUnit, 0, 0, 0);
        }
    }
    pplObj CentralValue = axis->DataUnit;
    CentralValue.flagComplex = 0;
    CentralValue.imag = 0.0;
    CentralValue.real = (axis->format == NULL) ? pplaxis_InvGetPosition(0.5, axis) : 1.0;
    if (CentralValue.real == 0.0) CentralValue.real = pplaxis_InvGetPosition(0.25, axis);
    UnitString = ppl_printUnit(x->c, &CentralValue, &UnitMultiplier, NULL, 0, 0, SW_DISPLAY_L);
    UnitMultiplier /= CentralValue.real;
    if (!gsl_finite(UnitMultiplier)) UnitMultiplier = 1.0;

    // Round the range outwards to round numbers, unless the user set the limits
    if (!axis->RangeFinalised) {

        // If axis does not have a user-specified range, round it outwards towards a round endpoint
        double min_prelim = axis->MinFinal * UnitMultiplier;
        double max_prelim = axis->MaxFinal * UnitMultiplier;

        // Work with the logarithms of the limits on logarithmic axes
        if (axis->LogFinal == SW_BOOL_TRUE) {
            min_prelim = log10(min_prelim);
            max_prelim = log10(max_prelim);
        }

        // Round each limit to a multiple of a power of ten, about a fifth of the range
        double OoM = pow(10.0, floor(log10(fabs(max_prelim - min_prelim) / 5)));
        min_prelim = floor(min_prelim / OoM) * OoM;
        max_prelim = ceil(max_prelim / OoM) * OoM;

        // Convert back from logarithms
        if (axis->LogFinal == SW_BOOL_TRUE) {
            min_prelim = pow(10.0, min_prelim);
            max_prelim = pow(10.0, max_prelim);
        }

        // Convert back from the units in which the axis is labelled
        min_prelim /= UnitMultiplier;
        max_prelim /= UnitMultiplier;

        // Use the rounded limits, unless the user set them
        if (gsl_finite(min_prelim) && (!axis->HardMinSet) &&
            ((axis->LogFinal != SW_BOOL_TRUE) || (min_prelim > 1e-300)))
            axis->MinFinal = min_prelim;
        if (gsl_finite(max_prelim) && (!axis->HardMaxSet) &&
            ((axis->LogFinal != SW_BOOL_TRUE) || (max_prelim > 1e-300)))
            axis->MaxFinal = max_prelim;

        // Print out debugging report
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Determined range for axis %c%d of plot %d. Usage was [",
                              "xyzc"[axis->xyz], axis->axis_n, axis->canvas_id);
            i = strlen(msg);
            // The smallest value plotted, if any
            if (axis->MinUsedSet) {
                ppl_snprintfTrunc(msg + i, sizeof(msg) - (i), "%f", axis->MinUsed);
                i += strlen(msg + i);
            } else msg[i++] = '*';
            msg[i++] = ':';
            // The largest value plotted, if any
            if (axis->MaxUsedSet) {
                ppl_snprintfTrunc(msg + i, sizeof(msg) - (i), "%f", axis->MaxUsed);
                i += strlen(msg + i);
            } else msg[i++] = '*';
            ppl_snprintfTrunc(msg + i, sizeof(msg) - (i), "]. Final range was [%f:%f].", axis->MinFinal,
                              axis->MaxFinal);
            ppl_log(&x->c->errcontext, msg);
        }

        // Flip axis range if it is reversed
        if (axis->RangeReversed) {
            double swap = axis->MinFinal;
            axis->MinFinal = axis->MaxFinal;
            axis->MaxFinal = swap;
        }

        // Set flag to show that we have finalised the range of this axis
        axis->RangeFinalised = 1;
    }

    // Secondly, decide what ticks to place on this axis
    if (!axis->TickListFinalised) {

        // If ticks have been manually specified, check that units are right
        if ((!ppl_unitsDimEqual(&axis->unit, &axis->DataUnit)) &&
            ((axis->tics.tickList != NULL) ||
             (((axis->log == SW_BOOL_TRUE) ? (axis->tics.tickMinSet) : (axis->tics.tickStepSet)) != 0) ||
             (axis->ticsM.tickList != NULL) ||
             (((axis->log == SW_BOOL_TRUE) ? (axis->ticsM.tickMinSet) : (axis->ticsM.tickStepSet)) != 0))) {
            ppl_snprintfTrunc(
                    msg, sizeof(msg),
                    "Cannot put any ticks on axis %c%d because their positions are specified in units of <%s> whilst the axis has units of <%s>.",
                    "xyzc"[axis->xyz], axis->axis_n, ppl_printUnit(x->c, &axis->unit, NULL, NULL, 0, 1, 0),
                    ppl_printUnit(x->c, &axis->DataUnit, NULL, NULL, 1, 1, 0));
            ppl_error(&x->c->errcontext, ERR_GENERIC, -1, -1, msg);
            axis->TickListPositions = NULL;
            axis->TickListStrings = NULL;
            axis->TickListPositions = NULL;
            axis->TickListStrings = NULL;
            return;
        }

        // Tick labels are allocated in the caller's memory context
        int OutContext = ppl_memAlloc_GetMemContext();

        // Finalise the label to be placed on the axis, quoting a physical unit as necessary
        if ((axis->DataUnit.dimensionless) || (axis->format != NULL)) {
            axis->FinalAxisLabel = axis->label;
        } // No units to append
        // Otherwise append the unit, in the style set with "set axisunitstyle"
        else {
            i = strlen(UnitString) + 16; // Room for the unit, and the brackets around it
            if (axis->label != NULL) i += strlen(axis->label);
            axis->FinalAxisLabel = (char *) ppl_memAlloc(i);
            // Failure to allocate memory is reported as an error
            if (axis->FinalAxisLabel == NULL) {
                ppl_error(&x->c->errcontext, ERR_MEMORY, -1, -1, "Out of memory (u).");
                return;
            }
            if (AxisUnitStyle == SW_AXISUNITSTY_BRACKET)
                ppl_snprintfTrunc(axis->FinalAxisLabel, i, "%s ($%s$)", (axis->label != NULL) ? axis->label : "",
                                  UnitString);
            else if (AxisUnitStyle == SW_AXISUNITSTY_RATIO)
                ppl_snprintfTrunc(axis->FinalAxisLabel, i, "%s / $%s$", (axis->label != NULL) ? axis->label : "",
                                  UnitString);
            else
                ppl_snprintfTrunc(axis->FinalAxisLabel, i, "%s [$%s$]", (axis->label != NULL) ? axis->label : "",
                                  UnitString);
        }

        // Minor ticks. Then major ticks.
        for (int MajMin = 0; MajMin < 2; MajMin++) {
            double TickMax, TickMin, TickStep;
            unsigned char TickMaxSet, TickMinSet, TickStepSet;
            double *TickList;
            char **TickStrs;
            double **TickListPositions;
            char ***TickListStrings;

            // Read the settings for minor ticks, or major ticks
            if (MajMin == 0) {
                TickMax = axis->ticsM.tickMax;
                TickMin = axis->ticsM.tickMin;
                TickStep = axis->ticsM.tickStep;
                TickMaxSet = axis->ticsM.tickMaxSet;
                TickMinSet = axis->ticsM.tickMinSet;
                TickStepSet = axis->ticsM.tickStepSet;
                TickList = axis->ticsM.tickList;
                TickStrs = axis->ticsM.tickStrs;
                TickListPositions = &axis->MTickListPositions;
                TickListStrings = &axis->MTickListStrings;
            } else {
                TickMax = axis->tics.tickMax;
                TickMin = axis->tics.tickMin;
                TickStep = axis->tics.tickStep;
                TickMaxSet = axis->tics.tickMaxSet;
                TickMinSet = axis->tics.tickMinSet;
                TickStepSet = axis->tics.tickStepSet;
                TickList = axis->tics.tickList;
                TickStrs = axis->tics.tickStrs;
                TickListPositions = &axis->TickListPositions;
                TickListStrings = &axis->TickListStrings;
            }

            if (TickList != NULL) // Ticks have been specified as an explicit list
            {
                for (N = 0; TickStrs[N] != NULL; N++); // Find length of list of ticks
                *TickListPositions = (double *) ppl_memAlloc((N + 1) * (axis->AxisValueTurnings + 1) * sizeof(double));
                *TickListStrings = (char **) ppl_memAlloc((N + 1) * (axis->AxisValueTurnings + 1) * sizeof(char *));
                if ((*TickListPositions == NULL) || (*TickListStrings == NULL)) {
                    ppl_error(&x->c->errcontext, ERR_MEMORY, -1, -1, "Out of memory (v).");
                    *TickListPositions = NULL;
                    *TickListStrings = NULL;
                    return;
                }
                for (i = j = 0; i < N; i++)
                    // Place each tick on each monotonic section of the axis, skipping those beyond its ends
                    for (int xrn = 0; xrn <= axis->AxisValueTurnings; xrn++) {
                        (*TickListPositions)[j] = pplaxis_GetPosition(TickList[i], axis, xrn, 0);
                        if ((!gsl_finite((*TickListPositions)[j])) || ((*TickListPositions)[j] < 0.0) ||
                            ((*TickListPositions)[j] > 1.0))
                            continue; // Filter out ticks which are off the end of the axis
                        if (TickStrs[i][0] != '\xFF') (*TickListStrings)[j] = TickStrs[i];
                        else if (MajMin == 0) (*TickListStrings)[j] = "";
                        else if (axis->format == NULL)
                            TickLabelAutoGen(x, &(*TickListStrings)[j], TickList[i] * UnitMultiplier,
                                             axis->tics.logBase, OutContext);
                        else
                            TickLabelFromFormat(x, &(*TickListStrings)[j], axis->format, TickList[i], &axis->DataUnit,
                                                axis->xyz, OutContext);
                        j++;
                    }
                (*TickListStrings)[j] = NULL; // null terminate list
            } else if (TickStepSet) {
                double TMin, TMax, tmp;
                unsigned char inverted = 0;
                *TickListPositions = (double *) ppl_memAlloc(102 * (axis->AxisValueTurnings + 1) * sizeof(double));
                *TickListStrings = (char **) ppl_memAlloc(102 * (axis->AxisValueTurnings + 1) * sizeof(char *));
                if ((*TickListPositions == NULL) || (*TickListStrings == NULL)) {
                    ppl_error(&x->c->errcontext, ERR_MEMORY, -1, -1, "Out of memory (w).");
                    *TickListPositions = NULL;
                    *TickListStrings = NULL;
                    return;
                }
                double TStep = TickStep;
                // A negative step means ticks run from the end of the range towards the start
                if (TStep < 0) {
                    TStep = -TStep;
                    inverted = 1;
                }
                // On logarithmic axes, the step is a factor; a factor less than one runs backwards
                if (axis->LogFinal == SW_BOOL_TRUE) {
                    // A factor less than one
                    if (TStep < 1) {
                        TStep = 1 / TStep;
                        inverted = 1;
                    } else inverted = 0;
                }

                // Place ticks on each monotonic section of the axis
                for (int xrn = j = 0; xrn <= axis->AxisValueTurnings; xrn++) {
                    // On an axis whose mapping is tabulated, the ticks run over the section's range of values
                    if (axis->AxisLinearInterpolation != NULL) {
                        double first = axis->AxisLinearInterpolation[axis->AxisTurnings[0]];
                        double RegionMin = axis->AxisLinearInterpolation[axis->AxisTurnings[xrn]];
                        double RegionMax = axis->AxisLinearInterpolation[axis->AxisTurnings[xrn + 1]];
                        // Make sure the section's range is the right way around
                        if (RegionMax < RegionMin) {
                            tmp = RegionMin;
                            RegionMin = RegionMax;
                            RegionMax = tmp;
                        }

                        // Ticks run from the start value given, or from the first value on the axis
                        if (!inverted) {
                            TMin = TickMinSet ? TickMin : first;
                            TMax = TickMaxSet ? TickMax : RegionMax;
                        } else {
                            TMin = TickMaxSet ? TickMax : first;
                            TMax = TickMinSet ? TickMin : RegionMax;
                        }

                        // Move the first tick forward by whole steps into the section
                        if ((TMin < RegionMin) || ((!inverted) && (!TickMinSet)) || ((inverted) && (!TickMaxSet))) {
                            if (axis->LogFinal == SW_BOOL_TRUE)
                                TMin *= exp(ceil((log(RegionMin / TMin)) / log(TStep)) * log(TStep));
                            else TMin += ceil((RegionMin - TMin) / TStep) * TStep;
                        }
                        // Move the last tick back by whole steps into the section
                        if (TMax > RegionMax) {
                            if (axis->LogFinal == SW_BOOL_TRUE)
                                TMax /= exp(floor((log(TMax / RegionMax)) / log(TStep)) * log(TStep));
                            else TMax -= floor((TMax - RegionMax) / TStep) * TStep;
                        }
                    } else {
                        // Ticks run from the start value given, or from the start of the axis
                        if (!inverted) {
                            TMin = TickMinSet ? TickMin : ppl_min(axis->MinFinal, axis->MaxFinal);
                            TMax = TickMaxSet ? TickMax : ppl_max(axis->MinFinal, axis->MaxFinal);
                        } else {
                            TMin = TickMaxSet ? TickMax : ppl_min(axis->MinFinal, axis->MaxFinal);
                            TMax = TickMinSet ? TickMin : ppl_max(axis->MinFinal, axis->MaxFinal);
                        }

                        // Move the first tick forward by whole steps into the axis's range
                        if (TMin < ppl_min(axis->MinFinal, axis->MaxFinal)) {
                            if (axis->LogFinal == SW_BOOL_TRUE)
                                TMin *= exp(ceil((log(axis->MinFinal / TMin)) / log(TStep)) * log(TStep));
                            else TMin += ceil((axis->MinFinal - TMin) / TStep) * TStep;
                        }
                        // Move the last tick back by whole steps into the axis's range
                        if (TMax > ppl_max(axis->MinFinal, axis->MaxFinal)) {
                            if (axis->LogFinal == SW_BOOL_TRUE)
                                TMax /= exp(floor((log(TMax / axis->MaxFinal)) / log(TStep)) * log(TStep));
                            else TMax -= floor((TMax - axis->MaxFinal) / TStep) * TStep;
                        }
                    }
                    // Place up to 100 ticks
                    for (i = 0; i < 100; i++) {
                        // Forwards from the start, or backwards from the end
                        if (!inverted) {
                            if (axis->LogFinal == SW_BOOL_TRUE) tmp = TMin * pow(TStep, i);
                            else tmp = TMin + i * TStep;
                        } else {
                            if (axis->LogFinal == SW_BOOL_TRUE) tmp = TMax * pow(TStep, -i);
                            else tmp = TMax - i * TStep;
                        }
                        if (((!inverted) && (tmp > TMax)) || ((inverted) && (tmp < TMin))) break;
                        if ((axis->LogFinal != SW_BOOL_TRUE) && (fabs(tmp) < fabs(TStep * 1e-14)))
                            tmp = 0; // Enforce that ticks close to zero are at zero
                        (*TickListPositions)[j] = pplaxis_GetPosition(tmp, axis, xrn, 0);
                        if ((!gsl_finite((*TickListPositions)[j])) || ((*TickListPositions)[j] < 0.0) ||
                            ((*TickListPositions)[j] > 1.0))
                            continue; // Filter out ticks which are off the end of the axis
                        if (MajMin == 0) (*TickListStrings)[j] = "";
                        else if (axis->format == NULL)
                            TickLabelAutoGen(x, &(*TickListStrings)[j], tmp * UnitMultiplier, axis->tics.logBase,
                                             OutContext);
                        else
                            TickLabelFromFormat(x, &(*TickListStrings)[j], axis->format, tmp, &axis->DataUnit,
                                                axis->xyz, OutContext);
                        // Failure to allocate memory is reported as an error
                        if ((*TickListStrings)[j] == NULL) {
                            ppl_error(&x->c->errcontext, ERR_MEMORY, -1, -1, "Out of memory (x).");
                            *TickListPositions = NULL;
                            *TickListStrings = NULL;
                            return;
                        }
                        j++;
                    }
                }
                (*TickListStrings)[j] = NULL; // null terminate list
            } else {
                AutoTicks[MajMin] = 1;
            }
        }

        // Do automatic ticking as required
        if (AutoTicks[1]) {
            if ((axis->format != NULL) || (axis->AxisLinearInterpolation != NULL))
                eps_plot_ticking_auto(x, axis, UnitMultiplier, AutoTicks, linkedto);
            // Axes linked to other axes copy their ticks if possible
            else {
                if (linkedto != NULL) eps_plot_ticking_auto3(x, axis, UnitMultiplier, AutoTicks, linkedto);
                else eps_plot_ticking_auto2(x, axis, UnitMultiplier, AutoTicks, linkedto);
            }
        }

        // Set flag to show that we have finalised the ticking of this axis
        axis->TickListFinalised = 1;
    }

    // Done
    return;
}

//! TickLabelAutoGen - Generate the label for a tick, rendering its value with the minimum number of significant figures
//! needed
//! \param [in] X - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [out] output - Set to point to the label, allocated in the ppl_memAlloc context <OutContext>
//! \param [in] x - The value of the tick
//! \param [in] log_base - The base of the logarithmic axis on which the tick lies, or 1 for a linear axis
//! \param [in] OutContext - The ppl_memAlloc context in which to allocate the label

void TickLabelAutoGen(EPSComm *X, char **output, double x, double log_base, int OutContext) {
    // Variables for formatting the label
    char msg[PPL_MSG_LENGTH];
    int SF = X->c->set->term_current.SignificantFigures;

    // Labels are compared with round numbers to within the precision displayed
    double ApproxMargin = pow(10, -SF + 1);
    if (ApproxMargin < 1e-15) ApproxMargin = 1e-15;

    // Moderate numbers are written in full; others as a mantissa times a power of the axis's logarithmic base
    if ((fabs(x) < DBL_MIN * 100) || ((fabs(x) > 1e-3) && (fabs(x) < 1e5))) {
        ppl_snprintfTrunc(msg, sizeof(msg), "%s", ppl_numericDisplay(x, X->c->numdispBuff[0], SF, 1));
    } else {
        unsigned char sgn = 0;
        // Write the sign separately
        if (x < 0) {
            sgn = 1;
            x = -x;
        }
        double e = floor(log(x) / log(log_base));
        double m = x / pow(log_base, e);
        // Round mantissas which are almost equal to the base up to the next power
        if (fabs(m - log_base) < ApproxMargin) {
            e++;
            m = 1;
        } // Avoid 10 x 10^2
        if (ppl_dblApprox(m, 1, pow(10, -SF + 1)))
            ppl_snprintfTrunc(msg, sizeof(msg), "%s%d^{%s}", sgn ? "-" : "", (int) log_base,
                              ppl_numericDisplay(e, X->c->numdispBuff[0], SF, 1));
        else
            ppl_snprintfTrunc(msg, sizeof(msg), "%s%s\\times %d^{%s}", sgn ? "-" : "",
                              ppl_numericDisplay(m, X->c->numdispBuff[0], SF, 1), (int) log_base,
                              ppl_numericDisplay(e, X->c->numdispBuff[1], SF, 1));
    }
    *output = (char *) ppl_memAlloc_incontext(strlen(msg) + 3, OutContext);
    if ((*output) == NULL) return;
    ppl_snprintfTrunc(*output, strlen(msg) + 3, "$%s$", msg);
    return;
}

//! TickLabelFromFormat - Generate the label for a tick by evaluating the axis's format expression
//! \param [in] X - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [out] output - Set to point to the label, allocated in the ppl_memAlloc context <OutContext>
//! \param [in] FormatExp - The axis's format expression
//! \param [in] x - The value of the tick
//! \param [in] xunit - The physical units of the axis
//! \param [in] xyz - The direction of the axis (0=x, 1=y, 2=z), which determines the name of the dummy variable in the
//! format expression
//! \param [in] OutContext - The ppl_memAlloc context in which to allocate the label

void TickLabelFromFormat(EPSComm *X, char **output, pplExpr *FormatExp, const double x, const pplObj *xunit,
                         const int xyz, int OutContext) {
    char msg[PPL_MSG_LENGTH];
    int lOP;
    const int stkLevelOld = X->c->stackPtr;
    char *VarName, *tmp_string;
    pplObj DummyTemp, *VarVal;

    // The variable which holds the value: x, y, z or c, according to the axis
    if (xyz == 0) VarName = "x";
    else if (xyz == 1) VarName = "y";
    else if (xyz == 2) VarName = "z";
    else VarName = "c";

    // Look up variable in user space and get pointer to its value
    ppl_contextGetVarPointer(X->c, VarName, &VarVal, &DummyTemp);

    // Set value of x (or y/z)
    int rc = VarVal->refCount;
    *VarVal = *xunit;
    VarVal->refCount = rc;
    VarVal->imag = 0.0;
    VarVal->flagComplex = 0;
    VarVal->real = x;

    // Generate tick string
    pplObj *outval = ppl_expEval(X->c, FormatExp, &lOP, 0, X->iterDepth + 1);
    // If the expression fails, or does not return a string, report an error and label the tick with a question mark
    if (X->c->errStat.status) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Error encountered whilst using format string: %s", FormatExp->ascii);
        ppl_error(&X->c->errcontext, ERR_PREFORMED, -1, -1, msg);
        ppl_tbWrite(X->c);
        ppl_tbClear(X->c);
        tmp_string = "{\\bf ?}";
    } else if (outval->objType != PPLOBJ_STR) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Error encountered whilst using format string: %s", FormatExp->ascii);
        ppl_error(&X->c->errcontext, ERR_PREFORMED, -1, -1, msg);
        ppl_error(&X->c->errcontext, ERR_PREFORMED, -1, -1, "Tick label was not a string.");
        tmp_string = "{\\bf ?}";
    } else {
        tmp_string = (char *) outval->auxil;
    }
    // Copy the label; if memory runs out, *output is left NULL, but the stack and variable are still restored
    *output = (char *) ppl_memAlloc_incontext(strlen(tmp_string) + 3, OutContext);
    if ((*output) != NULL) ppl_snprintfTrunc(*output, strlen(tmp_string) + 3, "%s", tmp_string);
    ppl_stackClean(X->c, stkLevelOld); // Discard anything left on the stack

    // Restore original value of x (or y/z)
    ppl_contextRestoreVarPointer(X->c, VarName, &DummyTemp);
    return;
}
