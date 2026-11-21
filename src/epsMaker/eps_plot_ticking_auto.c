// eps_plot_ticking_auto.c
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

// One of the algorithms for placing ticks automatically along an axis, chosen by eps_plot_ticking.c. Method 1 is used
// for axes linked through a "using" expression, or with a format expression, where values do not vary linearly along
// the axis. The axis is divided into many small intervals, and each argument of the format expression is evaluated at
// each step. Points where discrete arguments change, or where continuous ones pass round numbers (multiples of factors
// of their range, or of powers of the logarithmic base), are candidate ticks; schemes of candidates are then accepted
// in order of importance, as major or minor ticks, so long as the ticks do not become too crowded. Each accepted tick
// is then located precisely by bisection. If this fails, method 2 or 3 is used instead.

// METHOD 1: Axes which are 'linked using' or which have non-trivial 'format'
// expressions. The axis is broken up into many small sections and a list of
// 'significant' points constructed.

#define _PPL_EPS_PLOT_TICKING_AUTO_C 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <math.h>

#include <gsl/gsl_math.h>

#include "coreUtils/errorReport.h"
#include "userspace/stack.h"
#include "coreUtils/memAlloc.h"
#include "expressions/expCompile_fns.h"
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
#include "userspace/unitsDisp.h"

#include "epsMaker/eps_plot_canvas.h"
#include "epsMaker/eps_plot_ticking.h"
#include "epsMaker/eps_plot_ticking_auto.h"
#include "epsMaker/eps_plot_ticking_auto2.h"
#include "epsMaker/eps_plot_ticking_auto3.h"

#define MAX_ARGS 32 /* Maximum number of substitution arguments in 'set format' which we analyse */
#define STEP_DUPLICITY \
    100 /* Controls how many steps we divide axis into, multiplied by max number of ticks which fit on axis */
#define MAX_TICKS_PER_INTERVAL 20 /* Maximum number of ticks which we file in any interval */
#define MAX_FACTORS            32 /* Maximum number of factors of LogBase which we consider */
#define FACTOR_MULTIPLY        2.0 /* Factorise LogBase**2, so that 0.00,0.25,0.50,0.75,1.00 is a valid factorisation */
#define NOT_THROW 9999 /* Value we put in DivOfThrow when a tick isn't dividing throw (to get sorting right) */


// Structure used for storing information about a substitution argument in 'set format'
typedef struct ArgumentInfo {
    int id, score, NValueChanges, Throw, FactorsThrow[MAX_FACTORS], NFactorsThrow;
    unsigned char StringArg, ContinuousArg, MinValueSet, MaxValueSet, vetoed, OoM_RangeSet;
    int OoM_min, OoM_max;
    double MinValue, MaxValue; // Work out range of values which this argument takes (aka the 'throw')
    double *NumericValues; // Raster of values sampled along axis
    char **StringValues;
} ArgumentInfo;

// Structure used when sorting a list of ints according to some double-precision 'score'
typedef struct ArgLeagueTableEntry {
    int id;
    double score;
} ArgLeagueTableEntry;

// Structure used for storing information about a potential tick which we might put on the axis
typedef struct PotentialTick {
    int ArgNo; // The argument whose changing is marked by this tick
    int DivOfThrow; // This tick divides up throw of argument in one of its factors
    int OoM; // The order of magnitude of the digit which changes on this tick
    int DivOfOoM; // Order of magnitude is being split up into factors of log base
    int IntervalNum; // Number of the interval in which this tick lies
    double TargetValue; // Target value of argument where this tick should be placed
    int OrderPosition; // Position of this tick in TickOrder
} PotentialTick;

//! compare_arg - Comparison function for qsort(), used to sort the arguments of an axis's format expression into order
//! of how quickly they vary along the axis
//! \param [in] x - Pointer to the first ArgumentInfo structure
//! \param [in] y - Pointer to the second ArgumentInfo structure
//! \return -1, 0 or 1 depending on the order of the arguments

int compare_arg(const void *x, const void *y) {
    const ArgumentInfo *xai, *yai;
    xai = (const ArgumentInfo *) x;
    yai = (const ArgumentInfo *) y;
    if ((!xai->vetoed) && (yai->vetoed)) return -1;
    if ((xai->vetoed) && (!yai->vetoed)) return 1;
    if ((!xai->ContinuousArg) && (yai->ContinuousArg)) return -1; // Discrete arguments more important than continuous
    if ((xai->ContinuousArg) && (!yai->ContinuousArg)) return 1;
    // Discrete arguments which change least often are most important; continuous arguments which move most slowly are
    // most important
    if (!xai->ContinuousArg) {
        if (xai->NValueChanges < yai->NValueChanges) return -1; // Slow-moving continuous arguments most important
        if (xai->NValueChanges > yai->NValueChanges) return 1;
    } else {
        if (xai->score < yai->score) return -1; // Slow-moving continuous arguments most important
        if (xai->score > yai->score) return 1;
    }
    return 0;
}

//! compare_ArgLeagueEntry - Comparison function for qsort(), used to sort the entries of the league table of arguments
//! into order of priority
//! \param [in] x - Pointer to the first ArgLeagueTableEntry structure
//! \param [in] y - Pointer to the second ArgLeagueTableEntry structure
//! \return -1, 0 or 1 depending on the order of the entries

int compare_ArgLeagueEntry(const void *x, const void *y) {
    const ArgLeagueTableEntry *xalte, *yalte;
    xalte = (const ArgLeagueTableEntry *) x;
    yalte = (const ArgLeagueTableEntry *) y;
    if (xalte->score > yalte->score) return -1;
    if (xalte->score < yalte->score) return 1;
    return 0;
}

//! compare_PotentialTicks - Comparison function for qsort(), used to sort potential ticks into order of priority
//! \param [in] x - Pointer to the first PotentialTick structure
//! \param [in] y - Pointer to the second PotentialTick structure
//! \return -1, 0 or 1 depending on the order of the ticks

int compare_PotentialTicks(const void *x, const void *y) {
    const PotentialTick *xpt, *ypt;
    xpt = (const PotentialTick *) x;
    ypt = (const PotentialTick *) y;
    if (xpt->ArgNo > ypt->ArgNo) return 1;
    if (xpt->ArgNo < ypt->ArgNo) return -1;
    if (xpt->DivOfThrow > ypt->DivOfThrow) return 1;
    if (xpt->DivOfThrow < ypt->DivOfThrow) return -1;
    if (xpt->OoM > ypt->OoM) return -1;
    if (xpt->OoM < ypt->OoM) return 1;
    if (xpt->DivOfOoM > ypt->DivOfOoM) return 1;
    if (xpt->DivOfOoM < ypt->DivOfOoM) return -1;
    return 0;
}

//! factorise - Find the factors of an integer
//! \param [in] in - The integer to factorise
//! \param [out] out - Array into which to write the factors
//! \param [in] MaxFactors - The maximum number of factors to return
//! \param [in] FactorMax - The maximum size of factor to return
//! \param [out] NFactors - Set to the number of factors found

static void factorise(const int in, int *out, const int MaxFactors, const int FactorMax, int *NFactors) {
    int i, j = 0, k, N = sqrt(in);
    for (i = 2; i <= N; i++)
        // Record each factor, and its cofactor, filling the list from both ends
        if (in % i == 0) {
            out[j] = i;
            out[MaxFactors - 1 - j] = in / i;
            j++;
            if (j >= MaxFactors / 2) break;
        }
    // Merge the cofactors into the list, skipping duplicates, then drop factors whose cofactors exceed the limit
    for (i = MaxFactors - j; i <= MaxFactors - 1; i++) {
        out[j] = out[i];
        if (out[j] != out[j - 1]) j++;
    }
    for (i = 0; i < j; i++)
        if (in / out[i] <= FactorMax) break;
    for (k = 0; i < j; i++, k++) out[k] = out[i];
    *NFactors = k;
    return;
}

//! CountTicks - Count the number of major and minor ticks in a tick scheme
//! \param [in] TicksAcceptedIn - Array of flags, one per potential tick: zero if the tick is not used, 1 for a major
//! tick, or 2 for a minor tick
//! \param [in] NPotTicks - The number of potential ticks
//! \param [out] NTicksOutMajor - Set to the number of major ticks
//! \param [out] NTicksOutMinor - Set to the number of minor ticks

static void CountTicks(const unsigned char *TicksAcceptedIn, const int NPotTicks, int *NTicksOutMajor,
                       int *NTicksOutMinor) {
    int i, NMajor = 0, NMinor = 0;
    // Count the major and minor ticks
    for (i = 0; i < NPotTicks; i++) {
        if (TicksAcceptedIn[i] == 1) NMajor++;
        else if (TicksAcceptedIn[i] == 2) NMinor++;
    }
    *NTicksOutMajor = NMajor;
    *NTicksOutMinor = NMinor;
    return;
}

//! TickScheme_sprintf - Write a description of a tick scheme, for debugging
//! \param [out] out - Character buffer into which to write the description
//! \param [in] PotTickList - The list of potential ticks
//! \param [in] NPotTicks - The number of potential ticks
//! \param [in] TickOrder - The potential ticks, sorted into order of priority
//! \param [in] TicksAccepted - Array of flags indicating which ticks are used, and whether they are major or minor
//! \param [in] DisplayType - Unused

static void TickScheme_sprintf(char *out, const PotentialTick *PotTickList, const int NPotTicks,
                               const ArgLeagueTableEntry *TickOrder, const unsigned char *TicksAccepted,
                               const unsigned char DisplayType) {
    int i = 0, j = 0, k = 0;
    out[j++] = '(';
    for (i = 0; i < NPotTicks; i++)
        // Write each tick of the requested kind
        if (TicksAccepted[i] == DisplayType) {
            if (k != 0) out[j++] = ',';
            sprintf(out + j, "%g", PotTickList[TickOrder[i].id].TargetValue);
            j += strlen(out + j);
            k++;
        }
    out[j++] = ')';
    out[j++] = '.';
    out[j++] = '\0';
    return;
}

//! AddTickScheme - Add a set of ticks, dividing an argument of the axis's format expression in a particular way, to a
//! tick scheme, and check whether the result is acceptable
//! \param [in] PotTickList - The list of potential ticks
//! \param [in] NPotTicks - The number of potential ticks
//! \param [in] TickOrder - The potential ticks, sorted into order of priority
//! \param [in] NIntervals - The number of intervals into which the axis is divided
//! \param [in] ArgNo - The number of the argument of the format expression being divided
//! \param [in] DivOfThrow - The factor by which the range of the argument is divided
//! \param [in] OoM - The order of magnitude of the step between ticks
//! \param [in] DivOfOoM - The factor by which the order of magnitude is divided
//! \param [in] TicksAcceptedIn - The tick scheme to add to
//! \param [out] TicksAcceptedOut - The new tick scheme
//! \param [in] MAJORminor - 1 to add major ticks, or 2 to add minor ticks
//! \param [in] AxisLength - The length of the axis, in postscript points, used to check that ticks are not too closely
//! spaced
//! \param [out] FLAGacceptable - Set to one if the new tick scheme is acceptable
//! \param [out] NTicksOutMajor - Set to the number of major ticks in the new scheme
//! \param [out] NTicksOutMinor - Set to the number of minor ticks in the new scheme

static void AddTickScheme(const PotentialTick *PotTickList, const int NPotTicks, const ArgLeagueTableEntry *TickOrder,
                          const int NIntervals, const int ArgNo, const int DivOfThrow, const int OoM,
                          const int DivOfOoM, const unsigned char *TicksAcceptedIn, unsigned char *TicksAcceptedOut,
                          const unsigned char MAJORminor, const double AxisLength, unsigned char *FLAGacceptable,
                          int *NTicksOutMajor, int *NTicksOutMinor) {
    int i, j, id, IntNum, abort, imin = -1, imax = -1;
    *NTicksOutMajor = 0;
    *NTicksOutMinor = 0;
    *FLAGacceptable = 1;

    // Initialise output data array
    for (i = 0; i < NPotTicks; i++) { TicksAcceptedOut[i] = (TicksAcceptedIn == NULL) ? 0 : TicksAcceptedIn[i]; }

    // Loop over all potential ticks to check whether they are to be added
    for (i = 0; i < NPotTicks; i++) {
        id = TickOrder[i].id;
        if ((PotTickList[id].ArgNo == ArgNo) // Test whether potential tick is in newly accepted scheme
            && (PotTickList[id].DivOfThrow == DivOfThrow) && (PotTickList[id].OoM == OoM) &&
            (PotTickList[id].DivOfOoM == DivOfOoM) && (MAJORminor || (TicksAcceptedOut[i] == 0))) {
            // Check for ticks accepted immediately to the left of this tick to make sure this point isn't already ticked
            IntNum = PotTickList[id].IntervalNum;
            abort = 0;
            for (j = i - 1; ((j >= 0) && (PotTickList[TickOrder[j].id].IntervalNum >= IntNum - 1) &&
                             (PotTickList[TickOrder[j].id].IntervalNum <= IntNum + 1));
                 j--)
                if ((TicksAcceptedOut[j] > 0) &&
                    (PotTickList[id].TargetValue == PotTickList[TickOrder[j].id].TargetValue)) {
                    if (MAJORminor) TicksAcceptedOut[j] = 0; // Major ticks overwrite minor ticks
                    else if (TicksAcceptedOut[j] == 1) abort = 1; // Minor ticks don't overwrite major ticks
                    else if (TicksAcceptedOut[j] == 2) TicksAcceptedOut[j] = 0; // Minor ticks overwrite minor ticks
                }
            // Apply same tests to ticks accepted immediately to the right of this tick
            for (j = i + 1; ((j < NPotTicks) && (PotTickList[TickOrder[j].id].IntervalNum >= IntNum - 1) &&
                             (PotTickList[TickOrder[j].id].IntervalNum <= IntNum + 1));
                 j++)
                if ((TicksAcceptedOut[j] > 0) &&
                    (PotTickList[id].TargetValue == PotTickList[TickOrder[j].id].TargetValue)) {
                    if (MAJORminor) TicksAcceptedOut[j] = 0;
                    else if (TicksAcceptedOut[j] == 1) abort = 1;
                    else if (TicksAcceptedOut[j] == 2) TicksAcceptedOut[j] = 0;
                }
            // If above tests haven't cancelled this tick (minor tick trying to overwrite major tick), accept it
            if (!abort) {
                TicksAcceptedOut[i] = MAJORminor ? 1 : 2;
                if ((imin < 0) || (imin > PotTickList[id].IntervalNum))
                    imin = PotTickList[id].IntervalNum; // Update span of this tick scheme
                if ((imax < 0) || (imax < PotTickList[id].IntervalNum)) imax = PotTickList[id].IntervalNum;
            }
        }
        if (TicksAcceptedOut[i] == 1)
            (*NTicksOutMajor)++; // Count how many ticks are accepted in the scheme that we are outputing
        if (TicksAcceptedOut[i] == 2) (*NTicksOutMinor)++;
        if (MAJORminor && (TicksAcceptedOut[i] == 1)) // Check that no two major ticks are too close together
        {
            for (j = i - 1; j >= 0; j--) // Scan left for nearest accepted tick
                // Find the nearest major tick to the left, which must be at least a minimum distance away
                if (TicksAcceptedOut[j] == 1) {
                    const double gap = fabs(PotTickList[id].IntervalNum - PotTickList[TickOrder[j].id].IntervalNum) *
                            AxisLength / (NIntervals - 1);
                    if (gap < 1) *FLAGacceptable = 0; // Gap between this tick and that to our left is too small
                    break;
                }
        }
    }
    if (!MAJORminor) // Check that minor ticks don't exceed maximum allowed density
    {
        int Nnewticks = 0;
        // Count the ticks within the span of the new scheme
        for (i = 0; i < NPotTicks; i++) {
            id = PotTickList[TickOrder[i].id].IntervalNum;
            if ((TicksAcceptedOut[i] != 0) && (id >= imin) && (id <= imax)) Nnewticks++;
        }
        if (Nnewticks <= 1) *FLAGacceptable = 1;
        else
            *FLAGacceptable = (unsigned char) ((Nnewticks > 0) &&
                                               (AxisLength * (imax - imin) / (NIntervals - 1) / (Nnewticks - 1) > 1));
    }
    return;
}

//! AutoTickListFinalise - Convert a tick scheme into the final list of tick positions and labels for an axis
//! \param [in] X - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] axis - The axis
//! \param [in] UnitMultiplier - The factor by which values on the axis are multiplied to express them in the units
//! shown on the axis
//! \param [in] AutoTicks - Array of two flags indicating whether major and minor ticks are placed automatically
//! \param [in] VarVal - The dummy variable of the axis's format expression
//! \param [in] NIntervals - The number of intervals into which the axis is divided
//! \param [in] format - The axis's format expression, as text
//! \param [in] start - The position within <format> of the start of its arguments
//! \param [in] formatExp - The compiled arguments of the format expression
//! \param [in] args - Information about each argument of the format expression
//! \param [in] PotTickList - The list of potential ticks
//! \param [in] NPotTicks - The number of potential ticks
//! \param [in] TickOrder - The potential ticks, sorted into order of priority
//! \param [in] TicksAccepted - Array of flags indicating which ticks are used, and whether they are major or minor
//! \param [in] OutContext - The ppl_memAlloc context in which to allocate the tick lists
//! \return Zero on success, or one on failure

static int AutoTickListFinalise(EPSComm *X, pplset_axis *axis, const double UnitMultiplier,
                                const unsigned char *AutoTicks, pplObj *VarVal, const int NIntervals,
                                const char *format, const int start, pplExpr **formatExp, const ArgumentInfo *args,
                                const PotentialTick *PotTickList, const int NPotTicks,
                                const ArgLeagueTableEntry *TickOrder, const unsigned char *TicksAccepted,
                                int OutContext) {
    ppl_context *c = X->c;
    int i, jMAJ, jMIN, Nmajor = 0, Nminor = 0;
    double axispos, x, x1, x2;

    // Count number of accepted ticks
    for (i = 0; i < NPotTicks; i++) {
        if (TicksAccepted[i] == 1) Nmajor++;
        else if (TicksAccepted[i] == 2) Nminor++;
    }

    // Malloc list of accepted ticks
    if (AutoTicks[1]) {
        axis->TickListPositions = (double *) ppl_memAlloc_incontext((Nmajor + 1) * sizeof(double), OutContext);
        axis->TickListStrings = (char **) ppl_memAlloc_incontext((Nmajor + 1) * sizeof(char *), OutContext);
    }
    // Allocate the lists of minor ticks
    if (AutoTicks[0]) {
        axis->MTickListPositions = (double *) ppl_memAlloc_incontext((Nminor + 1) * sizeof(double), OutContext);
        axis->MTickListStrings = (char **) ppl_memAlloc_incontext((Nminor + 1) * sizeof(char *), OutContext);
    }
    if (((AutoTicks[1]) && ((axis->TickListPositions == NULL) || (axis->TickListStrings == NULL))) ||
        ((AutoTicks[0]) && ((axis->MTickListPositions == NULL) || (axis->MTickListStrings == NULL))))
        goto FAIL;

    // NULL terminate lists
    if (AutoTicks[1]) axis->TickListStrings[Nmajor] = NULL;
    if (AutoTicks[0]) axis->MTickListStrings[Nminor] = NULL;

    // Make ticks
    for (i = jMAJ = jMIN = 0; i < NPotTicks; i++)
        // Place each accepted tick
        if (TicksAccepted[i] > 0) {
            const PotentialTick *tick;
            const ArgumentInfo *arg;
            int count_steps = 0;
            double axispos_min, axispos_max, axispos_mid;
            unsigned char DiscreteMoveMin, SlopePositive;

            // If we're not doing automatic major/minor ticks, don't finalise these ticks
            if ((TicksAccepted[i] == 1) && (!AutoTicks[1])) continue;
            if ((TicksAccepted[i] == 2) && (!AutoTicks[0])) continue;

            // Find precise location of tick
            tick = &PotTickList[TickOrder[i].id];
            arg = &args[tick->ArgNo];
            axispos_min = ((double) tick->IntervalNum - 1.0) / (NIntervals - 1);
            axispos_max = ((double) tick->IntervalNum) / (NIntervals - 1);
            SlopePositive = (arg->NumericValues[tick->IntervalNum] >= arg->NumericValues[tick->IntervalNum - 1]);

            // Locate the tick precisely by bisection, finding where the argument passes its target value
            while (count_steps < 200) {
                const int stkLevelOld = c->stackPtr;
                int lOp;
                double x = GSL_NAN;
                pplObj *o = NULL;

                // Evaluate the argument at the middle of the interval
                axispos_mid = (axispos_min + axispos_max) / 2;
                VarVal->real = pplaxis_InvGetPosition(axispos_mid, axis) * UnitMultiplier;

                // Evaluate the argument
                o = ppl_expEval(c, formatExp[tick->ArgNo], &lOp, 1, X->iterDepth + 1);

                if (arg->StringArg) // Evaluate argument at midpoint of the interval we know it to be in
                {
                    char *DummyStr;
                    // A string which could not be evaluated is treated as empty
                    if ((o == NULL) || c->errStat.status || (o->objType != PPLOBJ_STR)) {
                        ppl_tbClear(c);
                        DummyStr = "";
                    } else {
                        DummyStr = (char *) o->auxil;
                    }
                    DiscreteMoveMin = (strcmp(DummyStr, arg->StringValues[tick->IntervalNum - 1]) == 0);
                } else {
                    if ((o == NULL) || c->errStat.status ||
                        ((o->objType != PPLOBJ_NUM) && (o->objType != PPLOBJ_DATE) && (o->objType != PPLOBJ_BOOL))) {
                        ppl_tbClear(c);
                        x = GSL_NAN;
                    } else {
                        x = o->real;
                    }
                    DiscreteMoveMin = (x == arg->NumericValues[tick->IntervalNum - 1]);
                }
                ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack

                // Decide whether to pick left half of interval or right half
                if (!arg->ContinuousArg) {
                    if (DiscreteMoveMin) axispos_min = axispos_mid;
                    else axispos_max = axispos_mid;
                } else {
                    if (SlopePositive ^ (x >= tick->TargetValue)) axispos_min = axispos_mid;
                    else axispos_max = axispos_mid;
                }
                if (axispos_max == axispos_min) break;
                count_steps++;
            }
            axispos = (axispos_min + axispos_max) / 2;
            x = pplaxis_InvGetPosition(axispos, axis);
            x1 = pplaxis_InvGetPosition(axispos - 1e-14, axis);
            x2 = pplaxis_InvGetPosition(axispos + 1e-14, axis);
            if (((x1 <= 0.0) && (x2 >= 0.0)) || ((x1 >= 0.0) && (x2 <= 0.0)))
                x = 0.0; // When tick is very close to a zero, enforce that it is at zero

            // File this tick
            if (TicksAccepted[i] == 1) // Major tick, with label
            {
                axis->TickListPositions[jMAJ] = axispos;
                if (axis->format == NULL)
                    TickLabelAutoGen(X, &axis->TickListStrings[jMAJ], x * UnitMultiplier, axis->tics.logBase,
                                     OutContext);
                else
                    TickLabelFromFormat(X, &axis->TickListStrings[jMAJ], axis->format, x, &axis->DataUnit, axis->xyz,
                                        OutContext);
                if (axis->TickListStrings[jMAJ] == NULL) goto FAIL;
                jMAJ++;
            } else if (TicksAccepted[i] == 2) // Minor tick, without label
            {
                axis->MTickListPositions[jMIN] = axispos;
                axis->MTickListStrings[jMIN] = "";
                jMIN++;
            }
        }

    // Success
    return 0;

// On failure, leave the axis with no ticks
FAIL:
    axis->TickListPositions = NULL;
    axis->TickListStrings = NULL;
    axis->MTickListPositions = NULL;
    axis->MTickListStrings = NULL;
    return 1;
}

//! addPotentialTick - Add a tick to the list of potential ticks which might be placed on an axis. If the list is
//! full, the tick is counted but not stored.
//! \param [in,out] list - The list of potential ticks
//! \param [in,out] N - The number of potential ticks found so far; incremented
//! \param [in] Nmax - The number of potential ticks which <list> has room for
//! \param [in] ArgNo - The argument whose changing is marked by this tick
//! \param [in] DivOfThrow - The factor of the argument's throw which this tick divides it into, or NOT_THROW
//! \param [in] OoM - The order of magnitude of the digit which changes on this tick
//! \param [in] DivOfOoM - The factor of the log base into which the order of magnitude is being split
//! \param [in] TargetValue - The target value of the argument where this tick should be placed
//! \param [in] IntervalNum - The number of the interval in which this tick lies

static void addPotentialTick(PotentialTick *list, int *N, const int Nmax, const int ArgNo, const int DivOfThrow,
                             const int OoM, const int DivOfOoM, const double TargetValue, const int IntervalNum) {
    // Add the tick, unless the list is full; count it regardless, so that overflow can be detected
    if (*N < Nmax) {
        list[*N].ArgNo = ArgNo;
        list[*N].DivOfThrow = DivOfThrow;
        list[*N].OoM = OoM;
        list[*N].DivOfOoM = DivOfOoM;
        list[*N].TargetValue = TargetValue;
        list[*N].IntervalNum = IntervalNum;
    }
    (*N)++;
}

//! eps_plot_ticking_auto - Place ticks automatically on an axis with a format expression, choosing ticks at which the
//! arguments of the format expression take round values
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] axis - The axis
//! \param [in] UnitMultiplier - The factor by which values on the axis are multiplied to express them in the units
//! shown on the axis
//! \param [in|out] AutoTicks - Array of two flags indicating whether major and minor ticks are to be placed
//! automatically
//! \param [in] linkedto - The axis this axis is linked to, or NULL

void eps_plot_ticking_auto(EPSComm *x, pplset_axis *axis, const double UnitMultiplier, unsigned char *AutoTicks,
                           pplset_axis *linkedto) {
    char msg[PPL_MSG_LENGTH];
    ppl_context *c = x->c;
    int i, j, k, start, NArgs, OutContext, ContextRough = -1, CommaPositions[MAX_ARGS + 2], NFactorsLogBase, LogBase;
    int FactorsLogBase[MAX_FACTORS];
    int N_STEPS, tripleQuote = 0, rawString = 0;
    char *format, VarName[2] = "\0\0", FormatTemp[32], quoteType, *DummyStr;
    pplObj CentralValue, *VarVal = NULL, DummyTemp;
    pplExpr **formatExp = NULL;
    ArgumentInfo *args;
    ArgLeagueTableEntry *ArgLeagueTable, *TickOrder;
    PotentialTick *PotTickList;
    int NPotTicks, NPotTicksMax;
    unsigned char *TicksAccepted, *TicksAcceptedNew, *TicksAcceptedNewTF, *TicksAcceptedNew2B, *TicksAcceptedRough,
            *TicksAcceptedRough2;

    // Log which algorithm is in use
    if (DEBUG) ppl_log(&x->c->errcontext, "Using eps_plot_ticking_auto()");
    N_STEPS = (2 + axis->PhysicalLengthMajor) * STEP_DUPLICITY; // Number of intervals into which we divide axis

    // Make temporary rough workspace
    OutContext = ppl_memAlloc_GetMemContext();
    ContextRough = ppl_memAlloc_DescendIntoNewContext();
    DummyStr = (char *) ppl_memAlloc(LSTR_LENGTH);
    if (DummyStr == NULL) goto FAIL;

    // Work through format string identifying substitution expressions
    VarName[0] = "xyzc"[axis->xyz];
    // Without a format, the label is simply the axis's value
    if (axis->format == NULL) {
        ppl_snprintfTrunc(FormatTemp, sizeof(FormatTemp), "\"%%s\"%%(%s)", VarName);
        format = FormatTemp;
    } else {
        format = ((pplExpr *) axis->format)->ascii;
    }
    // Log the format in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "format string is <<%s>>.", format);
        ppl_log(&x->c->errcontext, msg);
    }
    if ((format == FormatTemp) && (axis->AxisLinearInterpolation == NULL) && (axis->LogFinal == SW_BOOL_TRUE) &&
        (log(axis->MaxFinal / axis->MinFinal) / log(axis->tics.logBase) > axis->PhysicalLengthMajor)) {
        ppl_snprintfTrunc(FormatTemp, sizeof(FormatTemp), "\"%%s\"%%(logn(%s,%d))", VarName, axis->tics.logBase);
    }
    for (i = 0; ((format[i] != '\0') && (format[i] != '\'') && (format[i] != '\"')); i++);
    quoteType = format[i];
    if (quoteType == '\0') goto FAIL;
    if ((i > 0) && (format[i - 1] == 'r')) { rawString = 1; }
    // Skip over the opening quote, which may be tripled
    if ((format[i + 1] == quoteType) && (format[i + 2] == quoteType)) {
        tripleQuote = 1;
        i += 3;
    } else {
        i++;
    }
    // Find the closing quote, skipping escaped quotes unless the string is raw; then the format must be followed by %
    // and a bracketed list of arguments
    if (!rawString) {
        if (!tripleQuote)
            for (; ((format[i] != '\0') && ((format[i] != quoteType) || (format[i - 1] == '\\'))); i++);
        else
            for (; ((format[i] != '\0') &&
                    ((format[i] != quoteType) || (format[i + 1] != quoteType) || (format[i + 2] != quoteType) ||
                     (format[i - 1] == '\\')));
                 i++);
    } else {
        if (!tripleQuote)
            for (; ((format[i] != '\0') && ((format[i] != quoteType))); i++);
        else
            for (; ((format[i] != '\0') &&
                    ((format[i] != quoteType) || (format[i + 1] != quoteType) || (format[i + 2] != quoteType)));
                 i++);
    }
    if (format[i] != quoteType) goto FAIL;
    i += tripleQuote ? 3 : 1;
    for (; (((unsigned char) format[i] <= ' ') && (format[i] != '\0')); i++);
    if (format[i] != '%') goto FAIL;
    i++;
    for (; (((unsigned char) format[i] <= ' ') && (format[i] != '\0')); i++);
    if (format[i] != '(') goto FAIL;
    ppl_strBracketMatch(format + i, '(', ')', CommaPositions, &NArgs, &j, MAX_ARGS);
    if (j < 0) goto FAIL;
    if (NArgs < 1) goto FAIL;
    start = i;

    // Malloc structure to hold argument info
    args = (ArgumentInfo *) ppl_memAlloc(NArgs * sizeof(ArgumentInfo));
    if (args == NULL) goto FAIL;
    ArgLeagueTable = (ArgLeagueTableEntry *) ppl_memAlloc(NArgs * sizeof(ArgLeagueTableEntry));
    if (ArgLeagueTable == NULL) goto FAIL;

    // Compile argument expressions
    formatExp = (pplExpr **) ppl_memAlloc(NArgs * sizeof(pplExpr *));
    if (formatExp == NULL) goto FAIL;
    for (i = 0; i < NArgs; i++) formatExp[i] = NULL;
    // Compile each argument of the format
    for (i = 0; i < NArgs; i++) {
        int end = 0, ep = 0, es = 0;
        ppl_expCompile(c, 0, 0, "", format + start + CommaPositions[i] + 1, &end, 0, 0, 0, &formatExp[i], &ep, &es,
                       msg);
        // Give up if an argument cannot be compiled
        if (es || c->errStat.status) {
            ppl_tbClear(c);
            goto FAIL;
        }
    }

    // Look up variable x/y/z in user space and get pointer to its value
    ppl_contextGetVarPointer(x->c, VarName, &VarVal, &DummyTemp);

    // Identify string versus numeric arguments
    CentralValue = axis->DataUnit;
    CentralValue.refCount = VarVal->refCount;
    CentralValue.amMalloced = VarVal->amMalloced;
    CentralValue.flagComplex = 0;
    CentralValue.imag = 0.0;
    CentralValue.real = pplaxis_InvGetPosition(0.5, axis);
    *VarVal = CentralValue;
    for (i = 0; i < NArgs; i++) {
        const int stkLevelOld = c->stackPtr;
        int lOp;
        pplObj *o = ppl_expEval(c, formatExp[i], &lOp, 1, x->iterDepth + 1);
        // Give up if an argument cannot be evaluated at the centre of the axis
        if ((o == NULL) || c->errStat.status) {
            ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack before giving up
            ppl_tbClear(c);
            goto FAIL;
        }
        // Note whether each argument is a number or a string; other types cannot be used
        if ((o->objType == PPLOBJ_NUM) || (o->objType == PPLOBJ_DATE) || (o->objType == PPLOBJ_BOOL)) {
            args[i].StringArg = 0;
            ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack
            continue;
        } else if (o->objType == PPLOBJ_STR) {
            args[i].StringArg = 1;
            ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack
            continue;
        } else {
            ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack before giving up
            ppl_tbClear(c);
            goto FAIL;
        } // Not a number or a string
    }

    // Sample arguments at equally-spaced intervals along axis
    for (i = 0; i < NArgs; i++) {
        args[i].id = i;
        args[i].score = 0;
        args[i].vetoed = 0;
        args[i].NValueChanges = 0;
        args[i].MinValueSet = 0;
        args[i].MaxValueSet = 0;
        args[i].StringValues = (char **) ppl_memAlloc(N_STEPS * sizeof(char *));
        args[i].NumericValues = (double *) ppl_memAlloc(N_STEPS * sizeof(double));
        args[i].OoM_RangeSet = 0;
        args[i].OoM_min = 0;
        args[i].OoM_max = 0;
        if ((args[i].StringValues == NULL) || (args[i].NumericValues == NULL)) goto FAIL;
    }
    // Evaluate each argument at each step along the axis
    for (j = 0; j < N_STEPS; j++) {
        VarVal->real = pplaxis_InvGetPosition(j / (N_STEPS - 1.0), axis) * UnitMultiplier;
        // Each argument
        for (i = 0; i < NArgs; i++) {
            const int stkLevelOld = c->stackPtr;
            int lOp;
            pplObj *o = ppl_expEval(c, formatExp[i], &lOp, 1, x->iterDepth + 1);

            // Record each string value, and count how often it changes
            if (args[i].StringArg) {
                char *DummyStr;
                // A string which could not be evaluated is treated as empty
                if ((o == NULL) || c->errStat.status || (o->objType != PPLOBJ_STR)) {
                    ppl_tbClear(c);
                    DummyStr = "";
                } else {
                    DummyStr = (char *) o->auxil;
                }
                args[i].StringValues[j] = (char *) ppl_memAlloc(strlen(DummyStr) + 1);
                // Failure to allocate memory abandons the algorithm
                if (args[i].StringValues[j] == NULL) {
                    ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack before giving up
                    goto FAIL;
                }
                strcpy(args[i].StringValues[j], DummyStr);
                if ((j > 0) && (strcmp(args[i].StringValues[j], args[i].StringValues[j - 1]) != 0))
                    args[i].NValueChanges++;
            } else {
                int err;
                double x;
                if ((o == NULL) || c->errStat.status ||
                    ((o->objType != PPLOBJ_NUM) && (o->objType != PPLOBJ_DATE) && (o->objType != PPLOBJ_BOOL))) {
                    ppl_tbClear(c);
                    x = GSL_NAN;
                } else {
                    x = o->real;
                }
                err = !gsl_finite(x);
                args[i].NumericValues[j] = x;
                if ((!err) && (j > 0) && (x != args[i].NumericValues[j - 1])) args[i].NValueChanges++;
                // Track the smallest value
                if ((!err) && ((!args[i].MinValueSet) || (x < args[i].MinValue))) {
                    args[i].MinValue = x;
                    args[i].MinValueSet = 1;
                }
                // Track the largest value
                if ((!err) && ((!args[i].MaxValueSet) || (x > args[i].MaxValue))) {
                    args[i].MaxValue = x;
                    args[i].MaxValueSet = 1;
                }
            }
            ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack
        }
    }
    // Numerical arguments which change in more than a quarter of steps are continuous; discrete arguments which change
    // too often are not used
    for (i = 0; i < NArgs; i++) {
        args[i].ContinuousArg = (!args[i].StringArg) && (args[i].NValueChanges > N_STEPS / 4);
        args[i].vetoed = (!args[i].ContinuousArg) &&
                (args[i].NValueChanges > N_STEPS / STEP_DUPLICITY); // Discrete argument changes too fast to be useful
    }

    // Work out factors of log base of axis. In fact, work out factors of log base ** 2, so that ten divides by four.
    LogBase = (axis->LogFinal == SW_BOOL_TRUE) ? axis->tics.logBase : 10;
    factorise(pow(LogBase, FACTOR_MULTIPLY), FactorsLogBase, MAX_FACTORS, axis->PhysicalLengthMinor * 1.2,
              &NFactorsLogBase);

    // Work out throw of each argument (i.e. the spread of values that it takes)
    for (i = 0; i < NArgs; i++) {
        double throw;
        throw = args[i].MaxValue - args[i].MinValue;
        if ((fabs(throw -floor(throw +0.5)) < 1e-10) && (fabs(fmod(throw, 2.0) - 1.0) < 0.1)) throw = ceil(throw +0.5);
        throw = ceil(throw -1e-10);
        if ((throw > 1e6) || (args[i].MinValue < INT_MIN + 10) || (args[i].MaxValue > INT_MAX - 10))
            args[i].Throw = 0.0;
        else args[i].Throw = throw;
        args[i].MinValue = floor(args[i].MinValue);
        factorise(args[i].Throw * pow(LogBase, FACTOR_MULTIPLY - 1), args[i].FactorsThrow, MAX_FACTORS,
                  axis->PhysicalLengthMinor * 1.2, &args[i].NFactorsThrow);
    }

    // Compile scores of how fast each continuous argument moves
    for (j = 1; j < N_STEPS; j++) {
        // Rank the arguments by how much each changes over each step, and sum their ranks
        for (i = 0; i < NArgs; i++) {
            ArgLeagueTable[i].id = i;
            ArgLeagueTable[i].score =
                    args[i].StringArg ? 0.0 : fabs(args[i].NumericValues[j] - args[i].NumericValues[j - 1]);
        }
        qsort((void *) ArgLeagueTable, NArgs, sizeof(ArgLeagueTableEntry), compare_ArgLeagueEntry);
        for (i = 0; i < NArgs; i++) args[ArgLeagueTable[i].id].score += NArgs - i;
    }

    // Sort arguments
    qsort((void *) args, NArgs, sizeof(ArgumentInfo), compare_arg);

    // Malloc table of potential ticks
    NPotTicks = 0;
    NPotTicksMax = NArgs * N_STEPS * MAX_TICKS_PER_INTERVAL;
    PotTickList = (PotentialTick *) ppl_memAlloc(NPotTicksMax * sizeof(PotentialTick));
    if (PotTickList == NULL) goto FAIL;

    // Generate list of potential ticks

    // Find candidate ticks in each interval along the axis
    for (j = 1; j < N_STEPS; j++) {
        for (i = 0; i < NArgs; i++)
            // Only consider arguments which have not been vetoed
            if (!args[i].vetoed) {
                if (!args[i].ContinuousArg) // Discrete arguments... note everywhere where they change
                {
                    if (((args[i].StringArg) && (strcmp(args[i].StringValues[j], args[i].StringValues[j - 1]) != 0)) ||
                        ((!args[i].StringArg) && (args[i].NumericValues[j] != args[i].NumericValues[j - 1]))) {
                        addPotentialTick(PotTickList, &NPotTicks, NPotTicksMax, i, 0, 0, 0, 0.0, j);
                    }
                } else // Continuous arguments... study where they change OoM
                {
                    const double xn = args[i].NumericValues[j - 1];
                    const double yn = args[i].NumericValues[j];
                    const double OoM_n = log(fabs(args[i].NumericValues[j - 1])) / log(LogBase);
                    const double OoM_m = log(fabs(args[i].NumericValues[j])) / log(LogBase);
                    const int Nsteps = fabs(log(1e-15) / log(LogBase));
                    double OoM, nd;
                    int n = 0, m, ZeroInInterval = 0;
                    if (!gsl_finite(OoM_n)) OoM = floor(OoM_m);
                    else if (!gsl_finite(OoM_m)) OoM = floor(OoM_n);
                    else OoM = floor(ppl_max(OoM_n, OoM_m));

                    // Ticks which mark factors of throw
                    for (k = 0; k < args[i].NFactorsThrow; k++) {
                        nd = (args[i].NumericValues[j - 1] - args[i].MinValue) * pow(LogBase, FACTOR_MULTIPLY - 1) /
                                args[i].FactorsThrow[k];
                        n = floor(nd + 0.5);
                        if ((j == 1) && (fabs(nd - n) < 1e-12)) // A tick should go on the left extreme of the axis
                        {
                            addPotentialTick(PotTickList, &NPotTicks, NPotTicksMax, i, k, 0, 0,
                                             (args[i].MinValue +
                                              n / pow(LogBase, FACTOR_MULTIPLY - 1) * args[i].FactorsThrow[k]),
                                             j);
                        }
                        n = floor((args[i].NumericValues[j - 1] - args[i].MinValue) *
                                  pow(LogBase, FACTOR_MULTIPLY - 1) / args[i].FactorsThrow[k]);
                        m = floor((args[i].NumericValues[j] - args[i].MinValue) * pow(LogBase, FACTOR_MULTIPLY - 1) /
                                  args[i].FactorsThrow[k]);
                        // A tick where the value passes a multiple of a factor of its range
                        if (n != m) {
                            addPotentialTick(
                                    PotTickList, &NPotTicks, NPotTicksMax, i, k, 0, 0,
                                    (args[i].MinValue +
                                     ((n > m) ? n : m) / pow(LogBase, FACTOR_MULTIPLY - 1) * args[i].FactorsThrow[k]),
                                    j);
                        }
                    }

                    // Ticks which mark the changes of the Nth significant digit
                    if (((args[i].NumericValues[j - 1] != 0) && (args[i].NumericValues[j] == 0)) ||
                        ((args[i].NumericValues[j - 1] == 0) && (j == 1)) ||
                        ((args[i].NumericValues[j - 1] < 0) && (args[i].NumericValues[j] > 0)) ||
                        ((args[i].NumericValues[j - 1] > 0) && (args[i].NumericValues[j] < 0))) {
                        addPotentialTick(PotTickList, &NPotTicks, NPotTicksMax, i, NOT_THROW, INT_MAX, -10, 0.0,
                                         j); // Zero should be marked in this interval
                        n = 0;
                        ZeroInInterval = 1;
                    } else if ((args[i].NumericValues[j - 1] != 0) || (args[i].NumericValues[j] != 0)) {
                        // Should a tick go on the left extreme of the axis?
                        if (j == 1) {
                            // Mark the start of the axis if the value there is round, at the coarsest order of
                            // magnitude possible
                            for (n = 0; n < Nsteps; n++) {
                                const double divisor = pow(LogBase, OoM - n);
                                // The value is a multiple of this power of the base
                                if ((gsl_finite(divisor)) && (fabs(floor(xn / divisor + 0.5) - xn / divisor) < 1e-12)) {
                                    int priority = 0;
                                    // Ticks at the coarsest order of magnitude have a priority according to their
                                    // leading digit
                                    if (n == 0) {
                                        priority = -1; // Marking 0.1 is more important than marking 1.1
                                        if (fabs(floor(xn / divisor + 0.5)) < 2)
                                            priority = -3; // Marking 0.1 is more important than marking 0.3
                                        else if (fabs(floor(xn / divisor + 0.5)) == 2)
                                            priority = -2; // Marking 0.2 and 0.5 alone is allowable on log axes
                                        else if (fabs(floor(xn / divisor + 0.5)) == 5) priority = -2;
                                    }
                                    if ((!args[i].OoM_RangeSet) || (OoM - n < args[i].OoM_min))
                                        args[i].OoM_min = OoM - n;
                                    if ((!args[i].OoM_RangeSet) || (OoM - n > args[i].OoM_max))
                                        args[i].OoM_max = OoM - n;
                                    args[i].OoM_RangeSet = 1;
                                    addPotentialTick(PotTickList, &NPotTicks, NPotTicksMax, i, NOT_THROW, OoM - n,
                                                     priority, (floor(xn / divisor + 0.5) * divisor), j);
                                    break;
                                }
                            }
                        }
                        // Should tick go within body of interval j-1 -> j?
                        for (n = 0; n < Nsteps; n++) {
                            const double divisor = pow(LogBase, OoM - n);
                            // Find the coarsest order of magnitude at which the leading digits change across the
                            // interval
                            if ((gsl_finite(divisor)) && (floor(xn / divisor) != floor(yn / divisor))) {
                                const double zn = fabs(xn) > fabs(yn) ? xn : yn;
                                int priority = 0;
                                // Ticks at the coarsest order of magnitude have a priority according to their leading
                                // digit
                                if (n == 0) {
                                    priority = -1; // Marking 0.1 is more important than marking 1.1
                                    if (floor(fabs(zn / divisor) + 0.5) < 2)
                                        priority = -3; // Marking 0.1 is more important than marking 0.3
                                    else if (floor(fabs(zn / divisor) + 0.5) == 2)
                                        priority = -2; // Marking 0.2 and 0.5 alone is allowable on log axes
                                    else if (floor(fabs(zn / divisor) + 0.5) == 5) priority = -2;
                                }
                                if ((!args[i].OoM_RangeSet) || (OoM - n < args[i].OoM_min)) args[i].OoM_min = OoM - n;
                                if ((!args[i].OoM_RangeSet) || (OoM - n > args[i].OoM_max)) args[i].OoM_max = OoM - n;
                                args[i].OoM_RangeSet = 1;
                                addPotentialTick(PotTickList, &NPotTicks, NPotTicksMax, i, NOT_THROW, OoM - n, priority,
                                                 (floor(ppl_max(xn, yn) / divisor) * divisor), j);
                                break;
                            }
                        }
                    }

                    // Ticks which mark the passing of fractions of the Nth significant digit
                    if (n < Nsteps)
                        // Mark the passing of fractions of each digit, at several finer orders of magnitude
                        for (m = n; m >= n - ((ZeroInInterval || (j == 1)) ? Nsteps : 3); m--) {
                            const double divisor = pow(LogBase, OoM - m);
                            int o;
                            if (gsl_finite(divisor))
                                // Each factor of the base
                                for (o = 0; o < NFactorsLogBase; o++) {
                                    const double xnfd =
                                            xn / divisor * pow(LogBase, FACTOR_MULTIPLY) / FactorsLogBase[o];
                                    const double ynfd =
                                            yn / divisor * pow(LogBase, FACTOR_MULTIPLY) / FactorsLogBase[o];
                                    const int xnf = floor(xnfd);
                                    const int ynf = floor(ynfd);
                                    const int priority = (floor(fabs(xn / divisor)) == 0) ? 0 : -1;
                                    // A tick where the value passes a multiple of this fraction
                                    if (xnf != ynf) {
                                        addPotentialTick(PotTickList, &NPotTicks, NPotTicksMax, i, NOT_THROW, OoM - m,
                                                         2 * (o + 1) + priority,
                                                         ((xnf > ynf) ? xnf : ynf) * divisor /
                                                                 pow(LogBase, FACTOR_MULTIPLY) * FactorsLogBase[o],
                                                         j);
                                    } else if ((j == 1) && (fabs(floor(xnfd + 0.5) - xnfd) < 1e-12)) {
                                        addPotentialTick(PotTickList, &NPotTicks, NPotTicksMax, i, NOT_THROW, OoM - m,
                                                         2 * (o + 1) + priority,
                                                         floor(xnfd + 0.5) * divisor / pow(LogBase, FACTOR_MULTIPLY) *
                                                                 FactorsLogBase[o],
                                                         j);
                                    }
                                }
                        }
                }
            }
    }

    // Finished compiling list of potential ticks
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg),
                          "Finished compiling list of %d potential ticks. Maximum list size was %d ticks.", NPotTicks,
                          NPotTicksMax);
        ppl_log(&x->c->errcontext, msg);
    }
    if (DEBUG && (NPotTicks > NPotTicksMax)) {
        ppl_log(&x->c->errcontext, "Warning: Overflow of potential tick buffer.");
    }
    if (NPotTicks > NPotTicksMax) NPotTicks = NPotTicksMax; // Overflow happened

    // Sort list of potential ticks
    if (DEBUG) qsort((void *) PotTickList, NPotTicks, sizeof(PotentialTick), compare_PotentialTicks);

    // Create look-up table of ticks in order of position along axis
    TickOrder = (ArgLeagueTableEntry *) ppl_memAlloc(NPotTicks * sizeof(ArgLeagueTableEntry));
    if (TickOrder == NULL) goto FAIL;
    // Order the ticks by their position along the axis
    for (i = 0; i < NPotTicks; i++) {
        TickOrder[i].id = i;
        TickOrder[i].score = -PotTickList[i].IntervalNum;
    }
    if (DEBUG) qsort((void *) TickOrder, NPotTicks, sizeof(ArgLeagueTableEntry), compare_ArgLeagueEntry);
    for (i = 0; i < NPotTicks; i++) PotTickList[TickOrder[i].id].OrderPosition = i;

    // Debugging lines
    if (DEBUG && (VarName[0] != 'z')) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Potential ticks for %s axis (NArgs = %d)", VarName, NArgs);
        ppl_log(&x->c->errcontext, msg);
        // Log each argument
        for (i = 0; i < NArgs; i++) {
            ppl_snprintfTrunc(msg, sizeof(msg),
                              "Argument %d: id %d score %d StringArg %d ContinuousArg %d Vetoed %d NValueChanges %d", i,
                              (int) args[i].id, (int) args[i].score, (int) args[i].StringArg,
                              (int) args[i].ContinuousArg, (int) args[i].vetoed, args[i].NValueChanges);
            ppl_log(&x->c->errcontext, msg);
        }
        ppl_snprintfTrunc(msg, sizeof(msg), "Number of potential ticks: %d", NPotTicks);
        ppl_log(&x->c->errcontext, msg);
        // Log the first few potential ticks
        for (i = 0; ((i < NPotTicks) && (i < 50)); i++) {
            ppl_snprintfTrunc(msg, sizeof(msg),
                              "Tick %7d: Arg %3d DivOfThrow %4d OoM %10d DivOfOoM %3d IntervalNum %3d TargetVal %12.3e",
                              PotTickList[i].OrderPosition, PotTickList[i].ArgNo, PotTickList[i].DivOfThrow,
                              PotTickList[i].OoM, PotTickList[i].DivOfOoM, PotTickList[i].IntervalNum,
                              PotTickList[i].TargetValue);
            ppl_log(&x->c->errcontext, msg);
        }
    }

    // Make arrays for noting which ticks have been accepted
    TicksAccepted = (unsigned char *) ppl_memAlloc(NPotTicks);
    TicksAcceptedNew = (unsigned char *) ppl_memAlloc(NPotTicks);
    TicksAcceptedNewTF = (unsigned char *) ppl_memAlloc(NPotTicks);
    TicksAcceptedNew2B = (unsigned char *) ppl_memAlloc(NPotTicks);
    TicksAcceptedRough = (unsigned char *) ppl_memAlloc(NPotTicks);
    TicksAcceptedRough2 = (unsigned char *) ppl_memAlloc(NPotTicks);
    if ((TicksAccepted == NULL) || (TicksAcceptedNew == NULL) || (TicksAcceptedNewTF == NULL) ||
        (TicksAcceptedNew2B == NULL) || (TicksAcceptedRough == NULL) || (TicksAcceptedRough2 == NULL))
        goto FAIL;
    for (i = 0; i < NPotTicks; i++) TicksAccepted[i] = 0;

    // Start investigating which tick schemes to accept
    for (i = 0; i < NArgs; i++)
        // Accept schemes of ticks for each argument in turn, in order of importance
        if (!args[i].vetoed) {
            unsigned char acceptable = 1;

            if (!args[i].ContinuousArg) // Discrete arguments... note everywhere where they change
            {
                int NTicksMajor, NTicksMinor;

                // Try marking discrete changes to arguments with major ticks
                AddTickScheme(PotTickList, NPotTicks, TickOrder, N_STEPS, i, 0, 0, 0, TicksAccepted, TicksAcceptedNew,
                              1, axis->PhysicalLengthMajor, &acceptable, &NTicksMajor, &NTicksMinor);
                // Accept the scheme if the ticks are not too crowded
                if (acceptable) {
                    // Log the acceptance in debugging mode
                    if (DEBUG) {
                        ppl_snprintfTrunc(msg, sizeof(msg),
                                          "Accepted major ticks to mark changes of argument %d (now %d ticks).", i,
                                          NTicksMajor);
                        ppl_log(&x->c->errcontext, msg);
                    }
                    memcpy(TicksAccepted, TicksAcceptedNew, NPotTicks); // Update TicksAccepted
                    continue;
                }

                // Try marking discrete changes to arguments with minor ticks
                AddTickScheme(PotTickList, NPotTicks, TickOrder, N_STEPS, i, 0, 0, 0, TicksAccepted, TicksAcceptedNew,
                              0, axis->PhysicalLengthMinor, &acceptable, &NTicksMajor, &NTicksMinor);
                // Accept the scheme if the ticks are not too crowded
                if (acceptable) {
                    // Log the acceptance in debugging mode
                    if (DEBUG) {
                        ppl_snprintfTrunc(msg, sizeof(msg),
                                          "Accepted minor ticks to mark changes of argument %d (now %d ticks).", i,
                                          NTicksMajor);
                        ppl_log(&x->c->errcontext, msg);
                    }
                    memcpy(TicksAccepted, TicksAcceptedNew, NPotTicks); // Update TicksAccepted
                    continue;
                }
            } else // Continuous arguments are more complicated
            {
                int ThrowFactors_Nticks = -1, ThrowFactors_Nticks_minor, ThrowFactors_FactNum = -1;
                int Divide_LogBase_Nticks = -1, Divide_LogBase_Nticks_minor = -1;
                int Nticks_new, Nticks_new_minor, Nticks_new_accepted, Nticks_new_accepted_minor;
                int Nticks_new_prev, Nticks_new_prev_minor, Nticks_2B, Nticks_2B_minor;
                int Nticks_2B_best = -1, Nticks_2B_minor_best = -1;
                int l, l2, l3, l4, l_final = 0;
                int NMajorTicksIn, NMinorTicksIn;
                unsigned char MAJORminor;

                // Option 1: Divide throw
                for (k = 0; k < args[i].NFactorsThrow; k++) {
                    acceptable = 1;
                    AddTickScheme(PotTickList, NPotTicks, TickOrder, N_STEPS, i, k, 0, 0, TicksAccepted,
                                  TicksAcceptedNewTF, 1, axis->PhysicalLengthMajor, &acceptable, &ThrowFactors_Nticks,
                                  &ThrowFactors_Nticks_minor);
                    // Log the result in debugging mode
                    if (DEBUG) {
                        ppl_snprintfTrunc(msg, sizeof(msg),
                                          "Dividing throw of %3d into intervals of %.1f produces %d major ticks [%s].",
                                          args[i].Throw, args[i].FactorsThrow[k] / pow(LogBase, FACTOR_MULTIPLY - 1),
                                          ThrowFactors_Nticks, acceptable ? "pass" : "fail");
                        ppl_log(&x->c->errcontext, msg);
                    }
                    // Use the finest acceptable division
                    if (acceptable) {
                        ThrowFactors_FactNum = k;
                        break;
                    } else {
                        ThrowFactors_FactNum = -1;
                        ThrowFactors_Nticks = -1;
                    }
                }

                // Minor ticks: Try other factors of throw
                for (k = 0; k < ThrowFactors_FactNum; k++) {
                    // Minor ticks must divide the major ticks' interval exactly
                    if ((args[i].FactorsThrow[ThrowFactors_FactNum] % args[i].FactorsThrow[k]) == 0) {
                        acceptable = 1;
                        AddTickScheme(PotTickList, NPotTicks, TickOrder, N_STEPS, i, k, 0, 0, TicksAcceptedNewTF,
                                      TicksAcceptedRough, 0, axis->PhysicalLengthMinor, &acceptable,
                                      &ThrowFactors_Nticks, &ThrowFactors_Nticks_minor);
                        // Log the result in debugging mode
                        if (DEBUG) {
                            ppl_snprintfTrunc(
                                    msg, sizeof(msg),
                                    "Dividing throw of %3d into intervals of %.1f produces %d minor ticks [%s].",
                                    args[i].Throw, args[i].FactorsThrow[k] / pow(LogBase, FACTOR_MULTIPLY - 1),
                                    ThrowFactors_Nticks_minor, acceptable ? "pass" : "fail");
                            ppl_log(&x->c->errcontext, msg);
                        }
                        // Accept the first acceptable division
                        if (acceptable) {
                            memcpy(TicksAcceptedNewTF, TicksAcceptedRough, NPotTicks);
                            break;
                        }
                    } else if (DEBUG) {
                        ppl_snprintfTrunc(msg, sizeof(msg),
                                          "Dividing throw of %3d into intervals of %.1f for minor ticks not allowed.",
                                          args[i].Throw, args[i].FactorsThrow[k] / pow(LogBase, FACTOR_MULTIPLY - 1));
                        ppl_log(&x->c->errcontext, msg);
                    }
                }

                // Option 2: Divide log base
                if (DEBUG) {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Checking for ticks which divide OoMs %g to %g [i.e. %d to %d].",
                                      pow(LogBase, args[i].OoM_max), pow(LogBase, args[i].OoM_min), args[i].OoM_max,
                                      args[i].OoM_min);
                    ppl_log(&x->c->errcontext, msg);
                }
                // Option 2: try ticks at fractions of each order of magnitude, from coarsest to finest
                if (args[i].OoM_RangeSet) {
                    memcpy(TicksAcceptedNew, TicksAccepted, NPotTicks);
                    Nticks_new_prev = -1;
                    Nticks_new_prev_minor = -1;
                    // Each order of magnitude
                    for (k = args[i].OoM_max + 1; k >= args[i].OoM_min; k--) {
                        // Option 2B: Divide 10 into factor intervals
                        Nticks_2B = -1;
                        Nticks_2B_minor = -1;
                        CountTicks(TicksAcceptedNew, NPotTicks, &NMajorTicksIn, &NMinorTicksIn);
                        for (l2 = 0; l2 < 2; l2++)
                            // Try each factor of the base as the interval between major ticks
                            for (l = 0; l < NFactorsLogBase; l++) {
                                if (Nticks_2B >= 0) break;
                                l_final = l;
                                acceptable = 1;
                                if (!l2)
                                    AddTickScheme(PotTickList, NPotTicks, TickOrder, N_STEPS, i, NOT_THROW, k,
                                                  2 * (l + 1) - 1, TicksAcceptedNew, TicksAcceptedRough, 1,
                                                  axis->PhysicalLengthMajor, &acceptable, &Nticks_2B, &Nticks_2B_minor);
                                if (acceptable)
                                    AddTickScheme(PotTickList, NPotTicks, TickOrder, N_STEPS, i, NOT_THROW, k,
                                                  2 * (l + 1), l2 ? TicksAcceptedNew : TicksAcceptedRough,
                                                  TicksAcceptedRough, 1, axis->PhysicalLengthMajor, &acceptable,
                                                  &Nticks_2B, &Nticks_2B_minor);
                                // Log the result in debugging mode
                                if (DEBUG) {
                                    ppl_snprintfTrunc(
                                            msg, sizeof(msg),
                                            "Dividing OoM %g into intervals of %g produces %d major ticks [%s%s].",
                                            pow(LogBase, k), FactorsLogBase[l] * pow(LogBase, k - FACTOR_MULTIPLY),
                                            Nticks_2B, acceptable ? "pass" : "fail",
                                            (Nticks_2B <= NMajorTicksIn) ? "; no new ticks" : "");
                                    ppl_log(&x->c->errcontext, msg);
                                }
                                // Reject schemes which are not acceptable, or which add no ticks
                                if ((!acceptable) || (Nticks_2B <= NMajorTicksIn)) {
                                    Nticks_2B = -1;
                                    Nticks_2B_minor = -1;
                                }
                            }
                        // If no factor adds major ticks, keep the existing ticks
                        if (Nticks_2B <= NMajorTicksIn) {
                            l_final = -1;
                            memcpy(TicksAcceptedRough, TicksAcceptedNew, NPotTicks);
                        }
                        Nticks_2B_minor = NMinorTicksIn;
                        for (l4 = 0; l4 < 2; l4++)
                            // Try each factor of the base as the interval between minor ticks
                            for (l3 = 0; l3 < NFactorsLogBase; l3++) {
                                if (Nticks_2B_minor > NMinorTicksIn) break;
                                // Minor ticks must divide the interval between major ticks
                                if ((l_final >= 0) && (FactorsLogBase[l_final] % FactorsLogBase[l3] != 0)) {
                                    // Log the rejection in debugging mode
                                    if (DEBUG) {
                                        ppl_snprintfTrunc(
                                                msg, sizeof(msg),
                                                "Dividing OoM %g into intervals of %g for minor ticks not allowed.",
                                                pow(LogBase, k),
                                                FactorsLogBase[l3] * pow(LogBase, k - FACTOR_MULTIPLY));
                                        ppl_log(&x->c->errcontext, msg);
                                    }
                                    continue; // Minor ticks must mark common factors of log base and major tick interval
                                }
                                acceptable = 1;
                                if (!l4)
                                    AddTickScheme(PotTickList, NPotTicks, TickOrder, N_STEPS, i, NOT_THROW, k,
                                                  2 * (l3 + 1) - 1, TicksAcceptedRough, TicksAcceptedRough2, 0,
                                                  axis->PhysicalLengthMinor, &acceptable, &Nticks_2B, &Nticks_2B_minor);
                                if (acceptable)
                                    AddTickScheme(PotTickList, NPotTicks, TickOrder, N_STEPS, i, NOT_THROW, k,
                                                  2 * (l3 + 1), l4 ? TicksAcceptedRough : TicksAcceptedRough2,
                                                  TicksAcceptedRough2, 0, axis->PhysicalLengthMinor, &acceptable,
                                                  &Nticks_2B, &Nticks_2B_minor);
                                // Log the result in debugging mode
                                if (DEBUG) {
                                    ppl_snprintfTrunc(
                                            msg, sizeof(msg),
                                            "Dividing OoM %g into intervals of %g produces %d minor ticks [%s%s].",
                                            pow(LogBase, k), FactorsLogBase[l3] * pow(LogBase, k - FACTOR_MULTIPLY),
                                            Nticks_2B_minor, acceptable ? "pass" : "fail",
                                            (Nticks_2B_minor <= NMinorTicksIn) ? "; no new ticks" : "");
                                    ppl_log(&x->c->errcontext, msg);
                                }
                                if (!acceptable) Nticks_2B_minor = NMinorTicksIn;
                            }
                        if (Nticks_2B_minor > NMinorTicksIn) memcpy(TicksAcceptedRough, TicksAcceptedRough2, NPotTicks);

                        // Check whether this option 2B is better than previous option 2Bs
                        if ((Nticks_2B > Nticks_2B_best) ||
                            ((Nticks_2B == Nticks_2B_best) && (Nticks_2B_minor > Nticks_2B_minor_best))) {
                            memcpy(TicksAcceptedNew2B, TicksAcceptedRough, NPotTicks);
                            Nticks_2B_best = Nticks_2B;
                            Nticks_2B_minor_best = Nticks_2B_minor;
                        }

                        // Option 2A: Add 1, 1-2-5, or 1-2-3-4...
                        acceptable = 1;
                        Nticks_new_accepted = Nticks_new_prev;
                        Nticks_new_accepted_minor = Nticks_new_prev_minor;
                        MAJORminor = 1;

                        // On first pass, add zero with highest order-of-magnitude
                        if (k == args[i].OoM_max + 1) {
                            AddTickScheme(PotTickList, NPotTicks, TickOrder, N_STEPS, i, NOT_THROW, INT_MAX, -10,
                                          TicksAcceptedNew, TicksAcceptedRough, 1, axis->PhysicalLengthMajor,
                                          &acceptable, &Nticks_new,
                                          &Nticks_new_minor); // Must have zero, if present, with highest OoM
                            if (acceptable) memcpy(TicksAcceptedNew, TicksAcceptedRough, NPotTicks);
                        }
                        memcpy(TicksAcceptedRough, TicksAcceptedNew, NPotTicks);

                        // on with Option 2A....
                        acceptable = 1;
                        // Add ticks in order of priority, as major ticks until they become too crowded, then as minor
                        // ticks
                        for (l = -3; l <= 0; l++) {
                            AddTickScheme(PotTickList, NPotTicks, TickOrder, N_STEPS, i, NOT_THROW, k, l,
                                          TicksAcceptedNew, TicksAcceptedRough, MAJORminor,
                                          MAJORminor ? axis->PhysicalLengthMajor : axis->PhysicalLengthMinor,
                                          &acceptable, &Nticks_new, &Nticks_new_minor);
                            // Log the result in debugging mode
                            if (DEBUG) {
                                ppl_snprintfTrunc(msg, sizeof(msg), "OoM %g Priority %2d produces %d %s ticks [%s].",
                                                  pow(LogBase, k), l, Nticks_new, MAJORminor ? "major" : "minor",
                                                  acceptable ? "pass" : "fail");
                                ppl_log(&x->c->errcontext, msg);
                            }
                            // Keep the scheme if acceptable
                            if (acceptable) {
                                Nticks_new_accepted = Nticks_new;
                                Nticks_new_accepted_minor = Nticks_new_minor;
                                memcpy(TicksAcceptedNew, TicksAcceptedRough, NPotTicks);
                            } else {
                                // Switch to minor ticks, and retry this priority, or stop
                                if (MAJORminor) {
                                    MAJORminor = 0;
                                    l--;
                                    continue;
                                } else {
                                    break;
                                }
                            }
                        }

                        // Print results of options 2A and 2B
                        if (DEBUG) {
                            ppl_snprintfTrunc(
                                    msg, sizeof(msg),
                                    "Option 2A -- %d major ticks and %d minor ticks. Option 2B -- %d major ticks and %d minor ticks.",
                                    Nticks_new_accepted, Nticks_new_accepted_minor, Nticks_2B_best,
                                    Nticks_2B_minor_best);
                            ppl_log(&x->c->errcontext, msg);
                            ppl_snprintfTrunc(msg, sizeof(msg), "Option 2A Major: ");
                            TickScheme_sprintf(msg + strlen(msg), PotTickList, NPotTicks, TickOrder, TicksAcceptedNew,
                                               1);
                            ppl_log(&x->c->errcontext, msg);
                            ppl_snprintfTrunc(msg, sizeof(msg), "Option 2A Minor: ");
                            TickScheme_sprintf(msg + strlen(msg), PotTickList, NPotTicks, TickOrder, TicksAcceptedNew,
                                               1);
                            ppl_log(&x->c->errcontext, msg);
                            ppl_snprintfTrunc(msg, sizeof(msg), "Option 2B Major: ");
                            TickScheme_sprintf(msg + strlen(msg), PotTickList, NPotTicks, TickOrder, TicksAcceptedNew2B,
                                               1);
                            ppl_log(&x->c->errcontext, msg);
                            ppl_snprintfTrunc(msg, sizeof(msg), "Option 2B Minor: ");
                            TickScheme_sprintf(msg + strlen(msg), PotTickList, NPotTicks, TickOrder, TicksAcceptedNew2B,
                                               2);
                            ppl_log(&x->c->errcontext, msg);
                        }

                        // If not making progress, abort
                        if ((k < args[i].OoM_max) && (Nticks_new_accepted == Nticks_new_prev) &&
                            (Nticks_new_accepted_minor == Nticks_new_prev_minor) &&
                            (Nticks_2B <= Nticks_new_accepted) && (Nticks_2B_minor <= Nticks_new_accepted_minor)) {
                            if (DEBUG) ppl_log(&x->c->errcontext, "Making no further progress.");
                            break;
                        }
                        Nticks_new_prev = Nticks_new_accepted;
                        Nticks_new_prev_minor = Nticks_new_accepted_minor;
                    }
                    Divide_LogBase_Nticks = Nticks_new_prev;
                    Divide_LogBase_Nticks_minor = Nticks_new_prev_minor;
                }

                // Choose which option produced best result
                if (DEBUG) {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Dividing by factors of throw produced %d major ticks (%d minor ticks).",
                                      ThrowFactors_Nticks, ThrowFactors_Nticks_minor);
                    ppl_log(&x->c->errcontext, msg);
                }
                // Log the result of option 2 in debugging mode
                if (DEBUG) {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Dividing by factors of log base produced %d major ticks (%d minor ticks).",
                                      Divide_LogBase_Nticks, Divide_LogBase_Nticks_minor);
                    ppl_log(&x->c->errcontext, msg);
                }
                // Log the result of option 2B in debugging mode
                if (DEBUG) {
                    ppl_snprintfTrunc(msg, sizeof(msg), "Option 2B produced %d major ticks (%d minor ticks).",
                                      Nticks_2B_best, Nticks_2B_minor_best);
                    ppl_log(&x->c->errcontext, msg);
                }

                // Compare option 2A and option 2B
                if ((Nticks_2B_best > Divide_LogBase_Nticks) ||
                    ((Nticks_2B_best == Divide_LogBase_Nticks) &&
                     (Nticks_2B_minor_best > Divide_LogBase_Nticks_minor))) {
                    if (DEBUG) ppl_log(&x->c->errcontext, "Option 2B accepted over 2A.");
                    memcpy(TicksAcceptedNew, TicksAcceptedNew2B, NPotTicks);
                    Divide_LogBase_Nticks = Nticks_2B_best;
                    Divide_LogBase_Nticks_minor = Nticks_2B_minor_best;
                } else {
                    if (DEBUG) ppl_log(&x->c->errcontext, "Option 2A accepted over 2B.");
                }

                // Factors of throw are best tick scheme
                if ((ThrowFactors_Nticks > Divide_LogBase_Nticks) ||
                    ((ThrowFactors_Nticks == Divide_LogBase_Nticks) &&
                     (ThrowFactors_Nticks_minor > Divide_LogBase_Nticks_minor))) {
                    if (DEBUG) ppl_log(&x->c->errcontext, "Factors of throw accepted over option 2.");
                    if (ThrowFactors_Nticks > 0) memcpy(TicksAccepted, TicksAcceptedNewTF, NPotTicks);
                } else {
                    if (DEBUG) ppl_log(&x->c->errcontext, "Option 2 accepted over factors of throw.");
                    if (Divide_LogBase_Nticks > 0) memcpy(TicksAccepted, TicksAcceptedNew, NPotTicks);
                }
            }
        }

    // Finalise list of ticks
    AutoTickListFinalise(x, axis, UnitMultiplier, AutoTicks, VarVal, N_STEPS, format, start, formatExp, args,
                         PotTickList, NPotTicks, TickOrder, TicksAccepted, OutContext);
    goto CLEANUP;

// If the algorithm failed, fall back to another
FAIL:
    if (DEBUG) ppl_log(&x->c->errcontext, "eps_plot_ticking_auto() has failed");

    // Free the compiled expressions before the rough workspace, which holds the array of pointers to them
    if (formatExp != NULL)
        for (i = 0; i < NArgs; i++)
            // Each compiled expression
            if (formatExp[i] != NULL) {
                pplExpr_free(formatExp[i]);
                formatExp[i] = NULL;
            }
    formatExp = NULL;

    // When clever logic fails, revert to Pyxplot 0.7 ticking algorithm
    if (ContextRough > 0) ppl_memAlloc_AscendOutOfContext(ContextRough);
    ContextRough = -1;
    if (linkedto != NULL) eps_plot_ticking_auto3(x, axis, UnitMultiplier, AutoTicks, linkedto);
    else eps_plot_ticking_auto2(x, axis, UnitMultiplier, AutoTicks, linkedto);

// Free the compiled expressions, and restore the variable
CLEANUP:
    // Delete compiled expressions
    if (formatExp != NULL)
        for (i = 0; i < NArgs; i++)
            // Each compiled expression
            if (formatExp[i] != NULL) {
                pplExpr_free(formatExp[i]);
                formatExp[i] = NULL;
            }

    // Restore original value of x (or y/z)
    if (VarVal != NULL) ppl_contextRestoreVarPointer(x->c, VarName, &DummyTemp);

    // Delete rough workspace
    if (ContextRough > 0) ppl_memAlloc_AscendOutOfContext(ContextRough);
    return;
}
