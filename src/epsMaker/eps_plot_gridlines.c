// eps_plot_gridlines.c
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

#define _PPL_EPS_PLOT_GRIDLINES 1

#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include <gsl/gsl_math.h>

#include "settings/settings.h"
#include "settings/withWords_fns.h"
#include "settings/settingTypes.h"

#include "epsMaker/canvasDraw.h"
#include "epsMaker/eps_comm.h"
#include "epsMaker/eps_core.h"
#include "epsMaker/eps_plot_canvas.h"
#include "epsMaker/eps_plot_gridlines.h"
#include "epsMaker/eps_settings.h"

//! setGridColor - Set the colour and line width with which to draw major or minor grid lines
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [out] ww - Workspace into which to write the style of the grid lines
//! \param [in] major - True for major grid lines; false for minor grid lines

static void setGridColor(EPSComm *x, withWords *ww, const int major) {
    const pplset_graph *s = &x->current->settings;
    ppl_withWordsZero(x->c, ww);
    if ((major ? s->GridMajColor : s->GridMinColor) > 0) {
        ww->color = major ? s->GridMajColor : s->GridMinColor;
        ww->USEcolor = 1;
    } else {
        ww->Col1234Space = major ? s->GridMajCol1234Space : s->GridMinCol1234Space;
        ww->color1 = major ? s->GridMajColor1 : s->GridMinColor1;
        ww->color2 = major ? s->GridMajColor2 : s->GridMinColor2;
        ww->color3 = major ? s->GridMajColor3 : s->GridMinColor3;
        ww->color4 = major ? s->GridMajColor4 : s->GridMinColor4;
        ww->USEcolor1234 = 1;
    }
    eps_core_SetColor(x, ww, 1);
    if (eps_core_isVisible(x))
        eps_core_SetLinewidth(x, (major ? EPS_GRID_MAJLINEWIDTH : EPS_GRID_MINLINEWIDTH) * EPS_DEFAULT_LINEWIDTH, 1,
                              0.0);
}

//! eps_plot_gridlines - Draw the major and minor grid lines of a graph
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] origin_x - The x coordinate of the bottom-left corner of the graph, in postscript points
//! \param [in] origin_y - The y coordinate of the bottom-left corner of the graph, in postscript points
//! \param [in] width - The width of the graph, in postscript points
//! \param [in] height - The height of the graph, in postscript points
//! \param [in] zdepth - The depth of the graph, in postscript points, for 3D plots

void eps_plot_gridlines(EPSComm *x, double origin_x, double origin_y, double width, double height,
                        const double zdepth) {
    int i, j, k, l, m;
    double left, right, adepth, bdepth, cdepth, ddepth, x1, y1, x2, y2, ap_back[3];
    pplset_axis *axes;
    unsigned char *GridLines;
    withWords ww;

    if (x->current->settings.grid != SW_ONOFF_ON) return; // Grid lines are off

    // Work out which faces of 3D cube are at back
    if (x->current->ThreeDim) {
        eps_plot_ThreeDimProject(0.5, 0.5, 0.5, &x->current->settings, origin_x, origin_y, width, height, zdepth, &x1,
                                 &y1, &adepth);
        eps_plot_ThreeDimProject(1.0, 0.5, 0.5, &x->current->settings, origin_x, origin_y, width, height, zdepth, &x1,
                                 &y1, &bdepth);
        eps_plot_ThreeDimProject(0.5, 1.0, 0.5, &x->current->settings, origin_x, origin_y, width, height, zdepth, &x1,
                                 &y1, &cdepth);
        eps_plot_ThreeDimProject(0.5, 0.5, 1.0, &x->current->settings, origin_x, origin_y, width, height, zdepth, &x1,
                                 &y1, &ddepth);
        ap_back[0] = (adepth < bdepth);
        ap_back[1] = (adepth < cdepth);
        ap_back[2] = (adepth < ddepth);
    }

    for (j = 0; j < 3; j++) // Loop over x,y,z axes
    {
        if ((j == 2) && (!x->current->ThreeDim)) continue;

        if (j == 2) {
            axes = x->current->ZAxes;
            GridLines = x->current->settings.GridAxisZ;
            left = right = GSL_NAN;
        } else if (j == 1) {
            axes = x->current->YAxes;
            GridLines = x->current->settings.GridAxisY;
            left = origin_y;
            right = origin_y + height;
        } else {
            axes = x->current->XAxes;
            GridLines = x->current->settings.GridAxisX;
            left = origin_x;
            right = origin_x + width;
        }

        for (i = 0; i < MAX_AXES; i++) // Loop over all axes in a particular direction
            if ((axes[i].FinalActive) && (!axes[i].invisible) && (GridLines[i])) {
                for (k = 0; k < 2; k++) // Minor ticks. Then major ticks
                {
                    double *TLP;
                    char **TLS;
                    if (k != 0) {
                        TLP = axes[i].TickListPositions;
                        TLS = axes[i].TickListStrings;
                        setGridColor(x, &ww, 1);
                    } // Major ticks
                    else {
                        TLP = axes[i].MTickListPositions;
                        TLS = axes[i].MTickListStrings;
                        setGridColor(x, &ww, 0);
                    } // Minor ticks
                    if (TLP != NULL) {
                        for (l = 0; TLS[l] != NULL; l++) // Loop over all ticks along this axis
                        {
                            // Three-dimensional axes
                            if (x->current->ThreeDim) {
                                for (m = 0; m < 2; m++) {
                                    double ap[3];
                                    int n, o;
                                    ap[j] = TLP[l];
                                    n = (j != 0) ? 0 : 1;
                                    o = (j != 2) ? 2 : 1;
                                    if (m) {
                                        const int t = n;
                                        n = o;
                                        o = t;
                                    }
                                    ap[n] = ap_back[n];
                                    ap[o] = 0.0;
                                    eps_plot_ThreeDimProject(ap[0], ap[1], ap[2], &x->current->settings, origin_x,
                                                             origin_y, width, height, zdepth, &x1, &y1, &adepth);
                                    ap[o] = 1.0;
                                    eps_plot_ThreeDimProject(ap[0], ap[1], ap[2], &x->current->settings, origin_x,
                                                             origin_y, width, height, zdepth, &x2, &y2, &adepth);
                                    fprintf(x->epsbuffer, "newpath %.2f %.2f moveto %.2f %.2f lineto stroke\n", x1, y1,
                                            x2, y2);
                                    eps_core_BoundingBox(x, x1, y1, EPS_AXES_LINEWIDTH * EPS_DEFAULT_LINEWIDTH);
                                    eps_core_BoundingBox(x, x2, y2, EPS_AXES_LINEWIDTH * EPS_DEFAULT_LINEWIDTH);
                                }
                            }

                            // 2D Gnomonic axes
                            else if (x->current->settings.projection == SW_PROJ_GNOM) {
                            }

                            // 2D flat axes
                            else {
                                double lrpos = left + (right - left) * TLP[l]; // left--right position of tick
                                if (j == 1) {
                                    fprintf(x->epsbuffer, "newpath %.2f %.2f moveto %.2f %.2f lineto stroke\n",
                                            origin_x, lrpos, origin_x + width, lrpos);
                                    eps_core_BoundingBox(x, origin_x, lrpos,
                                                         EPS_AXES_LINEWIDTH * EPS_DEFAULT_LINEWIDTH);
                                    eps_core_BoundingBox(x, origin_x + width, lrpos,
                                                         EPS_AXES_LINEWIDTH * EPS_DEFAULT_LINEWIDTH);
                                } else {
                                    fprintf(x->epsbuffer, "newpath %.2f %.2f moveto %.2f %.2f lineto stroke\n", lrpos,
                                            origin_y, lrpos, origin_y + height);
                                    eps_core_BoundingBox(x, lrpos, origin_y,
                                                         EPS_AXES_LINEWIDTH * EPS_DEFAULT_LINEWIDTH);
                                    eps_core_BoundingBox(x, lrpos, origin_y + height,
                                                         EPS_AXES_LINEWIDTH * EPS_DEFAULT_LINEWIDTH);
                                }
                            }
                        }
                    }
                }
            }
    }

    return;
}
