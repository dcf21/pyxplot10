// eps_plot_axespaint.c
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

// Draws the axes of graphs, with their ticks, tick labels and axis labels. Axes are drawn along the edges of the graph
// (or of the cube of a three-dimensional graph), several axes on the same side being stacked outwards, or through the
// zero of the perpendicular axes for axes set with "set axis ... atzero". In three-dimensional plots, axes on the back
// of the cube are drawn before the data, and those at the front afterwards, through the depth-sorting buffer. Tick
// labels are aligned so that they point away from the axis whatever its direction.

#define _PPL_EPS_PLOT_AXESPAINT 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <gsl/gsl_math.h>

#include "coreUtils/memAlloc.h"
#include "coreUtils/errorReport.h"

#include "epsMaker/canvasDraw.h"
#include "settings/axes_fns.h"
#include "settings/settings.h"
#include "settings/withWords_fns.h"
#include "settings/settingTypes.h"
#include "stringTools/asciidouble.h"
#include "userspace/unitsDisp.h"

#include "epsMaker/eps_arrow.h"
#include "epsMaker/eps_comm.h"
#include "epsMaker/eps_core.h"
#include "epsMaker/eps_plot_axespaint.h"
#include "epsMaker/eps_plot_canvas.h"
#include "epsMaker/eps_plot_threedimbuff.h"
#include "epsMaker/eps_settings.h"

//! eps_plot_LabelAlignment - Choose the alignment of a text label relative to its anchor point, given the direction in
//! which the anchor point lies from the label
//! \param [in] theta_pinpoint - The direction from the label to its anchor point, in radians, measured clockwise from
//! upwards
//! \param [out] HALIGN - Set to the horizontal alignment, one of the SW_HALIGN_* constants
//! \param [out] VALIGN - Set to the vertical alignment, one of the SW_VALIGN_* constants

void eps_plot_LabelAlignment(double theta_pinpoint, int *HALIGN, int *VALIGN) {
    theta_pinpoint = fmod(theta_pinpoint, 2 * M_PI);
    while (theta_pinpoint < 0.0) theta_pinpoint += 2 * M_PI;
    // Choose the alignment from the direction, measured clockwise from upright, in which the label lies from its anchor
    // point
    if (ppl_dblApprox(theta_pinpoint, 0.0, 1e-6)) {
        *HALIGN = SW_HALIGN_CENT;
        *VALIGN = SW_VALIGN_TOP;
    } else if (ppl_dblApprox(theta_pinpoint, M_PI / 2, 1e-6)) {
        *HALIGN = SW_HALIGN_RIGHT;
        *VALIGN = SW_VALIGN_CENT;
    } else if (ppl_dblApprox(theta_pinpoint, M_PI, 1e-6)) {
        *HALIGN = SW_HALIGN_CENT;
        *VALIGN = SW_VALIGN_BOT;
    } else if (ppl_dblApprox(theta_pinpoint, 3 * M_PI / 2, 1e-6)) {
        *HALIGN = SW_HALIGN_LEFT;
        *VALIGN = SW_VALIGN_CENT;
    } else if (theta_pinpoint < M_PI / 2) {
        *HALIGN = SW_HALIGN_RIGHT;
        *VALIGN = SW_VALIGN_TOP;
    } else if (theta_pinpoint < M_PI) {
        *HALIGN = SW_HALIGN_RIGHT;
        *VALIGN = SW_VALIGN_BOT;
    } else if (theta_pinpoint < 3 * M_PI / 2) {
        *HALIGN = SW_HALIGN_LEFT;
        *VALIGN = SW_VALIGN_BOT;
    } else {
        *HALIGN = SW_HALIGN_LEFT;
        *VALIGN = SW_VALIGN_TOP;
    }
    return;
}

//! eps_plot_axispaint - Draw a single axis, with its ticks, tick labels and axis label
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] ww - The style with which to draw the axis line
//! \param [in] a - The axis to draw
//! \param [in] xyz - The direction of the axis: 0 for x, 1 for y, 2 for z
//! \param [in] CP2 - Unused
//! \param [in] Lr - If true, tick labels are placed on the right-hand side of the axis, when viewed from its start;
//! otherwise on the left
//! \param [in] x1 - The x coordinate of the start of the axis, in postscript points
//! \param [in] y1 - The y coordinate of the start of the axis, in postscript points
//! \param [in] z1 - The depth of the start of the axis, for 3D plots, or NULL
//! \param [in] x2 - The x coordinate of the end of the axis, in postscript points
//! \param [in] y2 - The y coordinate of the end of the axis, in postscript points
//! \param [in] z2 - The depth of the end of the axis, for 3D plots, or NULL
//! \param [in] theta_a - The direction in which ticks point on one side of the axis, in radians, or NaN for no ticks on
//! this side
//! \param [in] theta_b - The direction in which ticks point on the other side of the axis, in radians, or NaN for no
//! ticks on this side
//! \param [out] OutputWidth - Set to the distance by which the axis's labels extend out from the axis, so that further
//! axes can be placed beyond them
//! \param [in] PrintLabels - If true, draw the tick labels and axis label; otherwise draw only the axis line and ticks

void eps_plot_axispaint(EPSComm *x, withWords *ww, pplset_axis *a, const int xyz, const double CP2,
                        const unsigned char Lr, const double x1, const double y1, const double *z1, const double x2,
                        const double y2, const double *z2, const double theta_a, const double theta_b,
                        double *OutputWidth, const unsigned char PrintLabels) {
    char msg[PPL_MSG_LENGTH];
    int lt;
    double TickMaxHeight = 0.0, height, width;
    int HALIGN, VALIGN, HALIGN_THIS, VALIGN_THIS;
    double theta, theta_pinpoint; // clockwise rotation
    double lw_scale;
    char *last_colstr = NULL;

    // This axis's labels were typeset in order, starting from this page
    x->LaTeXpageno = a->FirstTextID;
    *OutputWidth = 0.0;

    // Set linewidth and linetype
    if (ww->USElinewidth) lw_scale = ww->linewidth;
    else lw_scale = x->current->settings.LineWidth;
    const double lw = EPS_DEFAULT_LINEWIDTH * lw_scale;

    // The line type is the one requested, or solid
    if (ww->USElinetype) lt = ww->linetype;
    else lt = 1;

    // Draw line of axis
    if (eps_core_isVisible(x)) {
        if (a->ArrowType == SW_AXISDISP_NOARR) eps_primitive_arrow(x, SW_ARROWTYPE_NOHEAD, x1, y1, z1, x2, y2, z2, ww);
        else if (a->ArrowType == SW_AXISDISP_ARROW)
            eps_primitive_arrow(x, SW_ARROWTYPE_HEAD, x1, y1, z1, x2, y2, z2, ww);
        else if (a->ArrowType == SW_AXISDISP_TWOAR)
            eps_primitive_arrow(x, SW_ARROWTYPE_TWOWAY, x1, y1, z1, x2, y2, z2, ww);
        else if (a->ArrowType == SW_AXISDISP_BACKA)
            eps_primitive_arrow(x, SW_ARROWTYPE_HEAD, x2, y2, z2, x1, y1, z1, ww);

        // The direction of the axis on the page
        double theta_axis = atan2(x2 - x1, y2 - y1);
        if (!gsl_finite(theta_axis)) theta_axis = 0.0;

        // Paint axis ticks
        for (int i = 0; i < 2; i++) {
            int TR;
            double *TLP, TRA, TLEN;
            char **TLS;

            // The positions, labels and length of the major ticks, or the minor ticks
            if (i == 0) {
                TLP = a->TickListPositions;
                TLS = a->TickListStrings;
                TRA = a->TickLabelRotate;
                TR = a->TickLabelRotation;
                TLEN = EPS_AXES_MAJTICKLEN;
            } // Major ticks
            // Minor ticks
            else {
                TLP = a->MTickListPositions;
                TLS = a->MTickListStrings;
                TRA = a->TickLabelRotate;
                TR = a->TickLabelRotation;
                TLEN = EPS_AXES_MINTICKLEN;
            } // Minor ticks

            // Work out the rotation of the tick labels
            if (TR == SW_TICLABDIR_HORI) theta = 0.0;
            else if (TR == SW_TICLABDIR_VERT)
                theta = -M_PI / 2; // the clockwise rotation of the labels relative to upright
            else theta = -TRA;
            theta_pinpoint =
                    theta + M_PI / 2 + theta_axis + M_PI * (!Lr); // Angle around textboxes where it is anchored
            eps_plot_LabelAlignment(theta_pinpoint, &HALIGN, &VALIGN);

            // Draw each tick, unless there are none
            if (TLP != NULL) {
                // Draw each tick and its label
                for (int l = 0; TLS[l] != NULL; l++) {
                    unsigned char state = 0;
                    double tic_lab_xoff = 0.0;
                    const double tic_x1 = x1 + (x2 - x1) * TLP[l];
                    const double tic_y1 = y1 + (y2 - y1) * TLP[l];

                    // Draw the tick on each face of the graph which meets the axis, until one is drawn
                    for (int m = 0; ((m < 2) && (!state)); m++) {
                        const double theta_ab = m ? theta_a : theta_b;
                        if (!gsl_finite(theta_ab)) continue;
                        const double tic_x2 = tic_x1 +
                                (a->tics.tickDir == SW_TICDIR_IN ? 0.0 : 1.0) * sin(theta_ab) * TLEN *
                                        M_TO_PS; // top of tick
                        const double tic_y2 =
                                tic_y1 + (a->tics.tickDir == SW_TICDIR_IN ? 0.0 : 1.0) * cos(theta_ab) * TLEN * M_TO_PS;
                        const double tic_x3 = tic_x1 +
                                (a->tics.tickDir == SW_TICDIR_OUT ? 0.0 : -1.0) * sin(theta_ab) * TLEN *
                                        M_TO_PS; // bottom of tick
                        const double tic_y3 = tic_y1 +
                                (a->tics.tickDir == SW_TICDIR_OUT ? 0.0 : -1.0) * cos(theta_ab) * TLEN * M_TO_PS;

                        // Check for special case of ticks at zero on axes crossed by atzero axes
                        HALIGN_THIS = HALIGN;
                        VALIGN_THIS = VALIGN;
                        // Where axes cross at zero, a tick at zero would clash with the other axis
                        if ((a->CrossedAtZero) && (a->atzero)) {
                            const double CP1 = TLP[l];
                            double xl = TLP[l] - 1e-3;
                            double xr = TLP[l] + 1e-3;
                            if (xl < 0.0) xl = 0.0;
                            if (xl > 1.0) xl = 1.0;
                            if (xr < 0.0) xr = 0.0;
                            if (xr > 1.0) xr = 1.0;
                            xl = pplaxis_InvGetPosition(xl, a);
                            xr = pplaxis_InvGetPosition(xr, a);
                            if ((gsl_finite(xl)) && (gsl_finite(xr)) &&
                                ((xl == 0) || (xr == 0) || ((xl < 0) && (xr > 0)) || ((xl > 0) && (xr < 0)))) {
                                if (xyz != 0) // y/z-axis -- only put label at zero if axis is at edge of graph
                                {
                                    // Skip the label at zero unless this axis is at the edge of the graph
                                    if (!(((CP2 < 1e-3) && (Lr)) || ((CP2 > 0.999) && (!Lr)))) {
                                        if (PrintLabels && (TLS[l][0] != '\0')) x->LaTeXpageno++;
                                        state = 1;
                                        continue;
                                    }
                                } else // x-axis -- if at edge of graph, put label at zero. If y-axis is labelled at zero, don't put label at zero...
                                {
                                    // Unless the y axis is at the edge of the graph, either skip the label at zero or
                                    // move it aside
                                    if (!(((CP2 < 1e-3) && (!Lr)) || ((CP2 > 0.999) && (Lr)))) {
                                        const int Lr1 = (xyz == 0) ^ (CP1 > 0.999);
                                        // Skip the label at zero if the x axis is at the edge of the graph
                                        if (((CP1 < 1e-3) && (Lr1)) || ((CP1 > 0.999) && (!Lr1))) {
                                            if (PrintLabels && (TLS[l][0] != '\0')) x->LaTeXpageno++;
                                            state = 1;
                                            continue;
                                        } else // ... otherwise, displace label at zero to one side
                                        {
                                            const double theta_pinpoint = theta + 1.5 * M_PI / 2 + theta_axis +
                                                    M_PI * (!Lr); // Angle around textboxes where it is anchored
                                            eps_plot_LabelAlignment(theta_pinpoint, &HALIGN_THIS, &VALIGN_THIS);
                                            tic_lab_xoff = -TLEN; // Move label to the left
                                        }
                                    }
                                }
                            }
                        }

                        // Stroke the tick, unless it is at the end of an axis with an arrowhead
                        if (!(((TLP[l] < 1e-3) &&
                               ((a->ArrowType == SW_AXISDISP_BACKA) || (a->ArrowType == SW_AXISDISP_TWOAR))) ||
                              ((TLP[l] > 0.999) &&
                               ((a->ArrowType == SW_AXISDISP_ARROW) || (a->ArrowType == SW_AXISDISP_TWOAR))))) {
                            if (z1 == NULL)
                                fprintf(x->epsbuffer, "newpath %.2f %.2f moveto %.2f %.2f lineto stroke\n", tic_x2,
                                        tic_y2, tic_x3, tic_y3);
                            // In three dimensions, the tick is drawn through the depth-sorting buffer, with a copy of
                            // the color
                            else {
                                if ((last_colstr == NULL) || (strcmp(last_colstr, x->CurrentColor) != 0))
                                    last_colstr = (char *) ppl_memAlloc(strlen(x->CurrentColor) + 1);
                                // Write the tick with its depth
                                if (last_colstr != NULL) {
                                    strcpy(last_colstr, x->CurrentColor);
                                    ppl_snprintfTrunc(msg, sizeof(msg),
                                                      "newpath %.2f %.2f moveto %.2f %.2f lineto stroke\n", tic_x2,
                                                      tic_y2, tic_x3, tic_y3);
                                    ThreeDimBuffer_writeps(x, *z1 + (*z2 - *z1) * TLP[l], lt, lw, 0.0, 1, last_colstr,
                                                           msg);
                                }
                            }
                            eps_core_PlotBoundingBox(x, tic_x2, tic_y2, EPS_AXES_LINEWIDTH * EPS_DEFAULT_LINEWIDTH, 1);
                            eps_core_PlotBoundingBox(x, tic_x3, tic_y3, EPS_AXES_LINEWIDTH * EPS_DEFAULT_LINEWIDTH, 1);
                        }
                    }
                    if (state) continue;

                    // Paint the tick label
                    if (PrintLabels && (TLS[l][0] != '\0')) {
                        const int pageno = x->LaTeXpageno++;

                        // Place the label beyond the tick, on the outward side of the axis
                        const double xlab = tic_x1 / M_TO_PS + (Lr ? -1.0 : 1.0) * sin(theta_axis + M_PI / 2) * EPS_AXES_TEXTGAP + tic_lab_xoff;
                        const double ylab = tic_y1 / M_TO_PS + (Lr ? -1.0 : 1.0) * cos(theta_axis + M_PI / 2) * EPS_AXES_TEXTGAP;

                        // Draw the label, unless it is invisible, and note the greatest extent of the labels away from
                        // the axis
                        if (eps_core_isVisible(x)) {
                            char *text = NULL;
                            canvas_EPSRenderTextItem(x, &text, pageno, xlab, ylab, HALIGN_THIS, VALIGN_THIS,
                                                     x->CurrentColor, x->current->settings.FontSize, theta, &width,
                                                     &height);
                            // Write the label directly, or through the depth-sorting buffer
                            if (text != NULL) {
                                if (z1 == NULL) fprintf(x->epsbuffer, "%s", text);
                                // In three dimensions, the label is drawn through the depth-sorting buffer, with a copy
                                // of the color
                                else {
                                    if ((last_colstr == NULL) || (strcmp(last_colstr, x->CurrentColor) != 0))
                                        last_colstr = (char *) ppl_memAlloc(strlen(x->CurrentColor) + 1);
                                    // Write the label with its depth
                                    if (last_colstr != NULL) {
                                        strcpy(last_colstr, x->CurrentColor);
                                        ThreeDimBuffer_writeps(x, *z1 + (*z2 - *z1) * TLP[l], lt, lw, 0.0, 1,
                                                               last_colstr, text);
                                    }
                                }
                            }
                            height = height * fabs(cos(theta_pinpoint)) + width * fabs(sin(theta_pinpoint));
                            if (height > TickMaxHeight) TickMaxHeight = height;
                        }
                    }
                }
            }
        }
        if (TickMaxHeight > 0.0)
            *OutputWidth = EPS_AXES_TEXTGAP * M_TO_PS + TickMaxHeight; // Allow a gap after axis labels

        // Write axis label
        if (PrintLabels && (a->FinalAxisLabel != NULL) && (a->FinalAxisLabel[0] != '\0')) {
            const int pageno = x->LaTeXpageno++;
            double width, height;

            // Work out the rotation of the tick label
            theta = -a->LabelRotate;
            theta_pinpoint = theta + M_PI * Lr; // Angle around textbox where it is anchored
            eps_plot_LabelAlignment(theta_pinpoint, &HALIGN, &VALIGN);

            // Place the label beyond the tick labels, on the outward side of the axis
            const double xlab = (x1 + x2) / 2 / M_TO_PS +
                    (Lr ? -1.0 : 1.0) * (2 * EPS_AXES_TEXTGAP + TickMaxHeight / M_TO_PS) * sin(theta_axis + M_PI / 2);
            const double ylab = (y1 + y2) / 2 / M_TO_PS +
                    (Lr ? -1.0 : 1.0) * (2 * EPS_AXES_TEXTGAP + TickMaxHeight / M_TO_PS) * cos(theta_axis + M_PI / 2);

            // Draw the label, unless it is invisible, rotated so that it reads along the axis but never upside down
            if (eps_core_isVisible(x)) {
                char *text = NULL;
                double theta_text = theta + M_PI / 2 - theta_axis;
                theta_text = fmod(theta_text, 2 * M_PI);
                if (theta_text < -M_PI) theta_text += 2 * M_PI;
                if (theta_text > M_PI) theta_text -= 2 * M_PI;
                if (theta_text > M_PI / 2) theta_text -= M_PI;
                if (theta_text < -M_PI / 2) theta_text += M_PI;
                canvas_EPSRenderTextItem(x, &text, pageno, xlab, ylab, HALIGN, VALIGN, x->CurrentColor,
                                         x->current->settings.FontSize, theta_text, &width, &height);
                *OutputWidth += (EPS_AXES_TEXTGAP * M_TO_PS + height * fabs(cos(theta_pinpoint)) +
                                 width * fabs(sin(theta_pinpoint))); // Allow gap after label
                // Write the label directly, or through the depth-sorting buffer
                if (text != NULL) {
                    if (z1 == NULL) fprintf(x->epsbuffer, "%s", text);
                    // In three dimensions, the label is drawn through the depth-sorting buffer, with a copy of the
                    // color
                    else {
                        if ((last_colstr == NULL) || (strcmp(last_colstr, x->CurrentColor) != 0))
                            last_colstr = (char *) ppl_memAlloc(strlen(x->CurrentColor) + 1);
                        // Write the label with its depth
                        if (last_colstr != NULL) {
                            strcpy(last_colstr, x->CurrentColor);
                            ThreeDimBuffer_writeps(x, (*z2 + *z1) / 2, lt, lw, 0.0, 1, last_colstr, text);
                        }
                    }
                }
            }
        }

        // Allow a gap before next axis
        *OutputWidth += EPS_AXES_SEPARATION * M_TO_PS;
    }
}

//! cubeEdgeDistance - Measure how far an edge of the cube which bounds a 3D plot lies from the origin of the plot,
//! in the plane of the page
//! \param [in] xpos1 - The x positions of one end of each edge
//! \param [in] ypos1 - The y positions of one end of each edge
//! \param [in] xpos2 - The x positions of the other end of each edge
//! \param [in] ypos2 - The y positions of the other end of each edge
//! \param [in] A - The number of the edge to measure
//! \param [in] origin_x - The x position of the origin of the plot
//! \param [in] origin_y - The y position of the origin of the plot
//! \return Twice the distance of the midpoint of the edge from the origin

static double cubeEdgeDistance(const double *xpos1, const double *ypos1, const double *xpos2, const double *ypos2,
                               const int A, const double origin_x, const double origin_y) {
    return hypot(xpos1[A] + xpos2[A] - 2 * origin_x, ypos1[A] + ypos2[A] - 2 * origin_y);
}

//! swapDoubles - Swap two elements of an array of doubles
//! \param [in,out] array - The array
//! \param [in] A - The index of the first element
//! \param [in] B - The index of the second element

static void swapDoubles(double *array, const int A, const int B) {
    const double tmp = array[A];
    array[A] = array[B];
    array[B] = tmp;
}

//! swapChars - Swap two elements of an array of unsigned chars
//! \param [in,out] array - The array
//! \param [in] A - The index of the first element
//! \param [in] B - The index of the second element

static void swapChars(unsigned char *array, const int A, const int B) {
    const unsigned char tmp = array[A];
    array[A] = array[B];
    array[B] = tmp;
}

//! eps_plot_axespaint - Draw all of the axes of a graph
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] origin_x - The x coordinate of the bottom-left corner of the graph, in postscript points
//! \param [in] origin_y - The y coordinate of the bottom-left corner of the graph, in postscript points
//! \param [in] width - The width of the graph, in postscript points
//! \param [in] height - The height of the graph, in postscript points
//! \param [in] zdepth - The depth of the graph, in postscript points, for 3D plots
//! \param [in] pass - For 3D plots, zero to draw the axes at the back of the plot (before the data), or one to draw
//! eps_plot_setAxesStyle - Set the color and line width for painting axes and color scales, which are drawn in the
//! color set with "set axescolor"
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [out] ww - The style of the axes, for use when painting them

void eps_plot_setAxesStyle(EPSComm *x, withWords *ww) {
    const pplset_graph *s = &x->current->settings;
    ppl_withWordsZero(x->c, ww);
    if (s->AxesColor > 0) {
        ww->color = s->AxesColor;
        ww->USEcolor = 1;
    } else {
        ww->Col1234Space = s->AxesCol1234Space;
        ww->color1 = s->AxesColor1;
        ww->color2 = s->AxesColor2;
        ww->color3 = s->AxesColor3;
        ww->color4 = s->AxesColor4;
        ww->USEcolor1234 = 1;
    }
    ww->linewidth = EPS_AXES_LINEWIDTH;
    ww->USElinewidth = 1;
    ww->linetype = 1;
    ww->USElinetype = 1;
    eps_core_SetColor(x, ww, 1);
    if (eps_core_isVisible(x)) eps_core_SetLinewidth(x, EPS_AXES_LINEWIDTH * EPS_DEFAULT_LINEWIDTH, 1, 0.0);
}

//! those at the front (after the data). 2D plots draw all their axes in pass one.

void eps_plot_axespaint(EPSComm *x, const double origin_x, const double origin_y, const double width,
                        const double height, const double zdepth, const int pass) {
    int k;
    double xc, yc, zc, theta[3] = {M_PI / 2, 0, 0}; // Coordinates of centre of graph
    pplset_axis *axes;
    withWords ww;

    if ((pass == 0) && (!x->current->ThreeDim)) return; // 2D plots do not have back axes

    // Set color and line width for painting axes
    eps_plot_setAxesStyle(x, &ww);

    // Reset plot bounding box; we're about to make this sensible below by factoring in plot vertices
    if ((pass == 0) || (!x->current->ThreeDim)) {
        x->current->PlotLeftMargin = x->current->PlotRightMargin = origin_x;
        x->current->PlotTopMargin = x->current->PlotBottomMargin = origin_y;
    }

    // Fetch coordinates of centre of graph
    if (x->current->ThreeDim) {
        eps_plot_ThreeDimProject(0.5, 0.5, 0.5, &x->current->settings, origin_x, origin_y, width, height, zdepth, &xc,
                                 &yc, &zc);
    } else {
        xc = origin_x + 0.5 * width;
        yc = origin_y + 0.5 * height;
        zc = 0.0;
    }

    // Work out directions of each of the axes
    if (x->current->ThreeDim)
        // Find the direction on the page of each of the three axes of the cube
        for (int j = 0; j < 3; j++) {
            double x1, y1, z1, x2, y2, z2;
            eps_plot_ThreeDimProject(0, 0, 0, &x->current->settings, origin_x, origin_y, width, height, zdepth, &x1,
                                     &y1, &z1);
            eps_plot_ThreeDimProject((j == 0), (j == 1), (j == 2), &x->current->settings, origin_x, origin_y, width,
                                     height, zdepth, &x2, &y2, &z2);
            theta[j] = atan2(x2 - x1, y2 - y1);
            if (!gsl_finite(theta[j])) theta[j] = 0.0;
        }

    // Set CrossedAtZero flags
    for (int j = 0; j < 3; j++) {
        if (j == 0) axes = x->current->XAxes;
        else if (j == 1) axes = x->current->YAxes;
        else axes = x->current->ZAxes;

        // Note whether any axis crosses the others at zero, in which case ticks at zero on the first axes are treated
        // specially
        for (int i = 0; i < MAX_AXES; i++)
            // An active, visible axis set at zero
            if ((axes[i].FinalActive) && (!axes[i].invisible) && (axes[i].atzero)) {
                if (j != 0) x->current->XAxes[1].CrossedAtZero = 1;
                if (j != 1) x->current->YAxes[1].CrossedAtZero = 1;
                if (j != 2) x->current->ZAxes[1].CrossedAtZero = 1;
                break;
            }
    }

    // Loop over all axes to draw. First loop over x, y and z directions.
    for (int j = 0; j < 2 + (x->current->ThreeDim != 0); j++) {
        int Naxes[4], FirstAutoMirror[4];
        unsigned char side_a[4] = {0, 1, 0, 1}, side_b[4] = {0, 0, 1, 1};
        double xpos1[4], ypos1[4], zpos1[4], xpos2[4], ypos2[4], zpos2[4];
        const int NDIRS = x->current->ThreeDim ? 4 : 2;
        const double theta_a = theta[(j == 0) ? 1 : 0];
        double theta_b = theta[(j == 2) ? 1 : 2];

        // Set count of axes to zero
        Naxes[0] = Naxes[1] = Naxes[2] = Naxes[3] = 0;
        FirstAutoMirror[0] = FirstAutoMirror[1] = FirstAutoMirror[2] = FirstAutoMirror[3] = -1;
        if (!x->current->ThreeDim) theta_b = GSL_NAN;

        // Loop over vertices of graph factoring into bounding box and setting pos1 and pos2
        for (int xap = 0; xap <= 1; xap++)
            for (int yap = 0; yap <= 1; yap++)
                // Each corner of the graph, as projected onto the page
                for (int zap = 0; zap <= 1; zap++) {
                    double tmp_x, tmp_y, tmp_z;
                    int k = 0, l = 0;
                    // Project the corner onto the page
                    if (x->current->ThreeDim) {
                        eps_plot_ThreeDimProject(xap, yap, zap, &x->current->settings, origin_x, origin_y, width,
                                                 height, zdepth, &tmp_x, &tmp_y, &tmp_z);
                    } else {
                        tmp_x = origin_x + xap * width;
                        tmp_y = origin_y + yap * height;
                        tmp_z = 0.0;
                    }
                    if (j != 2) k = 2 * k + zap;
                    else l = zap;
                    if (j != 1) k = 2 * k + yap;
                    else l = yap;
                    if (j != 0) k = 2 * k + xap;
                    else l = xap;
                    // Each edge parallel to the axis joins a pair of corners; number the edges by the coordinates of
                    // their other directions
                    if (l == 0) {
                        xpos1[k] = tmp_x;
                        ypos1[k] = tmp_y;
                        zpos1[k] = tmp_z;
                    } else {
                        xpos2[k] = tmp_x;
                        ypos2[k] = tmp_y;
                        zpos2[k] = tmp_z;
                    }
                    eps_core_PlotBoundingBox(x, tmp_x, tmp_y, 0.0, 0);
                }

        // Swap edges of cube around to put edges at (x,y)-extremes first
        if (x->current->ThreeDim) {
            // Bubble sort the four edges into order of decreasing distance from the origin
            static const int swaps[5][2] = {{2, 3}, {1, 2}, {0, 1}, {2, 3}, {1, 2}};
            // Sort the four edges into order of decreasing distance from the centre
            for (int sw = 0; sw < 5; sw++) {
                const int A = swaps[sw][0], B = swaps[sw][1];
                if (cubeEdgeDistance(xpos1, ypos1, xpos2, ypos2, A, origin_x, origin_y) <
                    0.9999999 * cubeEdgeDistance(xpos1, ypos1, xpos2, ypos2, B, origin_x, origin_y)) {
                    swapDoubles(xpos1, A, B);
                    swapDoubles(ypos1, A, B);
                    swapDoubles(zpos1, A, B);
                    swapDoubles(xpos2, A, B);
                    swapDoubles(ypos2, A, B);
                    swapDoubles(zpos2, A, B);
                    swapChars(side_a, A, B);
                    swapChars(side_b, A, B);
                }
            }
        }

        // Fetch relevant list of axes
        if (j == 0) axes = x->current->XAxes;
        else if (j == 1) axes = x->current->YAxes;
        else axes = x->current->ZAxes;

        // Loop over all axes in this direction and draw any which are active and visible
        for (int i = 0; i < MAX_AXES; i++)
            // Draw each active, visible axis
            if ((axes[i].FinalActive) && (!axes[i].invisible)) {
                // 2D Gnomonic axes
                if (x->current->settings.projection == SW_PROJ_GNOM) {
                }

                // Flat axes
                else {
                    // Axes at zero are drawn through the zero of each monotonic section of the perpendicular axes;
                    // others along the edges of the graph
                    if (axes[i].atzero) {
                        const pplset_axis *PerpAxisX =
                                &(x->current->XAxes[1]); // Put axis at zero of perpendicular x1-, y1- and z1-axes
                        const pplset_axis *PerpAxisY = &(x->current->YAxes[1]);
                        const pplset_axis *PerpAxisZ = &(x->current->ZAxes[1]);
                        unsigned char LastX, LastY, LastZ, DoneX, DoneY, DoneZ;
                        int xrn, yrn, zrn;
                        double pos[3], xpos1, ypos1, zpos1, xpos2, ypos2, zpos2, dummy;

                        // Each section of the perpendicular x axis
                        for (DoneX = LastX = 0, xrn = 0; !LastX; xrn++) {
                            pos[0] = 0.5;
                            LastX = ((j == 0) || (!PerpAxisX->FinalActive) || (xrn > PerpAxisX->AxisValueTurnings));
                            if ((j != 0) && (PerpAxisX->FinalActive))
                                pos[0] = pplaxis_GetPosition(0.0, PerpAxisX, xrn, 0);
                            if ((!gsl_finite(pos[0])) && (!DoneX) && LastX) pos[0] = 0.5;
                            if (!gsl_finite(pos[0])) continue;

                            // Each section of the perpendicular y axis
                            for (DoneY = LastY = 0, yrn = 0; !LastY; yrn++) {
                                pos[1] = 0.5;
                                LastY = ((j == 1) || (!PerpAxisY->FinalActive) || (yrn > PerpAxisY->AxisValueTurnings));
                                if ((j != 1) && (PerpAxisY->FinalActive))
                                    pos[1] = pplaxis_GetPosition(0.0, PerpAxisY, yrn, 0);
                                if ((!gsl_finite(pos[1])) && (!DoneY) && LastY) pos[1] = 0.5;
                                if (!gsl_finite(pos[1])) continue;

                                // Each section of the perpendicular z axis
                                for (DoneZ = LastZ = 0, zrn = 0; !LastZ; zrn++) {
                                    pos[2] = 0.5;
                                    LastZ = ((j == 2) || (!x->current->ThreeDim) || (!PerpAxisZ->FinalActive) ||
                                             (zrn > PerpAxisZ->AxisValueTurnings));
                                    if ((j != 2) && (PerpAxisZ->FinalActive))
                                        pos[2] = pplaxis_GetPosition(0.0, PerpAxisZ, zrn, 0);
                                    if ((!gsl_finite(pos[2])) && (!DoneZ) && LastZ) pos[2] = 0.5;
                                    if (!gsl_finite(pos[2])) continue;

                                    // The start of the axis
                                    pos[j] = 0.0;
                                    // Project it onto the page
                                    if (x->current->ThreeDim) {
                                        eps_plot_ThreeDimProject(pos[0], pos[1], pos[2], &x->current->settings,
                                                                 origin_x, origin_y, width, height, zdepth, &xpos1,
                                                                 &ypos1, &zpos1);
                                    } else {
                                        xpos1 = origin_x + pos[0] * width;
                                        ypos1 = origin_y + pos[1] * height;
                                    }
                                    pos[j] = 1.0;
                                    // Project the end of the axis onto the page
                                    if (x->current->ThreeDim) {
                                        eps_plot_ThreeDimProject(pos[0], pos[1], pos[2], &x->current->settings,
                                                                 origin_x, origin_y, width, height, zdepth, &xpos2,
                                                                 &ypos2, &zpos2);
                                    } else {
                                        xpos2 = origin_x + pos[0] * width;
                                        ypos2 = origin_y + pos[1] * height;
                                    }

                                    // Draw the axis, behind the data in three dimensions or in front of it in two
                                    if ((pass == 0) && (x->current->ThreeDim)) {
                                        const double b =
                                                atan2((xpos2 + xpos1) / 2 - xc, (ypos2 + ypos1) / 2 - yc) - theta[j];
                                        const unsigned char Lr = sin(b) < 0;
                                        eps_plot_axispaint(
                                                x, &ww, axes + i, j, GSL_NAN, Lr, xpos1, ypos1, &zpos1, xpos2, ypos2,
                                                &zpos2, theta_a + M_PI * ((j == 0) ? (pos[1] <= 0.5) : (pos[0] <= 0.5)),
                                                theta_b + M_PI * ((j == 2) ? (pos[1] <= 0.5) : (pos[2] <= 0.5)), &dummy,
                                                1);
                                    } else if ((pass == 1) && (!x->current->ThreeDim)) {
                                        eps_plot_axispaint(x, &ww, axes + i, j, pos[j == 0],
                                                           (j != 0) ^ (pos[j == 0] > 0.999), xpos1, ypos1, NULL, xpos2,
                                                           ypos2, NULL, theta_a, theta_b, &dummy, 1);
                                    }
                                }
                            }
                        } // Loop over xrn, yrn and zrn
                    } else // axis is notatzero... i.e. it is either at top or bottom of graph
                    {
                        if (axes[i].MirrorType == SW_AXISMIRROR_AUTO)
                            for (k = 0; k < 4; k++)
                                if ((k != axes[i].topbottom) && (FirstAutoMirror[k] < 0)) FirstAutoMirror[k] = i;

                        // Draw the axis on its own side of the graph, and on the others if it is mirrored
                        for (k = 0; k < NDIRS; k++)
                            if ((k == axes[i].topbottom) || (axes[i].MirrorType == SW_AXISMIRROR_MIRROR) ||
                                (axes[i].MirrorType == SW_AXISMIRROR_FULLMIRROR))
                                // In three dimensions, draw back axes on the first pass and front axes on the second;
                                // afterwards, move the side's position outwards past the axis
                                if ((!x->current->ThreeDim) || (((zpos1[k] + zpos2[k]) > 0.0) == (pass == 0))) {
                                    const double b =
                                            atan2((xpos2[k] + xpos1[k]) / 2 - xc, (ypos2[k] + ypos1[k]) / 2 - yc) -
                                            theta[j];
                                    const unsigned char Lr = sin(b) < 0;
                                    const unsigned char label = ((k == axes[i].topbottom) ||
                                                                 (axes[i].MirrorType == SW_AXISMIRROR_FULLMIRROR));
                                    double IncGap;
                                    eps_plot_axispaint(x, &ww, axes + i, j, GSL_NAN, Lr, xpos1[k], ypos1[k], NULL,
                                                       xpos2[k], ypos2[k], NULL, theta_a + (side_a[k] ? 0 : M_PI),
                                                       theta_b + (side_b[k] ? 0 : M_PI), &IncGap, label);
                                    const double ang = theta[j] + M_PI / 2 * ((Lr == 0) * 2 - 1);
                                    xpos1[k] += IncGap * sin(ang);
                                    ypos1[k] += IncGap * cos(ang);
                                    xpos2[k] += IncGap * sin(ang);
                                    ypos2[k] += IncGap * cos(ang);
                                    Naxes[k]++;
                                }
                    }
                }
            }

        // If there is no axis on any side, but was an auto-mirrored axis on another side, mirror it now
        for (k = 0; k < NDIRS; k++)
            if ((Naxes[k] == 0) && (FirstAutoMirror[k] >= 0))
                // In three dimensions, draw back axes on the first pass and front axes on the second; afterwards, move
                // the side's position outwards past the axis
                if ((!x->current->ThreeDim) || (((zpos1[k] + zpos2[k]) > 0.0) == (pass == 0))) {
                    const double b = atan2((xpos2[k] + xpos1[k]) / 2 - xc, (ypos2[k] + ypos1[k]) / 2 - yc) - theta[j];
                    const unsigned char Lr = sin(b) < 0;
                    double IncGap;
                    eps_plot_axispaint(x, &ww, axes + FirstAutoMirror[k], j, GSL_NAN, Lr, xpos1[k], ypos1[k], NULL,
                                       xpos2[k], ypos2[k], NULL, theta_a + (side_a[k] ? 0 : M_PI),
                                       theta_b + (side_b[k] ? 0 : M_PI), &IncGap, 0);
                    const double ang = theta[j] + M_PI / 2 * ((Lr == 0) * 2 - 1);
                    xpos1[k] += IncGap * sin(ang);
                    ypos1[k] += IncGap * cos(ang);
                    xpos2[k] += IncGap * sin(ang);
                    ypos2[k] += IncGap * cos(ang);
                    Naxes[k]++;
                }
    }
    return;
}
