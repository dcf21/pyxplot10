// eps_plot_ticking_auto3.c
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

// One of the algorithms for placing ticks automatically along an axis, chosen by eps_plot_ticking.c. Method 3 is used
// for an axis linked to another axis of the same length: it copies that axis's ticks, so that the two match. If this is
// not possible, it falls back to method 2 (eps_plot_ticking_auto2.c).

// METHOD 3: This axis is linked to another axis of the same length. Use the
// same ticking scheme.

#define _PPL_EPS_PLOT_TICKING_AUTO3_C 1

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
#include "epsMaker/eps_plot_ticking_auto2.h"
#include "epsMaker/eps_plot_ticking_auto3.h"

//! eps_plot_ticking_auto3 - Place ticks automatically on an axis which is linked, without a "using" expression, to
//! another axis of the same length, by copying that axis's ticks; tick labels are reformatted if the axis has its own
//! format. Falls back to eps_plot_ticking_auto2() if this is not possible.
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] axis - The axis
//! \param [in] UnitMultiplier - The factor by which values on the axis are multiplied to express them in the units
//! shown on the axis
//! \param [in] AutoTicks - Array of two flags indicating whether major and minor ticks are to be placed automatically
//! \param [in] linkedto - The axis this axis is linked to

void eps_plot_ticking_auto3(EPSComm *x, pplset_axis *axis, const double UnitMultiplier, const unsigned char *AutoTicks,
                            const pplset_axis *linkedto) {
    int i, Nticks_maj, Nticks_min;

    // Log which algorithm is in use, and note the memory context in which tick labels are to be allocated
    if (DEBUG) ppl_log(&x->c->errcontext, "Using eps_plot_ticking_auto3()");
    const int OutContext = ppl_memAlloc_GetMemContext();

    // This method only applies to axes linked, without a using expression, to an axis of the same length which has
    // ticks
    if (linkedto == NULL) goto FAIL;
    if (axis->linkusing != NULL) goto FAIL;
    if (linkedto->PhysicalLengthMinor != axis->PhysicalLengthMinor) goto FAIL;
    if ((linkedto->TickListStrings == NULL) || (linkedto->MTickListStrings == NULL)) goto FAIL;

    // Count the major and minor ticks on the other axis
    for (Nticks_maj = 0; linkedto->TickListStrings[Nticks_maj] != NULL; Nticks_maj++);
    for (Nticks_min = 0; linkedto->MTickListStrings[Nticks_min] != NULL; Nticks_min++);

    // Use the same tick positions
    axis->TickListPositions = linkedto->TickListPositions;
    axis->MTickListPositions = linkedto->MTickListPositions;

    // Use the same tick labels, unless this axis has its own format
    if (axis->format == NULL) {
        axis->TickListStrings = linkedto->TickListStrings;
    } else {
        axis->TickListStrings = (char **) ppl_memAlloc((Nticks_maj + 1) * sizeof(char *));
        if (axis->TickListStrings == NULL) goto FAIL;

        // Label each tick with this axis's format, using exactly zero for values which straddle zero
        for (i = 0; i < Nticks_maj; i++) {
            double xtmpA = pplaxis_InvGetPosition(axis->TickListPositions[i] - 1e-8, axis) * UnitMultiplier;
            double xtmp = pplaxis_InvGetPosition(axis->TickListPositions[i], axis) * UnitMultiplier;
            double xtmpB = pplaxis_InvGetPosition(axis->TickListPositions[i] + 1e-8, axis) * UnitMultiplier;
            if ((gsl_finite(xtmpA)) && (gsl_finite(xtmpB)) &&
                (((xtmpA <= 0.0) && (xtmpB >= 0.0)) || ((xtmpA >= 0.0) && (xtmpB <= 0.0))))
                xtmp = 0.0;
            TickLabelFromFormat(x, &axis->TickListStrings[i], axis->format, xtmp, &axis->DataUnit, axis->xyz,
                                OutContext);
        }
        axis->TickListStrings[i] = NULL;
    }

    // Minor ticks have no labels
    axis->MTickListStrings = (char **) ppl_memAlloc((Nticks_min + 1) * sizeof(char *));
    if (axis->MTickListStrings == NULL) goto FAIL;
    for (i = 0; i < Nticks_min; i++) axis->MTickListStrings[i] = "";
    axis->MTickListStrings[i] = NULL;

    // Finished
    goto CLEANUP;

// If this method cannot be used, fall back to method 2
FAIL:
    if (DEBUG) ppl_log(&x->c->errcontext, "eps_plot_ticking_auto3() has failed");
    eps_plot_ticking_auto2(x, axis, UnitMultiplier, AutoTicks, linkedto);

// Done
CLEANUP:
    return;
}
