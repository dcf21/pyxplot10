// axes.c
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

#define _AXES_C 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <gsl/gsl_math.h>

#include "stringTools/asciidouble.h"
#include "stringTools/strConstants.h"

#include "coreUtils/dict.h"
#include "coreUtils/list.h"
#include "coreUtils/errorReport.h"

#include "expressions/expCompile_fns.h"

#include "pplConstants.h"
#include "settings/settingTypes.h"

#include "settings/axes_fns.h"
#include "settings/settings.h"
#include "settings/withWords_fns.h"

#include "userspace/context.h"
#include "userspace/pplObj_fns.h"
#include "userspace/unitsArithmetic.h"

// ------------------------------------------------------
// Functions for creating and destroying axis descriptors
// ------------------------------------------------------

//! pplaxis_destroy - Free all of the memory allocated within an axis descriptor, including its format expression,
//! label, linkage expression and tick lists. The descriptor itself is not freed.
//! \param [in] context - The Pyxplot context
//! \param [in|out] in - The axis descriptor to clean up

void pplaxis_destroy(const ppl_context *context, pplset_axis *in) {
    int i;
    if (in->format != NULL) {
        pplExpr_free(in->format);
        in->format = NULL;
    }
    if (in->label != NULL) {
        free(in->label);
        in->label = NULL;
    }
    if (in->linkusing != NULL) {
        pplExpr_free((pplExpr *) in->linkusing);
        in->linkusing = NULL;
    }
    if (in->ticsM.tickList != NULL) {
        free(in->ticsM.tickList);
        in->ticsM.tickList = NULL;
    }
    if (in->ticsM.tickStrs != NULL) {
        for (i = 0; in->ticsM.tickStrs[i] != NULL; i++) free(in->ticsM.tickStrs[i]);
        free(in->ticsM.tickStrs);
        in->ticsM.tickStrs = NULL;
    }
    if (in->tics.tickList != NULL) {
        free(in->tics.tickList);
        in->tics.tickList = NULL;
    }
    if (in->tics.tickStrs != NULL) {
        for (i = 0; in->tics.tickStrs[i] != NULL; i++) free(in->tics.tickStrs[i]);
        free(in->tics.tickStrs);
        in->tics.tickStrs = NULL;
    }
    return;
}

// c->set->axis_default is a safe fallback axis because it contains no malloced strings
#define XMALLOC(X) \
    (tmp = malloc(X)); \
    if (tmp == NULL) { \
        ppl_error(&context->errcontext, ERR_MEMORY, -1, -1, "Out of memory"); \
        *out = context->set->axis_default; \
        return; \
    }

//! pplaxis_copy - Make a deep copy of an axis descriptor
//! \param [in] context - The Pyxplot context
//! \param [out] out - The axis descriptor to write the copy into
//! \param [in] in - The axis descriptor to copy

void pplaxis_copy(ppl_context *context, pplset_axis *out, const pplset_axis *in) {
    void *tmp;
    *out = *in;
    if (in->format != NULL) { out->format = (void *) pplExpr_cpy((pplExpr *) in->format); }
    if (in->label != NULL) {
        out->label = (char *) XMALLOC(strlen(in->label) + 1);
        strcpy(out->label, in->label);
    }
    if (in->linkusing != NULL) { out->linkusing = (void *) pplExpr_cpy((pplExpr *) in->linkusing); }
    pplaxis_copyTics(context, out, in);
    pplaxis_copyMTics(context, out, in);
    return;
}

//! pplaxis_copyTics - Make a deep copy of the list of major ticks of an axis, replacing those in <out>
//! \param [in] context - The Pyxplot context
//! \param [out] out - The axis descriptor to copy the ticks into
//! \param [in] in - The axis descriptor to copy the ticks from

void pplaxis_copyTics(ppl_context *context, pplset_axis *out, const pplset_axis *in) {
    int i = 0, j;
    void *tmp;
    if (in->tics.tickStrs != NULL) {
        for (i = 0; in->tics.tickStrs[i] != NULL; i++);
        out->tics.tickStrs = XMALLOC((i + 1) * sizeof(char *));
        for (j = 0; j < i; j++) {
            out->tics.tickStrs[j] = XMALLOC(strlen(in->tics.tickStrs[j]) + 1);
            strcpy(out->tics.tickStrs[j], in->tics.tickStrs[j]);
        }
        out->tics.tickStrs[i] = NULL;
    }
    if (in->tics.tickList != NULL) {
        out->tics.tickList = (double *) XMALLOC((i + 1) * sizeof(double));
        memcpy(out->tics.tickList, in->tics.tickList,
               (i +
                1) * sizeof(double)); // NB: For this to be safe, tics.tickLists MUST have double to correspond to NULL in tics.tickStrs
    }
    return;
}

//! pplaxis_copyMTics - Make a deep copy of the list of minor ticks of an axis, replacing those in <out>
//! \param [in] context - The Pyxplot context
//! \param [out] out - The axis descriptor to copy the ticks into
//! \param [in] in - The axis descriptor to copy the ticks from

void pplaxis_copyMTics(ppl_context *context, pplset_axis *out, const pplset_axis *in) {
    int i = 0, j;
    void *tmp;
    if (in->ticsM.tickStrs != NULL) {
        for (i = 0; in->ticsM.tickStrs[i] != NULL; i++);
        out->ticsM.tickStrs = XMALLOC((i + 1) * sizeof(char *));
        for (j = 0; j < i; j++) {
            out->ticsM.tickStrs[j] = XMALLOC(strlen(in->ticsM.tickStrs[j]) + 1);
            strcpy(out->ticsM.tickStrs[j], in->ticsM.tickStrs[j]);
        }
        out->ticsM.tickStrs[i] = NULL;
    }
    if (in->ticsM.tickList != NULL) {
        out->ticsM.tickList = (double *) XMALLOC((i + 1) * sizeof(double));
        memcpy(out->ticsM.tickList, in->ticsM.tickList,
               (i +
                1) * sizeof(double)); // NB: For this to be safe, tics.tickLists MUST have double to correspond to NULL in tics.tickStrs
    }
    return;
}

//! pplaxis_cmpTics - Test whether two axes' tick settings are identical
//! \param [in] context - The Pyxplot context
//! \param [in] ta - The tick settings of the first axis
//! \param [in] tb - The tick settings of the second axis
//! \param [in] ua - The physical units of the first axis
//! \param [in] ub - The physical units of the second axis
//! \param [in] la - Flag indicating whether the first axis is logarithmic
//! \param [in] lb - Flag indicating whether the second axis is logarithmic
//! \return One if the tick settings are identical, zero otherwise

unsigned char pplaxis_cmpTics(const ppl_context *context, const pplset_tics *ta, const pplset_tics *tb,
                              const pplObj *ua, const pplObj *ub, const int la, const int lb) {
    int i, j;
    if ((ta->tickList == NULL) && (tb->tickList == NULL) && (!ta->tickMinSet) && (!tb->tickMinSet) &&
        (!ta->tickStepSet) && (!tb->tickStepSet) && (!ta->tickMaxSet) && (!tb->tickMaxSet))
        return 1;
    if (!ppl_unitsDimEqual(ua, ub)) return 0;
    if ((ta->tickList == NULL) != (tb->tickList == NULL)) return 0;
    if (ta->tickList == NULL) {
        if (la != lb) return 0;
        if (ta->tickMinSet != tb->tickMinSet) return 0;
        if (ta->tickMaxSet != tb->tickMaxSet) return 0;
        if (ta->tickStepSet != tb->tickStepSet) return 0;
        if (ta->tickMinSet && (ta->tickMin != tb->tickMin)) return 0;
        if (ta->tickMaxSet && (ta->tickMax != tb->tickMax)) return 0;
        if (ta->tickStepSet && (ta->tickStep != tb->tickStep)) return 0;
    } else {
        for (i = 0; ta->tickStrs[i] != NULL; i++);
        for (j = 0; tb->tickStrs[j] != NULL; j++);
        if (i != j) return 0; // tick lists have different lengths
        for (j = 0; j < i; j++) {
            if (ta->tickList[j] != tb->tickList[j]) return 0;
            if (strcmp(ta->tickStrs[j], tb->tickStrs[j]) != 0) return 0;
        }
    }
    return 1;
}

//! pplaxis_GetPosition - Return the fractional position along an axis at which a value should be plotted
//! \param [in] xin - The value to locate on the axis
//! \param [in] xa - The axis, or NULL to return <xin> unchanged
//! \param [in] xrn - For axes whose values are linearly interpolated from a non-monotonic function, the number of
//! the monotonic region of the axis in which to look for <xin>
//! \param [in] AllowOffBounds - If true, values outside the range of the axis return positions outside the range
//! 0-1; otherwise they return NaN
//! \return The position along the axis, in the range 0 (left/bottom) to 1 (right/top), or NaN if the value cannot be
//! placed on the axis

double pplaxis_GetPosition(const double xin, const pplset_axis *xa, const int xrn, const unsigned char AllowOffBounds) {
    int imin, imax, i;
    if (xa == NULL) return xin;
    if (xa->AxisLinearInterpolation != NULL) // Axis is linearly interpolated
    {
        imin = xa->AxisTurnings[xrn];
        imax = xa->AxisTurnings[xrn + 1];
        for (i = imin; i < imax; i++) {
            if (((xa->AxisLinearInterpolation[i] < xin) && (xa->AxisLinearInterpolation[i + 1] >= xin)) ||
                ((xa->AxisLinearInterpolation[i] > xin) && (xa->AxisLinearInterpolation[i + 1] <= xin)))
                return (i +
                        (xin - xa->AxisLinearInterpolation[i]) /
                                (xa->AxisLinearInterpolation[i + 1] - xa->AxisLinearInterpolation[i])) /
                        (AXISLINEARINTERPOLATION_NPOINTS - 1);
        }
        return GSL_NAN;
    }
    if (!AllowOffBounds) {
        if (xa->MaxFinal > xa->MinFinal) {
            if ((xin < xa->MinFinal) || (xin > xa->MaxFinal)) return GSL_NAN;
        } else {
            if ((xin > xa->MinFinal) || (xin < xa->MaxFinal)) return GSL_NAN;
        }
    }
    if ((xa->LogFinal == SW_BOOL_TRUE) && (xin <= 0)) return GSL_NAN;
    if (xa->LogFinal != SW_BOOL_TRUE) return (xin - xa->MinFinal) / (xa->MaxFinal - xa->MinFinal); // Either linear...
    else return (log(xin) - log(xa->MinFinal)) / (log(xa->MaxFinal) - log(xa->MinFinal)); // ... or logarithmic
}

//! pplaxis_InvGetPosition - Return the value of an axis at a fractional position along it. This is the inverse of
//! pplaxis_GetPosition().
//! \param [in] xin - The position along the axis, in the range 0 (left/bottom) to 1 (right/top)
//! \param [in] xa - The axis
//! \return The value of the axis at position <xin>

double pplaxis_InvGetPosition(const double xin, const pplset_axis *xa) {
    if (xa->AxisLinearInterpolation != NULL) // Axis is linearly interpolated
    {
        const int i = floor(xin * (AXISLINEARINTERPOLATION_NPOINTS - 1));
        const double x = xin * (AXISLINEARINTERPOLATION_NPOINTS - 1) - i;
        if (i >= AXISLINEARINTERPOLATION_NPOINTS - 1)
            return xa->AxisLinearInterpolation[AXISLINEARINTERPOLATION_NPOINTS - 1];
        if (i < 0) return xa->AxisLinearInterpolation[0];
        return xa->AxisLinearInterpolation[i] * (1 - x) + xa->AxisLinearInterpolation[i + 1] * x;
    }
    if (xa->LogFinal != SW_BOOL_TRUE) return xa->MinFinal + xin * (xa->MaxFinal - xa->MinFinal); // Either linear...
    else return xa->MinFinal * pow(xa->MaxFinal / xa->MinFinal, xin); // ... or logarithmic
}
