// eps_plot_linkedaxes.c
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

// Handles axes linked to other axes with "set axis ... linked", on the same graph or on another multiplot item,
// optionally through a "using" expression which maps values on one axis to values on the other. Usage information (the
// range of data plotted) is propagated down the chain of links to the axis at its end, whose range is decided first and
// then propagated back up. Axes linked through an expression have their mapping tabulated at many points, and usage is
// propagated backwards through the expression by inverting it numerically with GSL's minimiser.

#define _PPL_EPS_PLOT_LINKEDAXES 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <gsl/gsl_errno.h>
#include <gsl/gsl_math.h>
#include <gsl/gsl_multimin.h>

#include "commands/eqnsolve.h"
#include "userspace/stack.h"
#include "coreUtils/errorReport.h"
#include "coreUtils/list.h"
#include "coreUtils/memAlloc.h"
#include "expressions/expEval.h"
#include "expressions/traceback_fns.h"
#include "mathsTools/dcfmath.h"
#include "settings/axes_fns.h"
#include "settings/settingTypes.h"
#include "stringTools/asciidouble.h"
#include "userspace/context.h"
#include "userspace/contextVarDef.h"
#include "userspace/garbageCollector.h"
#include "userspace/pplObj_fns.h"
#include "userspace/unitsArithmetic.h"
#include "userspace/unitsDisp.h"

#include "epsMaker/eps_comm.h"
#include "epsMaker/eps_core.h"
#include "epsMaker/eps_plot_canvas.h"
#include "epsMaker/eps_plot_linkedaxes.h"
#include "epsMaker/eps_plot_ticking.h"
#include "epsMaker/eps_settings.h"

// Whenever we update the usage variables MinUsed and MaxUsed for an axis, this
// procedure is called. It checks whether the axis is linked, and if so,
// updates the usage variables for the axis which it is linked to. This process
// may then iteration down a hierarchy of linked axes. As a rule, it is the
// axis at the bottom of the linkage hierarchy (i.e. at the end of the linked
// list) that has the canonical usage variables. Axes further up may not have
// complete information about the usage of the set of linked axes, since usage
// does not propagate UP the hierarchy.

//! eps_plot_LinkedAxisBackPropagate - Propagate the range of data plotted against an axis back to the axis it is linked
//! to, so that the linked axis's range takes it into account
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] source - The axis whose usage is to be propagated

void eps_plot_LinkedAxisBackPropagate(EPSComm *x, pplset_axis *source) {
    // Variables for following the chain of links
    char msg[PPL_MSG_LENGTH];
    pplset_axis *target;

    // Log progress in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Back-propagating axis usage for axis %c%d on plot %d", "xyzc"[source->xyz],
                          source->axis_n, source->canvas_id);
        ppl_log(&x->c->errcontext, msg);
    }
    const canvas_item *item = x->current;

    // Propagating MinUsed and MaxUsed variables along links between axes
    int IterDepth = 0;
    while (1) // loop over as many iterations of linkage as may be necessary
    {
        if (IterDepth++ > 100) return;
        if (!(source->linked && (source->MinUsedSet || source->MaxUsedSet || source->DataUnitSet))) {
            break;
        } // proceed only if axis is linked and has usage information
        if (source->LinkedAxisCanvasID <= 0) {
        } // Linked to an axis on the same graph; do not change item
        else // Linked to an axis on a different canvas item
        {
            item = x->itemlist->first;
            while ((item != NULL) && ((item->id) < source->LinkedAxisCanvasID)) item = item->next;
            if ((item == NULL) || (item->id != source->LinkedAxisCanvasID) || (item->type != CANVAS_PLOT) ||
                (item->XAxes == NULL) || (item->YAxes == NULL) || (item->ZAxes == NULL)) {
                break;
            }
        }
        if (source->LinkedAxisToXYZ == 0) target = item->XAxes + source->LinkedAxisToNum;
        else if (source->LinkedAxisToXYZ == 1) target = item->YAxes + source->LinkedAxisToNum;
        else target = item->ZAxes + source->LinkedAxisToNum;
        // Propagate the usage through the linking expression, if there is one, by inverting it; otherwise copy it
        // directly
        if (source->linkusing != NULL) {
            eps_plot_LinkUsingBackPropagate(x, source->MinUsed, target, source);
            eps_plot_LinkUsingBackPropagate(x, source->MaxUsed, target, source);
        } else {
            if ((target->DataUnitSet && source->DataUnitSet) &&
                (!ppl_unitsDimEqual(&target->DataUnit, &source->DataUnit))) {
                ppl_snprintfTrunc(
                        msg, sizeof(msg),
                        "Axis %c%d of plot %d is linked to axis %c%d of plot %d, but axes have data plotted against them with conflicting physical units. The former has units of <%s> whilst the latter has units of <%s>.",
                        "xyzc"[source->xyz], source->axis_n, source->canvas_id, "xyzc"[target->xyz], target->axis_n,
                        target->canvas_id, ppl_printUnit(x->c, &target->DataUnit, NULL, NULL, 0, 1, 0),
                        ppl_printUnit(x->c, &source->DataUnit, NULL, NULL, 1, 1, 0));
                ppl_warning(&x->c->errcontext, ERR_GENERIC, msg);
                break;
            } else {
                // Widen the target's range to include the source's minimum
                if ((source->MinUsedSet) && ((!target->MinUsedSet) || (target->MinUsed > source->MinUsed))) {
                    target->MinUsed = source->MinUsed;
                    target->MinUsedSet = 1;
                }
                // Widen the target's range to include the source's maximum
                if ((source->MaxUsedSet) && ((!target->MaxUsedSet) || (target->MaxUsed < source->MaxUsed))) {
                    target->MaxUsed = source->MaxUsed;
                    target->MaxUsedSet = 1;
                }
                // Copy the units of the data
                if (source->DataUnitSet) {
                    target->DataUnit = source->DataUnit;
                    target->DataUnitSet = 1;
                }
            }
        }
        source = target;
    }
    return;
}

// Loop through all of the axes on a single plot item, and decide its range. If
// the axis is a linked axis, must first decide the range of the axis at the
// bottom of the linkage hierarchy. The function
// eps_plot_LinkedAxisForwardPropagate deals with this.

//! eps_plot_DecideAxisRanges - Decide the final ranges of all of the axes of a graph, taking account of the data
//! plotted against them, the ranges set by the user, and any axes they are linked to
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn

void eps_plot_DecideAxisRanges(EPSComm *x) {
    pplset_axis *axes = NULL;

    // Decide the range of each axis in turn
    for (int j = 0; j < 3; j++) {
        if (j == 0) axes = x->current->XAxes;
        else if (j == 1) axes = x->current->YAxes;
        else if (j == 2) axes = x->current->ZAxes;
        else ppl_fatal(&x->c->errcontext, __FILE__, __LINE__, "Internal fail.");
        // Decide the range of each axis, following any links, and then its ticks
        for (int i = 0; i < MAX_AXES; i++) {
            // Decide the range, if this has not already been done
            if (!axes[i].RangeFinalised) {
                eps_plot_LinkedAxisForwardPropagate(x, &axes[i], 1);
                if (*x->status) return;
            }
            // Decide the ticks, if this has not already been done
            if (!axes[i].TickListFinalised) {
                eps_plot_ticking(x, &axes[i], x->current->settings.AxisUnitStyle, NULL);
                if (*x->status) return;
            }
        }
    }
    return;
}

// As part of the process of determining the range of axis xyz[axis_n], check
// whether the axis is linking, and if so fetch usage information from the
// bottom of the linkage hierarchy. Propagate this information up through all
// intermediate levels of the hierarchy before calling
// eps_plot_DecideAxisRange().

//! eps_plot_LinkedAxisForwardPropagate - Copy the final range and ticks of the axis which an axis is linked to,
//! following chains of linked axes
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] axis - The axis whose range is to be set
//! \param [in] mode - Zero to report warnings about broken links; one to fail silently

void eps_plot_LinkedAxisForwardPropagate(EPSComm *x, pplset_axis *axis, int mode) {
    // Variables for following the chain of links
    char msg[PPL_MSG_LENGTH];
    const int OriginalMode = mode;
    pplset_axis *target2;

    // Propagate MinUsed and MaxUsed variables along links
    int IterDepth = 0;
    pplset_axis *target = axis;
    const canvas_item *item = x->current;

    while (1) // loop over as many iterations of linkage as may be necessary to find MinFinal and MaxFinal at the bottom
    {
        if (IterDepth++ > 100) break;
        if ((!target->linked) || target->RangeFinalised) { break; } // proceed only if axis is linked
        if (target->LinkedAxisCanvasID <= 0) {
        } // Linked to an axis on the same graph; do not change item
        else // Linked to an axis on a different canvas item
        {
            item = x->itemlist->first;
            while ((item != NULL) && (item->id) < target->LinkedAxisCanvasID) item = item->next;
            // Warn about a link to a multiplot item which does not exist, once
            if ((item == NULL) || (item->id != target->LinkedAxisCanvasID)) {
                // Warn only once, on the first link of the chain
                if ((IterDepth == 1) && (mode == 0)) {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Axis %c%d of plot %d is linked to axis %c%d of plot %d, but no such plot exists.",
                            "xyzc"[target->xyz], target->axis_n, target->canvas_id, "xyzc"[target->LinkedAxisToXYZ],
                            target->LinkedAxisToNum, target->LinkedAxisCanvasID);
                    ppl_warning(&x->c->errcontext, ERR_GENERIC, msg);
                }
                break;
            }
            // Warn about a link to a multiplot item which is not a plot, once
            if (item->type != CANVAS_PLOT) {
                // Warn only once, on the first link of the chain
                if ((IterDepth == 1) && (mode == 0)) {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Axis %c%d of plot %d is linked to axis %c%d of plot %d, but this canvas item is not a plot.",
                            "xyzc"[target->xyz], target->axis_n, target->canvas_id, "xyzc"[target->LinkedAxisToXYZ],
                            target->LinkedAxisToNum, target->LinkedAxisCanvasID);
                    ppl_warning(&x->c->errcontext, ERR_GENERIC, msg);
                }
                break;
            }
            // Warn about a link to an item with no axes, once
            if ((item->XAxes == NULL) || (item->YAxes == NULL) || (item->ZAxes == NULL)) {
                // Warn only once, on the first link of the chain
                if ((IterDepth == 1) && (mode == 0)) {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Axis %c%d of plot %d is linked to axis %c%d of plot %d, but this item has NULL axes.",
                            "xyzc"[target->xyz], target->axis_n, target->canvas_id, "xyzc"[target->LinkedAxisToXYZ],
                            target->LinkedAxisToNum, target->LinkedAxisCanvasID);
                    ppl_warning(&x->c->errcontext, ERR_INTERNAL, msg);
                }
                break;
            }
        }
        if (target->LinkedAxisToXYZ == 0) target2 = item->XAxes + target->LinkedAxisToNum;
        else if (target->LinkedAxisToXYZ == 1) target2 = item->YAxes + target->LinkedAxisToNum;
        else target2 = item->ZAxes + target->LinkedAxisToNum;
        if (target->DataUnitSet && target2->DataUnitSet && (!ppl_unitsDimEqual(&target->DataUnit, &target2->DataUnit)))
            break; // If axes are dimensionally incompatible, stop
        target = target2;
    }
    // Decide the range of the axis at the end of the chain
    if ((mode == 1) && (!target->RangeFinalised)) {
        eps_plot_ticking(x, target, x->current->settings.AxisUnitStyle, NULL);
        if (*x->status) return;
    }
    IterDepth -= 2;
    pplset_axis *source = target;
    for (; IterDepth >= 0; IterDepth--) // loop over as many iterations of linkage as may be necessary
    {
        target = axis;
        // Follow the chain of links from the starting axis to the axis this many links along it
        for (int k = 0; k < IterDepth; k++) {
            if (target->LinkedAxisCanvasID <= 0) {
                item = x->current;
            } // Linked to an axis on the same graph
            else // Linked to an axis on a different canvas item
            {
                item = x->itemlist->first;
                while ((item != NULL) && (item->id) < target->LinkedAxisCanvasID) item = item->next;
                if ((item == NULL) || (item->id != target->LinkedAxisCanvasID) || (item->type != CANVAS_PLOT) ||
                    (item->XAxes == NULL) || (item->YAxes == NULL) || (item->ZAxes == NULL)) {
                    break;
                }
            }
            if (target->LinkedAxisToXYZ == 0) target2 = item->XAxes + target->LinkedAxisToNum;
            else if (target->LinkedAxisToXYZ == 1) target2 = item->YAxes + target->LinkedAxisToNum;
            else target2 = item->ZAxes + target->LinkedAxisToNum;
            target = target2;
        }

        // Stop at an axis whose range has already been decided, or which is not linked
        if (target->RangeFinalised) { break; }
        if (!target->linked) { break; } // proceed only if axis is linked
        if ((OriginalMode == 0) && (target->Mode0BackPropagated)) continue;
        target->Mode0BackPropagated = 1;
        if (mode == 0) // MODE 0: Propagate HardMin, HardMax, HardUnit
        {
            // An axis linked through an expression takes the end axis's limits only if both are fixed
            if (target->linkusing != NULL) {
                // Fixed limits are copied through the expression by evaluating it, in mode 1
                if (source->HardMinSet && source->HardMaxSet) {
                    source->MinFinal = source->HardMin;
                    source->MaxFinal = source->HardMax;
                    source->DataUnit = source->HardUnit;
                    source->DataUnitSet = 1;
                    mode = 1;
                } else {
                    target->HardMinSet = target->HardMaxSet = target->HardUnitSet = 0;
                }
            } else {
                target->HardMinSet = source->HardMinSet;
                target->HardMin = source->HardMin;
                target->HardMaxSet = source->HardMaxSet;
                target->HardMax = source->HardMax;
                target->HardUnitSet = source->HardUnitSet;
                target->HardUnit = source->HardUnit;
                target->HardAutoMinSet = source->HardAutoMinSet;
                target->HardAutoMaxSet = source->HardAutoMaxSet;
            }
        }
        if (mode == 1) // MODE 1: Propagate finalised ranges
        {
            target->LogFinal = source->LogFinal;
            target->MinFinal = source->MinFinal;
            target->MaxFinal = source->MaxFinal;
            // An axis linked through an expression tabulates its mapping; otherwise it copies the range and units
            if (target->linkusing != NULL) {
                if (eps_plot_LinkedAxisLinkUsing(x, target, source)) break;
            } else {
                if (target->DataUnitSet && source->DataUnitSet &&
                    (!ppl_unitsDimEqual(&target->DataUnit, &source->DataUnit)))
                    break; // If axes are dimensionally incompatible, stop
                target->DataUnit = source->DataUnit;
                target->DataUnitSet = source->DataUnitSet;
                target->AxisValueTurnings = source->AxisValueTurnings;
                target->AxisLinearInterpolation = source->AxisLinearInterpolation;
                target->AxisTurnings = source->AxisTurnings;
            }
            target->RangeFinalised = 1;
            eps_plot_ticking(x, target, x->current->settings.AxisUnitStyle, source);
        }
        source = target;
    }
    return;
}

//! eps_plot_LinkedAxisLinkUsing - Set the range of an axis which is linked to another axis through a "using"
//! expression, by sampling the expression over the range of the other axis
//! \param [in] X - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] out - The axis whose range is to be set
//! \param [in] in - The axis it is linked to
//! \return Zero on success, or one on failure

int eps_plot_LinkedAxisLinkUsing(EPSComm *X, pplset_axis *out, pplset_axis *in) {
    // Variables for sampling the linking expression
    char msg[PPL_MSG_LENGTH];
    const int stkLevelOld = X->c->stackPtr;
    int xrn = 0, lOP;
    int oldsgn = -10;
    double x;
    char *VarName;
    pplObj DummyTemp, *VarVal;
    if (in->xyz == 0) VarName = "x";
    else if (in->xyz == 1) VarName = "y";
    else if (in->xyz == 2) VarName = "z";
    else VarName = "c";

    // Look up variable in user space and get pointer to its value
    ppl_contextGetVarPointer(X->c, VarName, &VarVal, &DummyTemp);

    // Allocate the table of values, and the list of turning points
    out->AxisLinearInterpolation = (double *) ppl_memAlloc(AXISLINEARINTERPOLATION_NPOINTS * sizeof(double));
    out->AxisValueTurnings = 0;
    out->AxisTurnings = (int *) ppl_memAlloc((AXISLINEARINTERPOLATION_NPOINTS + 2) * sizeof(int));

    // Failure to allocate memory abandons the link
    if ((out->AxisLinearInterpolation == NULL) || (out->AxisTurnings == NULL)) goto FAIL;
    out->AxisTurnings[xrn++] = 0;

    // Evaluate the linking expression at evenly spaced points along the source axis
    for (int l = 0; l < AXISLINEARINTERPOLATION_NPOINTS;
         l++) // Loop over samples we are going to take from axis linkage function
    {
        const double p = ((double) l) / (AXISLINEARINTERPOLATION_NPOINTS - 1);
        x = pplaxis_InvGetPosition(p, in);
        pplExpr *olu = (pplExpr *) out->linkusing;

        // Set value of x (or y/z)
        {
            const int om = VarVal->amMalloced;
            const int rc = VarVal->refCount;
            *VarVal = in->DataUnit;
            VarVal->amMalloced = om;
            VarVal->refCount = rc;
            VarVal->self_lval = NULL;
            VarVal->self_this = NULL;
            VarVal->imag = 0.0;
            VarVal->flagComplex = 0;
            VarVal->real = x;
        }

        // Generate a sample from the axis linkage function
        pplObj *outVal = ppl_expEval(X->c, olu, &lOP, 0, X->iterDepth + 1);
        // Report a failure to evaluate the expression
        if (X->c->errStat.status) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Error encountered whilst evaluating axis linkage expression: %s",
                              olu->ascii);
            ppl_error(&X->c->errcontext, ERR_GENERIC, -1, -1, msg);
            ppl_tbWrite(X->c);
            ppl_tbClear(X->c);
            goto FAIL;
        }
        // The expression must return a number
        if ((outVal->objType != PPLOBJ_NUM) && (outVal->objType != PPLOBJ_DATE) && (outVal->objType != PPLOBJ_BOOL)) {
            ppl_snprintfTrunc(
                    msg, sizeof(msg),
                    "Error encountered whilst evaluating axis linkage expression: %s\nObject was not a number but an object of type <%s>",
                    olu->ascii, pplObjTypeNames[outVal->objType]);
            ppl_error(&X->c->errcontext, ERR_TYPE, -1, -1, msg);
            goto FAIL;
        }
        // The expression must return a real number
        if (outVal->flagComplex) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Error encountered whilst evaluating axis linkage expression: %s",
                              olu->ascii);
            ppl_error(&X->c->errcontext, ERR_GENERIC, -1, -1, msg);
            ppl_error(&X->c->errcontext, ERR_GENERIC, -1, -1,
                      "Received a complex number; axes must have strictly real values at all points.");
            goto FAIL;
        }
        // The expression must return a finite value
        if (!gsl_finite(outVal->real)) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Error encountered whilst evaluating axis linkage expression: %s",
                              olu->ascii);
            ppl_error(&X->c->errcontext, ERR_GENERIC, -1, -1, msg);
            ppl_error(&X->c->errcontext, ERR_GENERIC, -1, -1, "Expression returned non-finite result.");
            goto FAIL;
        }
        // The first value sets the units of the axis
        if (!out->DataUnitSet) {
            out->DataUnit = *outVal;
            out->DataUnitSet = 1;
        }
        // All values must have the same units
        if (!ppl_unitsDimEqual(&out->DataUnit, outVal)) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Error encountered whilst evaluating axis linkage expression: %s",
                              olu->ascii);
            ppl_error(&X->c->errcontext, ERR_GENERIC, -1, -1, msg);
            ppl_snprintfTrunc(
                    msg, sizeof(msg),
                    "Axis linkage function produces axis values with dimensions of <%s> whilst data plotted on this axis has dimensions of <%s>.",
                    ppl_printUnit(X->c, outVal, NULL, NULL, 0, 1, 0),
                    ppl_printUnit(X->c, &out->DataUnit, NULL, NULL, 1, 1, 0));
            ppl_error(&X->c->errcontext, ERR_GENERIC, -1, -1, msg);
            goto FAIL;
        }
        out->AxisLinearInterpolation[l] = outVal->real;
        ppl_stackClean(X->c, stkLevelOld); // Discard anything left on the stack
        // Check for turning points; flat steps are ignored
        if (l > 0) {
            const int newsgn = ppl_sgn(out->AxisLinearInterpolation[l] - out->AxisLinearInterpolation[l - 1]);
            if (newsgn == 0) continue;
            if ((newsgn != oldsgn) && (oldsgn > -10)) out->AxisTurnings[xrn++] = l - 1;
            oldsgn = newsgn;
        }
    }

    // Restore original value of x (or y/z)
    ppl_contextRestoreVarPointer(X->c, VarName, &DummyTemp);
    out->AxisTurnings[xrn--] = AXISLINEARINTERPOLATION_NPOINTS - 1;
    out->AxisValueTurnings = xrn;
    return 0;

// On failure, tidy up and leave the axis without a tabulated mapping
FAIL:
    {
        const EPSComm *x = X;
        ppl_stackClean(x->c, stkLevelOld); // Discard anything left on the stack
    }
    ppl_contextRestoreVarPointer(X->c, VarName, &DummyTemp);
    out->AxisLinearInterpolation = NULL;
    out->AxisValueTurnings = 0;
    out->AxisTurnings = NULL;
    return 1;
}

// Routines for the back propagation of usage information along non-linear links

typedef struct LAUComm {
    EPSComm *x;
    pplExpr *expr;
    char *VarName;
    pplObj *VarValue;
    int GoneNaN, mode, iter2;
    double norm;
    pplObj target;
    double WorstScore;
    int fail;
} LAUComm;

#define WORSTSCORE_INIT -(DBL_MAX / 1e10)

//! twinLog - A logarithm which is extended smoothly to values below 1e-200 (including zero and negative values), so
//! that it can be used to compare values on a logarithmic scale without failing
//! \param [in] x - The value
//! \return log(x) for x > 1e-200, or a reflection of the logarithm about x = 1e-200 otherwise

static double twinLog(const double x) {
    return (x > 1e-200) ? log(x) : (2 * log(1e-200) - log(2e-200 - x));
}

//! eps_plot_LAUSlave - Cost function passed to the GSL minimiser by eps_plot_LAUFitter(), which evaluates the linkage
//! expression at a trial point and returns its distance from the target value
//! \param [in] x - The trial value
//! \param [in] params - Pointer to the LAUComm structure describing the problem
//! \return The absolute difference between the linkage expression and the target value

double eps_plot_LAUSlave(const gsl_vector *x, void *params) {
    // Variables for evaluating the expression
    char msg[PPL_MSG_LENGTH];
    int lOP;
    double output;
    LAUComm *data = (LAUComm *) params;
    const int stkLevelOld = data->x->c->stackPtr;

    if (data->fail) return GSL_NAN; // We've previously had a serious error... so don't do any more work
    if (data->mode == 0) data->VarValue->real = ppl_optimise_LogToReal(gsl_vector_get(x, 0), data->iter2, &data->norm);
    else
        data->VarValue->exponent[data->mode - 1] =
                ppl_optimise_LogToReal(gsl_vector_get(x, 0), data->iter2, &data->norm);

    // Set the units of the trial value
    data->VarValue->dimensionless = 1;
    for (int i = 0; i < UNITS_MAX_BASEUNITS; i++)
        // The value is dimensionless only if all of its unit's exponents are zero
        if (ppl_dblEqual(data->VarValue->exponent[i], 0) == 0) {
            data->VarValue->dimensionless = 0;
            break;
        }
    const pplObj *outVal = ppl_expEval(data->x->c, data->expr, &lOP, 0, data->x->iterDepth + 1);

    // If a numerical error happened; ignore it for now, but return NAN
    if (data->x->c->errStat.status) {
        ppl_snprintfTrunc(msg, sizeof(msg), "An algebraic error was encountered at %s=%s:", data->VarName,
                          ppl_unitsNumericDisplay(data->x->c, data->VarValue, 0, 0, 0));
        ppl_error(&data->x->c->errcontext, ERR_PREFORMED, -1, -1, msg);
        ppl_tbWrite(data->x->c);
        ppl_tbClear(data->x->c);
        output = GSL_NAN;
        goto end;
    }
    // The expression must return a number
    if ((outVal->objType != PPLOBJ_NUM) && (outVal->objType != PPLOBJ_DATE) && (outVal->objType != PPLOBJ_BOOL)) {
        ppl_snprintfTrunc(
                msg, sizeof(msg),
                "Error encountered whilst evaluating axis linkage expression: %s\nObject was not a number but an object of type <%s>",
                data->expr->ascii, pplObjTypeNames[outVal->objType]);
        ppl_error(&data->x->c->errcontext, ERR_TYPE, -1, -1, msg);
        output = GSL_NAN;
        goto end;
    }


    // The score is the squared difference between the logarithms of the result and the target value, or of one exponent
    // of their units
    if (data->mode == 0) output = pow(twinLog(data->target.real) - twinLog(outVal->real), 2);
    else output = pow(twinLog(data->target.exponent[data->mode - 1]) - twinLog(outVal->exponent[data->mode - 1]), 2);
    // If the score is not finite, return a score worse than any seen so far, which steers the minimiser away
    if ((!gsl_finite(output)) && (data->WorstScore > WORSTSCORE_INIT)) {
        data->WorstScore *= 1.1;
        return data->WorstScore;
    }
    if ((gsl_finite(output)) && (data->WorstScore < output)) { data->WorstScore = output; }

// Discard anything left on the stack
end:
    {
        const EPSComm *x = data->x;
        ppl_stackClean(x->c, stkLevelOld); // Discard anything left on the stack before giving up
    }
    return output;
}

//! eps_plot_LAUFitter - Find the value on an axis which the linkage expression maps onto a target value on a linked
//! axis, by numerical minimisation
//! \param [in] commlink - The LAUComm structure describing the problem; the result is stored in it

void eps_plot_LAUFitter(LAUComm *commlink) {
    // Variables for driving the GSL minimiser
    size_t iter = 0, iter2 = 0;
    int status = 0;
    double size = 0, sizelast = 0, sizelast2 = 0;
    const gsl_multimin_fminimizer_type *T =
            gsl_multimin_fminimizer_nmsimplex; // We don't use nmsimplex2 here because it was new in gsl 1.12
    gsl_multimin_function fn;

    // Describe the function to minimise to GSL
    fn.n = 1;
    fn.f = &eps_plot_LAUSlave;
    fn.params = (void *) commlink;

    // The position of the minimiser, and its initial step size
    gsl_vector *x = gsl_vector_alloc(1);
    gsl_vector *ss = gsl_vector_alloc(1);

    // Restart the minimiser several times, transforming the variable differently on each pass, until the result stops
    // improving; when fitting the exponents of units, a single pass suffices
    iter2 = 0;
    // Each pass of the minimiser
    do {
        iter2++;
        if (commlink->mode != 0) iter2 = 999;
        commlink->iter2 = iter2;
        sizelast2 = size;
        gsl_vector_set(
                x, 0,
                ppl_optimise_RealToLog((commlink->mode == 0) ? commlink->VarValue->real : 1.0, iter2, &commlink->norm));
        gsl_vector_set(ss, 0, (fabs(gsl_vector_get(x, 0)) > 1e-6) ? 0.1 * (gsl_vector_get(x, 0)) : 0.1);
        gsl_multimin_fminimizer *s = gsl_multimin_fminimizer_alloc(T, fn.n);
        gsl_multimin_fminimizer_set(s, &fn, x, ss);

        // If initial value we are giving the minimiser produces an algebraic error, it's not worth continuing
        commlink->GoneNaN = 0;
        commlink->WorstScore = WORSTSCORE_INIT;
        const double testval = eps_plot_LAUSlave(x, (void *) commlink);
        // Give up if the expression cannot be evaluated at the starting point
        if (!gsl_finite(testval)) {
            commlink->fail = 1;
            gsl_multimin_fminimizer_free(s);
            s = NULL;
            gsl_vector_free(x);
            x = NULL;
            gsl_vector_free(ss);
            ss = NULL;
            return;
        }
        iter = 0;
        commlink->GoneNaN = 0;
        commlink->WorstScore = WORSTSCORE_INIT;
        // Iterate the minimiser until the simplex stops shrinking
        do {
            iter++;
            // Four iterations at a time
            for (int i = 0; i < 4; i++) {
                status = gsl_multimin_fminimizer_iterate(s);
                if (status) break;
            } // Run optimiser four times for good luck
            if (status) break;
            sizelast = size;
            size = gsl_multimin_fminimizer_minimum(s); // Do this ten times, or up to 50, until fit stops improving
        } while ((iter < 10) ||
                 ((size < sizelast) &&
                  (iter < 50))); // Iterate 10 times, and then see whether size carries on getting smaller

        // Read off best-fit value from s->x
        if (commlink->mode == 0)
            commlink->VarValue->real = ppl_optimise_LogToReal(gsl_vector_get(s->x, 0), iter2, &commlink->norm);
        else
            commlink->VarValue->exponent[commlink->mode - 1] =
                    ppl_optimise_LogToReal(gsl_vector_get(s->x, 0), iter2, &commlink->norm);
        gsl_multimin_fminimizer_free(s);
        s = NULL;
        // Only one pass is needed when fitting an exponent
        if (iter2 == 999) {
            iter2 = 2;
            break;
        }
    } while ((iter2 < 4) ||
             ((commlink->GoneNaN == 0) && (!status) && (size < sizelast2) &&
              (iter2 < 20))); // Iterate 2 times, and then see whether size carries on getting smaller

    // Too many passes means the minimiser failed to converge
    if (iter2 >= 20) status = 1;
    if (status) { commlink->fail = 1; }
    gsl_vector_free(x);
    x = NULL;
    gsl_vector_free(ss);
    ss = NULL;
    return;
}

//! eps_plot_LinkUsingBackPropagate - Propagate a value plotted against an axis which is linked through a "using"
//! expression back to the axis it is linked to, by inverting the linkage expression
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] val - The value plotted
//! \param [in] target - The axis which <source> is linked to
//! \param [in] source - The linked axis

void eps_plot_LinkUsingBackPropagate(EPSComm *x, const double val, pplset_axis *target, pplset_axis *source) {
    // Variables for inverting the expression
    char msg[PPL_MSG_LENGTH];
    LAUComm commlink;
    char *VarName;
    pplObj DummyTemp, *VarVal;

    // There is no need to propagate usage to an axis whose range is fixed
    if (target->HardMinSet && target->HardMaxSet) return;

    // The variable which holds the value: x, y, z or c, according to the axis
    if (target->xyz == 0) VarName = "x";
    else if (target->xyz == 1) VarName = "y";
    else if (target->xyz == 2) VarName = "z";
    else VarName = "c";

    // Look up xyz dummy variable in user space and get pointer to its value
    ppl_contextGetVarPointer(x->c, VarName, &VarVal, &DummyTemp);

    // Set up commlink structure to use in minimiser
    commlink.x = x;
    commlink.expr = (pplExpr *) source->linkusing;
    commlink.VarName = VarName;
    commlink.VarValue = VarVal;
    commlink.GoneNaN = commlink.mode = 0;
    commlink.WorstScore = WORSTSCORE_INIT;
    commlink.target = source->DataUnit;
    commlink.target.real = val;
    commlink.fail = 0;

    // First run minimiser to get numerical value of xyz. Then fit each dimension in turn.
    pplObjNum(VarVal, 0, 1, 0);
    // First fit the numerical value, then the exponent of each base unit in turn
    for (commlink.mode = 0; commlink.mode < UNITS_MAX_BASEUNITS + 1; commlink.mode++) {
        // if ((commlink.mode==UNIT_ANGLE+1)&&(x->c->set->term_current.UnitAngleDimless == SW_ONOFF_ON)) continue;
        eps_plot_LAUFitter(&commlink);
        if (commlink.fail) break;
    }

    // If there was a problem, throw a warning and proceed no further
    if (commlink.fail) {
        ppl_snprintfTrunc(
                msg, sizeof(msg),
                "Could not propagate axis range information from axis %c%d of plot %d to axis %c%d of plot %d using expression <%s>. Recommend setting an explicit range for axis %c%d of plot %d.",
                "xyzc"[source->xyz], source->axis_n, source->canvas_id, "xyzc"[target->xyz], target->axis_n,
                target->canvas_id, commlink.expr->ascii, "xyzc"[target->xyz], target->axis_n, target->canvas_id);
        ppl_warning(&x->c->errcontext, ERR_GENERIC, msg);
    } else {
        VarVal->dimensionless =
                1; // Cycle through powers of final value of xyz dummy value making things which are nearly ints into ints.
        // Round exponents which are very close to integers
        for (int i = 0; i < UNITS_MAX_BASEUNITS; i++) {
            if (fabs(floor(VarVal->exponent[i]) - VarVal->exponent[i]) < 1e-12)
                VarVal->exponent[i] = floor(VarVal->exponent[i]);
            else if (fabs(ceil(VarVal->exponent[i]) - VarVal->exponent[i]) < 1e-12)
                VarVal->exponent[i] = ceil(VarVal->exponent[i]);
            if (VarVal->exponent[i] != 0) VarVal->dimensionless = 0;
        }

        // Check that dimension of propagated value fits with existing unit of axis
        if ((target->HardUnitSet) && (!ppl_unitsDimEqual(&target->HardUnit, VarVal))) {
            ppl_snprintfTrunc(
                    msg, sizeof(msg),
                    "Could not propagate axis range information from axis %c%d of plot %d to axis %c%d of plot %d using expression <%s>. Propagated axis range has units of <%s>, but axis %c%d of plot %d has a range set with units of <%s>.",
                    "xyzc"[source->xyz], source->axis_n, source->canvas_id, "xyzc"[target->xyz], target->axis_n,
                    target->canvas_id, commlink.expr->ascii, ppl_printUnit(x->c, VarVal, NULL, NULL, 0, 1, 0),
                    "xyzc"[target->xyz], target->axis_n, target->canvas_id,
                    ppl_printUnit(x->c, &target->HardUnit, NULL, NULL, 1, 1, 0));
            ppl_warning(&x->c->errcontext, ERR_GENERIC, msg);
        } else if ((target->DataUnitSet) && (!ppl_unitsDimEqual(&target->DataUnit, VarVal))) {
            ppl_snprintfTrunc(
                    msg, sizeof(msg),
                    "Could not propagate axis range information from axis %c%d of plot %d to axis %c%d of plot %d using expression <%s>. Propagated axis range has units of <%s>, but axis %c%d of plot %d has data plotted against it with units of <%s>.",
                    "xyzc"[source->xyz], source->axis_n, source->canvas_id, "xyzc"[target->xyz], target->axis_n,
                    target->canvas_id, commlink.expr->ascii, ppl_printUnit(x->c, VarVal, NULL, NULL, 0, 1, 0),
                    "xyzc"[target->xyz], target->axis_n, target->canvas_id,
                    ppl_printUnit(x->c, &target->DataUnit, NULL, NULL, 1, 1, 0));
            ppl_warning(&x->c->errcontext, ERR_GENERIC, msg);
        } else if (VarVal->flagComplex) {
            ppl_snprintfTrunc(
                    msg, sizeof(msg),
                    "Could not propagate axis range information from axis %c%d of plot %d to axis %c%d of plot %d using expression <%s>. Axis usage was a complex number.",
                    "xyzc"[source->xyz], source->axis_n, source->canvas_id, "xyzc"[target->xyz], target->axis_n,
                    target->canvas_id, commlink.expr->ascii);
            ppl_warning(&x->c->errcontext, ERR_GENERIC, msg);
        } else if (!gsl_finite(VarVal->real)) {
            ppl_snprintfTrunc(
                    msg, sizeof(msg),
                    "Could not propagate axis range information from axis %c%d of plot %d to axis %c%d of plot %d using expression <%s>. Axis usage was a non-finite number.",
                    "xyzc"[source->xyz], source->axis_n, source->canvas_id, "xyzc"[target->xyz], target->axis_n,
                    target->canvas_id, commlink.expr->ascii);
            ppl_warning(&x->c->errcontext, ERR_GENERIC, msg);
        } else {
            // Widen the target's range to include the value
            if ((!target->MinUsedSet) || (target->MinUsed > VarVal->real)) {
                target->MinUsed = VarVal->real;
                target->MinUsedSet = 1;
            }
            // Widen the target's range to include the value
            if ((!target->MaxUsedSet) || (target->MaxUsed < VarVal->real)) {
                target->MaxUsed = VarVal->real;
                target->MaxUsedSet = 1;
            }
            // Copy the units of the value
            if (source->DataUnitSet) {
                target->DataUnit = *VarVal;
                target->DataUnitSet = 1;
            }
        }
    }

    // Restore original value of x (or y/z)
    ppl_contextRestoreVarPointer(x->c, VarName, &DummyTemp);
    return;
}
