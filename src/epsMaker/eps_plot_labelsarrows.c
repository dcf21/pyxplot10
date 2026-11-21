// eps_plot_labelsarrows.c
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

// Draws the arrows and text labels which have been placed on a graph with "set arrow" and "set label". Their positions
// may be given in the coordinates of any axis, or of the graph or page; they are converted to positions on the page,
// clipped to the faces of the graph where necessary, and drawn with eps_primitive_arrow() or as typeset text. Called by
// eps_plot.c while rendering a graph.

#define _PPL_EPS_PLOT_LABELSARROWS 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <gsl/gsl_math.h>

#include "coreUtils/memAlloc.h"
#include "coreUtils/errorReport.h"
#include "stringTools/asciidouble.h"
#include "settings/epsColors.h"
#include "settings/settings.h"
#include "settings/settingTypes.h"
#include "settings/withWords_fns.h"
#include "userspace/unitsArithmetic.h"
#include "userspace/unitsDisp.h"
#include "epsMaker/canvasDraw.h"
#include "epsMaker/eps_arrow.h"
#include "epsMaker/eps_comm.h"
#include "epsMaker/eps_core.h"
#include "epsMaker/eps_plot.h"
#include "epsMaker/eps_plot_canvas.h"
#include "epsMaker/eps_plot_labelsarrows.h"
#include "epsMaker/eps_plot_legend.h"
#include "epsMaker/eps_plot_threedimbuff.h"
#include "epsMaker/eps_settings.h"

//! eps_plot_labelsarrows_YieldUpText - Collect the text labels on a graph, so that they can be typeset
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn

void eps_plot_labelsarrows_YieldUpText(EPSComm *x) {
    ppllabel_object *li;
    for (li = x->current->label_list; li != NULL; li = li->next) {
        if (eps_yieldTextItem(x, li->text)) return;
    }
    return;
}

//! fetchAxis - Find the axis against which one coordinate of an arrow or text label is positioned, and check that the
//! coordinate's physical units are compatible with that axis
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] system - The coordinate system of the coordinate, one of the SW_SYSTEM_* constants
//! \param [in] axes - The axes of the graph in the direction of the coordinate (e.g. the x axes)
//! \param [in] axisN - The number of the axis, for the SW_SYSTEM_AXISN coordinate system
//! \param [in] in - The coordinate
//! \param [in] ItemName - The name of the arrow or label, used in error messages
//! \param [out] axisOut - Set to the axis, or NULL if the coordinate is not positioned against an axis
//! \param [out] out - Set to the value of the coordinate, or to a default position if there is no usable axis
//! \return Zero on success, or one if the coordinate's units were incompatible with the axis (an error is reported)

static int fetchAxis(EPSComm *x, const int system, pplset_axis *axes, const int axisN, const pplObj *in,
                     const char *ItemName, pplset_axis **axisOut, double *out) {
    char msg[PPL_MSG_LENGTH];
    pplset_axis *a;
    *out = in->real;
    *axisOut = NULL;
    if (system == SW_SYSTEM_FIRST) {
        a = &axes[1];
    } else if (system == SW_SYSTEM_SECOND) {
        a = &axes[2];
    } else if (system == SW_SYSTEM_AXISN) {
        // The axis number must be in range
        if ((axisN < 0) || (axisN >= MAX_AXES)) {
            *out = 0.0;
            ppl_error(&x->c->errcontext, ERR_INTERNAL, -1, -1, "Axis number out of range");
            return 0;
        }
        a = &axes[axisN];
    } else {
        // Page and graph coordinates are not positioned against any axis
        *out = 0.0;
        return 0;
    }

    if (!a->RangeFinalised) { a = &axes[1]; } // Default to first axis if desired axis doesn't exist
    // The position must have the same units as the axis's range, if one was set
    if (a->HardUnitSet && (!ppl_unitsDimEqual(in, &a->HardUnit))) {
        ppl_snprintfTrunc(msg, sizeof(msg),
                          "Position specified for %s dimensionally incompatible with the range specified for the axis. "
                          "Position has units of <%s> while axis has units of <%s>.",
                          ItemName, ppl_printUnit(x->c, in, NULL, NULL, 0, 1, 0),
                          ppl_printUnit(x->c, &a->HardUnit, NULL, NULL, 1, 1, 0));
        ppl_error(&x->c->errcontext, ERR_NUMERICAL, -1, -1, msg);
        *out = 0.5;
        return 1;
    }
    // The position must have the same units as the data plotted against the axis
    if (a->DataUnitSet && (!ppl_unitsDimEqual(in, &a->DataUnit))) {
        ppl_snprintfTrunc(msg, sizeof(msg),
                          "Position specified for %s dimensionally incompatible with data plotted against the axis. "
                          "Position has units of <%s> while axis has units of <%s>.",
                          ItemName, ppl_printUnit(x->c, in, NULL, NULL, 0, 1, 0),
                          ppl_printUnit(x->c, &a->DataUnit, NULL, NULL, 1, 1, 0));
        ppl_error(&x->c->errcontext, ERR_NUMERICAL, -1, -1, msg);
        *out = 0.5;
        return 1;
    }
    // If the axis does not exist, place the item in the middle of the graph
    if (!a->RangeFinalised) {
        *out = 0.5;
        return 0;
    }
    *axisOut = a;
    return 0;
}

//! addPageCoordinates - Add the offset of a coordinate given in the page or graph coordinate systems to a position
//! \param [in] system - The coordinate system of the coordinate, one of the SW_SYSTEM_* constants
//! \param [in] in - The coordinate, in metres
//! \param [in] theta - The direction on the page in which the coordinate's axis points, measured clockwise from
//! vertical
//! \param [in,out] xpos - The horizontal position, in postscript points
//! \param [in,out] ypos - The vertical position, in postscript points

static void addPageCoordinates(const int system, const pplObj *in, const double theta, double *xpos, double *ypos) {
    // Only page and graph coordinates are offsets on the page
    if ((system == SW_SYSTEM_PAGE) || (system == SW_SYSTEM_GRAPH)) {
        *xpos += in->real * sin(theta) * M_TO_PS;
        *ypos += in->real * cos(theta) * M_TO_PS;
    }
}

//! clipArrowEnd - Clip one end of an arrow to one face of the graph, if it lies beyond that face, by moving it along
//! the arrow to the face. If the end of the arrow which bears an arrow head is clipped, the head is removed.
//! \param [in,out] pos - Pointers to the arrays of the arrow's {x, y, depth, xap, yap, zap} coordinates; each array has
//! two elements, for the start and the end of the arrow. xap, yap and zap are the fractional positions along the
//! graph's x, y and z axes.
//! \param [in] grad - The rate of change of each of the arrow's coordinates with respect to xap, yap and zap
//! \param [in] xyz - Which axis the face is perpendicular to: 0 for x, 1 for y, 2 for z
//! \param [in] apv - The fractional axis position of the face: 0 or 1
//! \param [in] P - Which end of the arrow to clip: 0 for its start, 1 for its end
//! \param [in,out] ArrowType - The style of arrow head, one of the SW_ARROWTYPE_* constants
//! \return True if the arrow lies entirely beyond the face, and should not be drawn

static int clipArrowEnd(double *pos[6], const double grad[3][6], const int xyz, const double apv, const int P,
                        int *ArrowType) {
    const double *ap = pos[3 + xyz]; // The fractional axis positions of the ends of the arrow along this axis
    const double t = ap[P];
    int k;
    if (!(apv ? (t > 1) : (t < 0))) return 0; // This end of the arrow lies inside the face
    if (apv ? (ap[1 - P] > 1) : (ap[1 - P] < 0)) return 1; // Both ends lie beyond the face
    for (k = 0; k < 6; k++) pos[k][P] += grad[xyz][k] * (apv - t);

    // Remove the arrowhead from the end which has been clipped
    if (P == 0) {
        if (*ArrowType == SW_ARROWTYPE_TWOWAY) *ArrowType = SW_ARROWTYPE_HEAD;
    } else {
        if (*ArrowType == SW_ARROWTYPE_HEAD) *ArrowType = SW_ARROWTYPE_NOHEAD;
        // An arrow with two heads keeps only the head at its start
        if (*ArrowType == SW_ARROWTYPE_TWOWAY) {
            // Reverse the arrow, so that its remaining head is at its end
            for (k = 0; k < 6; k++) {
                const double tmp = pos[k][0];
                pos[k][0] = pos[k][1];
                pos[k][1] = tmp;
            }
            *ArrowType = SW_ARROWTYPE_HEAD;
        }
    }
    return 0;
}

//! eps_plot_labelsarrows - Draw the text labels and arrows on a graph
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] origin_x - The x coordinate of the bottom-left corner of the graph, in postscript points
//! \param [in] origin_y - The y coordinate of the bottom-left corner of the graph, in postscript points
//! \param [in] width - The width of the graph, in postscript points
//! \param [in] height - The height of the graph, in postscript points
//! \param [in] zdepth - The depth of the graph, in postscript points, for 3D plots

void eps_plot_labelsarrows(EPSComm *x, const double origin_x, const double origin_y, const double width,
                           const double height, const double zdepth) {
    int pageno, hal, val;
    char ItemName[64];
    pplarrow_object *ai;
    ppllabel_object *li;
    withWords ww, ww_default;

    // Arrows are drawn in solid black lines unless their style says otherwise
    ppl_withWordsZero(x->c, &ww_default);
    ww_default.USEcolor = 1;
    ww_default.color = COLOR_BLACK;
    ww_default.USElinetype = ww_default.USElinewidth = 1;
    ww_default.linetype = ww_default.linewidth = 1.0;

    // The labels' text was typeset in order, starting from this page
    pageno = x->LaTeXpageno = x->current->SetLabelTextID;

    // Loop through all arrows, rendering them in turn
    for (ai = x->current->arrow_list; ai != NULL; ai = ai->next) {
        pplset_axis *xa0, *ya0, *za0 = NULL, *xa1, *ya1, *za1 = NULL;
        double xin0, yin0, zin0 = 0.5, xin1, yin1, zin1 = 0.5;
        double xpos_, ypos_, xpos[2], ypos[2], depth[2];
        double xap[2], yap[2], zap[2], theta_x, theta_y, theta_z;
        int status = 0, xrn0, yrn0, zrn0, xrn1, yrn1, zrn1;
        int ArrowType = ai->pplarrow_style;
        ppl_snprintfTrunc(ItemName, sizeof(ItemName), "arrow %d on plot %d", ai->id, x->current->id);
        status |= fetchAxis(x, ai->system_x0, x->current->XAxes, ai->axis_x0, &ai->x0, ItemName, &xa0, &xin0);
        status |= fetchAxis(x, ai->system_y0, x->current->YAxes, ai->axis_y0, &ai->y0, ItemName, &ya0, &yin0);
        if (x->current->ThreeDim) {
            status |= fetchAxis(x, ai->system_z0, x->current->ZAxes, ai->axis_z0, &ai->z0, ItemName, &za0, &zin0);
        }
        status |= fetchAxis(x, ai->system_x1, x->current->XAxes, ai->axis_x1, &ai->x1, ItemName, &xa1, &xin1);
        status |= fetchAxis(x, ai->system_y1, x->current->YAxes, ai->axis_y1, &ai->y1, ItemName, &ya1, &yin1);
        if (x->current->ThreeDim) {
            status |= fetchAxis(x, ai->system_z1, x->current->ZAxes, ai->axis_z1, &ai->z1, ItemName, &za1, &zin1);
        }
        if (!status)
            for (xrn0 = 0; xrn0 <= ((xa0 != NULL) ? xa0->AxisValueTurnings : 0); xrn0++)
                for (yrn0 = 0; yrn0 <= ((ya0 != NULL) ? ya0->AxisValueTurnings : 0); yrn0++)
                    for (zrn0 = 0; zrn0 <= ((x->current->ThreeDim && (za0 != NULL)) ? za0->AxisValueTurnings : 0);
                         zrn0++)
                        for (xrn1 = 0; xrn1 <= ((xa1 != NULL) ? xa1->AxisValueTurnings : 0); xrn1++)
                            for (yrn1 = 0; yrn1 <= ((ya1 != NULL) ? ya1->AxisValueTurnings : 0); yrn1++)
                                for (zrn1 = 0;
                                     zrn1 <= ((x->current->ThreeDim && (za1 != NULL)) ? za1->AxisValueTurnings : 0);
                                     zrn1++) {
                                    if ((xa0 == xa1) && (xrn0 != xrn1)) continue;
                                    if ((ya0 == ya1) && (yrn0 != yrn1)) continue;
                                    if ((za0 == za1) && (zrn0 != zrn1)) continue;
                                    eps_plot_GetPosition(&xpos[0], &ypos[0], &depth[0], &xap[0], &yap[0], &zap[0],
                                                         &theta_x, &theta_y, &theta_z, x->current->ThreeDim, xin0, yin0,
                                                         zin0, xa0, ya0, za0, xrn0, yrn0, zrn0, &x->current->settings,
                                                         origin_x, origin_y, width, height, zdepth, 1);
                                    eps_plot_GetPosition(&xpos[1], &ypos[1], &depth[1], &xap[1], &yap[1], &zap[1],
                                                         &theta_x, &theta_y, &theta_z, x->current->ThreeDim, xin1, yin1,
                                                         zin1, xa1, ya1, za1, xrn1, yrn1, zrn1, &x->current->settings,
                                                         origin_x, origin_y, width, height, zdepth, 1);
                                    if (((xap[0] < 0) || (xap[0] > 1) || (yap[0] < 0) || (yap[0] > 1) || (zap[0] < 0) ||
                                         (zap[0] > 1)) &&
                                        ((xa0 == NULL) || (ya0 == NULL) || (x->current->ThreeDim && (za0 == NULL)) ||
                                         (xa1 == NULL) || (ya1 == NULL) || (x->current->ThreeDim && (za1 == NULL))))
                                        continue;
                                    xpos_ = xpos[0];
                                    ypos_ = ypos[0];
                                    addPageCoordinates(ai->system_x0, &ai->x0, theta_x, &xpos_, &ypos_);
                                    addPageCoordinates(ai->system_y0, &ai->y0, theta_y, &xpos_, &ypos_);
                                    if (x->current->ThreeDim) {
                                        addPageCoordinates(ai->system_z0, &ai->z0, theta_z, &xpos_, &ypos_);
                                    }
                                    xpos[0] = xpos_;
                                    ypos[0] = ypos_;
                                    xpos_ = xpos[1];
                                    ypos_ = ypos[1];
                                    addPageCoordinates(ai->system_x1, &ai->x1, theta_x, &xpos_, &ypos_);
                                    addPageCoordinates(ai->system_y1, &ai->y1, theta_y, &xpos_, &ypos_);
                                    if (x->current->ThreeDim) {
                                        addPageCoordinates(ai->system_z1, &ai->z1, theta_z, &xpos_, &ypos_);
                                    }
                                    xpos[1] = xpos_;
                                    ypos[1] = ypos_;

                                    {
                                        const double grad[3][6] = {
                                                // d[x y z xap yap zap]/d[xap yap zap]
                                                {
                                                        (xap[1] == xap[0]) ? 0.0
                                                                           : (xpos[1] - xpos[0]) / (xap[1] - xap[0]),
                                                        (xap[1] == xap[0]) ? 0.0
                                                                           : (ypos[1] - ypos[0]) / (xap[1] - xap[0]),
                                                        (xap[1] == xap[0]) ? 0.0
                                                                           : (depth[1] - depth[0]) / (xap[1] - xap[0]),
                                                        1.0,
                                                        (xap[1] == xap[0]) ? 0.0
                                                                           : (yap[1] - yap[0]) / (xap[1] - xap[0]),
                                                        (xap[1] == xap[0]) ? 0.0
                                                                           : (zap[1] - zap[0]) / (xap[1] - xap[0]),
                                                },
                                                {
                                                        (yap[1] == yap[0]) ? 0.0
                                                                           : (xpos[1] - xpos[0]) / (yap[1] - yap[0]),
                                                        (yap[1] == yap[0]) ? 0.0
                                                                           : (ypos[1] - ypos[0]) / (yap[1] - yap[0]),
                                                        (yap[1] == yap[0]) ? 0.0
                                                                           : (depth[1] - depth[0]) / (yap[1] - yap[0]),
                                                        (yap[1] == yap[0]) ? 0.0
                                                                           : (xap[1] - xap[0]) / (yap[1] - yap[0]),
                                                        1.0,
                                                        (yap[1] == yap[0]) ? 0.0
                                                                           : (zap[1] - zap[0]) / (yap[1] - yap[0]),
                                                },
                                                {(zap[1] == zap[0]) ? 0.0 : (xpos[1] - xpos[0]) / (zap[1] - zap[0]),
                                                 (zap[1] == zap[0]) ? 0.0 : (ypos[1] - ypos[0]) / (zap[1] - zap[0]),
                                                 (zap[1] == zap[0]) ? 0.0 : (depth[1] - depth[0]) / (zap[1] - zap[0]),
                                                 (zap[1] == zap[0]) ? 0.0 : (xap[1] - xap[0]) / (zap[1] - zap[0]),
                                                 (zap[1] == zap[0]) ? 0.0 : (yap[1] - yap[0]) / (zap[1] - zap[0]),
                                                 1.0}};
                                        double *pos[6] = {xpos, ypos, depth, xap, yap, zap};

                                        // Clip each end of the arrow to each face of the graph, skipping arrows which
                                        // lie wholly outside it
                                        if (clipArrowEnd(pos, grad, 0, 0.0, 0, &ArrowType)) continue;
                                        if (clipArrowEnd(pos, grad, 0, 1.0, 0, &ArrowType)) continue;
                                        if (clipArrowEnd(pos, grad, 0, 0.0, 1, &ArrowType)) continue;
                                        if (clipArrowEnd(pos, grad, 0, 1.0, 1, &ArrowType)) continue;
                                        if (clipArrowEnd(pos, grad, 1, 0.0, 0, &ArrowType)) continue;
                                        if (clipArrowEnd(pos, grad, 1, 1.0, 0, &ArrowType)) continue;
                                        if (clipArrowEnd(pos, grad, 1, 0.0, 1, &ArrowType)) continue;
                                        if (clipArrowEnd(pos, grad, 1, 1.0, 1, &ArrowType)) continue;
                                        if (clipArrowEnd(pos, grad, 2, 0.0, 0, &ArrowType)) continue;
                                        if (clipArrowEnd(pos, grad, 2, 1.0, 0, &ArrowType)) continue;
                                        if (clipArrowEnd(pos, grad, 2, 0.0, 1, &ArrowType)) continue;
                                        if (clipArrowEnd(pos, grad, 2, 1.0, 1, &ArrowType)) continue;
                                    }

                                    // Draw the arrow, if both ends can be placed on the page
                                    ppl_withWordsMerge(x->c, &ww, &ai->style, &ww_default, NULL, NULL, NULL, 1);
                                    if ((gsl_finite(xpos[0])) && (gsl_finite(ypos[0])) && (gsl_finite(xpos[1])) &&
                                        (gsl_finite(ypos[1])))
                                        eps_primitive_arrow(x, ArrowType, xpos[0], ypos[0],
                                                            x->current->ThreeDim ? &depth[0] : NULL, xpos[1], ypos[1],
                                                            x->current->ThreeDim ? &depth[1] : NULL, &ww);
                                }
    }

    // By default, use 'set textcolor' color for text labels
    if (x->current->settings.TextColor > 0) {
        ww_default.color = x->current->settings.TextColor;
        ww_default.USEcolor = 1;
        ww_default.USEcolor1234 = 0;
    } else {
        ww_default.Col1234Space = x->current->settings.TextCol1234Space;
        ww_default.color1 = x->current->settings.TextColor1;
        ww_default.color2 = x->current->settings.TextColor2;
        ww_default.color3 = x->current->settings.TextColor3;
        ww_default.color4 = x->current->settings.TextColor4;
        ww_default.USEcolor = 0;
        ww_default.USEcolor1234 = 1;
    }

    // Loop through all text labels, rendering them in turn
    for (li = x->current->label_list; li != NULL; li = li->next)
        // Labels with no text are skipped
        if ((li->text != NULL) && (li->text[0] != '\0')) {
            pplset_axis *xa, *ya, *za = NULL;
            double xin, yin, zin = 0.5;
            double xpos_, ypos_, depth, xap, yap, zap, theta_x, theta_y, theta_z;
            int status = 0, xrn, yrn, zrn;
            ppl_snprintfTrunc(ItemName, sizeof(ItemName), "label %d on plot %d", li->id, x->current->id);
            status |= fetchAxis(x, li->system_x, x->current->XAxes, li->axis_x, &li->x, ItemName, &xa, &xin);
            status |= fetchAxis(x, li->system_y, x->current->YAxes, li->axis_y, &li->y, ItemName, &ya, &yin);
            if (x->current->ThreeDim) {
                status |= fetchAxis(x, li->system_z, x->current->ZAxes, li->axis_z, &li->z, ItemName, &za, &zin);
            }
            if (!status)
                for (xrn = 0; xrn <= ((xa != NULL) ? xa->AxisValueTurnings : 0); xrn++)
                    for (yrn = 0; yrn <= ((ya != NULL) ? ya->AxisValueTurnings : 0); yrn++)
                        for (zrn = 0; zrn <= ((x->current->ThreeDim && (za != NULL)) ? za->AxisValueTurnings : 0);
                             zrn++) {
                            double xgap, ygap, xgap2, ygap2;
                            eps_plot_GetPosition(&xpos_, &ypos_, &depth, &xap, &yap, &zap, &theta_x, &theta_y, &theta_z,
                                                 x->current->ThreeDim, xin, yin, zin, xa, ya, za, xrn, yrn, zrn,
                                                 &x->current->settings, origin_x, origin_y, width, height, zdepth, 0);
                            addPageCoordinates(li->system_x, &li->x, theta_x, &xpos_, &ypos_);
                            addPageCoordinates(li->system_y, &li->y, theta_y, &xpos_, &ypos_);
                            if (x->current->ThreeDim) {
                                addPageCoordinates(li->system_z, &li->z, theta_z, &xpos_, &ypos_);
                            }
                            if (li->HAlign != 0) hal = li->HAlign;
                            else hal = x->current->settings.TextHAlign;
                            if (li->VAlign != 0) val = li->VAlign;
                            else val = x->current->settings.TextVAlign;
                            xgap = -(hal - SW_HALIGN_CENT) * li->gap;
                            ygap = (val - SW_VALIGN_CENT) * li->gap;
                            xgap2 = xgap * cos(li->rotation) - ygap * sin(li->rotation);
                            ygap2 = xgap * sin(li->rotation) + ygap * cos(li->rotation);
                            ppl_withWordsMerge(x->c, &ww, &li->style, &ww_default, NULL, NULL, NULL, 1);
                            eps_core_SetColor(x, &ww, 0);
                            if (eps_core_isVisible(x))
                                // Draw the label, if it can be placed on the page, through the depth-sorting buffer
                                if ((gsl_finite(xpos_)) && (gsl_finite(ypos_)) && (gsl_finite(depth))) {
                                    char *colstr = NULL, *text = NULL;
                                    colstr = (char *) ppl_memAlloc(strlen(x->CurrentColor) + 1);
                                    // Typeset the label at its position, offset by its gap in the direction of its
                                    // alignment
                                    if (colstr != NULL) {
                                        double fs = x->current->settings.FontSize;
                                        if (li->fontsizeSet) fs = li->fontsize;
                                        strcpy(colstr, x->CurrentColor);
                                        canvas_EPSRenderTextItem(x, &text, pageno, xpos_ / M_TO_PS + xgap2,
                                                                 ypos_ / M_TO_PS + ygap2, hal, val, x->CurrentColor, fs,
                                                                 li->rotation, NULL, NULL);
                                        if (text != NULL) ThreeDimBuffer_writeps(x, depth, 1, 1, 0, 1, colstr, text);
                                    }
                                }
                        }
            pageno++;
        }

    // Done
    return;
}
