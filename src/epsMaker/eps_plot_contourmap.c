// eps_plot_contourmap.c
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

// Draws the contourmap plot style: lines of constant value through data on a regular grid, optionally labelled with
// their values. Contours are traced cell by cell through the grid, first from the edges of the grid and then around
// closed loops within it, using flags stored in each grid point to mark which cell edges have been crossed. Open
// contours are closed around the corners of the grid so that their areas can be compared, and contours are drawn from
// the largest area down. Labels are placed on the flattest segment of each contour, with the contour line clipped away
// around them.

#define _PPL_EPS_PLOT_CONTOURMAP_C 1

#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <math.h>
#include <string.h>
#include <zlib.h>

#include <gsl/gsl_math.h>

#include "coreUtils/memAlloc.h"
#include "userspace/stack.h"
#include "expressions/expEval.h"
#include "expressions/traceback_fns.h"
#include "mathsTools/dcfmath.h"
#include "epsMaker/canvasDraw.h"
#include "canvasItems.h"
#include "coreUtils/errorReport.h"
#include "settings/settings.h"
#include "settings/withWords_fns.h"
#include "settings/settingTypes.h"
#include "stringTools/asciidouble.h"
#include "userspace/garbageCollector.h"
#include "userspace/unitsDisp.h"
#include "userspace/unitsArithmetic.h"
#include "userspace/pplObj_fns.h"
#include "userspace/contextVarDef.h"

#include "epsMaker/eps_comm.h"
#include "epsMaker/eps_core.h"
#include "settings/epsColors.h"
#include "epsMaker/eps_plot.h"
#include "epsMaker/eps_plot_canvas.h"
#include "epsMaker/eps_plot_colormap.h"
#include "epsMaker/eps_plot_contourmap.h"
#include "epsMaker/eps_plot_legend.h"
#include "epsMaker/eps_settings.h"
#include "epsMaker/eps_style.h"

// Codes for the four sides of a cell
#define FACE_ALL -1
#define FACE_T   1
#define FACE_R   2
#define FACE_B   3
#define FACE_L   4

#define MAX_CONTOUR_PATHS 1024

typedef struct ContourDesc {
    long Nvertices_min, Nvertices_max;
    long i, segment_flatest;
    unsigned char closepath;
    double *posdata;
    double area;
    double vreal;
} ContourDesc;

//! ContourCmp - Comparison function for qsort(), used to sort contours into decreasing order of the area they enclose,
//! so that larger filled contours are drawn first
//! \param [in] xv - Pointer to the first contour descriptor
//! \param [in] yv - Pointer to the second contour descriptor
//! \return -1, 0 or 1 depending on the order of the contours

static int ContourCmp(const void *xv, const void *yv) {
    const ContourDesc *x = (const ContourDesc *) xv;
    const ContourDesc *y = (const ContourDesc *) yv;

    // Sort into order of decreasing area
    if (x->area < y->area) return 1.0;
    else if (x->area > y->area) return -1.0;
    else return 0.0;
}

//! eps_plot_contourmap_YieldText - Collect the text items which label the contours of a contourmap, so that they can be
//! typeset
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] data - The data being plotted as a contourmap
//! \param [in] sg - The graph settings
//! \param [in] pd - The descriptor of the plot item

void eps_plot_contourmap_YieldText(EPSComm *x, const dataTable *data, const pplset_graph *sg, canvas_plotdesc *pd) {
    // Variables for finding the range of c1
    dataBlock *blk;
    const int XSize = (x->current->settings.SamplesXAuto == SW_BOOL_TRUE) ? x->current->settings.samples
                                                                          : x->current->settings.SamplesX;
    const int YSize = (x->current->settings.SamplesYAuto == SW_BOOL_TRUE) ? x->current->settings.samples
                                                                          : x->current->settings.SamplesY;
    int k, Ncol;
    double CMin, CMax, min_prelim, max_prelim, OoM, UnitMultiplier;
    unsigned char CMinAuto, CMaxAuto, CLog;
    char *UnitString;

    // Check that we have some data
    if ((data == NULL) || (data->Nrows < 1)) return; // No data present
    Ncol = data->Ncolumns_real;
    blk = data->first;

    // Work out normalisation of variable c1
    if (!eps_plot_colormap_findCRange(sg, 0, blk, Ncol, XSize, YSize, &CMin, &CMax, &CLog)) return; // No data present
    CMinAuto = (sg->Cminauto[0] == SW_BOOL_TRUE);
    CMaxAuto = (sg->Cmaxauto[0] == SW_BOOL_TRUE);

    // If there's no spread of data, make a spread up
    if ((fabs(CMax) <= fabs(CMin)) || (fabs(CMin - CMax) <= fabs(1e-14 * CMax))) {
        // Widen the range by a small amount, or by a factor of ten on logarithmic scales
        if (!CLog) {
            const double step = ppl_max(1.0, 1e-3 * fabs(CMin));
            CMin -= step;
            CMax += step;
        } else {
            if (CMin > 1e-300) CMin /= 10.0;
            if (CMax < 1e300) CMax *= 10.0;
        }
    }

    // Work out units in which contours will be labelled
    pd->CRangeUnit = data->firstEntries[2];
    pd->CRangeUnit.flagComplex = 0;
    pd->CRangeUnit.imag = 0.0;
    pd->CRangeUnit.real = CLog ? (CMin * sqrt(CMax / CMin)) : ((CMin + CMax) / 2);
    UnitString = ppl_printUnit(x->c, &pd->CRangeUnit, &UnitMultiplier, NULL, 0, 0, SW_DISPLAY_L);
    UnitMultiplier /= pd->CRangeUnit.real;
    if (!gsl_finite(UnitMultiplier)) UnitMultiplier = 1.0;

    // Round range outwards to round endpoints
    min_prelim = CMin * UnitMultiplier;
    max_prelim = CMax * UnitMultiplier;

    // Work with logarithms on logarithmic scales
    if (CLog) {
        min_prelim = log10(min_prelim);
        max_prelim = log10(max_prelim);
    }

    // Round each end of the range to a multiple of a power of ten, about a fifth of the range
    OoM = pow(10.0, floor(log10(fabs(max_prelim - min_prelim) / 5)));
    min_prelim = floor(min_prelim / OoM) * OoM;
    max_prelim = ceil(max_prelim / OoM) * OoM;

    // Convert back from logarithms
    if (CLog) {
        min_prelim = pow(10.0, min_prelim);
        max_prelim = pow(10.0, max_prelim);
    }

    // Convert back from the units in which the contours are labelled
    min_prelim /= UnitMultiplier;
    max_prelim /= UnitMultiplier;

    // Use the rounded range, if it is sensible
    // (on logarithmic scales the limits must be positive, as in eps_plot_ticking.c)
    if (gsl_finite(min_prelim) && ((!CLog) || (min_prelim > 1e-300))) CMin = min_prelim;
    if (gsl_finite(max_prelim) && ((!CLog) || (max_prelim > 1e-300))) CMax = max_prelim;

    // Loop over all contours submitting labels
    for (k = 0; (k < MAX_CONTOURS) &&
         (((sg->ContoursListLen < 0) && (k < sg->ContoursN)) ||
          ((sg->ContoursListLen >= 0) && (k < sg->ContoursListLen)));
         k++) {
        pplObj v = pd->CRangeUnit;

        // Contours are evenly spaced across the range, or at the values listed by the user; ends of the range set by
        // the user have contours on them, while automatic ends do not
        if (sg->ContoursListLen < 0)
            v.real = CLog
                    ? (CMin *
                       pow(CMax / CMin, (k + 0.5 * CMinAuto) / (sg->ContoursN - 0.5 * ((!CMinAuto) + (!CMaxAuto)))))
                    : (CMin +
                       (CMax - CMin) * (k + 0.5 * CMinAuto) / (sg->ContoursN - 0.5 * ((!CMinAuto) + (!CMaxAuto))));
        else v.real = sg->ContoursList[k];

        // UnitString points to a buffer of LSTR_LENGTH bytes in the context, returned by ppl_printUnit()
        ppl_snprintfTrunc(UnitString, LSTR_LENGTH, "%s", ppl_unitsNumericDisplay(x->c, &v, 0, SW_DISPLAY_L, 0));
        if (eps_yieldTextItemCopy(x, UnitString)) return;
    }

    // Store range of values for which contours will be drawn
    pd->CMinFinal = CMin;
    pd->CMaxFinal = CMax;
    return;
}

// Routines for tracking the paths of contours

// -----+-----+-----+-----+
//      |     |     |     |  blk->split[X] contains flags indicating whether crossing points on lines
//      |     |     |     |  A (in bit 1) and B (in bit 2) have been used.
//      |     |--A--|     |  on successive passes, bits (3,4), (5,6) are used, etc.
// -----+-----X-----+-----+
//      |    ||     |     |
//      |    B|     |     |
//      |    ||     |     |
// -----+-----+-----+-----+
//      |     |     |     |
//      |     |     |     |
//      |     |     |     |
// -----+-----+-----+-----+


//! IsBetween - Test whether a value lies between two others, and if so, at what fraction of the way between them
//! \param [in] x - The value to test
//! \param [in] a - The first limit
//! \param [in] b - The second limit
//! \param [out] f - Set to the fraction of the way from <a> to <b> at which <x> lies
//! \return One if <x> lies between <a> and <b>, zero otherwise

static int IsBetween(const double x, const double a, const double b, double *f) {
    *f = 0.5;
    if (a == b) { return x == a; }
    // The value lies between a and b, in increasing order
    if ((a < b) && (x >= a) && (x <= b)) {
        *f = (x - a) / (b - a);
        return 1;
    }
    // The value lies between a and b, in decreasing order
    if ((a > b) && (x <= a) && (x >= b)) {
        *f = (x - a) / (b - a);
        return 1;
    }
    return 0;
}

//! GetStartPoint - Search a grid cell for a point at which a contour at a given level crosses one of its faces, and
//! which has not already been used as the starting point of a contour
//! \param [in] c1 - The contour level
//! \param [in] data - The gridded data
//! \param [in] flags - Array of flags marking which cell faces have already been crossed by contours
//! \param [in] XSize - The number of grid points in the x direction
//! \param [in] YSize - The number of grid points in the y direction
//! \param [in] x0 - The x index of the cell
//! \param [in] y0 - The y index of the cell
//! \param [in] EntryFace - The face through which a contour entered the cell, or -1 to search all faces
//! \param [out] ExitFace - Set to the face at which the contour was found
//! \param [out] xcell - Set to the x index of the cell into which the contour crosses
//! \param [out] ycell - Set to the y index of the cell into which the contour crosses
//! \param [out] Xout - Set to the x coordinate of the crossing point, in grid units
//! \param [out] Yout - Set to the y coordinate of the crossing point, in grid units
//! \return One if a starting point was found, zero otherwise

static int GetStartPoint(const double c1, const dataTable *data, unsigned char **flags, int XSize, const int YSize,
                         int x0, int y0, const int EntryFace, int *ExitFace, int *xcell, int *ycell, double *Xout,
                         double *Yout) {
    dataBlock *blk = data->first;
    const int Ncol = data->Ncolumns_real;
    const int pass = 0;
    double f;

    *xcell = x0;
    *ycell = y0;

    // Look for a crossing on the top edge of the cell
    if (((EntryFace < 0) || (EntryFace == FACE_T)) && (x0 <= XSize - 2) && (x0 >= 0) && (y0 >= 0) &&
        (y0 <= YSize - 1) && (((blk->split[(x0) + (y0) *XSize] >> (0 + 2 * pass)) & 1) == 0) &&
        IsBetween(c1, blk->data_real[2 + Ncol * ((x0) + (y0) *XSize)],
                  blk->data_real[2 + Ncol * ((x0 + 1) + (y0) *XSize)], &f)) {
        *flags = &blk->split[(x0) + (y0) *XSize];
        *ExitFace = FACE_T;
        *Xout = x0 + f;
        *Yout = y0;
        return 1;
    }

    // Look for a crossing on the right edge of the cell
    if (((EntryFace < 0) || (EntryFace == FACE_R)) && (x0 <= XSize - 2) && (x0 >= -1) && (y0 >= 0) &&
        (y0 <= YSize - 2) && (((blk->split[(x0 + 1) + (y0) *XSize] >> (1 + 2 * pass)) & 1) == 0) &&
        IsBetween(c1, blk->data_real[2 + Ncol * ((x0 + 1) + (y0) *XSize)],
                  blk->data_real[2 + Ncol * ((x0 + 1) + (y0 + 1) * XSize)], &f)) {
        *flags = &blk->split[(x0 + 1) + (y0) *XSize];
        *ExitFace = FACE_R;
        *Xout = x0 + 1;
        *Yout = y0 + f;
        return 1;
    }

    // Look for a crossing on the bottom edge of the cell
    if (((EntryFace < 0) || (EntryFace == FACE_B)) && (x0 <= XSize - 2) && (x0 >= 0) && (y0 >= -1) &&
        (y0 <= YSize - 2) && (((blk->split[(x0) + (y0 + 1) * XSize] >> (0 + 2 * pass)) & 1) == 0) &&
        IsBetween(c1, blk->data_real[2 + Ncol * ((x0) + (y0 + 1) * XSize)],
                  blk->data_real[2 + Ncol * ((x0 + 1) + (y0 + 1) * XSize)], &f)) {
        *flags = &blk->split[(x0) + (y0 + 1) * XSize];
        *ExitFace = FACE_B;
        *Xout = x0 + f;
        *Yout = y0 + 1;
        return 1;
    }

    // Look for a crossing on the left edge of the cell
    if (((EntryFace < 0) || (EntryFace == FACE_L)) && (x0 <= XSize - 1) && (x0 >= 0) && (y0 >= 0) &&
        (y0 <= YSize - 2) && (((blk->split[(x0) + (y0) *XSize] >> (1 + 2 * pass)) & 1) == 0) &&
        IsBetween(c1, blk->data_real[2 + Ncol * ((x0) + (y0) *XSize)],
                  blk->data_real[2 + Ncol * ((x0) + (y0 + 1) * XSize)], &f)) {
        *flags = &blk->split[(x0) + (y0) *XSize];
        *ExitFace = FACE_L;
        *Xout = x0;
        *Yout = y0 + f;
        return 1;
    }
    return 0;
}

//! GetNextPoint - Given a contour which has entered a grid cell through one face, find the face through which it leaves
//! \param [in] c1 - The contour level
//! \param [in] data - The gridded data
//! \param [in] pass - The number of the attempt; on the second pass, ambiguous saddle points are resolved the other way
//! \param [in] XSize - The number of grid points in the x direction
//! \param [in] YSize - The number of grid points in the y direction
//! \param [in] x0 - The x index of the cell
//! \param [in] y0 - The y index of the cell
//! \param [in] EntryFace - The face through which the contour entered the cell
//! \param [out] ExitFace - Set to the face through which the contour leaves
//! \param [out] xcell - Set to the x index of the cell into which the contour crosses
//! \param [out] ycell - Set to the y index of the cell into which the contour crosses
//! \param [out] Xout - Set to the x coordinate of the exit point, in grid units
//! \param [out] Yout - Set to the y coordinate of the exit point, in grid units
//! \return One if an exit point was found, zero otherwise

static int GetNextPoint(const double c1, const dataTable *data, const int pass, const int XSize, const int YSize,
                        const int x0, const int y0, const int EntryFace, int *ExitFace, int *xcell, int *ycell,
                        double *Xout, double *Yout) {
    dataBlock *blk = data->first;
    const int Ncol = data->Ncolumns_real;
    int j;
    double f;

    // Look for an exit from the cell, first opposite the entry, then on any unused edge
    for (j = 0; j < 2; j++) {
        // An exit through the top edge
        if (((j == 1) || (EntryFace == FACE_B)) && (x0 <= XSize - 2) && (x0 >= 0) && (y0 >= 0) && (y0 <= YSize - 1) &&
            (((blk->split[(x0) + (y0) *XSize] >> (0 + 2 * pass)) & 1) == 0) &&
            IsBetween(c1, blk->data_real[2 + Ncol * ((x0) + (y0) *XSize)],
                      blk->data_real[2 + Ncol * ((x0 + 1) + (y0) *XSize)], &f)) {
            blk->split[(x0) + (y0) *XSize] |= 1 << (0 + 2 * pass);
            *ExitFace = FACE_T;
            *xcell = x0;
            *ycell = y0 - 1;
            *Xout = x0 + f;
            *Yout = y0;
            return 1;
        }

        // An exit through the right edge
        if (((j == 1) || (EntryFace == FACE_L)) && (x0 <= XSize - 2) && (x0 >= -1) && (y0 >= 0) && (y0 <= YSize - 2) &&
            (((blk->split[(x0 + 1) + (y0) *XSize] >> (1 + 2 * pass)) & 1) == 0) &&
            IsBetween(c1, blk->data_real[2 + Ncol * ((x0 + 1) + (y0) *XSize)],
                      blk->data_real[2 + Ncol * ((x0 + 1) + (y0 + 1) * XSize)], &f)) {
            blk->split[(x0 + 1) + (y0) *XSize] |= 1 << (1 + 2 * pass);
            *ExitFace = FACE_R;
            *xcell = x0 + 1;
            *ycell = y0;
            *Xout = x0 + 1;
            *Yout = y0 + f;
            return 1;
        }

        // An exit through the bottom edge
        if (((j == 1) || (EntryFace == FACE_T)) && (x0 <= XSize - 2) && (x0 >= 0) && (y0 >= -1) && (y0 <= YSize - 2) &&
            (((blk->split[(x0) + (y0 + 1) * XSize] >> (0 + 2 * pass)) & 1) == 0) &&
            IsBetween(c1, blk->data_real[2 + Ncol * ((x0) + (y0 + 1) * XSize)],
                      blk->data_real[2 + Ncol * ((x0 + 1) + (y0 + 1) * XSize)], &f)) {
            blk->split[(x0) + (y0 + 1) * XSize] |= 1 << (0 + 2 * pass);
            *ExitFace = FACE_B;
            *xcell = x0;
            *ycell = y0 + 1;
            *Xout = x0 + f;
            *Yout = y0 + 1;
            return 1;
        }

        // An exit through the left edge
        if (((j == 1) || (EntryFace == FACE_R)) && (x0 <= XSize - 1) && (x0 >= 0) && (y0 >= 0) && (y0 <= YSize - 2) &&
            (((blk->split[(x0) + (y0) *XSize] >> (1 + 2 * pass)) & 1) == 0) &&
            IsBetween(c1, blk->data_real[2 + Ncol * ((x0) + (y0) *XSize)],
                      blk->data_real[2 + Ncol * ((x0) + (y0 + 1) * XSize)], &f)) {
            blk->split[(x0) + (y0) *XSize] |= 1 << (1 + 2 * pass);
            *ExitFace = FACE_L;
            *xcell = x0 - 1;
            *ycell = y0;
            *Xout = x0;
            *Yout = y0 + f;
            return 1;
        }
    }
    return 0;
}

//! FollowContour - Trace a contour across the grid from its starting point, until it closes or leaves the edge of the
//! grid, recording the path in a contour descriptor
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] data - The gridded data
//! \param [out] cd - The contour descriptor in which to record the path
//! \param [in] v - The contour level
//! \param [in|out] flags - Array of flags marking which cell faces have already been crossed by contours
//! \param [in] XSize - The number of grid points in the x direction
//! \param [in] YSize - The number of grid points in the y direction
//! \param [in] xcell - The x index of the starting cell
//! \param [in] ycell - The y index of the starting cell
//! \param [in] face - The face of the starting cell through which the contour enters
//! \param [in] xpos - The x coordinate of the starting point, in grid units
//! \param [in] ypos - The y coordinate of the starting point, in grid units
//! \param [in] Lx - The width of the graph, used to compute the area enclosed by the contour
//! \param [in] ThetaX - The direction of the x axis on the page
//! \param [in] Ly - The height of the graph, used to compute the area enclosed by the contour
//! \param [in] ThetaY - The direction of the y axis on the page

static void FollowContour(const EPSComm *x, dataTable *data, ContourDesc *cd, const pplObj *v, unsigned char *flags,
                          const int XSize, const int YSize, int xcell, int ycell, int face, double xpos, double ypos,
                          const double Lx, const double ThetaX, const double Ly, const double ThetaY) {
    long i, j, i_flatest = 0;
    int xcelli = xcell, ycelli = ycell;
    double xposi = xpos, yposi = ypos, grad_flatest = 1e100;
    const int facei = face;
    double xold = xpos, yold = ypos;

    // Starting point has been used on pass zero
    *flags |= 1 << (((face == FACE_L) || (face == FACE_R)) + 0);

    // Trace path, looking for flattest segment and counting length
    for (i = 1;
         (GetNextPoint(v->real, data, 0, XSize, YSize, xcell, ycell, face, &face, &xcell, &ycell, &xpos, &ypos) != 0);
         i++) {
        double grad;
        // Measure the gradient of each segment on the page, ignoring vertical ones
        if (xpos != xold) {
            double x0, y0, x1, y1;
            x0 = Lx * xold / (XSize - 1) * sin(ThetaX) + Ly * yold / (YSize - 1) * sin(ThetaY);
            y0 = Lx * xold / (XSize - 1) * cos(ThetaX) + Ly * yold / (YSize - 1) * cos(ThetaY);
            x1 = Lx * xpos / (XSize - 1) * sin(ThetaX) + Ly * ypos / (YSize - 1) * sin(ThetaY);
            y1 = Lx * xpos / (XSize - 1) * cos(ThetaX) + Ly * ypos / (YSize - 1) * cos(ThetaY);
            grad = fabs((y1 - y0) / (x1 - x0));
            // Remember the flattest segment, where the label will go
            if (grad < grad_flatest) {
                grad_flatest = grad;
                i_flatest = i;
            }
        }
        xold = xpos;
        yold = ypos;
    }

    // Fill out information in contour descriptor
    cd->Nvertices_min = cd->Nvertices_max = i;
    cd->segment_flatest = i_flatest;
    cd->posdata = (double *) ppl_memAlloc((i + 8) * 2 * sizeof(double));
    if (cd->posdata == NULL) return;

    // Begin pass one
    xcell = xcelli;
    ycell = ycelli;
    xpos = xposi;
    ypos = yposi;
    face = facei;

    // Starting point has been used on pass one
    *flags |= 1 << (((face == FACE_L) || (face == FACE_R)) + 2);

    // Trace path, looking for flattest segment and counting length
    cd->posdata[2 * 0] = xpos;
    cd->posdata[2 * 0 + 1] = ypos;
    for (j = 1;
         (GetNextPoint(v->real, data, 1, XSize, YSize, xcell, ycell, face, &face, &xcell, &ycell, &xpos, &ypos) != 0);
         j++) {
        cd->posdata[2 * j] = xpos;
        cd->posdata[2 * j + 1] = ypos;
    }
    return;
}

//! contourProjection - The mapping from positions on the grid of samples of a contour map onto the page
typedef struct contourProjection {
    double xo, yo; // The position of the grid's origin on the page, in postscript points
    double Lx, Ly; // The lengths of the grid's x and y edges on the page, in postscript points
    double ThetaX, ThetaY; // The directions of the grid's x and y edges on the page, measured clockwise from vertical
    int XSize, YSize; // The number of samples along each edge of the grid
} contourProjection;

//! contourToPostscript - Convert a position on the grid of samples of a contour map into a position on the page
//! \param [in] proj - The mapping from the grid onto the page
//! \param [in] xpos - The horizontal position on the grid, in units of samples
//! \param [in] ypos - The vertical position on the grid, in units of samples
//! \param [out] xps - Set to the horizontal position on the page, in postscript points
//! \param [out] yps - Set to the vertical position on the page, in postscript points

static void contourToPostscript(const contourProjection *proj, const double xpos, const double ypos, double *xps,
                                double *yps) {
    *xps = proj->xo + proj->Lx * xpos / (proj->XSize - 1) * sin(proj->ThetaX) +
            proj->Ly * ypos / (proj->YSize - 1) * sin(proj->ThetaY);
    *yps = proj->yo + proj->Lx * xpos / (proj->XSize - 1) * cos(proj->ThetaX) +
            proj->Ly * ypos / (proj->YSize - 1) * cos(proj->ThetaY);
}

//! eps_plot_contourmap - Render a dataset as a contourmap, by resampling it onto a regular grid and tracing contours at
//! the requested levels
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] data - The data to plot
//! \param [in] ThreeDim - Flag indicating whether the graph is three-dimensional
//! \param [in] xn - The axis direction (0=x, 1=y, 2=z) onto which the first data column is plotted
//! \param [in] yn - The axis direction (0=x, 1=y, 2=z) onto which the second data column is plotted
//! \param [in] zn - The axis direction (0=x, 1=y, 2=z) onto which the third data column is plotted
//! \param [in] sg - The graph settings
//! \param [in] pd - The descriptor of the plot item being drawn
//! \param [in] pdn - The number of the plot item being drawn within the graph
//! \param [in] origin_x - The x coordinate of the bottom-left corner of the graph, in postscript points
//! \param [in] origin_y - The y coordinate of the bottom-left corner of the graph, in postscript points
//! \param [in] width - The width of the graph, in postscript points
//! \param [in] height - The height of the graph, in postscript points
//! \param [in] zdepth - The depth of the graph, in postscript points, for 3D plots
//! \return Zero on success, or one on failure

int eps_plot_contourmap(EPSComm *x, dataTable *data, const unsigned char ThreeDim, const int xn, const int yn,
                        const int zn, pplset_graph *sg, canvas_plotdesc *pd, const int pdn, const double origin_x,
                        const double origin_y, const double width, const double height, const double zdepth) {
    char msg[PPL_MSG_LENGTH];
    dataBlock *blk;
    const int XSize = (x->current->settings.SamplesXAuto == SW_BOOL_TRUE) ? x->current->settings.samples
                                                                          : x->current->settings.SamplesX;
    const int YSize = (x->current->settings.SamplesYAuto == SW_BOOL_TRUE) ? x->current->settings.samples
                                                                          : x->current->settings.SamplesY;
    int i, j, k, cn, pass, face, xcell, ycell;
    double xo, yo, Lx, Ly, ThetaX, ThetaY, CMin, CMax, xpos, ypos;
    double col = GSL_NAN, col1 = -1, col2 = -1, col3 = -1, col4 = -1, fc = GSL_NAN, fc1 = -1, fc2 = -1, fc3 = -1,
           fc4 = -1;
    unsigned char CLog, CMinAuto, CMaxAuto, CRenorm, *flags;
    char *errtext, c1name[] = "c1";
    pplObj *CVar = NULL, CDummy;
    ContourDesc *clist;
    int cpos = 0;
    // int         Ncol_real, Ncol_obj;

    if ((data == NULL) || (data->Nrows < 1)) return 0; // No data present
    // Ncol_real = data->Ncolumns_real;
    // Ncol_obj  = data->Ncolumns_obj;
    // if (eps_plot_WithWordsCheckUsingItemsDimLess(&pd->ww_final, data->firstEntries, Ncol_real, NULL, Ncol_obj)) return 1;
    blk = data->first;

    // A list of the contours traced, and a buffer for error messages
    clist = (ContourDesc *) ppl_memAlloc(MAX_CONTOUR_PATHS * sizeof(ContourDesc));
    errtext = ppl_memAlloc(LSTR_LENGTH);
    // Failure to allocate memory is reported as an error
    if ((clist == NULL) || (errtext == NULL)) {
        ppl_error(&x->c->errcontext, ERR_MEMORY, -1, -1, "Out of memory (p).");
        return 1;
    }

    // Work out orientation of the grid on the page
    eps_plot_colormap_orientation(sg, ThreeDim, xn, yn, zn, origin_x, origin_y, width, height, zdepth, &xo, &yo, &Lx,
                                  &Ly, &ThetaX, &ThetaY);

    // Look up normalisation of variable c1
    CLog = (sg->Clog[0] == SW_BOOL_TRUE);
    CMin = pd->CMinFinal;
    CMax = pd->CMaxFinal;
    CMinAuto = (sg->Cminauto[0] == SW_BOOL_TRUE);
    CMaxAuto = (sg->Cmaxauto[0] == SW_BOOL_TRUE);
    CRenorm = (sg->Crenorm[0] == SW_BOOL_TRUE);

    // If no data present, stop now
    if ((CMin == 0) && (CMax == 0)) return 0;

    // Output result to debugging output
    if (DEBUG) {
        int SF = x->c->set->term_current.SignificantFigures;
        ppl_snprintfTrunc(msg, sizeof(msg), "Range for variable c1 is [%s:%s]",
                          ppl_numericDisplay(CMin, x->c->numdispBuff[0], SF, 0),
                          ppl_numericDisplay(CMax, x->c->numdispBuff[1], SF, 0));
        ppl_log(&x->c->errcontext, msg);
    }

    // Check that variable c1 has appropriate units
    if (((!CMinAuto) || (!CMaxAuto)) &&
        (!ppl_unitsDimEqual(&data->firstEntries[2],
                            (sg->Cminauto[0] == SW_BOOL_TRUE) ? (&sg->Cmax[0]) : (&sg->Cmin[0])))) {
        ppl_snprintfTrunc(
                msg, sizeof(msg),
                "Column 3 of data supplied to the colormap plot style has conflicting units with those set in the 'set crange' command. The former has units of <%s> whilst the latter has units of <%s>.",
                ppl_printUnit(x->c, &data->firstEntries[2], NULL, NULL, 0, 1, 0),
                ppl_printUnit(x->c, (sg->Cminauto[0] == SW_BOOL_TRUE) ? (&sg->Cmax[0]) : (&sg->Cmin[0]), NULL, NULL, 1,
                              1, 0));
        ppl_error(&x->c->errcontext, ERR_NUMERICAL, -1, -1, msg);
        return 1;
    }

    // Get pointer to variable c1 in the user's variable space
    ppl_contextGetVarPointer(x->c, c1name, &CVar, &CDummy);
    // Without renormalisation, c1 takes the units of the data; otherwise it is dimensionless
    if (!CRenorm) {
        *CVar = pd->CRangeUnit;
        CVar->flagComplex = 0;
        CVar->imag = 0.0;
    } else pplObjNum(CVar, 0, 0, 0); // c1 is a dimensionless number in range 0-1, regardless of units of input data

    // Loop over contours
    for (k = 0; (k < MAX_CONTOURS) &&
         (((sg->ContoursListLen < 0) && (k < sg->ContoursN)) ||
          ((sg->ContoursListLen >= 0) && (k < sg->ContoursListLen)));
         k++) {
        pplObj v = pd->CRangeUnit;

        // Contours are evenly spaced across the range, or at the values listed by the user
        if (sg->ContoursListLen < 0)
            v.real = CLog
                    ? (CMin *
                       pow(CMax / CMin, (k + 0.5 * CMinAuto) / (sg->ContoursN - 0.5 * ((!CMinAuto) + (!CMaxAuto)))))
                    : (CMin +
                       (CMax - CMin) * (k + 0.5 * CMinAuto) / (sg->ContoursN - 0.5 * ((!CMinAuto) + (!CMaxAuto))));
        else v.real = sg->ContoursList[k];

        // Write debugging output
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Beginning to trace path of contour at c1=%g.", v.real);
            ppl_log(&x->c->errcontext, msg);
        }

        // Reset contour map usage flags
        for (j = 0; j < YSize; j++)
            for (i = 0; i < XSize; i++) blk->split[i + XSize * j] = 0;

        // Set value of c1
        if (CRenorm) {
            if (sg->ContoursListLen < 0) CVar->real = ((double) (k + 1) / (sg->ContoursN + 1));
            else CVar->real = CLog ? (log(v.real / CMin) / log(CMax / CMin)) : ((v.real - CMin) / (CMax - CMin));
            if (CVar->real < 0.0) CVar->real = 0.0;
            if (CVar->real > 1.0) CVar->real = 1.0;
            if (!gsl_finite(CVar->real)) CVar->real = 0.5;
        } else CVar->real = v.real;

        // Scan edges of plot looking for contour start points
        for (i = 0; i < XSize - 1; i++) // Top face
            // Trace each contour which starts on the edge of the grid, and then each closed contour within it
            if (GetStartPoint(v.real, data, &flags, XSize, YSize, i, 0, FACE_T, &face, &xcell, &ycell, &xpos, &ypos)) {
                clist[cpos].i = k;
                clist[cpos].vreal = CVar->real;
                clist[cpos].closepath = 0;
                FollowContour(x, data, &clist[cpos], &v, flags, XSize, YSize, xcell, ycell, face, xpos, ypos, Lx,
                              ThetaX, Ly, ThetaY);
                if (++cpos >= MAX_CONTOUR_PATHS) goto GOT_CONTOURS;
            }
        for (i = 0; i < YSize - 1; i++) // Right face
            if (GetStartPoint(v.real, data, &flags, XSize, YSize, XSize - 2, i, FACE_R, &face, &xcell, &ycell, &xpos,
                              &ypos)) {
                clist[cpos].i = k;
                clist[cpos].vreal = CVar->real;
                clist[cpos].closepath = 0;
                FollowContour(x, data, &clist[cpos], &v, flags, XSize, YSize, xcell, ycell, face, xpos, ypos, Lx,
                              ThetaX, Ly, ThetaY);
                if (++cpos >= MAX_CONTOUR_PATHS) goto GOT_CONTOURS;
            }
        for (i = 0; i < XSize - 1; i++) // Bottom face
            if (GetStartPoint(v.real, data, &flags, XSize, YSize, XSize - 2 - i, YSize - 2, FACE_B, &face, &xcell,
                              &ycell, &xpos, &ypos)) {
                clist[cpos].i = k;
                clist[cpos].vreal = CVar->real;
                clist[cpos].closepath = 0;
                FollowContour(x, data, &clist[cpos], &v, flags, XSize, YSize, xcell, ycell, face, xpos, ypos, Lx,
                              ThetaX, Ly, ThetaY);
                if (++cpos >= MAX_CONTOUR_PATHS) goto GOT_CONTOURS;
            }
        for (i = 0; i < YSize - 1; i++) // Left face
            if (GetStartPoint(v.real, data, &flags, XSize, YSize, 0, YSize - 2 - i, FACE_L, &face, &xcell, &ycell,
                              &xpos, &ypos)) {
                clist[cpos].i = k;
                clist[cpos].vreal = CVar->real;
                clist[cpos].closepath = 0;
                FollowContour(x, data, &clist[cpos], &v, flags, XSize, YSize, xcell, ycell, face, xpos, ypos, Lx,
                              ThetaX, Ly, ThetaY);
                if (++cpos >= MAX_CONTOUR_PATHS) goto GOT_CONTOURS;
            }

        // Scan body of plot looking for undrawn contours
        for (j = 0; j < YSize - 1; j++)
            for (i = 0; i < XSize - 1; i++)
                if (GetStartPoint(v.real, data, &flags, XSize, YSize, i, j, FACE_ALL, &face, &xcell, &ycell, &xpos,
                                  &ypos)) {
                    clist[cpos].i = k;
                    clist[cpos].vreal = CVar->real;
                    clist[cpos].closepath = 1;
                    FollowContour(x, data, &clist[cpos], &v, flags, XSize, YSize, xcell, ycell, face, xpos, ypos, Lx,
                                  ThetaX, Ly, ThetaY);
                    if (++cpos >= MAX_CONTOUR_PATHS) goto GOT_CONTOURS;
                }
    } // Finish looping over contours we are to trace

// Contours have been traced
GOT_CONTOURS:

    // Add corners of plot to paths of non-closed contours
    for (cn = 0; cn < cpos; cn++)
        // Close each open contour around the corners of the grid, according to which edges its ends lie on
        if (!clist[cn].closepath) {
            int faceA, faceB;
            const int n = clist[cn].Nvertices_min;
            if (clist[cn].posdata == NULL) continue;

            // The edge on which the contour starts
            if (clist[cn].posdata[0] < 0.0001) faceA = FACE_L;
            else if (clist[cn].posdata[0] > (XSize - 0.0001)) faceA = FACE_R;
            else if (clist[cn].posdata[1] < 0.0001) faceA = FACE_T;
            else faceA = FACE_B;

            // The edge on which the contour ends
            if (clist[cn].posdata[2 * (n - 1) + 0] < 0.0001) faceB = FACE_L;
            else if (clist[cn].posdata[2 * (n - 1) + 0] > (XSize - 0.0001)) faceB = FACE_R;
            else if (clist[cn].posdata[2 * (n - 1) + 1] < 0.0001) faceB = FACE_T;
            else faceB = FACE_B;

            // Add the corner, or corners, between the two edges
            if (((faceA == FACE_L) && (faceB == FACE_T)) || ((faceA == FACE_T) && (faceB == FACE_L))) {
                clist[cn].posdata[2 * n + 0] = 0;
                clist[cn].posdata[2 * n + 1] = 0;
                clist[cn].Nvertices_max += 1;
            } else if (((faceA == FACE_R) && (faceB == FACE_T)) || ((faceA == FACE_T) && (faceB == FACE_R))) {
                clist[cn].posdata[2 * n + 0] = XSize;
                clist[cn].posdata[2 * n + 1] = 0;
                clist[cn].Nvertices_max += 1;
            } else if (((faceA == FACE_L) && (faceB == FACE_B)) || ((faceA == FACE_B) && (faceB == FACE_L))) {
                clist[cn].posdata[2 * n + 0] = 0;
                clist[cn].posdata[2 * n + 1] = YSize;
                clist[cn].Nvertices_max += 1;
            } else if (((faceA == FACE_R) && (faceB == FACE_B)) || ((faceA == FACE_B) && (faceB == FACE_R))) {
                clist[cn].posdata[2 * n + 0] = XSize;
                clist[cn].posdata[2 * n + 1] = YSize;
                clist[cn].Nvertices_max += 1;
            } else if ((faceA == FACE_L) && (faceB == FACE_R)) {
                clist[cn].posdata[2 * n + 0] = XSize;
                clist[cn].posdata[2 * n + 1] = 0;
                clist[cn].posdata[2 * n + 2] = 0;
                clist[cn].posdata[2 * n + 3] = 0;
                clist[cn].Nvertices_max += 2;
            } else if ((faceA == FACE_R) && (faceB == FACE_L)) {
                clist[cn].posdata[2 * n + 0] = 0;
                clist[cn].posdata[2 * n + 1] = 0;
                clist[cn].posdata[2 * n + 2] = XSize;
                clist[cn].posdata[2 * n + 3] = 0;
                clist[cn].Nvertices_max += 2;
            } else if ((faceA == FACE_T) && (faceB == FACE_B)) {
                clist[cn].posdata[2 * n + 0] = 0;
                clist[cn].posdata[2 * n + 1] = YSize;
                clist[cn].posdata[2 * n + 2] = 0;
                clist[cn].posdata[2 * n + 3] = 0;
                clist[cn].Nvertices_max += 2;
            } else if ((faceA == FACE_B) && (faceB == FACE_T)) {
                clist[cn].posdata[2 * n + 0] = 0;
                clist[cn].posdata[2 * n + 1] = 0;
                clist[cn].posdata[2 * n + 2] = 0;
                clist[cn].posdata[2 * n + 3] = YSize;
                clist[cn].Nvertices_max += 2;
            }
        }

    // Calculate area of each contour
    for (cn = 0; cn < cpos; cn++) {
        int i;
        double area = 0.0;
        // Find the area enclosed, using the shoelace formula
        if (clist[cn].posdata != NULL) {
            // Sum over the edges of the polygon
            for (i = 0; i < clist[cn].Nvertices_max - 1; i++) {
                area += clist[cn].posdata[2 * (i) + 0] * clist[cn].posdata[2 * (i + 1) + 1] -
                        clist[cn].posdata[2 * (i + 1) + 0] * clist[cn].posdata[2 * (i) + 1];
            }
            area += clist[cn].posdata[2 * (i) + 0] * clist[cn].posdata[2 * (0) + 1] -
                    clist[cn].posdata[2 * (0) + 0] * clist[cn].posdata[2 * (i) + 1]; // Close sum around a closed path
        }
        clist[cn].area = fabs(area / 2);
    }

    // Sort contours into order of descending enclosed area
    qsort((void *) clist, cpos, sizeof(ContourDesc), ContourCmp);

    // Now loop over the contours that we have traced, drawing them
    for (pass = 1; pass <= 4; pass++) // Fill contour, Stroke contour, Label contour
    {
        if ((pass == 2) && (sg->ContoursLabel != SW_ONOFF_ON)) continue;
        if ((pass == 2) && (sg->ContoursLabel == SW_ONOFF_ON)) fprintf(x->epsbuffer, "gsave\n");
        if ((pass == 4) && (sg->ContoursLabel == SW_ONOFF_ON)) fprintf(x->epsbuffer, "grestore\n");

        // Draw each contour, in each pass
        for (cn = 0; cn < cpos; cn++) {
            const int stkLevelOld = x->c->stackPtr;

            // Skip empty contours
            if (clist[cn].Nvertices_max < 1) continue;

            // Set value of c1
            if (clist[cn].posdata == NULL) continue;
            CVar->real = clist[cn].vreal;

            if ((pass == 1) || (pass == 3)) // Set before filling and stroking
            {
                // Evaluate any expressions in style information for next contour
                for (i = 0;; i++) {
                    int lOP;
                    pplExpr *expr[] = {pd->ww_final.EXPlinetype,       pd->ww_final.EXPlinewidth,
                                       pd->ww_final.EXPpointlinewidth, pd->ww_final.EXPpointsize,
                                       pd->ww_final.EXPpointtype,      pd->ww_final.EXPcolor,
                                       pd->ww_final.EXPfillcolor,      NULL};
                    double *outD[] = {NULL,
                                      &pd->ww_final.linewidth,
                                      &pd->ww_final.pointlinewidth,
                                      &pd->ww_final.pointsize,
                                      NULL,
                                      &col,
                                      &fc,
                                      NULL};
                    int *outI[] = {&pd->ww_final.linetype, NULL, NULL, NULL, &pd->ww_final.pointtype, NULL, NULL, NULL};
                    unsigned char *flagU[] = {&pd->ww_final.USElinetype,
                                              &pd->ww_final.USElinewidth,
                                              &pd->ww_final.USEpointlinewidth,
                                              &pd->ww_final.USEpointsize,
                                              &pd->ww_final.USEpointtype,
                                              NULL,
                                              NULL,
                                              NULL};
                    int *flagA[] = {&pd->ww_final.AUTOlinetype,  NULL, NULL, NULL,
                                    &pd->ww_final.AUTOpointtype, NULL, NULL, NULL};
                    unsigned char clip[] = {0, 0, 0, 0, 0, 0, 0, 2};
                    pplObj *outval;
                    double dbl;

                    // Stop at the end of the list of expressions
                    if (clip[i] > 1) break;
                    if (expr[i] == NULL) continue;

                    // Evaluate the expression
                    outval = ppl_expEval(x->c, expr[i], &lOP, 1, x->iterDepth + 1);

                    // Report an expression which could not be evaluated, followed by the traceback of the error
                    if (x->c->errStat.status) {
                        ppl_snprintfTrunc(msg, sizeof(msg), "Could not evaluate the style expression <%s>.",
                                          expr[i]->ascii);
                        ppl_error(&x->c->errcontext, ERR_NUMERICAL, -1, -1, msg);
                        ppl_tbWrite(x->c);
                        ppl_tbClear(x->c);
                        ppl_stackClean(x->c, stkLevelOld); // Discard anything left on the stack
                        continue;
                    }
                    // The result must be dimensionless
                    if (!outval->dimensionless) {
                        ppl_snprintfTrunc(
                                msg, sizeof(msg),
                                "The style expression <%s> yielded a result which was not a dimensionless number.",
                                expr[i]->ascii);
                        ppl_error(&x->c->errcontext, ERR_NUMERICAL, -1, -1, msg);
                        continue;
                    }
                    // The result must be real
                    if (outval->flagComplex) {
                        ppl_snprintfTrunc(msg, sizeof(msg),
                                          "The style expression <%s> yielded a result which was a complex number.",
                                          expr[i]->ascii);
                        ppl_error(&x->c->errcontext, ERR_NUMERICAL, -1, -1, msg);
                        continue;
                    }
                    // The result must be finite
                    if (!gsl_finite(outval->real)) {
                        ppl_snprintfTrunc(msg, sizeof(msg),
                                          "The style expression <%s> yielded a result which was not a finite number.",
                                          expr[i]->ascii);
                        ppl_error(&x->c->errcontext, ERR_NUMERICAL, -1, -1, msg);
                        continue;
                    }
                    dbl = outval->real;

                    // Store the result in the style
                    if (outD[i] != NULL) *outD[i] = dbl;
                    // Integer style values are clipped to the range of an int
                    if (outI[i] != NULL) {
                        if (dbl < INT_MIN) dbl = INT_MIN + 1;
                        if (dbl > INT_MAX) dbl = INT_MAX - 1;
                        *outI[i] = (int) dbl;
                    }
                    if (flagU[i] != NULL) *flagU[i] = 1;
                    if (flagA[i] != NULL) *flagA[i] = 0;
                    ppl_stackClean(x->c, stkLevelOld); // Discard anything left on the stack
                }
                ppl_stackClean(x->c, stkLevelOld); // Discard anything left on the stack

                // Colors given by their components
                if ((col1 >= 0.0) && (col2 >= 0.0) && (col3 >= 0.0) &&
                    ((pd->ww_final.Col1234Space != SW_COLSPACE_CMYK) || (col4 >= 0.0))) {
                    pd->ww_final.color1 = col1;
                    pd->ww_final.color2 = col2;
                    pd->ww_final.color3 = col3;
                    pd->ww_final.color4 = col4;
                    pd->ww_final.USEcolor = 0;
                    pd->ww_final.USEcolor1234 = 1;
                    pd->ww_final.AUTOcolor = 0;
                }
                if ((fc1 >= 0.0) && (fc2 >= 0.0) && (fc3 >= 0.0) &&
                    ((pd->ww_final.FillCol1234Space != SW_COLSPACE_CMYK) || (fc4 >= 0.0))) {
                    pd->ww_final.fillcolor1 = fc1;
                    pd->ww_final.fillcolor2 = fc2;
                    pd->ww_final.fillcolor3 = fc3;
                    pd->ww_final.fillcolor4 = fc4;
                    pd->ww_final.USEfillcolor = 0;
                    pd->ww_final.USEfillcolor1234 = 1;
                }

                // A color given as a number selects a color from the palette
                if (gsl_finite(col)) {
                    int j, palette_index;
                    for (j = 1; j < PALETTE_LENGTH; j++)
                        if (x->c->set->palette_current[j] == -1) break;
                    palette_index = (((int) col) - 1) % j;
                    while (palette_index < 0) palette_index += j;
                    pd->ww_final.color = x->c->set->palette_current[palette_index];
                    pd->ww_final.Col1234Space = x->c->set->paletteS_current[palette_index];
                    pd->ww_final.color1 = x->c->set->palette1_current[palette_index];
                    pd->ww_final.color2 = x->c->set->palette2_current[palette_index];
                    pd->ww_final.color3 = x->c->set->palette3_current[palette_index];
                    pd->ww_final.color4 = x->c->set->palette4_current[palette_index];
                    pd->ww_final.USEcolor1234 = (pd->ww_final.color == 0);
                    pd->ww_final.AUTOcolor = 0;
                    pd->ww_final.USEcolor = (pd->ww_final.color > 0);
                }
                // A fill color given as a number selects a color from the palette
                if (gsl_finite(fc)) {
                    int j, palette_index;
                    for (j = 1; j < PALETTE_LENGTH; j++)
                        if (x->c->set->palette_current[j] == -1) break;
                    palette_index = (((int) fc) - 1) % j;
                    while (palette_index < 0) palette_index += j;
                    pd->ww_final.fillcolor = x->c->set->palette_current[palette_index];
                    pd->ww_final.FillCol1234Space = x->c->set->paletteS_current[palette_index];
                    pd->ww_final.fillcolor1 = x->c->set->palette1_current[palette_index];
                    pd->ww_final.fillcolor2 = x->c->set->palette2_current[palette_index];
                    pd->ww_final.fillcolor3 = x->c->set->palette3_current[palette_index];
                    pd->ww_final.fillcolor4 = x->c->set->palette4_current[palette_index];
                    pd->ww_final.USEfillcolor1234 = (pd->ww_final.fillcolor == 0);
                    pd->ww_final.USEfillcolor = (pd->ww_final.fillcolor > 0);
                }

                // Advance automatic plot styles
                if (pd->ww_final.AUTOcolor) {
                    int i, j;
                    for (j = 0; j < PALETTE_LENGTH; j++)
                        if (x->c->set->palette_current[j] == -1) break; // j now contains length of palette
                    i = ((pd->ww_final.AUTOcolor + clist[cn].i) - 5) % j; // i is now the palette color number to use
                    while (i < 0) i += j;
                    // A named color, or one given by its components
                    if (x->c->set->palette_current[i] > 0) {
                        pd->ww_final.color = x->c->set->palette_current[i];
                        pd->ww_final.USEcolor = 1;
                    } else {
                        pd->ww_final.Col1234Space = x->c->set->paletteS_current[i];
                        pd->ww_final.color1 = x->c->set->palette1_current[i];
                        pd->ww_final.color2 = x->c->set->palette2_current[i];
                        pd->ww_final.color3 = x->c->set->palette3_current[i];
                        pd->ww_final.color4 = x->c->set->palette4_current[i];
                        pd->ww_final.USEcolor1234 = 1;
                    }
                } else if (pd->ww_final.AUTOlinetype) pd->ww_final.linetype = pd->ww_final.AUTOlinetype + clist[cn].i;
            }

            // PASS 1: Fill path, if required
            if (pass == 1) {
                //      eps_core_SetFillColor(x, &pd->ww_final);
                //      eps_core_SwitchTo_FillColor(x,1);
                //      if (eps_core_isVisible(x))
                //       {
                //        double xps, yps; long c=0;
                //        xpos = clist[cn].posdata[c++];
                //        ypos = clist[cn].posdata[c++];
                //        contourToPostscript(&proj, xpos, ypos, &xps, &yps);
                //        fprintf(x->epsbuffer, "newpath %.2f %.2f moveto\n", xps, yps);
                //        while (c<2*clist[cn].Nvertices_max)
                //         {
                //          xpos = clist[cn].posdata[c++];
                //          ypos = clist[cn].posdata[c++];
                //          contourToPostscript(&proj, xpos, ypos, &xps, &yps);
                //          fprintf(x->epsbuffer, "%.2f %.2f lineto\n", xps, yps);
                //         }
                //        fprintf(x->epsbuffer, "closepath fill\n");
                //       }
            }

            // PASS 2: Set clip path before stroking
            else if (pass == 2) {
                const long n = clist[cn].Nvertices_max;
                if (n < 1) continue;
                // Clip away the area around the contour's label, so that the contour is not drawn through it
                if (sg->ContoursLabel == SW_ONOFF_ON) {
                    int page = x->current->DatasetTextID[pdn] + clist[cn].i;
                    const long i = clist[cn].segment_flatest;
                    const double xlab0 =
                            (clist[cn].posdata[2 * ((i) % n) + 0] + clist[cn].posdata[2 * ((i + 1) % n) + 0]) / 2;
                    const double ylab0 =
                            (clist[cn].posdata[2 * ((i) % n) + 1] + clist[cn].posdata[2 * ((i + 1) % n) + 1]) / 2;
                    double xlab, ylab, wlab, hlab;
                    double bb_top, bb_bottom, ab_left, ab_right;
                    postscriptPage *dviPage;
                    const contourProjection proj = {xo, yo, Lx, Ly, ThetaX, ThetaY, XSize, YSize};

                    // Find where the label goes on the page, and how big it is
                    contourToPostscript(&proj, xlab0, ylab0, &xlab, &ylab);
                    if (x->dvi == NULL) { continue; }
                    dviPage = (postscriptPage *) ppl_listGetItem(x->dvi->output->pages, page + 1);
                    if (dviPage == NULL) { continue; }
                    //bb_left   = dviPage->boundingBox[0];
                    bb_bottom = dviPage->boundingBox[1];
                    //bb_right  = dviPage->boundingBox[2];
                    bb_top = dviPage->boundingBox[3];
                    ab_left = dviPage->textSizeBox[0];
                    //ab_bottom = dviPage->textSizeBox[1];
                    ab_right = dviPage->textSizeBox[2];
                    //ab_top    = dviPage->textSizeBox[3];
                    hlab = (fabs(bb_top - bb_bottom) + 2) * x->current->settings.FontSize;
                    wlab = (fabs(ab_right - ab_left) + 2) * x->current->settings.FontSize;

                    // Set a clipping path which excludes the label's box, using the even-odd rule
                    fprintf(x->epsbuffer,
                            "newpath %.2f %.2f moveto %.2f %.2f lineto %.2f %.2f lineto %.2f %.2f lineto closepath %.2f %.2f %.2f 0 360 arc closepath eoclip\n",
                            xlab - wlab / 2, ylab - hlab / 2, xlab + wlab / 2, ylab - hlab / 2, xlab + wlab / 2,
                            ylab + hlab / 2, xlab - wlab / 2, ylab + hlab / 2, xlab, ylab, 2 * (Lx + Ly));
                }
            }

            // PASS 3: Stroke path
            else if (pass == 3) {
                eps_core_SetColor(x, &pd->ww_final, 1);
                eps_core_SetLinewidth(x, EPS_DEFAULT_LINEWIDTH * pd->ww_final.linewidth, pd->ww_final.linetype, 0);
                // Draw the contour, unless it is invisible
                if (eps_core_isVisible(x)) {
                    double xps, yps;
                    long c = 0;
                    const long n = clist[cn].Nvertices_max;
                    const contourProjection proj = {xo, yo, Lx, Ly, ThetaX, ThetaY, XSize, YSize};
                    if (n < 1) continue;
                    xpos = clist[cn].posdata[c++];
                    ypos = clist[cn].posdata[c++];
                    contourToPostscript(&proj, xpos, ypos, &xps, &yps);
                    fprintf(x->epsbuffer, "newpath %.2f %.2f moveto\n", xps, yps);
                    // Draw a line to each vertex
                    while (c < 2 * clist[cn].Nvertices_min) {
                        xpos = clist[cn].posdata[c++];
                        ypos = clist[cn].posdata[c++];
                        contourToPostscript(&proj, xpos, ypos, &xps, &yps);
                        fprintf(x->epsbuffer, "%.2f %.2f lineto\n", xps, yps);
                    }
                    fprintf(x->epsbuffer, "%sstroke\n", clist[cn].closepath ? "closepath " : "");
                }
            }

            // PASS 4: Label contours
            else if ((pass == 4) && (sg->ContoursLabel == SW_ONOFF_ON)) {
                const long i = clist[cn].segment_flatest;
                const long n = clist[cn].Nvertices_max;
                withWords ww;

                // Skip empty contours
                if (n < 1) continue;
                ppl_withWordsZero(x->c, &ww);
                // Write labels in the color set with "set textcolor"
                if (x->current->settings.TextColor > 0) {
                    ww.color = x->current->settings.TextColor;
                    ww.USEcolor = 1;
                } else {
                    ww.Col1234Space = x->current->settings.TextCol1234Space;
                    ww.color1 = x->current->settings.TextColor1;
                    ww.color2 = x->current->settings.TextColor2;
                    ww.color3 = x->current->settings.TextColor3;
                    ww.color4 = x->current->settings.TextColor4;
                    ww.USEcolor1234 = 1;
                }
                eps_core_SetColor(x, &ww, 1);

                // Write the label at the middle of the flattest segment, unless it is invisible
                if (eps_core_isVisible(x)) {
                    const double xlab =
                            (clist[cn].posdata[2 * ((i) % n) + 0] + clist[cn].posdata[2 * ((i + 1) % n) + 0]) / 2;
                    const double ylab =
                            (clist[cn].posdata[2 * ((i) % n) + 1] + clist[cn].posdata[2 * ((i + 1) % n) + 1]) / 2;
                    canvas_EPSRenderTextItem(
                            x, NULL, x->current->DatasetTextID[pdn] + clist[cn].i,
                            (xo + Lx * xlab / (XSize - 1) * sin(ThetaX) + Ly * ylab / (YSize - 1) * sin(ThetaY)) /
                                    M_TO_PS,
                            (yo + Lx * xlab / (XSize - 1) * cos(ThetaX) + Ly * ylab / (YSize - 1) * cos(ThetaY)) /
                                    M_TO_PS,
                            SW_HALIGN_CENT, SW_VALIGN_CENT, x->CurrentColor, x->current->settings.FontSize, 0.0, NULL,
                            NULL);
                }
            }
        }
    }

    // Reset value of variable c1
    if (CVar != NULL) ppl_contextRestoreVarPointer(x->c, c1name, &CDummy);

    // Done
    return 0;
}
