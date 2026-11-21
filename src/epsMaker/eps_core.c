// eps_core.c
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

// Basic functions used by all of the code which writes postscript: setting the line and fill colors, line width and
// line type, and tracking the bounding box of what has been drawn. The last color, width and type written are
// remembered, so that postscript commands are only written when they change. A color of "" means invisible ink, in
// which case nothing need be drawn.

#define _PPL_EPS_CORE_C 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <gsl/gsl_math.h>

#include "coreUtils/errorReport.h"
#include "stringTools/asciidouble.h"
#include "settings/epsColors.h"
#include "settings/settingTypes.h"
#include "epsMaker/eps_comm.h"
#include "epsMaker/eps_core.h"
#include "epsMaker/eps_style.h"
#include "userspace/context.h"

//! eps_core_clear - Clear the record of the current postscript color, line width and line type, so that they are
//! restated when the next item is drawn
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn

void eps_core_clear(EPSComm *x) {
    x->LastLinewidth = -1;
    strcpy(x->LastPSColor, "");
    strcpy(x->CurrentColor, "");
    strcpy(x->CurrentFillColor, "");
    x->LastLinetype = 0;
    return;
}

//! eps_core_WritePSColor - Write a postscript command to set the current color, if it differs from the color last
//! written
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn

void eps_core_WritePSColor(EPSComm *x) {
    // Write the color only if it has changed, and is not invisible
    if ((strcmp(x->CurrentColor, x->LastPSColor) != 0) && (x->CurrentColor[0] != '\0')) {
        ppl_snprintfTrunc(x->LastPSColor, sizeof(x->LastPSColor), "%s", x->CurrentColor);
        fprintf(x->epsbuffer, "%s\n", x->LastPSColor);
    }
    return;
}

//! eps_core_isVisible - Test whether the current colour is visible, i.e. not set to "transparent". Nothing need be
//! drawn in an invisible colour.
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \return True if the current colour is visible

int eps_core_isVisible(const EPSComm *x) {
    return x->CurrentColor[0] != '\0';
}

//! eps_core_SetColor - Set the color with which subsequent items are drawn, from a set of plot style modifiers
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] ww - The plot style modifiers specifying the color
//! \param [in] WritePS - If true, also write the postscript command to set the color

void eps_core_SetColor(EPSComm *x, withWords *ww, const unsigned char WritePS) {
    // Color may be specified as a named color, or as RGB components, or may not be specified at all, in which case we use black
    if (ww->USEcolor1234) {
        // Colors with non-finite components are drawn in black
        if ((!gsl_finite(ww->color1)) || (!gsl_finite(ww->color2)) || (!gsl_finite(ww->color3))) {
            snprintf(x->CurrentColor, sizeof(x->CurrentColor), "0 0 0 setrgbcolor");
        } else if ((ww->Col1234Space == SW_COLSPACE_CMYK) && (!gsl_finite(ww->color4))) {
            snprintf(x->CurrentColor, sizeof(x->CurrentColor), "0 0 0 setrgbcolor");
        } else if (ww->Col1234Space == SW_COLSPACE_RGB)
            snprintf(x->CurrentColor, sizeof(x->CurrentColor), "%.3f %.3f %.3f setrgbcolor", ww->color1, ww->color2,
                     ww->color3);
        else if (ww->Col1234Space == SW_COLSPACE_HSB)
            snprintf(x->CurrentColor, sizeof(x->CurrentColor), "%.3f %.3f %.3f sethsbcolor", ww->color1, ww->color2,
                     ww->color3);
        else if (ww->Col1234Space == SW_COLSPACE_CMYK)
            snprintf(x->CurrentColor, sizeof(x->CurrentColor), "%.3f %.3f %.3f %.3f setcmykcolor", ww->color1,
                     ww->color2, ww->color3, ww->color4);
        else ppl_error(&x->c->errcontext, ERR_INTERNAL, -1, -1, "Illegal setting for color space switch.");
    } else if ((ww->USEcolor) &&
               ((ww->color == COLOR_NULL) || (ww->color == COLOR_INVISIBLE) || (ww->color == COLOR_TRANSPARENT)))
        strcpy(x->CurrentColor, ""); // This is code to tell us we're writing in invisible ink
    else if (ww->USEcolor)
        snprintf(x->CurrentColor, sizeof(x->CurrentColor), "%.3f %.3f %.3f %.3f setcmykcolor",
                 *(double *) ppl_fetchSettingName(&x->c->errcontext, ww->color, SW_COLOR_INT, (void *) SW_COLOR_CMYK_C,
                                                  sizeof(double)),
                 *(double *) ppl_fetchSettingName(&x->c->errcontext, ww->color, SW_COLOR_INT, (void *) SW_COLOR_CMYK_M,
                                                  sizeof(double)),
                 *(double *) ppl_fetchSettingName(&x->c->errcontext, ww->color, SW_COLOR_INT, (void *) SW_COLOR_CMYK_Y,
                                                  sizeof(double)),
                 *(double *) ppl_fetchSettingName(&x->c->errcontext, ww->color, SW_COLOR_INT, (void *) SW_COLOR_CMYK_K,
                                                  sizeof(double)));
    else snprintf(x->CurrentColor, sizeof(x->CurrentColor), "0 0 0 setrgbcolor");

    // Only change postscript color if the color we want isn't the one we are already using
    if ((strcmp(x->CurrentColor, x->LastPSColor) != 0) && WritePS && (x->CurrentColor[0] != '\0'))
        eps_core_WritePSColor(x);
    return;
}

//! eps_core_SetFillColor - Set the fill color with which subsequent items are drawn, from a set of plot style modifiers
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] ww - The plot style modifiers specifying the fill color

void eps_core_SetFillColor(EPSComm *x, withWords *ww) {
    // Color may be specified as a named color, or as RGB components, or may not be specified at all, in which case we use black
    if (ww->USEfillcolor1234) {
        if (ww->FillCol1234Space == SW_COLSPACE_RGB)
            snprintf(x->CurrentFillColor, sizeof(x->CurrentFillColor), "%.3f %.3f %.3f setrgbcolor", ww->fillcolor1,
                     ww->fillcolor2, ww->fillcolor3);
        else if (ww->FillCol1234Space == SW_COLSPACE_HSB)
            snprintf(x->CurrentFillColor, sizeof(x->CurrentFillColor), "%.3f %.3f %.3f sethsbcolor", ww->fillcolor1,
                     ww->fillcolor2, ww->fillcolor3);
        else if (ww->FillCol1234Space == SW_COLSPACE_CMYK)
            snprintf(x->CurrentFillColor, sizeof(x->CurrentFillColor), "%.3f %.3f %.3f %.3f setcmykcolor",
                     ww->fillcolor1, ww->fillcolor2, ww->fillcolor3, ww->fillcolor4);
        else ppl_error(&x->c->errcontext, ERR_INTERNAL, -1, -1, "Illegal setting for color space switch.");
    } else if ((ww->USEfillcolor) &&
               ((ww->fillcolor == COLOR_NULL) || (ww->fillcolor == COLOR_INVISIBLE) ||
                (ww->fillcolor == COLOR_TRANSPARENT)))
        strcpy(x->CurrentFillColor, ""); // This is code to tell us we're writing in invisible ink
    else if (ww->USEfillcolor)
        snprintf(x->CurrentFillColor, sizeof(x->CurrentFillColor), "%.3f %.3f %.3f %.3f setcmykcolor",
                 *(double *) ppl_fetchSettingName(&x->c->errcontext, ww->fillcolor, SW_COLOR_INT,
                                                  (void *) SW_COLOR_CMYK_C, sizeof(double)),
                 *(double *) ppl_fetchSettingName(&x->c->errcontext, ww->fillcolor, SW_COLOR_INT,
                                                  (void *) SW_COLOR_CMYK_M, sizeof(double)),
                 *(double *) ppl_fetchSettingName(&x->c->errcontext, ww->fillcolor, SW_COLOR_INT,
                                                  (void *) SW_COLOR_CMYK_Y, sizeof(double)),
                 *(double *) ppl_fetchSettingName(&x->c->errcontext, ww->fillcolor, SW_COLOR_INT,
                                                  (void *) SW_COLOR_CMYK_K, sizeof(double)));
    else strcpy(x->CurrentFillColor, ""); // If no fill color is specified, we don't fill

    // Done
    return;
}

static char TempColor[256];

//! eps_core_SwitchTo_FillColor - Switch to drawing in the current fill color, rather than the line color
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] WritePS - If true, also write the postscript command to set the color

void eps_core_SwitchTo_FillColor(EPSComm *x, const unsigned char WritePS) {
    ppl_snprintfTrunc(
            TempColor, sizeof(TempColor), "%s",
            x->CurrentColor); // Buffer the color we're stroking with so we can restore it in eps_core_SwitchFrom_FillColor
    // Write the fill color only if it has changed, and is not invisible
    if ((strcmp(x->CurrentFillColor, x->LastPSColor) != 0) && WritePS && (x->CurrentFillColor[0] != '\0')) {
        ppl_snprintfTrunc(x->LastPSColor, sizeof(x->LastPSColor), "%s", x->CurrentFillColor);
        fprintf(x->epsbuffer, "%s\n", x->LastPSColor);
    }
    ppl_snprintfTrunc(x->CurrentColor, sizeof(x->CurrentColor), "%s",
                      x->CurrentFillColor); // This make the supression of invisible ink work...
    return;
}

//! eps_core_SwitchFrom_FillColor - Switch back to drawing in the current line color, after
//! eps_core_SwitchTo_FillColor()
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] WritePS - If true, also write the postscript command to set the color

void eps_core_SwitchFrom_FillColor(EPSComm *x, const unsigned char WritePS) {
    // Write the line color only if it has changed, and is not invisible
    if ((strcmp(TempColor, x->LastPSColor) != 0) && WritePS && (TempColor[0] != '\0')) {
        ppl_snprintfTrunc(x->LastPSColor, sizeof(x->LastPSColor), "%s", TempColor);
        fprintf(x->epsbuffer, "%s\n", x->LastPSColor);
    }
    ppl_snprintfTrunc(x->CurrentColor, sizeof(x->CurrentColor), "%s",
                      TempColor); // Restore the color we're stroking with
    return;
}

//! eps_core_SetLinewidth - Set the line width and line type (dash pattern) with which subsequent lines are drawn,
//! writing postscript if they have changed
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] lw - The line width, as a multiple of the default line width
//! \param [in] lt - The line type
//! \param [in] offset - The offset into the dash pattern at which to start

void eps_core_SetLinewidth(EPSComm *x, double lw, int lt, double offset) {
    if ((lw == x->LastLinewidth) && (lt == x->LastLinetype) && (offset == 0.0)) return;
    if (lw != x->LastLinewidth) fprintf(x->epsbuffer, "%f setlinewidth\n", lw);
    fprintf(x->epsbuffer, "%s\n", eps_LineType(lt, lw, offset));
    x->LastLinewidth = lw;
    x->LastLinetype = lt;
    return;
}

//! eps_core_BoundingBox - Expand the bounding box of the postscript output to include a point
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] xpos - The x coordinate of the point, in postscript points
//! \param [in] ypos - The y coordinate of the point, in postscript points
//! \param [in] lw - The width of the line drawn through the point, by which the bounding box is expanded

void eps_core_BoundingBox(EPSComm *x, const double xpos, const double ypos, const double lw) {
    if ((!x->bb_set) || (x->bb_left > (xpos - lw / 2))) x->bb_left = (xpos - lw / 2);
    if ((!x->bb_set) || (x->bb_right < (xpos + lw / 2))) x->bb_right = (xpos + lw / 2);
    if ((!x->bb_set) || (x->bb_bottom > (ypos - lw / 2))) x->bb_bottom = (ypos - lw / 2);
    if ((!x->bb_set) || (x->bb_top < (ypos + lw / 2))) x->bb_top = (ypos + lw / 2);
    x->bb_set = 1;
    return;
}

//! eps_core_PlotBoundingBox - Expand the bounding box of the current graph to include a point, and optionally also the
//! bounding box of the postscript output
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] xpos - The x coordinate of the point, in postscript points
//! \param [in] ypos - The y coordinate of the point, in postscript points
//! \param [in] lw - The width of the line drawn through the point, by which the bounding box is expanded
//! \param [in] UpdatePsBB - If true, also expand the bounding box of the postscript output

void eps_core_PlotBoundingBox(EPSComm *x, const double xpos, const double ypos, const double lw,
                              const unsigned char UpdatePsBB) {
    if (x->current->PlotLeftMargin > (xpos - lw / 2)) x->current->PlotLeftMargin = (xpos - lw / 2);
    if (x->current->PlotRightMargin < (xpos + lw / 2)) x->current->PlotRightMargin = (xpos + lw / 2);
    if (x->current->PlotBottomMargin > (ypos - lw / 2)) x->current->PlotBottomMargin = (ypos - lw / 2);
    if (x->current->PlotTopMargin < (ypos + lw / 2)) x->current->PlotTopMargin = (ypos + lw / 2);
    if (UpdatePsBB) eps_core_BoundingBox(x, xpos, ypos, lw);
    return;
}
