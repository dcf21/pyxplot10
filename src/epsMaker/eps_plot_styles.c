// eps_plot_styles.c
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

// Draws datasets on graphs in each of Pyxplot's plot styles (points, lines, error bars, boxes, steps, arrows, filled
// regions, surfaces and so on), and the icons which represent them in legends. Also assigns default colors, line types
// and point types to successive datasets; says how many columns of data each style needs; and, while data are read,
// widens the ranges of the axes to include the data, checking that the data have consistent units. Colormaps and
// contourmaps are drawn by eps_plot_colormap.c and eps_plot_contourmap.c.

#define _PPL_EPS_PLOT_STYLES_C 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <gsl/gsl_math.h>

#include "coreUtils/memAlloc.h"

#include "epsMaker/canvasDraw.h"
#include "canvasItems.h"
#include "coreUtils/errorReport.h"
#include "mathsTools/dcfmath.h"
#include "settings/epsColors.h"
#include "settings/settingTypes.h"
#include "stringTools/asciidouble.h"
#include "userspace/pplObjPrint.h"
#include "userspace/unitsDisp.h"
#include "userspace/unitsArithmetic.h"

#include "epsMaker/eps_arrow.h"
#include "epsMaker/eps_comm.h"
#include "epsMaker/eps_core.h"
#include "epsMaker/eps_plot.h"
#include "epsMaker/eps_plot_canvas.h"
#include "epsMaker/eps_plot_filledregion.h"
#include "epsMaker/eps_plot_linedraw.h"
#include "epsMaker/eps_plot_styles.h"
#include "epsMaker/eps_plot_threedimbuff.h"
#include "epsMaker/eps_settings.h"
#include "epsMaker/eps_style.h"

//! eps_withwords_default - Work out the default plot style modifiers for a plot item, cycling through colors, line
//! types and point types for successive items
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [out] output - The default plot style modifiers
//! \param [in] sg - The graph settings
//! \param [in] functions - If true, the plot item is a function rather than a data file
//! \param [in] Ccounter - The number of plot items which have been assigned colors so far
//! \param [in] LTcounter - The number of plot items which have been assigned line types so far
//! \param [in] PTcounter - The number of plot items which have been assigned point types so far
//! \param [in] color - If true, cycle through colors; otherwise plot everything in black and cycle through line types

void eps_withwords_default(const EPSComm *x, withWords *output, const pplset_graph *sg, const unsigned char functions,
                           const int Ccounter, const int LTcounter, const int PTcounter, const unsigned char color) {
    int j;

    // Start from blank style modifiers
    ppl_withWordsZero(x->c, output);
    // In monochrome, plot everything in black; in color, use the next color of the palette
    if (!color) {
        output->color = COLOR_BLACK;
        output->USEcolor = 1;
    } else {
        for (j = 0; j < PALETTE_LENGTH; j++)
            if (x->c->set->palette_current[j] == -1) break; // j now contains length of palette
        int i = Ccounter % j; // i is now the palette color number to use
        while (i < 0) i += j;
        // A named color, or one given by its components
        if (x->c->set->palette_current[i] > 0) {
            output->color = x->c->set->palette_current[i];
            output->USEcolor = 1;
        } else {
            output->Col1234Space = x->c->set->paletteS_current[i];
            output->color1 = x->c->set->palette1_current[i];
            output->color2 = x->c->set->palette2_current[i];
            output->color3 = x->c->set->palette3_current[i];
            output->color4 = x->c->set->palette4_current[i];
            output->USEcolor1234 = 1;
        }
        output->AUTOcolor = Ccounter + 5;
    }
    output->linespoints = (functions ? SW_STYLE_LINES : SW_STYLE_POINTS);
    output->USElinespoints = 1;
    output->fillcolor = COLOR_NULL;
    output->USEfillcolor = 1;
    output->linetype = color ? 1 : LTcounter + 1;
    output->pointtype = PTcounter + 1;
    output->USElinetype = output->USEpointtype = 1;
    output->AUTOlinetype = !color;
    output->AUTOpointtype = PTcounter + 6;
    output->linewidth = sg->LineWidth;
    output->pointlinewidth = sg->PointLineWidth;
    output->pointsize = sg->PointSize;
    output->USElinewidth = output->USEpointlinewidth = output->USEpointsize = 1;
    return;
}

//! eps_withwords_default_counterinc - Advance the counters of colors, line types and point types used by
//! eps_withwords_default(), after a plot item has been drawn
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in|out] Ccounter - The number of plot items which have been assigned colors so far
//! \param [in|out] LTcounter - The number of plot items which have been assigned line types so far
//! \param [in|out] PTcounter - The number of plot items which have been assigned point types so far
//! \param [in] color - If true, colors are cycled; otherwise line types are cycled
//! \param [in] ww_final - The plot style modifiers with which the plot item was drawn, used to skip counters which it
//! set explicitly
//! \param [in] sg - The graph settings

void eps_withwords_default_counterinc(const EPSComm *x, int *Ccounter, int *LTcounter, int *PTcounter,
                                      const unsigned char color, const withWords *ww_final, const pplset_graph *sg) {
    const int style = ww_final->linespoints;

    // Advance each counter which this dataset used, so that the next dataset gets the next color, line type or point
    // type
    if (ww_final->AUTOcolor && (style != SW_STYLE_COLORMAP) && (style != SW_STYLE_CONTOURMAP)) { (*Ccounter)++; }

    // Styles which draw lines use up a line type
    if (ww_final->AUTOlinetype &&
        ((style == SW_STYLE_ARROWS_HEAD) || (style == SW_STYLE_ARROWS_NOHEAD) || (style == SW_STYLE_ARROWS_TWOHEAD) ||
         (style == SW_STYLE_BOXES) || (style == SW_STYLE_FILLEDREGION) || (style == SW_STYLE_FSTEPS) ||
         (style == SW_STYLE_HISTEPS) || (style == SW_STYLE_IMPULSES) || (style == SW_STYLE_LINES) ||
         (style == SW_STYLE_LINESPOINTS) || (style == SW_STYLE_STEPS) || (style == SW_STYLE_SURFACE) ||
         (style == SW_STYLE_WBOXES) || (style == SW_STYLE_XERRORBARS) || (style == SW_STYLE_XERRORRANGE) ||
         (style == SW_STYLE_XYERRORBARS) || (style == SW_STYLE_XYERRORRANGE) || (style == SW_STYLE_XYZERRORBARS) ||
         (style == SW_STYLE_XYZERRORRANGE) || (style == SW_STYLE_YERRORBARS) || (style == SW_STYLE_YERRORRANGE) ||
         (style == SW_STYLE_YZERRORBARS) || (style == SW_STYLE_YZERRORRANGE) || (style == SW_STYLE_ZERRORBARS) ||
         (style == SW_STYLE_ZERRORRANGE))) {
        (*LTcounter)++;
    }

    // Styles which draw points use up a point type
    if (ww_final->AUTOpointtype &&
        ((style == SW_STYLE_LINESPOINTS) || (style == SW_STYLE_POINTS) || (style == SW_STYLE_STARS))) {
        (*PTcounter)++;
    }

    // Contourmaps use up a color and line type for each contour
    if (style == SW_STYLE_CONTOURMAP) {
        const int Ncontours = (sg->ContoursListLen >= 0) ? sg->ContoursListLen : sg->ContoursN;
        if (ww_final->AUTOcolor) { (*Ccounter) += Ncontours; }
        if (ww_final->AUTOlinetype) { (*LTcounter) += Ncontours; }
    }

    // Done
    return;
}

//! eps_plot_styles_NDataColumns - Return the number of columns of data which are required to plot in a given plot style
//! \param [in] ec - The error-reporting context
//! \param [in] style - The plot style, one of the SW_STYLE_* constants
//! \param [in] ThreeDim - Flag indicating whether the graph is three-dimensional
//! \return The number of columns of data required

int eps_plot_styles_NDataColumns(pplerr_context *ec, const int style, const unsigned char ThreeDim) {
    // The number of columns depends on the style, and on whether the plot is three-dimensional
    if (style == SW_STYLE_POINTS) return 2 + (ThreeDim != 0);
    else if (style == SW_STYLE_LINES) return 2 + (ThreeDim != 0);
    else if (style == SW_STYLE_LINESPOINTS) return 2 + (ThreeDim != 0);
    else if (style == SW_STYLE_XERRORBARS) return 3 + (ThreeDim != 0);
    else if (style == SW_STYLE_YERRORBARS) return 3 + (ThreeDim != 0);
    else if (style == SW_STYLE_ZERRORBARS) return 3 + 1;
    else if (style == SW_STYLE_XYERRORBARS) return 4 + (ThreeDim != 0);
    else if (style == SW_STYLE_XZERRORBARS) return 4 + 1;
    else if (style == SW_STYLE_YZERRORBARS) return 4 + 1;
    else if (style == SW_STYLE_XYZERRORBARS) return 5 + 1;
    else if (style == SW_STYLE_XERRORRANGE) return 4 + (ThreeDim != 0);
    else if (style == SW_STYLE_YERRORRANGE) return 4 + (ThreeDim != 0);
    else if (style == SW_STYLE_ZERRORRANGE) return 4 + 1;
    else if (style == SW_STYLE_XYERRORRANGE) return 6 + (ThreeDim != 0);
    else if (style == SW_STYLE_XZERRORRANGE) return 6 + 1;
    else if (style == SW_STYLE_YZERRORRANGE) return 6 + 1;
    else if (style == SW_STYLE_XYZERRORRANGE) return 8 + 1;
    else if (style == SW_STYLE_FILLEDREGION) return 2;
    else if (style == SW_STYLE_YERRORSHADED) return 3;
    else if (style == SW_STYLE_UPPERLIMITS) return 2 + (ThreeDim != 0);
    else if (style == SW_STYLE_LOWERLIMITS) return 2 + (ThreeDim != 0);
    else if (style == SW_STYLE_DOTS) return 2 + (ThreeDim != 0);
    else if (style == SW_STYLE_IMPULSES) return 2 + (ThreeDim != 0);
    else if (style == SW_STYLE_BOXES) return 2;
    else if (style == SW_STYLE_WBOXES) return 3;
    else if (style == SW_STYLE_STEPS) return 2;
    else if (style == SW_STYLE_FSTEPS) return 2;
    else if (style == SW_STYLE_HISTEPS) return 2;
    else if (style == SW_STYLE_STARS) return 2 + (ThreeDim != 0);
    else if (style == SW_STYLE_ARROWS_HEAD) return 4 + 2 * (ThreeDim != 0);
    else if (style == SW_STYLE_ARROWS_NOHEAD) return 4 + 2 * (ThreeDim != 0);
    else if (style == SW_STYLE_ARROWS_TWOHEAD) return 4 + 2 * (ThreeDim != 0);
    else if (style == SW_STYLE_SURFACE) return 3;
    else if (style == SW_STYLE_COLORMAP) return 3;
    else if (style == SW_STYLE_CONTOURMAP) return 3;

    // Any other style is an internal error
    ppl_fatal(ec, __FILE__, __LINE__, "Unrecognised style type passed to eps_plot_styles_NDataColumns()");
    return -1;
}

//! usageRange - Tracks whether a data point lies within the ranges of the axes, while the ranges of data plotted
//! against each axis are being measured. A plot style may test several alternative sets of positions for each data
//! point (e.g. the two ends of an error bar); the point counts as partially in range if any of them are in range.
typedef struct usageRange {
    unsigned char InRange; // True if every position tested so far in the current set is within the axes' ranges
    unsigned char PartiallyInRange; // True if any previous set of positions was entirely within range
    unsigned char InRangeMemory; // The value of <InRange> to start each alternative set of positions with
} usageRange;

//! usageReset - Begin testing whether a new data point lies within the ranges of the axes
//! \param [out] r - The range-tracking state

static void usageReset(usageRange *r) {
    r->InRange = 1;
    r->PartiallyInRange = 0;
    r->InRangeMemory = 1;
}

//! usageCheck - Test whether a position lies within the range of an axis
//! \param [in,out] r - The range-tracking state; <InRange> is cleared if the position is out of range
//! \param [in] axis - The axis
//! \param [in] value - The position along the axis

static void usageCheck(usageRange *r, pplset_axis *axis, const double value) {
    if (r->InRange && (!eps_plot_axis_InRange(axis, value))) r->InRange = 0;
}

//! usageCheckAlternative - Begin testing an alternative set of positions for the current data point, starting with a
//! position along one axis
//! \param [in,out] r - The range-tracking state
//! \param [in] axis - The axis
//! \param [in] value - The position along the axis

static void usageCheckAlternative(usageRange *r, pplset_axis *axis, const double value) {
    r->PartiallyInRange = r->PartiallyInRange || r->InRange;
    r->InRange = r->InRangeMemory;
    usageCheck(r, axis, value);
}

//! usageCheckRemember - Test a position which is common to all of the alternative sets of positions which follow, and
//! remember the result as the starting point for each of them
//! \param [in,out] r - The range-tracking state
//! \param [in] axis - The axis
//! \param [in] value - The position along the axis

static void usageCheckRemember(usageRange *r, pplset_axis *axis, const double value) {
    r->InRangeMemory = r->InRange;
    usageCheck(r, axis, value);
}

//! usageUpdate - If the current data point lies within the ranges of the axes, extend the range of data plotted
//! against an axis to include a position. Non-finite positions, and non-positive positions on logarithmic axes, are
//! ignored.
//! \param [in] r - The range-tracking state
//! \param [in,out] axis - The axis, whose MinUsed and MaxUsed are updated
//! \param [in] z - The position along the axis

static void usageUpdate(const usageRange *r, pplset_axis *axis, const double z) {
    if (!(r->InRange || r->PartiallyInRange)) return;
    if ((gsl_finite(z)) && ((!axis->MinUsedSet) || (axis->MinUsed > z)) &&
        ((axis->LogFinal != SW_BOOL_TRUE) || (z > 0.0))) {
        axis->MinUsedSet = 1;
        axis->MinUsed = z;
    }
    if ((gsl_finite(z)) && ((!axis->MaxUsedSet) || (axis->MaxUsed < z)) &&
        ((axis->LogFinal != SW_BOOL_TRUE) || (z > 0.0))) {
        axis->MaxUsedSet = 1;
        axis->MaxUsed = z;
    }
}

//! usageCheckUpdate - Test whether a position lies within the range of an axis, and if the data point is in range,
//! extend the range of data plotted against the axis to include it
//! \param [in,out] r - The range-tracking state
//! \param [in,out] axis - The axis
//! \param [in] value - The position along the axis

static void usageCheckUpdate(usageRange *r, pplset_axis *axis, const double value) {
    usageCheck(r, axis, value);
    usageUpdate(r, axis, value);
}

//! usageUpdateBoxFrom - Extend the range of data plotted against an axis to include the position from which boxes
//! are drawn ("set boxfrom"), unless its physical units are incompatible with the axis
//! \param [in] r - The range-tracking state
//! \param [in,out] axis - The axis
//! \param [in] sg - The graph settings

static void usageUpdateBoxFrom(const usageRange *r, pplset_axis *axis, const pplset_graph *sg) {
    if (!(axis->DataUnitSet && (!ppl_unitsDimEqual(&sg->BoxFrom, &axis->DataUnit))))
        usageUpdate(r, axis, sg->BoxFrom.real);
}

//! usageUnitsCheck - Check that the physical units of a column of data are compatible with the axis it is plotted
//! against, and record them as the units of data plotted against that axis
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] id - The ID number of the canvas item, used in error messages
//! \param [in] xyz - The direction of the axis (0=x, 1=y, 2=z)
//! \param [in] xyzn - The number of the axis
//! \param [in,out] axis - The axis
//! \param [in] value - A value from the column of data, whose units are checked
//! \return Zero on success, or one if the units conflict (in which case an error is reported)

static int usageUnitsCheck(EPSComm *x, const int id, const int xyz, const int xyzn, pplset_axis *axis,
                           const pplObj *value) {
    char msg[PPL_MSG_LENGTH];
    // The data must have the same units as any range set for the axis
    if ((axis->HardUnitSet) && (!ppl_unitsDimEqual(&axis->HardUnit, value))) {
        ppl_snprintfTrunc(msg, sizeof(msg),
                          "Axis %c%d on plot %d has data plotted against it with conflicting physical units of <%s> as "
                          "compared to range of axis, which has units of <%s>.",
                          "xyzc"[xyz], xyzn, id, ppl_printUnit(x->c, value, NULL, NULL, 0, 1, 0),
                          ppl_printUnit(x->c, &axis->HardUnit, NULL, NULL, 1, 1, 0));
        ppl_error(&x->c->errcontext, ERR_GENERIC, -1, -1, msg);
        return 1;
    }
    // The data must have the same units as other data plotted against the axis
    if ((axis->DataUnitSet) && (!ppl_unitsDimEqual(&axis->DataUnit, value))) {
        ppl_snprintfTrunc(msg, sizeof(msg),
                          "Axis %c%d on plot %d has data plotted against it with conflicting physical units of <%s> "
                          "and <%s>.",
                          "xyzc"[xyz], xyzn, id, ppl_printUnit(x->c, &axis->DataUnit, NULL, NULL, 0, 1, 0),
                          ppl_printUnit(x->c, value, NULL, NULL, 1, 1, 0));
        ppl_error(&x->c->errcontext, ERR_GENERIC, -1, -1, msg);
        return 1;
    }
    // The first data plotted set the units of the axis
    if (!axis->DataUnitSet) {
        axis->DataUnitSet = 1;
        axis->DataUnit = *value;
    }
    return 0;
}

//! finishFactoringBoxes - Extend the ranges of data plotted against the axes to include the final box or step at the
//! end of a dataset (or of a block of it) plotted with the boxes, steps, fsteps or histeps styles. The extent of each
//! box or step depends on the positions of its neighbours, so the last one is only known when the data ends.
//! \param [in,out] r - The range-tracking state
//! \param [in] style - The plot style, one of the SW_STYLE_* constants
//! \param [in] sg - The graph settings
//! \param [in,out] a1 - The axis along which boxes or steps are spread
//! \param [in,out] a2 - The axis along which the heights of boxes or steps are measured
//! \param [in] ptBset - True if there is a point before the last one
//! \param [in] ptCset - True if there is a last point
//! \param [in] ptBx - The position along <a1> of the point before the last one (logged if <a1> is logarithmic)
//! \param [in] ptCx - The position along <a1> of the last point (logged if <a1> is logarithmic)
//! \param [in] lasty - The height of the last point

static void finishFactoringBoxes(usageRange *r, const int style, const pplset_graph *sg, pplset_axis *a1,
                                 pplset_axis *a2, const unsigned char ptBset, const unsigned char ptCset,
                                 const double ptBx, const double ptCx, const double lasty) {
    const unsigned char logaxis = (a1->LogFinal == SW_BOOL_TRUE);
    if (!(ptCset &&
          ((style == SW_STYLE_BOXES) || (style == SW_STYLE_STEPS) || (style == SW_STYLE_FSTEPS) ||
           (style == SW_STYLE_HISTEPS))))
        return;
    usageReset(r);
    usageCheck(r, a2, lasty);
    // Process the last box or step of the run
    if (ptBset) {
        // We have one final box/step to process
        if ((style == SW_STYLE_BOXES) && (sg->BoxWidth.real < 1e-200)) {
            usageCheckUpdate(r, a1, logaxis ? exp(ptCx - (ptCx - ptBx) / 2) : (ptCx - (ptCx - ptBx) / 2));
            usageCheckUpdate(r, a1, logaxis ? exp(ptCx + (ptCx - ptBx) / 2) : (ptCx + (ptCx - ptBx) / 2));
            usageUpdateBoxFrom(r, a2, sg);
        } else if (style == SW_STYLE_BOXES) {
            usageCheckUpdate(r, a1, logaxis ? exp(ptCx - sg->BoxWidth.real / 2) : (ptCx - sg->BoxWidth.real / 2));
            usageCheckUpdate(r, a1, logaxis ? exp(ptCx + sg->BoxWidth.real / 2) : (ptCx + sg->BoxWidth.real / 2));
            usageUpdateBoxFrom(r, a2, sg);
        } else if (style == SW_STYLE_HISTEPS) {
            usageCheckUpdate(r, a1, logaxis ? exp(ptCx - (ptCx - ptBx) / 2) : (ptCx - (ptCx - ptBx) / 2));
            usageCheckUpdate(r, a1, logaxis ? exp(ptCx + (ptCx - ptBx) / 2) : (ptCx + (ptCx - ptBx) / 2));
        } else if (style == SW_STYLE_STEPS) {
            usageCheckUpdate(r, a1,
                             logaxis ? exp((ptBx + ptCx) / 2 - (ptCx - ptBx) / 2)
                                     : ((ptBx + ptCx) / 2 - (ptCx - ptBx) / 2));
            usageCheckUpdate(r, a1,
                             logaxis ? exp((ptBx + ptCx) / 2 + (ptCx - ptBx) / 2)
                                     : ((ptBx + ptCx) / 2 + (ptCx - ptBx) / 2));
        } else if (style == SW_STYLE_FSTEPS) {
            usageCheckUpdate(r, a1, logaxis ? exp(ptCx) : (ptCx));
        }
    } else {
        // We have a dataset with only a single box/step
        if ((style == SW_STYLE_BOXES) && (sg->BoxWidth.real < 1e-200)) {
            usageCheckUpdate(r, a1, logaxis ? exp(ptCx - 0.5) : (ptCx - 0.5));
            usageCheckUpdate(r, a1, logaxis ? exp(ptCx + 0.5) : (ptCx + 0.5));
            usageUpdateBoxFrom(r, a2, sg);
        } else if (style == SW_STYLE_BOXES) {
            usageCheckUpdate(r, a1, logaxis ? exp(ptCx - sg->BoxWidth.real / 2) : (ptCx - sg->BoxWidth.real / 2));
            usageCheckUpdate(r, a1, logaxis ? exp(ptCx + sg->BoxWidth.real / 2) : (ptCx + sg->BoxWidth.real / 2));
            usageUpdateBoxFrom(r, a2, sg);
        } else if (sg->BoxWidth.real < 1e-200) {
            usageCheckUpdate(r, a1, logaxis ? exp(ptCx - 0.5) : (ptCx - 0.5));
            usageCheckUpdate(r, a1, logaxis ? exp(ptCx + 0.5) : (ptCx + 0.5));
        } else {
            usageCheckUpdate(r, a1, logaxis ? exp(ptCx - sg->BoxWidth.real / 2) : (ptCx - sg->BoxWidth.real / 2));
            usageCheckUpdate(r, a1, logaxis ? exp(ptCx + sg->BoxWidth.real / 2) : (ptCx + sg->BoxWidth.real / 2));
        }
    }
}

//! eps_plot_styles_UpdateUsage - Update the record of the range of values plotted against a set of axes, to include a
//! data table plotted in a given style
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] data - The data to plot
//! \param [in] style - The plot style, one of the SW_STYLE_* constants
//! \param [in] ThreeDim - Flag indicating whether the graph is three-dimensional
//! \param [in] a1 - The axis onto which the first data column is plotted
//! \param [in] a2 - The axis onto which the second data column is plotted
//! \param [in] a3 - The axis onto which the third data column is plotted
//! \param [in] sg - The graph settings
//! \param [in] xyz1 - The direction (0=x, 1=y, 2=z) of the axis <a1>
//! \param [in] xyz2 - The direction (0=x, 1=y, 2=z) of the axis <a2>
//! \param [in] xyz3 - The direction (0=x, 1=y, 2=z) of the axis <a3>
//! \param [in] n1 - The number of the axis <a1>
//! \param [in] n2 - The number of the axis <a2>
//! \param [in] n3 - The number of the axis <a3>
//! \param [in] id - The ID number of the canvas item, used in error messages
//! \return Zero on success, or one if the data had physical units which conflict with the axes

int eps_plot_styles_UpdateUsage(EPSComm *x, dataTable *data, const int style, unsigned char ThreeDim, pplset_axis *a1,
                                pplset_axis *a2, pplset_axis *a3, pplset_graph *sg, int xyz1, int xyz2, int xyz3,
                                int n1, int n2, int n3, int id) {
    double ptBx = 0, ptCx = 0, lasty = 0;
    unsigned char ptAset = 0, ptBset = 0, ptCset = 0;
    usageRange r;

    if ((data == NULL) || (data->Nrows < 1)) return 0; // No data present

    // Cycle through data table acting upon the physical units of all of the columns
    if (style == SW_STYLE_POINTS) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
        if (ThreeDim) {
            if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[2])) return 1;
        }
    } else if (style == SW_STYLE_LINES) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
        if (ThreeDim) {
            if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[2])) return 1;
        }
    } else if (style == SW_STYLE_LINESPOINTS) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
        if (ThreeDim) {
            if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[2])) return 1;
        }
    } else if (style == SW_STYLE_XERRORBARS) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[2 + ThreeDim])) return 1;
        if (ThreeDim) {
            if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[2])) return 1;
        }
    } else if (style == SW_STYLE_YERRORBARS) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[2 + ThreeDim])) return 1;
        if (ThreeDim) {
            if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[2])) return 1;
        }
    } else if (style == SW_STYLE_ZERRORBARS) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
        if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[2])) return 1;
        if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[3])) return 1;
    } else if (style == SW_STYLE_XYERRORBARS) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[2 + ThreeDim])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[3 + ThreeDim])) return 1;
        if (ThreeDim) {
            if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[2])) return 1;
        }
    } else if (style == SW_STYLE_XZERRORBARS) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
        if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[2])) return 1;
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[3])) return 1;
        if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[4])) return 1;
    } else if (style == SW_STYLE_YZERRORBARS) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
        if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[2])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[3])) return 1;
        if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[4])) return 1;
    } else if (style == SW_STYLE_XYZERRORBARS) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
        if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[2])) return 1;
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[3])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[4])) return 1;
        if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[5])) return 1;
    } else if (style == SW_STYLE_XERRORRANGE) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[2 + ThreeDim])) return 1;
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[3 + ThreeDim])) return 1;
        if (ThreeDim) {
            if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[2])) return 1;
        }
    } else if (style == SW_STYLE_YERRORRANGE) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[2 + ThreeDim])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[3 + ThreeDim])) return 1;
        if (ThreeDim) {
            if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[2])) return 1;
        }
    } else if (style == SW_STYLE_ZERRORRANGE) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
        if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[2])) return 1;
        if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[3])) return 1;
        if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[4])) return 1;
    } else if (style == SW_STYLE_XYERRORRANGE) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[2 + ThreeDim])) return 1;
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[3 + ThreeDim])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[4 + ThreeDim])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[5 + ThreeDim])) return 1;
        if (ThreeDim)
            if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[2])) return 1;
    } else if (style == SW_STYLE_XZERRORRANGE) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
        if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[2])) return 1;
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[3])) return 1;
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[4])) return 1;
        if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[5])) return 1;
        if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[6])) return 1;
    } else if (style == SW_STYLE_YZERRORRANGE) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
        if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[2])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[3])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[4])) return 1;
        if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[5])) return 1;
        if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[6])) return 1;
    } else if (style == SW_STYLE_XYZERRORRANGE) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
        if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[2])) return 1;
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[3])) return 1;
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[4])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[5])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[6])) return 1;
        if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[7])) return 1;
        if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[8])) return 1;
    } else if (style == SW_STYLE_FILLEDREGION) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
    } else if (style == SW_STYLE_YERRORSHADED) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[2])) return 1;
    } else if (style == SW_STYLE_UPPERLIMITS) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
        if (ThreeDim) {
            if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[2])) return 1;
        }
    } else if (style == SW_STYLE_LOWERLIMITS) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
        if (ThreeDim) {
            if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[2])) return 1;
        }
    } else if (style == SW_STYLE_DOTS) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
        if (ThreeDim) {
            if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[2])) return 1;
        }
    } else if (style == SW_STYLE_IMPULSES) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
        if (ThreeDim) {
            if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[2])) return 1;
        }
    } else if (style == SW_STYLE_BOXES) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
    } else if (style == SW_STYLE_WBOXES) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[2])) return 1;
    } else if (style == SW_STYLE_STEPS) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
    } else if (style == SW_STYLE_FSTEPS) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
    } else if (style == SW_STYLE_HISTEPS) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
    } else if (style == SW_STYLE_STARS) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
        if (ThreeDim) {
            if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[2])) return 1;
        }
    } else if (style == SW_STYLE_ARROWS_HEAD) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[2 + ThreeDim])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[3 + ThreeDim])) return 1;
        // The third column is plotted against the z axis
        if (ThreeDim) {
            if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[2])) return 1;
            if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[5])) return 1;
        }
    } else if (style == SW_STYLE_ARROWS_NOHEAD) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[2 + ThreeDim])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[3 + ThreeDim])) return 1;
        // The third column is plotted against the z axis
        if (ThreeDim) {
            if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[2])) return 1;
            if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[5])) return 1;
        }
    } else if (style == SW_STYLE_ARROWS_TWOHEAD) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[2 + ThreeDim])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[3 + ThreeDim])) return 1;
        // The third column is plotted against the z axis
        if (ThreeDim) {
            if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[2])) return 1;
            if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[5])) return 1;
        }
    } else if (style == SW_STYLE_SURFACE) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
        if (ThreeDim) {
            if (usageUnitsCheck(x, id, xyz3, n3, a3, &data->firstEntries[2])) return 1;
        }
    } else if (style == SW_STYLE_COLORMAP) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
    } else if (style == SW_STYLE_CONTOURMAP) {
        if (usageUnitsCheck(x, id, xyz1, n1, a1, &data->firstEntries[0])) return 1;
        if (usageUnitsCheck(x, id, xyz2, n2, a2, &data->firstEntries[1])) return 1;
    }

    // Cycle through data table, ensuring that axis ranges are sufficient to include all data
    const int Ncolumns = data->Ncolumns_real;
    dataBlock *blk = data->first;
    int i = 0;
    // Loop over the blocks of the data table
    while (blk != NULL) {
        // For each data point, check whether it lies within the axes' fixed ranges, and if so, widen the automatic
        // ranges to include it
        for (int j = 0; j < blk->blockPosition; j++) {
            const double *row = blk->data_real + Ncolumns * j; // The data in this row
            usageReset(&r);
            // Each style has its own columns to test; error bars and boxes extend either side of the point
            if (style == SW_STYLE_POINTS) {
                usageCheck(&r, a1, row[0]);
                usageCheck(&r, a2, row[1]);
                if (ThreeDim) usageCheck(&r, a3, row[2]);
                usageUpdate(&r, a1, row[0]);
                usageUpdate(&r, a2, row[1]);
                if (ThreeDim) usageUpdate(&r, a3, row[2]);
            } else if (style == SW_STYLE_LINES) {
                usageCheck(&r, a1, row[0]);
                usageCheck(&r, a2, row[1]);
                if (ThreeDim) usageCheck(&r, a3, row[2]);
                usageUpdate(&r, a1, row[0]);
                usageUpdate(&r, a2, row[1]);
                if (ThreeDim) usageUpdate(&r, a3, row[2]);
            } else if (style == SW_STYLE_LINESPOINTS) {
                usageCheck(&r, a1, row[0]);
                usageCheck(&r, a2, row[1]);
                if (ThreeDim) usageCheck(&r, a3, row[2]);
                usageUpdate(&r, a1, row[0]);
                usageUpdate(&r, a2, row[1]);
                if (ThreeDim) usageUpdate(&r, a3, row[2]);
            } else if (style == SW_STYLE_XERRORBARS) {
                usageCheck(&r, a2, row[1]);
                if (ThreeDim) usageCheck(&r, a3, row[2]);
                usageCheckRemember(&r, a1, row[0] - row[2 + ThreeDim]);
                usageCheckAlternative(&r, a1, row[0]);
                usageCheckAlternative(&r, a1, row[0] + row[2 + ThreeDim]);
                usageUpdate(&r, a1, row[0]);
                usageUpdate(&r, a1, row[0] - row[2 + ThreeDim]);
                usageUpdate(&r, a1, row[0] + row[2 + ThreeDim]);
                usageUpdate(&r, a2, row[1]);
                if (ThreeDim) usageUpdate(&r, a3, row[2]);
            } else if (style == SW_STYLE_YERRORBARS) {
                usageCheck(&r, a1, row[0]);
                if (ThreeDim) usageCheck(&r, a3, row[2]);
                usageCheckRemember(&r, a2, row[1] - row[2 + ThreeDim]);
                usageCheckAlternative(&r, a2, row[1]);
                usageCheckAlternative(&r, a2, row[1] + row[2 + ThreeDim]);
                usageUpdate(&r, a1, row[0]);
                usageUpdate(&r, a2, row[1]);
                usageUpdate(&r, a2, row[1] - row[2 + ThreeDim]);
                usageUpdate(&r, a2, row[1] + row[2 + ThreeDim]);
                if (ThreeDim) usageUpdate(&r, a3, row[2]);
            } else if (style == SW_STYLE_ZERRORBARS) {
                usageCheck(&r, a1, row[0]);
                usageCheck(&r, a2, row[1]);
                usageCheckRemember(&r, a3, row[2]);
                usageCheckAlternative(&r, a3, row[2] - row[3]);
                usageCheckAlternative(&r, a3, row[2] + row[3]);
                usageUpdate(&r, a1, row[0]);
                usageUpdate(&r, a2, row[1]);
                usageUpdate(&r, a3, row[2]);
                usageUpdate(&r, a3, row[2] - row[3]);
                usageUpdate(&r, a3, row[2] + row[3]);
            } else if (style == SW_STYLE_XYERRORBARS) {
                if (ThreeDim) usageCheck(&r, a3, row[2]);
                usageCheckRemember(&r, a1, row[0]);
                usageCheck(&r, a2, row[1] - row[3 + ThreeDim]);
                usageCheckAlternative(&r, a1, row[0]);
                usageCheck(&r, a2, row[1]);
                usageCheckAlternative(&r, a1, row[0]);
                usageCheck(&r, a2, row[1] + row[3 + ThreeDim]);
                usageCheckAlternative(&r, a1, row[0] - row[2 + ThreeDim]);
                usageCheck(&r, a2, row[1]);
                usageCheckAlternative(&r, a1, row[0] + row[2 + ThreeDim]);
                usageCheck(&r, a2, row[1]);
                usageUpdate(&r, a1, row[0]);
                usageUpdate(&r, a1, row[0] - row[2 + ThreeDim]);
                usageUpdate(&r, a1, row[0] + row[2 + ThreeDim]);
                usageUpdate(&r, a2, row[1]);
                usageUpdate(&r, a2, row[1] - row[3 + ThreeDim]);
                usageUpdate(&r, a2, row[1] + row[3 + ThreeDim]);
                if (ThreeDim) usageUpdate(&r, a3, row[2]);
            } else if (style == SW_STYLE_XZERRORBARS) {
                usageCheck(&r, a2, row[1]);
                usageCheckRemember(&r, a1, row[0]);
                usageCheck(&r, a3, row[2] - row[4]);
                usageCheckAlternative(&r, a1, row[0]);
                usageCheck(&r, a3, row[2]);
                usageCheckAlternative(&r, a1, row[0]);
                usageCheck(&r, a3, row[2] + row[4]);
                usageCheckAlternative(&r, a1, row[0] - row[3]);
                usageCheck(&r, a3, row[2]);
                usageCheckAlternative(&r, a1, row[0] + row[3]);
                usageCheck(&r, a3, row[2]);
                usageUpdate(&r, a1, row[0]);
                usageUpdate(&r, a1, row[0] - row[3]);
                usageUpdate(&r, a1, row[0] + row[3]);
                usageUpdate(&r, a2, row[1]);
                usageUpdate(&r, a3, row[2]);
                usageUpdate(&r, a3, row[2] - row[4]);
                usageUpdate(&r, a3, row[2] + row[4]);
            } else if (style == SW_STYLE_YZERRORBARS) {
                usageCheck(&r, a1, row[0]);
                usageCheckRemember(&r, a2, row[1]);
                usageCheck(&r, a3, row[2] - row[4]);
                usageCheckAlternative(&r, a2, row[1]);
                usageCheck(&r, a3, row[2]);
                usageCheckAlternative(&r, a2, row[1]);
                usageCheck(&r, a3, row[2] + row[4]);
                usageCheckAlternative(&r, a2, row[1] - row[3]);
                usageCheck(&r, a3, row[2]);
                usageCheckAlternative(&r, a2, row[1] + row[3]);
                usageCheck(&r, a3, row[2]);
                usageUpdate(&r, a1, row[0]);
                usageUpdate(&r, a2, row[1]);
                usageUpdate(&r, a2, row[1] - row[3]);
                usageUpdate(&r, a2, row[1] + row[3]);
                usageUpdate(&r, a3, row[2]);
                usageUpdate(&r, a3, row[2] - row[4]);
                usageUpdate(&r, a3, row[2] + row[4]);
            } else if (style == SW_STYLE_XYZERRORBARS) {
                usageCheck(&r, a1, row[0]);
                usageCheck(&r, a2, row[1]);
                usageCheck(&r, a3, row[2] - row[5]);
                usageCheckAlternative(&r, a1, row[0]);
                usageCheck(&r, a2, row[1]);
                usageCheck(&r, a3, row[2]);
                usageCheckAlternative(&r, a1, row[0]);
                usageCheck(&r, a2, row[1]);
                usageCheck(&r, a3, row[2] + row[5]);
                usageCheckAlternative(&r, a1, row[0]);
                usageCheck(&r, a2, row[1] - row[4]);
                usageCheck(&r, a3, row[2]);
                usageCheckAlternative(&r, a1, row[0]);
                usageCheck(&r, a2, row[1] + row[4]);
                usageCheck(&r, a3, row[2]);
                usageCheckAlternative(&r, a1, row[0] - row[3]);
                usageCheck(&r, a2, row[1]);
                usageCheck(&r, a3, row[2]);
                usageCheckAlternative(&r, a1, row[0] + row[3]);
                usageCheck(&r, a2, row[1]);
                usageCheck(&r, a3, row[2]);
                usageUpdate(&r, a1, row[0]);
                usageUpdate(&r, a1, row[0] - row[3]);
                usageUpdate(&r, a1, row[0] + row[3]);
                usageUpdate(&r, a2, row[1]);
                usageUpdate(&r, a2, row[1] - row[4]);
                usageUpdate(&r, a2, row[1] + row[4]);
                usageUpdate(&r, a3, row[2]);
                usageUpdate(&r, a3, row[2] - row[5]);
                usageUpdate(&r, a3, row[2] + row[5]);
            } else if (style == SW_STYLE_XERRORRANGE) {
                usageCheck(&r, a2, row[1]);
                if (ThreeDim) usageCheck(&r, a3, row[2]);
                usageCheckRemember(&r, a1, row[2 + ThreeDim]);
                usageCheckAlternative(&r, a1, row[0]);
                usageCheckAlternative(&r, a1, row[3 + ThreeDim]);
                usageUpdate(&r, a1, row[0]);
                usageUpdate(&r, a2, row[1]);
                usageUpdate(&r, a1, row[2 + ThreeDim]);
                usageUpdate(&r, a1, row[3 + ThreeDim]);
                if (ThreeDim) usageUpdate(&r, a3, row[2]);
            } else if (style == SW_STYLE_YERRORRANGE) {
                usageCheck(&r, a1, row[0]);
                if (ThreeDim) usageCheck(&r, a3, row[2]);
                usageCheckRemember(&r, a2, row[2 + ThreeDim]);
                usageCheckAlternative(&r, a2, row[1]);
                usageCheckAlternative(&r, a2, row[3 + ThreeDim]);
                usageUpdate(&r, a1, row[0]);
                usageUpdate(&r, a2, row[1]);
                usageUpdate(&r, a2, row[2 + ThreeDim]);
                usageUpdate(&r, a2, row[3 + ThreeDim]);
                if (ThreeDim) usageUpdate(&r, a3, row[2]);
            } else if (style == SW_STYLE_ZERRORRANGE) {
                usageCheck(&r, a1, row[0]);
                usageCheck(&r, a2, row[1]);
                usageCheckRemember(&r, a3, row[2]);
                usageCheckAlternative(&r, a3, row[3]);
                usageCheckAlternative(&r, a3, row[4]);
                usageUpdate(&r, a1, row[0]);
                usageUpdate(&r, a2, row[1]);
                usageUpdate(&r, a3, row[2]);
                usageUpdate(&r, a3, row[3]);
                usageUpdate(&r, a3, row[4]);
            } else if (style == SW_STYLE_XYERRORRANGE) {
                if (ThreeDim) usageCheck(&r, a3, row[2]);
                usageCheckRemember(&r, a1, row[0]);
                usageCheck(&r, a2, row[4 + ThreeDim]);
                usageCheckAlternative(&r, a1, row[0]);
                usageCheck(&r, a2, row[1]);
                usageCheckAlternative(&r, a1, row[0]);
                usageCheck(&r, a2, row[5 + ThreeDim]);
                usageCheckAlternative(&r, a1, row[2 + ThreeDim]);
                usageCheck(&r, a2, row[1]);
                usageCheckAlternative(&r, a1, row[3 + ThreeDim]);
                usageCheck(&r, a2, row[1]);
                usageUpdate(&r, a1, row[0]);
                usageUpdate(&r, a2, row[1]);
                usageUpdate(&r, a1, row[2 + ThreeDim]);
                usageUpdate(&r, a1, row[3 + ThreeDim]);
                usageUpdate(&r, a2, row[4 + ThreeDim]);
                usageUpdate(&r, a2, row[5 + ThreeDim]);
                if (ThreeDim) usageUpdate(&r, a3, row[2]);
            } else if (style == SW_STYLE_XZERRORRANGE) {
                usageCheck(&r, a2, row[1]);
                usageCheckRemember(&r, a1, row[0]);
                usageCheck(&r, a3, row[5]);
                usageCheckAlternative(&r, a1, row[0]);
                usageCheck(&r, a3, row[2]);
                usageCheckAlternative(&r, a1, row[0]);
                usageCheck(&r, a3, row[6]);
                usageCheckAlternative(&r, a1, row[3]);
                usageCheck(&r, a3, row[2]);
                usageCheckAlternative(&r, a1, row[4]);
                usageCheck(&r, a3, row[2]);
                usageUpdate(&r, a2, row[1]);
                usageUpdate(&r, a1, row[0]);
                usageUpdate(&r, a1, row[3]);
                usageUpdate(&r, a1, row[4]);
                usageUpdate(&r, a3, row[2]);
                usageUpdate(&r, a3, row[5]);
                usageUpdate(&r, a3, row[6]);
            } else if (style == SW_STYLE_YZERRORRANGE) {
                usageCheck(&r, a1, row[0]);
                usageCheckRemember(&r, a2, row[1]);
                usageCheck(&r, a3, row[5]);
                usageCheckAlternative(&r, a2, row[1]);
                usageCheck(&r, a3, row[2]);
                usageCheckAlternative(&r, a2, row[1]);
                usageCheck(&r, a3, row[6]);
                usageCheckAlternative(&r, a2, row[3]);
                usageCheck(&r, a3, row[2]);
                usageCheckAlternative(&r, a2, row[4]);
                usageCheck(&r, a3, row[2]);
                usageUpdate(&r, a1, row[0]);
                usageUpdate(&r, a1, row[3]);
                usageUpdate(&r, a1, row[4]);
                usageUpdate(&r, a2, row[1]);
                usageUpdate(&r, a2, row[5]);
                usageUpdate(&r, a2, row[6]);
                usageUpdate(&r, a3, row[2]);
                usageUpdate(&r, a3, row[7]);
                usageUpdate(&r, a3, row[8]);
            } else if (style == SW_STYLE_XYZERRORRANGE) {
                usageCheck(&r, a1, row[0]);
                usageCheck(&r, a2, row[1]);
                usageCheck(&r, a3, row[7]);
                usageCheckAlternative(&r, a1, row[0]);
                usageCheck(&r, a2, row[1]);
                usageCheck(&r, a3, row[2]);
                usageCheckAlternative(&r, a1, row[0]);
                usageCheck(&r, a2, row[1]);
                usageCheck(&r, a3, row[8]);
                usageCheckAlternative(&r, a1, row[0]);
                usageCheck(&r, a2, row[5]);
                usageCheck(&r, a3, row[2]);
                usageCheckAlternative(&r, a1, row[0]);
                usageCheck(&r, a2, row[6]);
                usageCheck(&r, a3, row[2]);
                usageCheckAlternative(&r, a1, row[3]);
                usageCheck(&r, a2, row[1]);
                usageCheck(&r, a3, row[2]);
                usageCheckAlternative(&r, a1, row[4]);
                usageCheck(&r, a2, row[1]);
                usageCheck(&r, a3, row[2]);
                usageUpdate(&r, a1, row[0]);
                usageUpdate(&r, a1, row[0] - row[3]);
                usageUpdate(&r, a1, row[0] + row[3]);
                usageUpdate(&r, a2, row[1]);
                usageUpdate(&r, a2, row[1] - row[4]);
                usageUpdate(&r, a2, row[1] + row[4]);
                usageUpdate(&r, a3, row[2]);
                usageUpdate(&r, a3, row[2] - row[5]);
                usageUpdate(&r, a3, row[2] + row[5]);
            } else if (style == SW_STYLE_FILLEDREGION) {
                usageCheck(&r, a1, row[0]);
                usageCheck(&r, a2, row[1]);
                usageUpdate(&r, a1, row[0]);
                usageUpdate(&r, a2, row[1]);
            } else if (style == SW_STYLE_YERRORSHADED) {
                usageCheck(&r, a1, row[0]);
                usageCheck(&r, a2, row[1]);
                usageCheck(&r, a2, row[2]);
                usageUpdate(&r, a1, row[0]);
                usageUpdate(&r, a2, row[1]);
                usageUpdate(&r, a2, row[2]);
            } else if (style == SW_STYLE_LOWERLIMITS) {
                usageCheck(&r, a1, row[0]);
                usageCheck(&r, a2, row[1]);
                if (ThreeDim) usageCheck(&r, a3, row[2]);
                usageUpdate(&r, a1, row[0]);
                usageUpdate(&r, a2, row[1]);
                if (ThreeDim) usageUpdate(&r, a3, row[2]);
            } else if (style == SW_STYLE_UPPERLIMITS) {
                usageCheck(&r, a1, row[0]);
                usageCheck(&r, a2, row[1]);
                if (ThreeDim) usageCheck(&r, a3, row[2]);
                usageUpdate(&r, a1, row[0]);
                usageUpdate(&r, a2, row[1]);
                if (ThreeDim) usageUpdate(&r, a3, row[2]);
            } else if (style == SW_STYLE_DOTS) {
                usageCheck(&r, a1, row[0]);
                usageCheck(&r, a2, row[1]);
                if (ThreeDim) usageCheck(&r, a3, row[2]);
                usageUpdate(&r, a1, row[0]);
                usageUpdate(&r, a2, row[1]);
                if (ThreeDim) usageUpdate(&r, a3, row[2]);
            } else if (style == SW_STYLE_IMPULSES) {
                usageCheck(&r, a1, row[0]);
                usageCheck(&r, a2, row[1]);
                if (ThreeDim) usageCheck(&r, a3, row[2]);
                usageUpdate(&r, a1, row[0]);
                usageUpdate(&r, a2, row[1]);
                if (ThreeDim) usageUpdate(&r, a3, row[2]);
                usageUpdateBoxFrom(&r, a2, sg);
            } else if (style == SW_STYLE_WBOXES) {
                usageCheck(&r, a2, row[1]);
                usageCheckRemember(&r, a1, row[0]);
                usageCheckAlternative(&r, a1, row[0] - row[2]);
                usageCheckAlternative(&r, a1, row[0] + row[2]);
                usageUpdate(&r, a2, row[1]);
                usageUpdate(&r, a1, row[0]);
                usageUpdate(&r, a1, row[0] - row[2]);
                usageUpdate(&r, a1, row[0] + row[2]);
                usageUpdateBoxFrom(&r, a2, sg);
            } else if (style == SW_STYLE_STARS) {
                usageCheck(&r, a1, row[0]);
                usageCheck(&r, a2, row[1]);
                if (ThreeDim) usageCheck(&r, a3, row[2]);
                usageUpdate(&r, a1, row[0]);
                usageUpdate(&r, a2, row[1]);
                if (ThreeDim) usageUpdate(&r, a3, row[2]);
            } else if ((style == SW_STYLE_ARROWS_HEAD) || (style == SW_STYLE_ARROWS_NOHEAD) ||
                       (style == SW_STYLE_ARROWS_TWOHEAD)) {
                usageCheck(&r, a1, row[0]);
                usageCheck(&r, a2, row[1]);
                if (ThreeDim) usageCheck(&r, a3, row[2]);
                usageCheckAlternative(&r, a1, row[2 + ThreeDim]);
                usageCheck(&r, a2, row[3 + ThreeDim]);
                if (ThreeDim) usageCheck(&r, a3, row[5]);
                usageUpdate(&r, a1, row[0]);
                usageUpdate(&r, a2, row[1]);
                usageUpdate(&r, a1, row[2 + ThreeDim]);
                usageUpdate(&r, a2, row[3 + ThreeDim]);
                // The third column is plotted against the z axis
                if (ThreeDim) {
                    usageUpdate(&r, a3, row[2]);
                    usageUpdate(&r, a3, row[5]);
                }
            } else if (style == SW_STYLE_SURFACE) {
                usageCheck(&r, a1, row[0]);
                usageCheck(&r, a2, row[1]);
                if (ThreeDim) usageCheck(&r, a3, row[2]);
                usageUpdate(&r, a1, row[0]);
                usageUpdate(&r, a2, row[1]);
                if (ThreeDim) usageUpdate(&r, a3, row[2]);
            } else if ((style == SW_STYLE_COLORMAP) || (style == SW_STYLE_CONTOURMAP)) {
                usageCheck(&r, a1, row[0]);
                usageCheck(&r, a2, row[1]);
                usageUpdate(&r, a1, row[0]);
                usageUpdate(&r, a2, row[1]);
            } else if ((style == SW_STYLE_BOXES) || (style == SW_STYLE_STEPS) || (style == SW_STYLE_FSTEPS) ||
                       (style == SW_STYLE_HISTEPS)) {
                // Boxes and steps need slightly more complicated logic to take into account finite width of boxes/steps

                // Boxes and steps extend half way to the neighbouring points, so the range is updated one point behind,
                // as each box's width becomes known
                const unsigned char logaxis = (a1->LogFinal == SW_BOOL_TRUE);
                if (blk->split[j]) { finishFactoringBoxes(&r, style, sg, a1, a2, ptBset, ptCset, ptBx, ptCx, lasty); }
                usageCheck(&r, a2, row[1]); // y-coordinates are easy
                const double ptAx = ptBx;
                ptAset = ptBset;
                ptBx = ptCx;
                ptBset = ptCset;
                ptCx = logaxis ? log(row[0]) : (row[0]);
                ptCset = 1;
                // Process the box or step centred on the previous point
                if (ptBset) {
                    if (ptAset) // We are processing a box in the midst of many
                    {
                        // Automatic box widths extend half way to the neighbouring points
                        if ((style == SW_STYLE_BOXES) && (sg->BoxWidth.real < 1e-200)) {
                            usageCheckUpdate(&r, a1,
                                             logaxis ? exp(((ptBx + (ptAx + ptCx) / 2) / 2 - (ptCx - ptAx) / 4))
                                                     : (((ptBx + (ptAx + ptCx) / 2) / 2 - (ptCx - ptAx) / 4)));
                            usageCheckUpdate(&r, a1,
                                             logaxis ? exp(((ptBx + (ptAx + ptCx) / 2) / 2 + (ptCx - ptAx) / 4))
                                                     : (((ptBx + (ptAx + ptCx) / 2) / 2 + (ptCx - ptAx) / 4)));
                            usageUpdateBoxFrom(&r, a2, sg);
                        } else if (style == SW_STYLE_BOXES) {
                            usageCheckUpdate(&r, a1,
                                             logaxis ? exp((ptBx - sg->BoxWidth.real / 2))
                                                     : ((ptBx - sg->BoxWidth.real / 2)));
                            usageCheckUpdate(&r, a1,
                                             logaxis ? exp((ptBx + sg->BoxWidth.real / 2))
                                                     : ((ptBx + sg->BoxWidth.real / 2)));
                            usageUpdateBoxFrom(&r, a2, sg);
                        } else if (style == SW_STYLE_HISTEPS) {
                            usageCheckUpdate(&r, a1,
                                             logaxis ? exp(((ptBx + (ptAx + ptCx) / 2) / 2 - (ptCx - ptAx) / 4))
                                                     : (((ptBx + (ptAx + ptCx) / 2) / 2 - (ptCx - ptAx) / 4)));
                            usageCheckUpdate(&r, a1,
                                             logaxis ? exp(((ptBx + (ptAx + ptCx) / 2) / 2 + (ptCx - ptAx) / 4))
                                                     : (((ptBx + (ptAx + ptCx) / 2) / 2 + (ptCx - ptAx) / 4)));
                            usageUpdateBoxFrom(&r, a2, sg);
                        } else if (style == SW_STYLE_STEPS) {
                            usageCheckUpdate(&r, a1,
                                             logaxis ? exp(((ptAx + ptBx) / 2 - (ptBx - ptAx) / 2))
                                                     : (((ptAx + ptBx) / 2 - (ptBx - ptAx) / 2)));
                            usageCheckUpdate(&r, a1,
                                             logaxis ? exp(((ptAx + ptBx) / 2 + (ptBx - ptAx) / 2))
                                                     : (((ptAx + ptBx) / 2 + (ptBx - ptAx) / 2)));
                        } else if (style == SW_STYLE_FSTEPS) {
                            usageCheckUpdate(&r, a1,
                                             logaxis ? exp(((ptBx + ptCx) / 2 - (ptCx - ptBx) / 2))
                                                     : (((ptBx + ptCx) / 2 - (ptCx - ptBx) / 2)));
                            usageCheckUpdate(&r, a1,
                                             logaxis ? exp(((ptBx + ptCx) / 2 + (ptCx - ptBx) / 2))
                                                     : (((ptBx + ptCx) / 2 + (ptCx - ptBx) / 2)));
                        }
                    } else // The first box/step we work out the width of
                    {
                        // Automatic box widths extend half way to the neighbouring point
                        if ((style == SW_STYLE_BOXES) && (sg->BoxWidth.real < 1e-200)) {
                            usageCheckUpdate(&r, a1,
                                             logaxis ? exp((ptBx - (ptCx - ptBx) / 2)) : ((ptBx - (ptCx - ptBx) / 2)));
                            usageCheckUpdate(&r, a1,
                                             logaxis ? exp((ptBx + (ptCx - ptBx) / 2)) : ((ptBx + (ptCx - ptBx) / 2)));
                            usageUpdateBoxFrom(&r, a2, sg);
                        } else if (style == SW_STYLE_BOXES) {
                            usageCheckUpdate(&r, a1,
                                             logaxis ? exp((ptBx - sg->BoxWidth.real / 2))
                                                     : ((ptBx - sg->BoxWidth.real / 2)));
                            usageCheckUpdate(&r, a1,
                                             logaxis ? exp((ptBx + sg->BoxWidth.real / 2))
                                                     : ((ptBx + sg->BoxWidth.real / 2)));
                            usageUpdateBoxFrom(&r, a2, sg);
                        } else if (style == SW_STYLE_HISTEPS) {
                            usageCheckUpdate(&r, a1,
                                             logaxis ? exp((ptBx - (ptCx - ptBx) / 2)) : ((ptBx - (ptCx - ptBx) / 2)));
                            usageCheckUpdate(&r, a1,
                                             logaxis ? exp((ptBx + (ptCx - ptBx) / 2)) : ((ptBx + (ptCx - ptBx) / 2)));
                        } else if (style == SW_STYLE_STEPS) {
                            usageCheckUpdate(&r, a1, logaxis ? exp(ptBx) : (ptBx));
                        } else if (style == SW_STYLE_FSTEPS) {
                            usageCheckUpdate(&r, a1,
                                             logaxis ? exp(((ptBx + ptCx) / 2 - (ptCx - ptBx) / 2))
                                                     : (((ptBx + ptCx) / 2 - (ptCx - ptBx) / 2)));
                            usageCheckUpdate(&r, a1,
                                             logaxis ? exp(((ptBx + ptCx) / 2 + (ptCx - ptBx) / 2))
                                                     : (((ptBx + ptCx) / 2 + (ptCx - ptBx) / 2)));
                        }
                    }
                }
                usageUpdate(&r, a2, row[1]); // y-coordinates are easy
                lasty = row[1];
            }
            i++;
        }
        blk = blk->next;
    }
    finishFactoringBoxes(&r, style, sg, a1, a2, ptBset, ptCset, ptBx, ptCx, lasty);
    return 0;
}

//! graphProjection - The mapping from data coordinates onto the page for a graph
typedef struct graphProjection {
    pplset_axis *xa, *ya, *za; // The axes against which data are plotted
    int xrn, yrn, zrn; // The monotonic regions of the axes in which the data lie
    pplset_graph *sg; // The graph settings
    unsigned char ThreeDim; // Flag indicating whether the graph is three-dimensional
    double origin_x, origin_y; // The position of the bottom-left corner of the graph, in postscript points
    double scale_x, scale_y, scale_z; // The width, height and depth of the graph, in postscript points
} graphProjection;

//! surfacePoint - Find the position on the page of one corner of a cell of a surface plot
//! \param [in] proj - The mapping from data coordinates onto the page
//! \param [in] row - The data for the grid point at the corner of the cell
//! \param [in] xn - The column of <row> which is plotted against the x axis
//! \param [in] yn - The column of <row> which is plotted against the y axis
//! \param [in] zn - The column of <row> which is plotted against the z axis
//! \param [out] xo - Set to the horizontal position on the page, in postscript points
//! \param [out] yo - Set to the vertical position on the page, in postscript points
//! \param [out] zo - Set to the depth of the point
//! \return True if the point lies outside the graph, or has a non-finite position, so the cell should not be drawn

static int surfacePoint(const graphProjection *proj, const double *row, const int xn, const int yn, const int zn,
                        double *xo, double *yo, double *zo) {
    double xap, yap, zap, theta_x, theta_y, theta_z;
    eps_plot_GetPosition(xo, yo, zo, &xap, &yap, &zap, &theta_x, &theta_y, &theta_z, proj->ThreeDim, row[xn], row[yn],
                         proj->ThreeDim ? row[zn] : 0.0, proj->xa, proj->ya, proj->za, proj->xrn, proj->yrn, proj->zrn,
                         proj->sg, proj->origin_x, proj->origin_y, proj->scale_x, proj->scale_y, proj->scale_z, 1);
    if ((xap < 0) || (xap > 1) || (yap < 0) || (yap > 1) || (zap < 0) || (zap > 1)) return 1;
    if ((!gsl_finite(*xo)) || (!gsl_finite(*yo)) || (!gsl_finite(*zo))) return 1;
    return 0;
}

//! boxDrawer - The state needed to draw the boxes or steps of a dataset plotted with the boxes, wboxes, steps,
//! fsteps or histeps styles
typedef struct boxDrawer {
    EPSComm *x; // The state of the EPS/PS rendering process for the canvas currently being drawn
    LineDrawHandle *ld; // The line along which the outlines of boxes and steps are drawn
    canvas_plotdesc *pd; // The descriptor of the plot item
    graphProjection proj; // The mapping from data coordinates onto the page
    pplset_axis **a; // The axes against which each data column is plotted
    int xn, yn, zn; // The data columns which are plotted against the x, y and z axes
    unsigned char logaxis; // True if the axis along which the boxes are spread is logarithmic
    char *last_colstr; // The colour in which the line was last drawn, or NULL
} boxDrawer;

//! boxColourUpdate - Record the current colour as the colour in which the outline of a box or step is drawn
//! \param [in,out] bd - The box-drawing state
//! \return Zero on success, or nonzero if memory ran out

static int boxColourUpdate(boxDrawer *bd) {
    EPSComm *x = bd->x;
    // Keep a copy of the current color for the depth-sorting buffer, if it has changed
    if ((bd->last_colstr == NULL) || (strcmp(bd->last_colstr, x->CurrentColor) != 0)) {
        bd->last_colstr = (char *) ppl_memAlloc(strlen(x->CurrentColor) + 1);
        if (bd->last_colstr == NULL) return 1;
        strcpy(bd->last_colstr, x->CurrentColor);
    }
    return 0;
}

//! makeStep - Draw one horizontal step of a dataset plotted with the steps, fsteps or histeps styles
//! \param [in,out] bd - The box-drawing state
//! \param [in] X0 - The horizontal centre of the step (logged if the axis is logarithmic)
//! \param [in] Y0 - The height of the step
//! \param [in] WIDTH - The half-width of the step
//! \return Zero on success, or nonzero if memory ran out

static int makeStep(boxDrawer *bd, const double X0, const double Y0, const double WIDTH) {
    EPSComm *x = bd->x;
    const canvas_plotdesc *pd = bd->pd;
    const int xn = bd->xn, yn = bd->yn, zn = bd->zn;
    const unsigned char ThreeDim = bd->proj.ThreeDim;
    double pA[3] = {(X0) - (WIDTH), (Y0), 0}, pB[3] = {(X0) + (WIDTH), (Y0), 0};
    if (!eps_core_isVisible(x)) return 0;
    if (boxColourUpdate(bd)) return 1;
    // On logarithmic axes, box positions are worked out in logarithms
    if (bd->logaxis) {
        pA[0] = exp(pA[0]);
        pB[0] = exp(pB[0]);
    }
    LineDraw_Point(x, bd->ld, pA[xn], pA[yn], ThreeDim ? (pA[zn]) : 0, 0, 0, 0, 0, 0, 0, pd->ww_final.linetype,
                   pd->ww_final.linewidth, bd->last_colstr);
    LineDraw_Point(x, bd->ld, pB[xn], pB[yn], ThreeDim ? (pB[zn]) : 0, 0, 0, 0, 0, 0, 0, pd->ww_final.linetype,
                   pd->ww_final.linewidth, bd->last_colstr);
    return 0;
}

//! makeBox - Draw one box of a dataset plotted with the boxes or wboxes styles, filling it and stroking its outline
//! \param [in,out] bd - The box-drawing state
//! \param [in] X0 - The horizontal centre of the box (logged if the axis is logarithmic)
//! \param [in] Y0 - The height of the top of the box; its bottom is at "set boxfrom"
//! \param [in] WIDTH - The half-width of the box
//! \return Zero on success, or nonzero if memory ran out

static int makeBox(boxDrawer *bd, const double X0, const double Y0, const double WIDTH) {
    // Variables for drawing the box
    EPSComm *x = bd->x;
    canvas_plotdesc *pd = bd->pd;
    const graphProjection *g = &bd->proj;
    const int xn = bd->xn, yn = bd->yn, zn = bd->zn;
    const unsigned char ThreeDim = g->ThreeDim;
    int an;
    double pA[3] = {(X0) - (WIDTH), g->sg->BoxFrom.real, 0};
    double pB[3] = {(X0) - (WIDTH), (Y0), 0};
    double pC[3] = {(X0) + (WIDTH), (Y0), 0};
    double pD[3] = {(X0) + (WIDTH), g->sg->BoxFrom.real, 0};
    // On logarithmic axes, box positions are worked out in logarithms
    if (bd->logaxis) {
        pA[0] = exp(pA[0]);
        pB[0] = exp(pB[0]);
        pC[0] = exp(pC[0]);
        pD[0] = exp(pD[0]);
    }
    if (xn == 1) an = 0;
    else if (yn == 1) an = 1;
    else an = 2;
    if ((bd->a[an]->LogFinal == SW_BOOL_TRUE) && (pA[1] < DBL_MIN)) pA[1] = DBL_MIN;
    if ((bd->a[an]->LogFinal == SW_BOOL_TRUE) && (pB[1] < DBL_MIN)) pB[1] = DBL_MIN;
    if ((bd->a[an]->LogFinal == SW_BOOL_TRUE) && (pC[1] < DBL_MIN)) pC[1] = DBL_MIN;
    if ((bd->a[an]->LogFinal == SW_BOOL_TRUE) && (pD[1] < DBL_MIN)) pD[1] = DBL_MIN;

    // Set fill color of box
    eps_core_SetFillColor(x, &pd->ww_final);
    eps_core_SwitchTo_FillColor(x, 0);

    // Fill box
    if (eps_core_isVisible(x) && (!ThreeDim)) {
        FilledRegionHandle *fr = FilledRegion_Init(x, g->xa, g->ya, g->za, g->xrn, g->yrn, g->zrn, g->sg, ThreeDim,
                                                   g->origin_x, g->origin_y, g->scale_x, g->scale_y, g->scale_z);
        FilledRegion_Point(x, fr, pA[xn], pA[yn]);
        FilledRegion_Point(x, fr, pB[xn], pB[yn]);
        FilledRegion_Point(x, fr, pC[xn], pC[yn]);
        FilledRegion_Point(x, fr, pD[xn], pD[yn]);
        FilledRegion_Finish(x, fr, pd->ww_final.linetype, pd->ww_final.linewidth, 0);
    }
    eps_core_SwitchFrom_FillColor(x, 0);

    // Stroke outline of box
    if (eps_core_isVisible(x)) {
        if (boxColourUpdate(bd)) return 1;
        LineDraw_Point(x, bd->ld, pA[xn], pA[yn], ThreeDim ? pA[zn] : 0, 0, 0, 0, 0, 0, 0, pd->ww_final.linetype,
                       pd->ww_final.linewidth, bd->last_colstr);
        LineDraw_Point(x, bd->ld, pB[xn], pB[yn], ThreeDim ? pB[zn] : 0, 0, 0, 0, 0, 0, 0, pd->ww_final.linetype,
                       pd->ww_final.linewidth, bd->last_colstr);
        LineDraw_Point(x, bd->ld, pC[xn], pC[yn], ThreeDim ? pC[zn] : 0, 0, 0, 0, 0, 0, 0, pd->ww_final.linetype,
                       pd->ww_final.linewidth, bd->last_colstr);
        LineDraw_Point(x, bd->ld, pD[xn], pD[yn], ThreeDim ? pD[zn] : 0, 0, 0, 0, 0, 0, 0, pd->ww_final.linetype,
                       pd->ww_final.linewidth, bd->last_colstr);
        LineDraw_Point(x, bd->ld, pA[xn], pA[yn], ThreeDim ? pA[zn] : 0, 0, 0, 0, 0, 0, 0, pd->ww_final.linetype,
                       pd->ww_final.linewidth, bd->last_colstr);
        LineDraw_PenUp(x, bd->ld);
    }
    return 0;
}

//! finishRowOfBoxes - Draw the final box or step at the end of a dataset (or of a block of it), whose extent was not
//! known until the data ended, and start afresh
//! \param [in,out] bd - The box-drawing state
//! \param [in] style - The plot style, one of the SW_STYLE_* constants
//! \param [out] ptAset - Flag indicating whether the point before last is set; cleared
//! \param [in,out] ptBset - Flag indicating whether there is a point before the last one; cleared
//! \param [in,out] ptCset - Flag indicating whether there is a last point; cleared
//! \param [in] ptBx - The horizontal position of the point before the last one (logged if the axis is logarithmic)
//! \param [in] ptCx - The horizontal position of the last point (logged if the axis is logarithmic)
//! \param [in] ptCy - The height of the last point
//! \return Zero on success, or nonzero if memory ran out

static int finishRowOfBoxes(boxDrawer *bd, const int style, unsigned char *ptAset, unsigned char *ptBset,
                            unsigned char *ptCset, const double ptBx, const double ptCx, const double ptCy) {
    const pplset_graph *sg = bd->proj.sg;
    int status = 0;
    // Process the last box or step of the run
    if (*ptBset) {
        // We have one final box/step to process
        if ((style == SW_STYLE_BOXES) && (sg->BoxWidth.real < 1e-200)) {
            status = makeBox(bd, ptCx, ptCy, (ptCx - ptBx) / 2);
        } else if (style == SW_STYLE_BOXES) {
            status = makeBox(bd, ptCx, ptCy, sg->BoxWidth.real / 2);
        } else if (style == SW_STYLE_HISTEPS) {
            status = makeStep(bd, ptCx, ptCy, (ptCx - ptBx) / 2);
        } else if (style == SW_STYLE_STEPS) {
            status = makeStep(bd, (ptBx + ptCx) / 2, ptCy, (ptCx - ptBx) / 2);
        } else if (style == SW_STYLE_FSTEPS) {
            status = makeStep(bd, ptCx, ptCy, 0.0);
        }
    } else if (*ptCset) {
        // We have a dataset with only a single box/step
        if ((style == SW_STYLE_BOXES) && (sg->BoxWidth.real < 1e-200)) {
            status = makeBox(bd, ptCx, ptCy, 0.5);
        } else if (style == SW_STYLE_BOXES) {
            status = makeBox(bd, ptCx, ptCy, sg->BoxWidth.real / 2);
        } else if (sg->BoxWidth.real < 1e-200) {
            status = makeStep(bd, ptCx, ptCy, 0.5);
        } else {
            status = makeStep(bd, ptCx, ptCy, sg->BoxWidth.real / 2);
        }
    }
    if (status) return status;
    *ptAset = *ptBset = *ptCset = 0;
    LineDraw_PenUp(bd->x, bd->ld);
    return 0;
}

//! eps_plot_dataset - Render a data table to postscript, in a given plot style
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] data - The data to plot
//! \param [in] style - The plot style, one of the SW_STYLE_* constants
//! \param [in] ThreeDim - Flag indicating whether the graph is three-dimensional
//! \param [in] a1 - The axis onto which the first data column is plotted
//! \param [in] a2 - The axis onto which the second data column is plotted
//! \param [in] a3 - The axis onto which the third data column is plotted
//! \param [in] xn - The axis direction (0=x, 1=y, 2=z) onto which the first data column is plotted
//! \param [in] yn - The axis direction (0=x, 1=y, 2=z) onto which the second data column is plotted
//! \param [in] zn - The axis direction (0=x, 1=y, 2=z) onto which the third data column is plotted
//! \param [in] sg - The graph settings
//! \param [in] pd - The descriptor of the plot item being drawn
//! \param [in] origin_x - The x coordinate of the bottom-left corner of the graph, in postscript points
//! \param [in] origin_y - The y coordinate of the bottom-left corner of the graph, in postscript points
//! \param [in] width - The width of the graph, in postscript points
//! \param [in] height - The height of the graph, in postscript points
//! \param [in] zdepth - The depth of the graph, in postscript points, for 3D plots
//! \return Zero on success, or one on failure

int eps_plot_dataset(EPSComm *x, const dataTable *data, const int style, const unsigned char ThreeDim, pplset_axis *a1,
                     pplset_axis *a2, pplset_axis *a3, int xn, int yn, const int zn, pplset_graph *sg,
                     canvas_plotdesc *pd, const double origin_x, const double origin_y, const double width,
                     const double height, const double zdepth) {
    char msg[LSTR_LENGTH];
    int i, j, pt = 0;
    double xpos, ypos, depth, xap, yap, zap, scale_x, scale_y, scale_z;
    char epsbuff[FNAME_LENGTH], *last_colstr = NULL;
    LineDrawHandle *ld;
    pplset_axis *a[3] = {a1, a2, a3};

    // Warn about datasets with no data, describing them by their filename or functions
    if ((data == NULL) || (data->Nrows < 1)) {
        char *output = msg;
        int i = 0;
        ppl_strAppendf(output, sizeof(msg), &i, "plotted item");
        // Data from a file or from vectors, or from functions
        if (!pd->function) {
            // The name of the data file, or the vectors
            if (pd->filename != NULL) {
                ppl_strAppendChar(output, sizeof(msg), &i, ' ');
                ppl_strEscapify(pd->filename, output + i, sizeof(msg) - i);
                i += strlen(output + i); // Filename of datafile we are plotting
            } else if (pd->vectors != NULL) {
                for (j = 0; j < pd->NFunctions; j++) // Print out the list of functions which we are plotting
                {
                    ppl_strAppendChar(output, sizeof(msg), &i, (j != 0) ? ':' : ' ');
                    pplObjPrint(x->c, &pd->vectors[j], NULL, output + i, ppl_min(250, sizeof(msg) - i), 0, 0);
                    i += strlen(output + i);
                }
            }
        } else
            for (j = 0; j < pd->NFunctions; j++) // Print out the list of functions which we are plotting
            {
                ppl_strAppendChar(output, sizeof(msg), &i, (j != 0) ? ':' : ' ');
                ppl_strAppendStripped(output, sizeof(msg), &i, pd->functions[j]->ascii);
            }
        ppl_strAppendf(output, sizeof(msg), &i, " produced no data.");
        ppl_warning(&x->c->errcontext, ERR_NUMERICAL, msg);
        return 0; // No data present
    }

    // The number of columns of numbers and of objects in each row
    const int Ncolumns = data->Ncolumns_real;
    const int Ncol_obj = data->Ncolumns_obj;
    if (eps_plot_WithWordsCheckUsingItemsDimLess(x->c, &pd->ww_final, data->firstEntries, Ncolumns, Ncol_obj, NULL))
        return 1;

    // The size of the graph
    if (!ThreeDim) {
        scale_x = width;
        scale_y = height;
        scale_z = 1.0;
    } else {
        scale_x = width;
        scale_y = height;
        scale_z = zdepth;
    }

    // If axes have value-turning points, loop over all monotonic regions of axis space
    for (int xrn = 0; xrn <= a[xn]->AxisValueTurnings; xrn++)
        for (int yrn = 0; yrn <= a[yn]->AxisValueTurnings; yrn++)
            // Each monotonic section of the z axis
            for (int zrn = 0; zrn <= (ThreeDim ? a[zn]->AxisValueTurnings : 0); zrn++) {
                dataBlock *blk = data->first;

                if ((style == SW_STYLE_LINES) || (style == SW_STYLE_LINESPOINTS)) // LINES
                {
                    ld = LineDraw_Init(x, a[xn], a[yn], a[zn], xrn, yrn, zrn, sg, ThreeDim, origin_x, origin_y, scale_x,
                                       scale_y, scale_z);
                    last_colstr = NULL;

                    // Loop over the blocks of the data table
                    while (blk != NULL) {
                        // Draw a line through each data point, breaking it where the data are split
                        for (j = 0; j < blk->blockPosition; j++) {
                            const double *row = blk->data_real + Ncolumns * j; // The data in this row
                            // Work out style information for next point
                            eps_plot_WithWordsFromUsingItems(x->c, &pd->ww_final, &blk->data_real[Ncolumns * j],
                                                             &blk->data_obj[Ncol_obj * j], Ncolumns, Ncol_obj);
                            eps_core_SetColor(x, &pd->ww_final, 0);
                            if (blk->split[j]) { LineDraw_PenUp(x, ld); }
                            // Draw the next segment, unless it is invisible
                            if (eps_core_isVisible(x)) {
                                // Keep a copy of the current color for the depth-sorting buffer, if it has changed
                                if ((last_colstr == NULL) || (strcmp(last_colstr, x->CurrentColor) != 0)) {
                                    last_colstr = (char *) ppl_memAlloc(strlen(x->CurrentColor) + 1);
                                    if (last_colstr == NULL) break;
                                    strcpy(last_colstr, x->CurrentColor);
                                }
                                LineDraw_Point(x, ld, row[xn], row[yn], ThreeDim ? row[zn] : 0.0, 0, 0, 0, 0, 0, 0,
                                               pd->ww_final.linetype, pd->ww_final.linewidth, last_colstr);
                            } else {
                                LineDraw_PenUp(x, ld);
                            }
                        }
                        blk = blk->next;
                    }
                    LineDraw_PenUp(x, ld);
                }

                // Points, dots and stars
                if ((style == SW_STYLE_POINTS) || (style == SW_STYLE_LINESPOINTS) || (style == SW_STYLE_STARS) ||
                    (style == SW_STYLE_DOTS)) // POINTS, DOTS, STARS
                {
                    last_colstr = NULL;

                    // Start from the first block of the data table
                    blk = data->first;
                    // Loop over the blocks of the data table
                    while (blk != NULL) {
                        // Draw each data point
                        for (j = 0; j < blk->blockPosition; j++) {
                            const double *row = blk->data_real + Ncolumns * j; // The data in this row
                            double final_pointsize = 0.0;
                            eps_plot_GetPosition(&xpos, &ypos, &depth, &xap, &yap, &zap, NULL, NULL, NULL, ThreeDim,
                                                 row[xn], row[yn], ThreeDim ? row[zn] : 0.0, a[xn], a[yn], a[zn], xrn,
                                                 yrn, zrn, sg, origin_x, origin_y, scale_x, scale_y, scale_z, 0);
                            if (!gsl_finite(xpos)) // Position of point is off side of graph
                            {
                                if ((blk->text[j] != NULL) && (blk->text[j][0] != '\0')) x->LaTeXpageno++;
                                continue;
                            }

                            // Work out style information for next point
                            eps_plot_WithWordsFromUsingItems(x->c, &pd->ww_final, &blk->data_real[Ncolumns * j],
                                                             &blk->data_obj[Ncol_obj * j], Ncolumns, Ncol_obj);
                            final_pointsize = pd->ww_final.pointsize;
                            if (style == SW_STYLE_DOTS) final_pointsize *= 0.05; // Dots are 1/20th size of points
                            else if (style == SW_STYLE_STARS) final_pointsize *= 12.0; // Stars are BIG
                            eps_core_SetColor(x, &pd->ww_final, 0);
                            // Draw the point, unless it is invisible
                            if (eps_core_isVisible(x)) {
                                // Keep a copy of the current color for the depth-sorting buffer, if it has changed
                                if ((last_colstr == NULL) || (strcmp(last_colstr, x->CurrentColor) != 0)) {
                                    last_colstr = (char *) ppl_memAlloc(strlen(x->CurrentColor) + 1);
                                    if (last_colstr == NULL) break;
                                    strcpy(last_colstr, x->CurrentColor);
                                }
                                // Points and dots use the point symbols; stars have their own symbols
                                if (style != SW_STYLE_STARS) {
                                    pt = (style == SW_STYLE_DOTS)
                                            ? 16
                                            : ((pd->ww_final.pointtype - 1) %
                                               N_POINTTYPES); // Dots are always pt 17 (filled circle)
                                    while (pt < 0) pt += N_POINTTYPES;
                                    x->PointTypesUsed[pt] = 1;
                                    ppl_snprintfTrunc(epsbuff, sizeof(epsbuff), "%.2f %.2f pt%d", xpos, ypos, pt + 1);
                                    eps_core_BoundingBox(x, xpos, ypos,
                                                         2 * final_pointsize * eps_PointSize[pt] * EPS_DEFAULT_PS);
                                } else {
                                    pt = ((pd->ww_final.pointtype - 1) % N_STARTYPES);
                                    while (pt < 0) pt += N_STARTYPES;
                                    x->StarTypesUsed[pt] = 1;
                                    ppl_snprintfTrunc(epsbuff, sizeof(epsbuff), "/angle { 40 } def %.2f %.2f st%d",
                                                      xpos, ypos, pt + 1);
                                    eps_core_BoundingBox(x, xpos, ypos,
                                                         2 * final_pointsize * eps_StarSize[pt] * EPS_DEFAULT_PS);
                                }
                                ThreeDimBuffer_writeps(x, depth, 1, pd->ww_final.pointlinewidth, 0.0, final_pointsize,
                                                       last_colstr, epsbuff);
                            }

                            // label point if instructed to do so
                            if ((blk->text[j] != NULL) && (blk->text[j][0] != '\0')) {
                                char *text = NULL;
                                const double xsp = (x->current->settings.TextVAlign == SW_VALIGN_CENT) ? 1.7 : 1.13;
                                const double ysp = (x->current->settings.TextHAlign == SW_HALIGN_CENT) ? 1.7 : 1.13;
                                // Keep a copy of the current color for the depth-sorting buffer, if it has changed
                                if ((last_colstr == NULL) || (strcmp(last_colstr, x->CurrentColor) != 0)) {
                                    last_colstr = (char *) ppl_memAlloc(strlen(x->CurrentColor) + 1);
                                    if (last_colstr == NULL) break;
                                    strcpy(last_colstr, x->CurrentColor);
                                }
                                canvas_EPSRenderTextItem(
                                        x, &text, x->LaTeXpageno++,
                                        xpos / M_TO_PS -
                                                (x->current->settings.TextHAlign - SW_HALIGN_CENT) * final_pointsize *
                                                        eps_PointSize[pt] * EPS_DEFAULT_PS / M_TO_PS * xsp,
                                        ypos / M_TO_PS +
                                                (x->current->settings.TextVAlign - SW_VALIGN_CENT) * final_pointsize *
                                                        eps_PointSize[pt] * EPS_DEFAULT_PS / M_TO_PS * ysp,
                                        x->current->settings.TextHAlign, x->current->settings.TextVAlign,
                                        x->CurrentColor, x->current->settings.FontSize, 0.0, NULL, NULL);
                                if (text != NULL) ThreeDimBuffer_writeps(x, depth, 1, 1, 0, 1, last_colstr, text);
                            }
                        }
                        blk = blk->next;
                    }
                }

                // Error bars and error ranges
                if ((style == SW_STYLE_XERRORBARS) || (style == SW_STYLE_YERRORBARS) ||
                    (style == SW_STYLE_ZERRORBARS) || (style == SW_STYLE_XYERRORBARS) ||
                    (style == SW_STYLE_YZERRORBARS) || (style == SW_STYLE_XZERRORBARS) ||
                    (style == SW_STYLE_XYZERRORBARS) || (style == SW_STYLE_XERRORRANGE) ||
                    (style == SW_STYLE_YERRORRANGE) || (style == SW_STYLE_ZERRORRANGE) ||
                    (style == SW_STYLE_XYERRORRANGE) || (style == SW_STYLE_YZERRORRANGE) ||
                    (style == SW_STYLE_XZERRORRANGE) ||
                    (style == SW_STYLE_XYZERRORRANGE)) // XERRORBARS , YERRORBARS , ZERRORBARS
                {
                    unsigned char ac[3] = {0, 0, 0};
                    double min[3], max[3];
                    int NDirections = 0;

                    // The size of the bars at the ends of the error bars, and of the points
                    const double b = 0.0005 * sg->bar;
                    const double ps = pd->ww_final.pointsize * EPS_DEFAULT_PS / M_TO_PS;

                    // Draw the error bars with line segments
                    ld = LineDraw_Init(x, a[xn], a[yn], a[zn], xrn, yrn, zrn, sg, ThreeDim, origin_x, origin_y, scale_x,
                                       scale_y, scale_z);
                    last_colstr = NULL;

                    // Loop over the blocks of the data table
                    while (blk != NULL) {
                        // Draw each data point's error bars
                        for (j = 0; j < blk->blockPosition; j++) {
                            const double *row = blk->data_real + Ncolumns * j; // The data in this row
                            // Work out style information for next point
                            eps_plot_WithWordsFromUsingItems(x->c, &pd->ww_final, &blk->data_real[Ncolumns * j],
                                                             &blk->data_obj[Ncol_obj * j], Ncolumns, Ncol_obj);
                            eps_core_SetColor(x, &pd->ww_final, 0);
                            LineDraw_PenUp(x, ld);
                            // Draw the error bars, unless they are invisible
                            if (eps_core_isVisible(x)) {
                                for (i = 0; i < 3; i++) { min[i] = max[i] = row[i]; }
                                // Work out the extent of the error bars in each direction, which are given either as
                                // errors or as ranges
                                if (style == SW_STYLE_XERRORBARS) {
                                    NDirections = 1;
                                    ac[0] = 1;
                                    min[0] = row[0] - row[2 + ThreeDim];
                                    max[0] = row[0] + row[2 + ThreeDim];
                                } else if (style == SW_STYLE_YERRORBARS) {
                                    NDirections = 1;
                                    ac[1] = 1;
                                    min[1] = row[1] - row[2 + ThreeDim];
                                    max[1] = row[1] + row[2 + ThreeDim];
                                } else if (style == SW_STYLE_ZERRORBARS) {
                                    NDirections = 1;
                                    ac[2] = 1;
                                    min[2] = row[2] - row[3];
                                    max[2] = row[2] + row[3];
                                } else if (style == SW_STYLE_XERRORRANGE) {
                                    NDirections = 1;
                                    ac[0] = 1;
                                    min[0] = row[2 + ThreeDim];
                                    max[0] = row[3 + ThreeDim];
                                } else if (style == SW_STYLE_YERRORRANGE) {
                                    NDirections = 1;
                                    ac[1] = 1;
                                    min[1] = row[2 + ThreeDim];
                                    max[1] = row[3 + ThreeDim];
                                } else if (style == SW_STYLE_ZERRORRANGE) {
                                    NDirections = 1;
                                    ac[2] = 1;
                                    min[2] = row[3];
                                    max[2] = row[4];
                                } else if (style == SW_STYLE_XYERRORBARS) {
                                    NDirections = 2;
                                    ac[0] = ac[1] = 1;
                                    min[0] = row[0] - row[2 + ThreeDim];
                                    max[0] = row[0] + row[2 + ThreeDim];
                                    min[1] = row[1] - row[3 + ThreeDim];
                                    max[1] = row[1] + row[3 + ThreeDim];
                                } else if (style == SW_STYLE_XZERRORBARS) {
                                    NDirections = 2;
                                    ac[0] = ac[2] = 1;
                                    min[0] = row[0] - row[3];
                                    max[0] = row[0] + row[3];
                                    min[2] = row[2] - row[4];
                                    max[2] = row[2] + row[4];
                                } else if (style == SW_STYLE_YZERRORBARS) {
                                    NDirections = 2;
                                    ac[1] = ac[2] = 1;
                                    min[1] = row[1] - row[3];
                                    max[1] = row[1] + row[3];
                                    min[2] = row[2] - row[4];
                                    max[2] = row[2] + row[4];
                                } else if (style == SW_STYLE_XYERRORRANGE) {
                                    NDirections = 2;
                                    ac[0] = ac[1] = 1;
                                    min[0] = row[2 + ThreeDim];
                                    max[0] = row[3 + ThreeDim];
                                    min[1] = row[4 + ThreeDim];
                                    max[1] = row[5 + ThreeDim];
                                } else if (style == SW_STYLE_YZERRORRANGE) {
                                    NDirections = 2;
                                    ac[1] = ac[2] = 1;
                                    min[1] = row[3];
                                    max[1] = row[4];
                                    min[2] = row[5];
                                    max[2] = row[6];
                                } else if (style == SW_STYLE_XZERRORRANGE) {
                                    NDirections = 2;
                                    ac[0] = ac[2] = 1;
                                    min[0] = row[3];
                                    max[0] = row[4];
                                    min[2] = row[5];
                                    max[2] = row[6];
                                } else if (style == SW_STYLE_XYZERRORBARS) {
                                    NDirections = 3;
                                    ac[0] = ac[1] = ac[2] = 1;
                                    min[0] = row[0] - row[3];
                                    max[0] = row[0] + row[3];
                                    min[1] = row[1] - row[4];
                                    max[1] = row[1] + row[4];
                                    min[2] = row[2] - row[5];
                                    max[2] = row[2] + row[6];
                                } else if (style == SW_STYLE_XYZERRORRANGE) {
                                    NDirections = 3;
                                    ac[0] = ac[1] = ac[2] = 1;
                                    min[0] = row[3];
                                    max[0] = row[4];
                                    min[1] = row[5];
                                    max[1] = row[6];
                                    min[2] = row[7];
                                    max[2] = row[8];
                                }

                                // On logarithmic axes, error bars may not extend to zero or below
                                if (a[0]->LogFinal == SW_BOOL_TRUE) {
                                    if (min[0] < DBL_MIN) min[0] = DBL_MIN;
                                    if (max[0] < DBL_MIN) max[0] = DBL_MIN;
                                }
                                // On logarithmic axes, error bars may not extend to zero or below
                                if (a[1]->LogFinal == SW_BOOL_TRUE) {
                                    if (min[1] < DBL_MIN) min[1] = DBL_MIN;
                                    if (max[1] < DBL_MIN) max[1] = DBL_MIN;
                                }
                                // On logarithmic axes, error bars may not extend to zero or below
                                if (a[2]->LogFinal == SW_BOOL_TRUE) {
                                    if (min[2] < DBL_MIN) min[2] = DBL_MIN;
                                    if (max[2] < DBL_MIN) max[2] = DBL_MIN;
                                }

                                // Keep a copy of the current color for the depth-sorting buffer, if it has changed
                                if ((last_colstr == NULL) || (strcmp(last_colstr, x->CurrentColor) != 0)) {
                                    last_colstr = (char *) ppl_memAlloc(strlen(x->CurrentColor) + 1);
                                    if (last_colstr == NULL) break;
                                    strcpy(last_colstr, x->CurrentColor);
                                }

                                // Draw an error bar in each direction which has one
                                for (i = 0; i < 3; i++)
                                    // Draw the error bar, with bars across its ends
                                    if (ac[i]) {
                                        LineDraw_Point(x, ld, (i == xn) ? (min[xn]) : (row[xn]),
                                                       (i == yn) ? (min[yn]) : (row[yn]),
                                                       ThreeDim ? ((i == zn) ? (min[zn]) : (row[zn])) : 0.0, 0, 0, 0,
                                                       (i == xn) ? (b) : 0, (i == yn) ? (b) : 0, (i == zn) ? (b) : 0,
                                                       pd->ww_final.linetype, pd->ww_final.linewidth, last_colstr);
                                        LineDraw_Point(x, ld, (i == xn) ? (min[xn]) : (row[xn]),
                                                       (i == yn) ? (min[yn]) : (row[yn]),
                                                       ThreeDim ? ((i == zn) ? (min[zn]) : (row[zn])) : 0.0, 0, 0, 0,
                                                       (i == xn) ? (-b) : 0, (i == yn) ? (-b) : 0, (i == zn) ? (-b) : 0,
                                                       pd->ww_final.linetype, pd->ww_final.linewidth, last_colstr);
                                        LineDraw_PenUp(x, ld);
                                        LineDraw_Point(x, ld, (i == xn) ? (min[xn]) : (row[xn]),
                                                       (i == yn) ? (min[yn]) : (row[yn]),
                                                       ThreeDim ? ((i == zn) ? (min[zn]) : (row[zn])) : 0.0, 0, 0, 0, 0,
                                                       0, 0, pd->ww_final.linetype, pd->ww_final.linewidth,
                                                       last_colstr);
                                        LineDraw_Point(x, ld, (i == xn) ? (max[xn]) : (row[xn]),
                                                       (i == yn) ? (max[yn]) : (row[yn]),
                                                       ThreeDim ? ((i == zn) ? (max[zn]) : (row[zn])) : 0.0, 0, 0, 0, 0,
                                                       0, 0, pd->ww_final.linetype, pd->ww_final.linewidth,
                                                       last_colstr);
                                        LineDraw_PenUp(x, ld);
                                        LineDraw_Point(x, ld, (i == xn) ? (max[xn]) : (row[xn]),
                                                       (i == yn) ? (max[yn]) : (row[yn]),
                                                       ThreeDim ? ((i == zn) ? (max[zn]) : (row[zn])) : 0.0, 0, 0, 0,
                                                       (i == xn) ? (b) : 0, (i == yn) ? (b) : 0, (i == zn) ? (b) : 0,
                                                       pd->ww_final.linetype, pd->ww_final.linewidth, last_colstr);
                                        LineDraw_Point(x, ld, (i == xn) ? (max[xn]) : (row[xn]),
                                                       (i == yn) ? (max[yn]) : (row[yn]),
                                                       ThreeDim ? ((i == zn) ? (max[zn]) : (row[zn])) : 0.0, 0, 0, 0,
                                                       (i == xn) ? (-b) : 0, (i == yn) ? (-b) : 0, (i == zn) ? (-b) : 0,
                                                       pd->ww_final.linetype, pd->ww_final.linewidth, last_colstr);
                                        LineDraw_PenUp(x, ld);
                                        if (NDirections != 1)
                                            continue; // Only put a central bar through errorbars which only go in a single direction
                                        LineDraw_Point(x, ld, (row[xn]), (row[yn]), ThreeDim ? (row[zn]) : 0.0, 0, 0, 0,
                                                       (i == xn) ? (ps) : 0, (i == yn) ? (ps) : 0, (i == zn) ? (ps) : 0,
                                                       pd->ww_final.linetype, pd->ww_final.linewidth, last_colstr);
                                        LineDraw_Point(x, ld, (row[xn]), (row[yn]), ThreeDim ? (row[zn]) : 0.0, 0, 0, 0,
                                                       (i == xn) ? (-ps) : 0, (i == yn) ? (-ps) : 0,
                                                       (i == zn) ? (-ps) : 0, pd->ww_final.linetype,
                                                       pd->ww_final.linewidth, last_colstr);
                                        LineDraw_PenUp(x, ld);
                                    }
                            }

                            // label point if instructed to do so
                            if ((blk->text[j] != NULL) && (blk->text[j][0] != '\0')) {
                                char *text = NULL;
                                // Keep a copy of the current color for the depth-sorting buffer, if it has changed
                                if ((last_colstr == NULL) || (strcmp(last_colstr, x->CurrentColor) != 0)) {
                                    last_colstr = (char *) ppl_memAlloc(strlen(x->CurrentColor) + 1);
                                    if (last_colstr == NULL) break;
                                    strcpy(last_colstr, x->CurrentColor);
                                }
                                eps_plot_GetPosition(&xpos, &ypos, &depth, &xap, &yap, &zap, NULL, NULL, NULL, ThreeDim,
                                                     row[xn], row[yn], ThreeDim ? row[zn] : 0.0, a[xn], a[yn], a[zn],
                                                     xrn, yrn, zrn, sg, origin_x, origin_y, scale_x, scale_y, scale_z,
                                                     0);
                                // Labels of points off the edge of the graph are skipped
                                if (!gsl_finite(xpos)) {
                                    x->LaTeXpageno++;
                                    continue;
                                } // Position of point is off side of graph
                                canvas_EPSRenderTextItem(x, &text, x->LaTeXpageno++, xpos / M_TO_PS, ypos / M_TO_PS,
                                                         x->current->settings.TextHAlign,
                                                         x->current->settings.TextVAlign, x->CurrentColor,
                                                         x->current->settings.FontSize, 0.0, NULL, NULL);
                                if (text != NULL) ThreeDimBuffer_writeps(x, depth, 1, 1, 0, 1, last_colstr, text);
                            }
                        }
                        blk = blk->next;
                    }
                }

                else if (style == SW_STYLE_IMPULSES) // IMPULSES
                {
                    ld = LineDraw_Init(x, a[xn], a[yn], a[zn], xrn, yrn, zrn, sg, ThreeDim, origin_x, origin_y, scale_x,
                                       scale_y, scale_z);
                    last_colstr = NULL;

                    // Impulses run from the value set with "set boxfrom", which must have the same units as the data
                    if (a[yn]->DataUnitSet && (!ppl_unitsDimEqual(&sg->BoxFrom, &a[yn]->DataUnit))) {
                        ppl_snprintfTrunc(
                                msg, sizeof(msg),
                                "Data with units of <%s> plotted with impulses when BoxFrom is set to a value with units of <%s>.",
                                ppl_printUnit(x->c, &a[yn]->DataUnit, NULL, NULL, 0, 1, 0),
                                ppl_printUnit(x->c, &sg->BoxFrom, NULL, NULL, 1, 1, 0));
                        ppl_error(&x->c->errcontext, ERR_GENERIC, -1, -1, msg);
                    } else {
                        // Loop over the blocks of the data table
                        while (blk != NULL) {
                            // Draw a line from the box origin to each data point
                            for (j = 0; j < blk->blockPosition; j++) {
                                const double *row = blk->data_real + Ncolumns * j; // The data in this row
                                // Work out style information for next point
                                eps_plot_WithWordsFromUsingItems(x->c, &pd->ww_final, &blk->data_real[Ncolumns * j],
                                                                 &blk->data_obj[Ncol_obj * j], Ncolumns, Ncol_obj);
                                eps_core_SetColor(x, &pd->ww_final, 0);
                                LineDraw_PenUp(x, ld);
                                // Draw the impulse, unless it is invisible
                                if (eps_core_isVisible(x)) {
                                    double boxfrom = sg->BoxFrom.real;
                                    if ((a[yn]->LogFinal == SW_BOOL_TRUE) && (boxfrom < DBL_MIN)) boxfrom = DBL_MIN;
                                    // Keep a copy of the current color for the depth-sorting buffer, if it has changed
                                    if ((last_colstr == NULL) || (strcmp(last_colstr, x->CurrentColor) != 0)) {
                                        last_colstr = (char *) ppl_memAlloc(strlen(x->CurrentColor) + 1);
                                        if (last_colstr == NULL) break;
                                        strcpy(last_colstr, x->CurrentColor);
                                    }
                                    LineDraw_Point(x, ld, (xn == 1) ? boxfrom : (row[xn]),
                                                   (yn == 1) ? boxfrom : (row[yn]),
                                                   ThreeDim ? ((zn == 1) ? boxfrom : (row[zn])) : 0.0, 0, 0, 0, 0, 0, 0,
                                                   pd->ww_final.linetype, pd->ww_final.linewidth, last_colstr);
                                    LineDraw_Point(x, ld, row[xn], row[yn], ThreeDim ? row[zn] : 0.0, 0, 0, 0, 0, 0, 0,
                                                   pd->ww_final.linetype, pd->ww_final.linewidth, last_colstr);
                                }
                                LineDraw_PenUp(x, ld);

                                // label point if instructed to do so
                                if ((blk->text[j] != NULL) && (blk->text[j][0] != '\0')) {
                                    char *text = NULL;
                                    // Keep a copy of the current color for the depth-sorting buffer, if it has changed
                                    if ((last_colstr == NULL) || (strcmp(last_colstr, x->CurrentColor) != 0)) {
                                        last_colstr = (char *) ppl_memAlloc(strlen(x->CurrentColor) + 1);
                                        if (last_colstr == NULL) break;
                                        strcpy(last_colstr, x->CurrentColor);
                                    }
                                    eps_plot_GetPosition(&xpos, &ypos, &depth, &xap, &yap, &zap, NULL, NULL, NULL,
                                                         ThreeDim, row[xn], row[yn], ThreeDim ? row[zn] : 0.0, a[xn],
                                                         a[yn], a[zn], xrn, yrn, zrn, sg, origin_x, origin_y, scale_x,
                                                         scale_y, scale_z, 0);
                                    // Labels of points off the edge of the graph are skipped
                                    if (!gsl_finite(xpos)) {
                                        x->LaTeXpageno++;
                                        continue;
                                    } // Position of point is off side of graph
                                    canvas_EPSRenderTextItem(x, &text, x->LaTeXpageno++, xpos / M_TO_PS, ypos / M_TO_PS,
                                                             x->current->settings.TextHAlign,
                                                             x->current->settings.TextVAlign, x->CurrentColor,
                                                             x->current->settings.FontSize, 0.0, NULL, NULL);
                                    if (text != NULL) ThreeDimBuffer_writeps(x, depth, 1, 1, 0, 1, last_colstr, text);
                                }
                            }
                            blk = blk->next;
                        }
                    }
                }

                else if ((style == SW_STYLE_LOWERLIMITS) || (style == SW_STYLE_UPPERLIMITS)) // LOWERLIMITS, UPPERLIMITS
                {
                    int an;
                    double theta[3];
                    const double ps = pd->ww_final.pointsize * EPS_DEFAULT_PS / M_TO_PS;
                    const double sgn = ((style == SW_STYLE_UPPERLIMITS) ^ (a[yn]->MaxFinal > a[yn]->MinFinal)) ? 1.0 : -1.0;
                    const double lw = pd->ww_final.pointlinewidth;

                    // Draw the limits with line segments
                    ld = LineDraw_Init(x, a[xn], a[yn], a[zn], xrn, yrn, zrn, sg, ThreeDim, origin_x, origin_y, scale_x,
                                       scale_y, scale_z);
                    last_colstr = NULL;

                    // Loop over the blocks of the data table
                    while (blk != NULL) {
                        // Draw a bar and an arrow at each data point, pointing up for lower limits and down for upper
                        // limits
                        for (j = 0; j < blk->blockPosition; j++) {
                            const double *row = blk->data_real + Ncolumns * j; // The data in this row
                            // Work out style information for next point
                            eps_plot_WithWordsFromUsingItems(x->c, &pd->ww_final, &blk->data_real[Ncolumns * j],
                                                             &blk->data_obj[Ncol_obj * j], Ncolumns, Ncol_obj);
                            eps_core_SetColor(x, &pd->ww_final, 0);
                            LineDraw_PenUp(x, ld);
                            // Draw the limit, unless it is invisible
                            if (eps_core_isVisible(x)) {
                                eps_plot_GetPosition(&xpos, &ypos, &depth, &xap, &yap, &zap, &theta[0], &theta[1],
                                                     &theta[2], ThreeDim, row[xn], row[yn], ThreeDim ? row[zn] : 0.0,
                                                     a[xn], a[yn], a[zn], xrn, yrn, zrn, sg, origin_x, origin_y,
                                                     scale_x, scale_y, scale_z, 0);
                                if (!gsl_finite(xpos)) // Position of point is off side of graph
                                {
                                    if ((blk->text[j] != NULL) && (blk->text[j][0] != '\0')) x->LaTeXpageno++;
                                    continue;
                                }

                                // The direction in which the arrow points is that of the vertical axis
                                if (xn == 1) an = 0;
                                else if (yn == 1) an = 1;
                                else an = 2;
                                // Keep a copy of the current color for the depth-sorting buffer, if it has changed
                                if ((last_colstr == NULL) || (strcmp(last_colstr, x->CurrentColor) != 0)) {
                                    last_colstr = (char *) ppl_memAlloc(strlen(x->CurrentColor) + 1);
                                    if (last_colstr == NULL) break;
                                    strcpy(last_colstr, x->CurrentColor);
                                }

                                // The offsets of the ends of the bar, and of the tip of the arrow
                                const double offset_barx = ps * sin(theta[an] + M_PI / 2);
                                const double offset_bary = ps * cos(theta[an] + M_PI / 2);
                                const double offset_arrx = sgn * 2 * ps * sin(theta[an]);
                                const double offset_arry = sgn * 2 * ps * cos(theta[an]);

                                // Draw the bar across the point
                                LineDraw_Point(x, ld, row[xn], row[yn], ThreeDim ? row[zn] : 0.0, -offset_barx,
                                               -offset_bary, 0, 0, 0, 0, pd->ww_final.linetype, lw, last_colstr);
                                LineDraw_Point(x, ld, row[xn], row[yn], ThreeDim ? row[zn] : 0.0, offset_barx,
                                               offset_bary, 0, 0, 0, 0, pd->ww_final.linetype, lw, last_colstr);
                                LineDraw_PenUp(x, ld);
                                LineDraw_Point(x, ld, row[xn], row[yn], ThreeDim ? row[zn] : 0.0, 0, 0, 0, 0, 0, 0,
                                               pd->ww_final.linetype, lw, last_colstr);
                                LineDraw_Point(x, ld, row[xn], row[yn], ThreeDim ? row[zn] : 0.0, offset_arrx,
                                               offset_arry, 0, 0, 0, 0, pd->ww_final.linetype, lw, last_colstr);
                                LineDraw_PenUp(x, ld);
                                const double x2 = xpos + sgn * 2 * ps * M_TO_PS * sin(theta[an]);
                                const double y2 = ypos + sgn * 2 * ps * M_TO_PS * cos(theta[an]);
                                if (sgn < 0.0) theta[an] += M_PI;
                                eps_arrowHead(x2, y2, theta[an], EPS_ARROW_HEADSIZE / 2 * lw, epsbuff, sizeof(epsbuff),
                                              NULL);
                                ThreeDimBuffer_writeps(x, depth, 1, lw, 0.0, 1.0, last_colstr, epsbuff);
                            }

                            // label point if instructed to do so
                            if ((blk->text[j] != NULL) && (blk->text[j][0] != '\0')) {
                                char *text = NULL;
                                // Keep a copy of the current color for the depth-sorting buffer, if it has changed
                                if ((last_colstr == NULL) || (strcmp(last_colstr, x->CurrentColor) != 0)) {
                                    last_colstr = (char *) ppl_memAlloc(strlen(x->CurrentColor) + 1);
                                    if (last_colstr == NULL) break;
                                    strcpy(last_colstr, x->CurrentColor);
                                }
                                canvas_EPSRenderTextItem(x, &text, x->LaTeXpageno++, xpos / M_TO_PS, ypos / M_TO_PS,
                                                         x->current->settings.TextHAlign,
                                                         x->current->settings.TextVAlign, x->CurrentColor,
                                                         x->current->settings.FontSize, 0.0, NULL, NULL);
                                if (text != NULL) ThreeDimBuffer_writeps(x, depth, 1, 1, 0, 1, last_colstr, text);
                            }
                        }
                        blk = blk->next;
                    }
                }

                // Boxes and steps
                else if ((style == SW_STYLE_BOXES) || (style == SW_STYLE_WBOXES) || (style == SW_STYLE_STEPS) ||
                         (style == SW_STYLE_FSTEPS) ||
                         (style == SW_STYLE_HISTEPS)) // BOXES, WBOXES, STEPS, FSTEPS, HISTEPS
                {
                    double ptBx = 0, ptBy = 0, ptCx = 0, ptCy = 0;
                    unsigned char ptAset = 0, ptBset = 0, ptCset = 0;

                    // Draw the outlines of the boxes and steps with line segments
                    ld = LineDraw_Init(x, a[xn], a[yn], a[zn], xrn, yrn, zrn, sg, ThreeDim, origin_x, origin_y, scale_x,
                                       scale_y, scale_z);
                    last_colstr = NULL;

                    // Boxes and steps run from the value set with "set boxfrom", which must have the same units as the
                    // data, and their widths must have the units of the x axis
                    if (a[yn]->DataUnitSet && (!ppl_unitsDimEqual(&sg->BoxFrom, &a[yn]->DataUnit))) {
                        ppl_snprintfTrunc(
                                msg, sizeof(msg),
                                "Data with units of <%s> plotted as boxes/steps when BoxFrom is set to a value with units of <%s>.",
                                ppl_printUnit(x->c, &a[yn]->DataUnit, NULL, NULL, 0, 1, 0),
                                ppl_printUnit(x->c, &sg->BoxFrom, NULL, NULL, 1, 1, 0));
                        ppl_error(&x->c->errcontext, ERR_GENERIC, -1, -1, msg);
                    } else if (a[xn]->DataUnitSet && (sg->BoxWidth.real > 0.0) &&
                               (!ppl_unitsDimEqual(&sg->BoxWidth, &a[xn]->DataUnit))) {
                        ppl_snprintfTrunc(
                                msg, sizeof(msg),
                                "Data with ordinate units of <%s> plotted as boxes/steps when BoxWidth is set to a value with units of <%s>.",
                                ppl_printUnit(x->c, &a[xn]->DataUnit, NULL, NULL, 0, 1, 0),
                                ppl_printUnit(x->c, &sg->BoxWidth, NULL, NULL, 1, 1, 0));
                        ppl_error(&x->c->errcontext, ERR_GENERIC, -1, -1, msg);
                    } else {
                        const unsigned char logaxis = (a[xn]->LogFinal == SW_BOOL_TRUE);
                        boxDrawer bd = {
                                x,
                                ld,
                                pd,
                                {a[xn], a[yn], a[zn], xrn, yrn, zrn, sg, ThreeDim, origin_x, origin_y, scale_x, scale_y, scale_z},
                                a,
                                xn,
                                yn,
                                zn,
                                logaxis,
                                NULL};
                        // Loop over the blocks of the data table
                        while (blk != NULL) {
                            // Each box or step is drawn when the next point is read, since its width depends on its
                            // neighbours
                            for (j = 0; j < blk->blockPosition; j++) {
                                const double *row = blk->data_real + Ncolumns * j; // The data in this row
                                // A split in the data ends the run of boxes or steps
                                if (blk->split[j]) {
                                    if (finishRowOfBoxes(&bd, style, &ptAset, &ptBset, &ptCset, ptBx, ptCx, ptCy))
                                        break;
                                }
                                const double ptAx = ptBx;
                                ptAset = ptBset;
                                ptBx = ptCx;
                                ptBy = ptCy;
                                ptBset = ptCset;
                                ptCx = logaxis ? log(row[0]) : (row[0]);
                                ptCy = row[1];
                                ptCset = 1;
                                // Draw the box or step centred on the previous point
                                if (ptBset) {
                                    if (ptAset) // We are processing a box in the midst of many
                                    {
                                        // Automatic box widths extend half way to the neighbouring points
                                        if ((style == SW_STYLE_BOXES) && (sg->BoxWidth.real < 1e-200)) {
                                            if (makeBox(&bd, (ptBx + (ptAx + ptCx) / 2) / 2, ptBy, (ptCx - ptAx) / 4))
                                                break;
                                        } else if (style == SW_STYLE_BOXES) {
                                            if (makeBox(&bd, ptBx, ptBy, sg->BoxWidth.real / 2)) break;
                                        } else if (style == SW_STYLE_HISTEPS) {
                                            if (makeStep(&bd, (ptBx + (ptAx + ptCx) / 2) / 2, ptBy, (ptCx - ptAx) / 4))
                                                break;
                                        } else if (style == SW_STYLE_STEPS) {
                                            if (makeStep(&bd, (ptAx + ptBx) / 2, ptBy, (ptBx - ptAx) / 2)) break;
                                        } else if (style == SW_STYLE_FSTEPS) {
                                            if (makeStep(&bd, (ptBx + ptCx) / 2, ptBy, (ptCx - ptBx) / 2)) break;
                                        }
                                    } else // The first box/step we work out the width of
                                    {
                                        // Automatic box widths extend half way to the neighbouring point
                                        if ((style == SW_STYLE_BOXES) && (sg->BoxWidth.real < 1e-200)) {
                                            if (makeBox(&bd, ptBx, ptBy, (ptCx - ptBx) / 2)) break;
                                        } else if (style == SW_STYLE_BOXES) {
                                            if (makeBox(&bd, ptBx, ptBy, sg->BoxWidth.real / 2)) break;
                                        } else if (style == SW_STYLE_HISTEPS) {
                                            if (makeStep(&bd, ptBx, ptBy, (ptCx - ptBx) / 2)) break;
                                        } else if (style == SW_STYLE_STEPS) {
                                            if (makeStep(&bd, ptBx, ptBy, 0.0)) break;
                                        } else if (style == SW_STYLE_FSTEPS) {
                                            if (makeStep(&bd, (ptBx + ptCx) / 2, ptBy, (ptCx - ptBx) / 2)) break;
                                        }
                                    }
                                }
                                // Work out style information for next point
                                eps_plot_WithWordsFromUsingItems(x->c, &pd->ww_final, &blk->data_real[Ncolumns * j],
                                                                 &blk->data_obj[Ncol_obj * j], Ncolumns, Ncol_obj);
                                eps_core_SetColor(x, &pd->ww_final, 0);
                                if (style == SW_STYLE_WBOXES) {
                                    if (makeBox(&bd, ptCx, ptCy, row[2] / 2)) break;
                                }
                            }
                            blk = blk->next;
                        }
                        if (finishRowOfBoxes(&bd, style, &ptAset, &ptBset, &ptCset, ptBx, ptCx, ptCy)) break;
                    }
                    LineDraw_PenUp(x, ld);
                }

                // Arrows
                else if ((style == SW_STYLE_ARROWS_HEAD) || (style == SW_STYLE_ARROWS_NOHEAD) ||
                         (style == SW_STYLE_ARROWS_TWOHEAD)) // ARROWS_HEAD, ARROWS_NOHEAD, ARROWS_TWOHEAD
                {
                    double xpos2, ypos2, depth2, xap2, yap2, zap2;
                    const double lw = pd->ww_final.linewidth;

                    // Draw the shafts of the arrows with line segments
                    ld = LineDraw_Init(x, a[xn], a[yn], a[zn], xrn, yrn, zrn, sg, ThreeDim, origin_x, origin_y, scale_x,
                                       scale_y, scale_z);
                    last_colstr = NULL;

                    // Loop over the blocks of the data table
                    while (blk != NULL) {
                        // Draw an arrow for each data row, from its first point to its second
                        for (j = 0; j < blk->blockPosition; j++) {
                            const double *row = blk->data_real + Ncolumns * j; // The data in this row
                            // Work out style information for next point
                            eps_plot_WithWordsFromUsingItems(x->c, &pd->ww_final, &blk->data_real[Ncolumns * j],
                                                             &blk->data_obj[Ncol_obj * j], Ncolumns, Ncol_obj);
                            eps_core_SetColor(x, &pd->ww_final, 0);
                            LineDraw_PenUp(x, ld);
                            // Draw the arrow, unless it is invisible
                            if (eps_core_isVisible(x)) {
                                // Keep a copy of the current color for the depth-sorting buffer, if it has changed
                                if ((last_colstr == NULL) || (strcmp(last_colstr, x->CurrentColor) != 0)) {
                                    last_colstr = (char *) ppl_memAlloc(strlen(x->CurrentColor) + 1);
                                    if (last_colstr == NULL) break;
                                    strcpy(last_colstr, x->CurrentColor);
                                }
                                LineDraw_Point(x, ld, row[xn], row[yn], ThreeDim ? row[zn] : 0.0, 0, 0, 0, 0, 0, 0,
                                               pd->ww_final.linetype, lw, last_colstr);
                                LineDraw_Point(x, ld, row[xn + 2 + ThreeDim], row[yn + 2 + ThreeDim],
                                               ThreeDim ? row[zn + 3] : 0.0, 0, 0, 0, 0, 0, 0, pd->ww_final.linetype,
                                               lw, last_colstr);
                                LineDraw_PenUp(x, ld);

                                // Find where the two ends of the arrow lie on the page
                                eps_plot_GetPosition(&xpos, &ypos, &depth, &xap, &yap, &zap, NULL, NULL, NULL, ThreeDim,
                                                     row[xn], row[yn], ThreeDim ? row[zn] : 0.0, a[xn], a[yn], a[zn],
                                                     xrn, yrn, zrn, sg, origin_x, origin_y, scale_x, scale_y, scale_z,
                                                     1);
                                eps_plot_GetPosition(&xpos2, &ypos2, &depth2, &xap2, &yap2, &zap2, NULL, NULL, NULL,
                                                     ThreeDim, row[xn + 2 + ThreeDim], row[yn + 2 + ThreeDim],
                                                     ThreeDim ? row[zn + 3] : 0.0, a[xn], a[yn], a[zn], xrn, yrn, zrn,
                                                     sg, origin_x, origin_y, scale_x, scale_y, scale_z, 1);
                                if ((!gsl_finite(xpos)) || (!gsl_finite(ypos)) || (!gsl_finite(xpos2)) ||
                                    (!gsl_finite(ypos2)) ||
                                    (ThreeDim && ((!gsl_finite(depth)) || (!gsl_finite(depth2))))) {
                                    if ((blk->text[j] != NULL) && (blk->text[j][0] != '\0')) x->LaTeXpageno++;
                                    continue;
                                }
                                double theta = atan2(xpos2 - xpos, ypos2 - ypos);
                                if (!gsl_finite(theta)) theta = 0.0;

                                // Draw an arrowhead at the start, if it is within the graph
                                if ((style == SW_STYLE_ARROWS_TWOHEAD) && (xap >= 0.0) && (xap <= 1.0) &&
                                    (yap >= 0.0) && (yap <= 1.0) && ((!ThreeDim) || ((zap >= 0.0) && (zap <= 1.0)))) {
                                    eps_arrowHead(xpos, ypos, theta + M_PI, EPS_ARROW_HEADSIZE * lw, epsbuff, sizeof(epsbuff), NULL);
                                    ThreeDimBuffer_writeps(x, depth, 1, lw, 0.0, 1.0, last_colstr, epsbuff);
                                }

                                // Draw an arrowhead at the end, if it is within the graph
                                if (((style == SW_STYLE_ARROWS_TWOHEAD) || (style == SW_STYLE_ARROWS_HEAD)) &&
                                    (xap2 >= 0.0) && (xap2 <= 1.0) && (yap2 >= 0.0) && (yap2 <= 1.0) &&
                                    ((!ThreeDim) || ((zap2 >= 0.0) && (zap2 <= 1.0)))) {
                                    eps_arrowHead(xpos2, ypos2, theta, EPS_ARROW_HEADSIZE * lw, epsbuff, sizeof(epsbuff), NULL);
                                    ThreeDimBuffer_writeps(x, depth2, 1, lw, 0.0, 1.0, last_colstr, epsbuff);
                                }
                            }

                            // label point if instructed to do so
                            if ((blk->text[j] != NULL) && (blk->text[j][0] != '\0')) {
                                char *text = NULL;
                                if ((xap2 < 0.0) || (xap2 > 1.0) || (yap2 < 0.0) || (yap2 > 1.0) ||
                                    (ThreeDim && ((zap2 < 0.0) || (zap2 > 1.0)))) {
                                    x->LaTeXpageno++;
                                    continue;
                                }
                                // Keep a copy of the current color for the depth-sorting buffer, if it has changed
                                if ((last_colstr == NULL) || (strcmp(last_colstr, x->CurrentColor) != 0)) {
                                    last_colstr = (char *) ppl_memAlloc(strlen(x->CurrentColor) + 1);
                                    if (last_colstr == NULL) break;
                                    strcpy(last_colstr, x->CurrentColor);
                                }
                                canvas_EPSRenderTextItem(x, &text, x->LaTeXpageno++, xpos2 / M_TO_PS, ypos2 / M_TO_PS,
                                                         x->current->settings.TextHAlign,
                                                         x->current->settings.TextVAlign, x->CurrentColor,
                                                         x->current->settings.FontSize, 0.0, NULL, NULL);
                                if (text != NULL) ThreeDimBuffer_writeps(x, depth2, 1, 1, 0, 1, last_colstr, text);
                            }
                        }
                        blk = blk->next;
                    }
                }

                else if (style == SW_STYLE_FILLEDREGION) // FILLEDREGION
                {
                    FilledRegionHandle *fr = FilledRegion_Init(x, a[xn], a[yn], a[zn], xrn, yrn, zrn, sg, ThreeDim,
                                                               origin_x, origin_y, scale_x, scale_y, scale_z);

                    // Loop over the blocks of the data table
                    while (blk != NULL) {
                        // Add each data point to the outline of the region
                        for (j = 0; j < blk->blockPosition; j++) {
                            const double *row = blk->data_real + Ncolumns * j; // The data in this row
                            // Work out style information for next point
                            eps_plot_WithWordsFromUsingItems(x->c, &pd->ww_final, &blk->data_real[Ncolumns * j],
                                                             &blk->data_obj[Ncol_obj * j], Ncolumns, Ncol_obj);
                            FilledRegion_Point(x, fr, row[xn], row[yn]);
                        }
                        blk = blk->next;
                    }
                    eps_core_SetColor(x, &pd->ww_final, 1);
                    eps_core_SetFillColor(x, &pd->ww_final);
                    eps_core_SwitchTo_FillColor(x, 0);
                    FilledRegion_Finish(x, fr, pd->ww_final.linetype, pd->ww_final.linewidth, 1);
                    eps_core_SwitchFrom_FillColor(x, 1);
                }

                else if (style == SW_STYLE_YERRORSHADED) // YERRORSHADED
                {
                    int BlkNo = 0;

                    // Outline the region along the tops of the error bars, then back along their bottoms
                    FilledRegionHandle *fr = FilledRegion_Init(x, a[xn], a[yn], a[zn], xrn, yrn, zrn, sg, ThreeDim,
                                                               origin_x, origin_y, scale_x, scale_y, scale_z);

                    // First add all of the points along the tops of the error bars, moving from left to right
                    while (blk != NULL) {
                        // Add each data point along the tops
                        for (j = 0; j < blk->blockPosition; j++) {
                            const double *row = blk->data_real + Ncolumns * j; // The data in this row
                            FilledRegion_Point(x, fr, row[xn], row[yn]);
                        }
                        blk = blk->next;
                        BlkNo++;
                    }

                    // Now add the points along the bottoms of all of the error bars, moving from right to left
                    for (BlkNo--; BlkNo >= 0; BlkNo--) {
                        blk = data->first;
                        for (j = 0; j < BlkNo; j++) blk = blk->next;
                        // Add each data point along the bottoms, in reverse order
                        for (j = blk->blockPosition - 1; j >= 0; j--) {
                            const double *row = blk->data_real + Ncolumns * j; // The data in this row
                            // Work out style information for next point
                            eps_plot_WithWordsFromUsingItems(x->c, &pd->ww_final, &blk->data_real[Ncolumns * j],
                                                             &blk->data_obj[Ncol_obj * j], Ncolumns, Ncol_obj);
                            if (xn == 0) FilledRegion_Point(x, fr, row[0], row[2]);
                            else FilledRegion_Point(x, fr, row[2], row[0]);
                        }
                    }

                    // Fill the region, and stroke its outline
                    eps_core_SetColor(x, &pd->ww_final, 1);
                    eps_core_SetFillColor(x, &pd->ww_final);
                    eps_core_SwitchTo_FillColor(x, 0);
                    FilledRegion_Finish(x, fr, pd->ww_final.linetype, pd->ww_final.linewidth, 1);
                    eps_core_SwitchFrom_FillColor(x, 1);
                }

                else if ((style == SW_STYLE_SURFACE) && (xrn == 0) && (yrn == 0) && (zrn == 0)) // SURFACE
                {
                    const int XSize = pd->GridXSize;
                    const int YSize = pd->GridYSize;
                    const graphProjection proj = {a[xn],    a[yn],    a[zn],    xrn,     yrn,     zrn,    sg,
                                                  ThreeDim, origin_x, origin_y, scale_x, scale_y, scale_z};
                    double x1, y1, z1, x2, y2, z2, x3, y3, z3, x4, y4, z4;

                    // The data must form a complete grid
                    if (blk->blockPosition != ((long) XSize) * YSize) {
                        ppl_error(&x->c->errcontext, ERR_INTERNAL, -1, -1,
                                  "Surface plot yielded data grid of the wrong size.");
                    } else {
                        last_colstr = NULL;
                        for (int fill = 1; fill >= 0; fill--)
                            for (long Y = 0; Y < YSize - 1; Y++)
                                // Draw each cell of the grid as a quadrilateral, filling all the cells first and then
                                // stroking them
                                for (long X = 0; X < XSize - 1; X++) {
                                    long j = X + Y * ((long) XSize);
                                    eps_plot_WithWordsFromUsingItems(
                                            x->c, &pd->ww_final, &blk->data_real[Ncolumns * j],
                                            &blk->data_obj[Ncol_obj * j], Ncolumns,
                                            Ncol_obj); // Work out style information for next point
                                    // Use the fill color when filling, and the line color when stroking
                                    if (fill) {
                                        eps_core_SetFillColor(x, &pd->ww_final);
                                        eps_core_SwitchTo_FillColor(x, 0);
                                    } else {
                                        eps_core_SetColor(x, &pd->ww_final, 0);
                                    }
                                    // Draw the cell, unless it is invisible
                                    if (eps_core_isVisible(x)) {
                                        // Keep a copy of the current color for the depth-sorting buffer, if it has
                                        // changed
                                        if ((last_colstr == NULL) || (strcmp(last_colstr, x->CurrentColor) != 0)) {
                                            last_colstr = (char *) ppl_memAlloc(strlen(x->CurrentColor) + 1);
                                            if (last_colstr == NULL) break;
                                            strcpy(last_colstr, x->CurrentColor);
                                        }
                                        if (surfacePoint(&proj, blk->data_real + Ncolumns * j, xn, yn, zn, &x1, &y1,
                                                         &z1))
                                            continue;
                                        j++;
                                        if (surfacePoint(&proj, blk->data_real + Ncolumns * j, xn, yn, zn, &x2, &y2,
                                                         &z2))
                                            continue;
                                        j += XSize;
                                        if (surfacePoint(&proj, blk->data_real + Ncolumns * j, xn, yn, zn, &x3, &y3,
                                                         &z3))
                                            continue;
                                        j--;
                                        if (surfacePoint(&proj, blk->data_real + Ncolumns * j, xn, yn, zn, &x4, &y4,
                                                         &z4))
                                            continue; // Draw a square
                                        double depth = (z1 + z2 + z3 + z4) / 4.0;
                                        if (fill) depth += 1e-6 * fabs(depth);
                                        ppl_snprintfTrunc(
                                                epsbuff, sizeof(epsbuff),
                                                "newpath %.2f %.2f moveto %.2f %.2f lineto %.2f %.2f lineto %.2f %.2f lineto closepath %s\n",
                                                x1, y1, x2, y2, x3, y3, x4, y4, fill ? "eofill" : "stroke");
                                        ThreeDimBuffer_writeps(x, depth, pd->ww_final.linetype, pd->ww_final.linewidth,
                                                               0.0, 1, last_colstr, epsbuff);
                                        eps_core_BoundingBox(x, x1, y1, pd->ww_final.linewidth * EPS_DEFAULT_LINEWIDTH);
                                        eps_core_BoundingBox(x, x2, y2, pd->ww_final.linewidth * EPS_DEFAULT_LINEWIDTH);
                                        eps_core_BoundingBox(x, x3, y3, pd->ww_final.linewidth * EPS_DEFAULT_LINEWIDTH);
                                        eps_core_BoundingBox(x, x4, y4, pd->ww_final.linewidth * EPS_DEFAULT_LINEWIDTH);
                                    }
                                }
                    }
                }

                else if (style == SW_STYLE_COLORMAP) // COLORMAP
                {
                    // Dealt with in advance of drawing backmost axes
                }

                else if ((style == SW_STYLE_CONTOURMAP) && (xrn == 0) && (yrn == 0) && (zrn == 0)) // CONTOURMAP
                {
                    // Dealt with in advance of drawing backmost axes
                }

                // End looping over monotonic regions of axis space
            }

    // Done
    return 0;
}

//! eps_plot_LegendIcon - Draw the icon representing a plot item in a graph's legend
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] i - The number of the plot item
//! \param [in] pd - The descriptor of the plot item being drawn
//! \param [in] xpos - The x position of the icon, in postscript points
//! \param [in] ypos - The y position of the icon, in postscript points
//! \param [in] scale - The scale factor by which to draw the icon
//! \param [in] a1 - The axis onto which the first data column is plotted
//! \param [in] a2 - The axis onto which the second data column is plotted
//! \param [in] a3 - The axis onto which the third data column is plotted
//! \param [in] xn - The axis direction (0=x, 1=y, 2=z) onto which the first data column is plotted
//! \param [in] yn - The axis direction (0=x, 1=y, 2=z) onto which the second data column is plotted
//! \param [in] zn - The axis direction (0=x, 1=y, 2=z) onto which the third data column is plotted

void eps_plot_LegendIcon(EPSComm *x, const int i, canvas_plotdesc *pd, double xpos, double ypos, double scale,
                         pplset_axis *a1, pplset_axis *a2, pplset_axis *a3, const int xn, const int yn, const int zn) {
    const int style = pd->ww_final.linespoints;
    dataTable *data = x->current->plotdata[i];
    pplset_axis *a[3] = {a1, a2, a3};

    if ((data == NULL) || (data->Nrows < 1)) return; // No data present

    // Styles which draw lines are shown by a short line
    if ((style == SW_STYLE_LINES) || (style == SW_STYLE_LINESPOINTS) || (style == SW_STYLE_IMPULSES) ||
        (style == SW_STYLE_BOXES) || (style == SW_STYLE_WBOXES) || (style == SW_STYLE_STEPS) ||
        (style == SW_STYLE_FSTEPS) || (style == SW_STYLE_HISTEPS) || (style == SW_STYLE_SURFACE) ||
        (style == SW_STYLE_CONTOURMAP)) {
        eps_core_SetColor(x, &pd->ww_final, 1);
        eps_core_SetLinewidth(x, EPS_DEFAULT_LINEWIDTH * pd->ww_final.linewidth, pd->ww_final.linetype, 0);
        // Draw the line, unless it is invisible
        if (eps_core_isVisible(x)) {
            fprintf(x->epsbuffer, "newpath %.2f %.2f moveto %.2f %.2f lineto stroke\n", xpos - scale * 0.60 / 2, ypos,
                    xpos + scale * 0.60 / 2, ypos);
            eps_core_BoundingBox(x, xpos - scale * 0.60 / 2, ypos, pd->ww_final.linewidth);
            eps_core_BoundingBox(x, xpos + scale * 0.60 / 2, ypos, pd->ww_final.linewidth);
        }
    }

    // Styles which draw points are shown by a point
    if ((style == SW_STYLE_POINTS) || (style == SW_STYLE_LINESPOINTS) || (style == SW_STYLE_STARS) ||
        (style == SW_STYLE_DOTS)) {
        double final_pointsize = pd->ww_final.pointsize;
        // Dots are a twentieth of the size of points, but not too small to see
        if (style == SW_STYLE_DOTS) {
            final_pointsize *= 0.05;
            if (final_pointsize < 0.25) final_pointsize = 0.25;
        } // Dots are 1/20th size of points
        if (style == SW_STYLE_STARS) final_pointsize = 1;
        eps_core_SetColor(x, &pd->ww_final, 1);
        eps_core_SetLinewidth(x, EPS_DEFAULT_LINEWIDTH * pd->ww_final.pointlinewidth, 1, 0);
        // Draw the point, unless it is invisible
        if (eps_core_isVisible(x)) {
            int pt = (pd->ww_final.pointtype - 1) % N_POINTTYPES;
            if (style == SW_STYLE_DOTS) pt = 16; // Dots are always pt 17 (circle)
            if (style == SW_STYLE_STARS) pt = 24; // Stars are always pt 25 (star)
            while (pt < 0) pt += N_POINTTYPES;
            x->PointTypesUsed[pt] = 1;
            fprintf(x->epsbuffer, "/ps { %f } def %.2f %.2f pt%d\n", final_pointsize * EPS_DEFAULT_PS, xpos, ypos,
                    pt + 1);
            eps_core_BoundingBox(x, xpos, ypos, 2 * final_pointsize * eps_PointSize[pt] * EPS_DEFAULT_PS);
        }
    }

    // Limits are shown by a bar and a small arrow
    else if ((style == SW_STYLE_LOWERLIMITS) || (style == SW_STYLE_UPPERLIMITS)) {
        const double eah_old = EPS_ARROW_HEADSIZE;
        const double ps = pd->ww_final.pointsize * EPS_DEFAULT_PS;
        const double sgn = ((style == SW_STYLE_UPPERLIMITS) ^ (a[yn]->MaxFinal > a[yn]->MinFinal)) ? 1.0 : -1.0;
        eps_core_SetColor(x, &pd->ww_final, 1);
        eps_core_SetLinewidth(x, EPS_DEFAULT_LINEWIDTH * pd->ww_final.pointlinewidth, pd->ww_final.linetype, 0);
        // Draw the bar and the arrow, unless they are invisible
        if (eps_core_isVisible(x)) {
            fprintf(x->epsbuffer, "newpath %.2f %.2f moveto %.2f %.2f lineto stroke\n", xpos - ps, ypos - ps * sgn,
                    xpos + ps, ypos - ps * sgn);
            eps_core_BoundingBox(x, xpos - ps, ypos - ps * sgn, pd->ww_final.linewidth);
            eps_core_BoundingBox(x, xpos + ps, ypos - ps * sgn, pd->ww_final.linewidth);
            EPS_ARROW_HEADSIZE /= 2;
            eps_primitive_arrow(x, SW_ARROWTYPE_HEAD, xpos, ypos - ps * sgn, NULL, xpos, ypos + ps * sgn, NULL,
                                &pd->ww_final);
            EPS_ARROW_HEADSIZE = eah_old;
        }
    }

    // Arrows are shown by an arrow
    else if ((style == SW_STYLE_ARROWS_HEAD) || (style == SW_STYLE_ARROWS_NOHEAD) ||
             (style == SW_STYLE_ARROWS_TWOHEAD)) {
        int ArrowStyle;
        if (style == SW_STYLE_ARROWS_TWOHEAD) ArrowStyle = SW_ARROWTYPE_TWOWAY;
        else if (style == SW_STYLE_ARROWS_NOHEAD) ArrowStyle = SW_ARROWTYPE_NOHEAD;
        else ArrowStyle = SW_ARROWTYPE_HEAD;
        eps_core_SetColor(x, &pd->ww_final, 1);
        eps_core_SetLinewidth(x, EPS_DEFAULT_LINEWIDTH * pd->ww_final.pointlinewidth, pd->ww_final.linetype, 0);
        if (eps_core_isVisible(x))
            eps_primitive_arrow(x, ArrowStyle, xpos - scale * 0.60 / 2, ypos, NULL, xpos + scale * 0.60 / 2, ypos, NULL,
                                &pd->ww_final);
    }

    // Filled styles are shown by a filled square
    else if ((style == SW_STYLE_FILLEDREGION) || (style == SW_STYLE_YERRORSHADED) || (style == SW_STYLE_COLORMAP)) {
        const double s = scale * 0.45 / 2;
        eps_core_SetColor(x, &pd->ww_final, 1);
        eps_core_SetFillColor(x, &pd->ww_final);
        eps_core_SwitchTo_FillColor(x, 1);
        // Fill the square, unless it is invisible
        if (eps_core_isVisible(x)) {
            fprintf(x->epsbuffer,
                    "newpath %.2f %.2f moveto %.2f %.2f lineto %.2f %.2f lineto %.2f %.2f lineto closepath fill\n",
                    xpos - s, ypos - s, xpos + s, ypos - s, xpos + s, ypos + s, xpos - s, ypos + s);
            eps_core_BoundingBox(x, xpos - s, ypos - s, 0);
            eps_core_BoundingBox(x, xpos + s, ypos - s, 0);
            eps_core_BoundingBox(x, xpos - s, ypos + s, 0);
            eps_core_BoundingBox(x, xpos + s, ypos + s, 0);
        }
        eps_core_SwitchFrom_FillColor(x, 1);
        eps_core_SetLinewidth(x, EPS_DEFAULT_LINEWIDTH * pd->ww_final.pointlinewidth, pd->ww_final.linetype, 0);
        // Stroke the square, unless it is invisible
        if (eps_core_isVisible(x)) {
            fprintf(x->epsbuffer,
                    "newpath %.2f %.2f moveto %.2f %.2f lineto %.2f %.2f lineto %.2f %.2f lineto closepath stroke\n",
                    xpos - s, ypos - s, xpos + s, ypos - s, xpos + s, ypos + s, xpos - s, ypos + s);
            eps_core_BoundingBox(x, xpos - s, ypos - s, pd->ww_final.linewidth);
            eps_core_BoundingBox(x, xpos + s, ypos - s, pd->ww_final.linewidth);
            eps_core_BoundingBox(x, xpos - s, ypos + s, pd->ww_final.linewidth);
            eps_core_BoundingBox(x, xpos + s, ypos + s, pd->ww_final.linewidth);
        }
    }

    // Error bars are shown by a short line with bars at its ends
    else if ((style == SW_STYLE_XERRORBARS) || (style == SW_STYLE_YERRORBARS) || (style == SW_STYLE_ZERRORBARS) ||
             (style == SW_STYLE_XYERRORBARS) || (style == SW_STYLE_XZERRORBARS) || (style == SW_STYLE_XZERRORBARS) ||
             (style == SW_STYLE_XZERRORBARS) || (style == SW_STYLE_XERRORRANGE) || (style == SW_STYLE_YERRORRANGE) ||
             (style == SW_STYLE_ZERRORRANGE) || (style == SW_STYLE_XYERRORRANGE) || (style == SW_STYLE_XZERRORRANGE) ||
             (style == SW_STYLE_YZERRORRANGE)) {
        const double s = scale * 0.6 / 2;
        const double b = 0.0005 * x->current->settings.bar * M_TO_PS;
        const double ps = pd->ww_final.pointsize * EPS_DEFAULT_PS;

        // Draw the error bar, unless it is invisible
        eps_core_SetColor(x, &pd->ww_final, 1);
        // Draw the error bar, unless it is invisible
        if (eps_core_isVisible(x)) {
            eps_core_SetLinewidth(x, EPS_DEFAULT_LINEWIDTH * pd->ww_final.linewidth, pd->ww_final.linetype, 0);
            fprintf(x->epsbuffer, "newpath %.2f %.2f moveto %.2f %.2f lineto stroke\n", xpos - s, ypos, xpos + s, ypos);
            fprintf(x->epsbuffer, "newpath %.2f %.2f moveto %.2f %.2f lineto stroke\n", xpos - s, ypos - b, xpos - s,
                    ypos + b);
            fprintf(x->epsbuffer, "newpath %.2f %.2f moveto %.2f %.2f lineto stroke\n", xpos + s, ypos - b, xpos + s,
                    ypos + b);
            fprintf(x->epsbuffer, "newpath %.2f %.2f moveto %.2f %.2f lineto stroke\n", xpos, ypos - ps, xpos,
                    ypos + ps);
            eps_core_BoundingBox(x, xpos - s, ypos - ps, pd->ww_final.linewidth);
            eps_core_BoundingBox(x, xpos - s, ypos + ps, pd->ww_final.linewidth);
            eps_core_BoundingBox(x, xpos + s, ypos - ps, pd->ww_final.linewidth);
            eps_core_BoundingBox(x, xpos + s, ypos + ps, pd->ww_final.linewidth);
        }
    }

    // Done
    return;
}
