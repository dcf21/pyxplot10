// eps_plot_linedraw.c
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

#define _PPL_EPS_PLOT_LINEDRAW_C 1

#include <stdlib.h>
#include <stdio.h>

#include <gsl/gsl_math.h>

#include "coreUtils/memAlloc.h"
#include "epsMaker/eps_comm.h"
#include "epsMaker/eps_plot_canvas.h"
#include "epsMaker/eps_plot_linedraw.h"
#include "epsMaker/eps_plot_threedimbuff.h"
#include "epsMaker/eps_settings.h"

//! LineDraw_FindCrossingPoints - Find where a line segment between two points crosses the edges of a graph, so that it
//! can be clipped
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] x1 - The x coordinate of the first point, in postscript points
//! \param [in] y1 - The y coordinate of the first point, in postscript points
//! \param [in] z1 - The depth of the first point
//! \param [in] xap1 - The fractional position of the first point along the x axis
//! \param [in] yap1 - The fractional position of the first point along the y axis
//! \param [in] zap1 - The fractional position of the first point along the z axis
//! \param [in] x2 - The x coordinate of the second point, in postscript points
//! \param [in] y2 - The y coordinate of the second point, in postscript points
//! \param [in] z2 - The depth of the second point
//! \param [in] xap2 - The fractional position of the second point along the x axis
//! \param [in] yap2 - The fractional position of the second point along the y axis
//! \param [in] zap2 - The fractional position of the second point along the z axis
//! \param [out] Inside1 - Set to one if the first point lies inside the graph
//! \param [out] Inside2 - Set to one if the second point lies inside the graph
//! \param [out] cx1 - Set to the x coordinate of the first crossing point
//! \param [out] cy1 - Set to the y coordinate of the first crossing point
//! \param [out] cz1 - Set to the depth of the first crossing point
//! \param [out] cx2 - Set to the x coordinate of the second crossing point
//! \param [out] cy2 - Set to the y coordinate of the second crossing point
//! \param [out] cz2 - Set to the depth of the second crossing point
//! \param [out] face1 - Set to the face of the graph crossed at the first crossing point
//! \param [out] AxisPos1 - Set to the fractional position along that face of the first crossing point
//! \param [out] face2 - Set to the face of the graph crossed at the second crossing point
//! \param [out] AxisPos2 - Set to the fractional position along that face of the second crossing point
//! \param [out] NCrossings - Set to the number of crossing points found

void LineDraw_FindCrossingPoints(const EPSComm *x, const double x1, const double y1, const double z1, const double xap1,
                                 const double yap1, const double zap1, const double x2, const double y2,
                                 const double z2, const double xap2, const double yap2, const double zap2, int *Inside1,
                                 int *Inside2, double *cx1, double *cy1, double *cz1, double *cx2, double *cy2,
                                 double *cz2, unsigned char *face1, double *AxisPos1, unsigned char *face2,
                                 double *AxisPos2, int *NCrossings) {
    double fr, cx, cy, cz;

    *Inside1 = ((xap1 >= 0.0) && (xap1 <= 1.0) && (yap1 >= 0.0) && (yap1 <= 1.0) && (zap1 >= 0.0) && (zap1 <= 1.0));
    *Inside2 = ((xap2 >= 0.0) && (xap2 <= 1.0) && (yap2 >= 0.0) && (yap2 <= 1.0) && (zap2 >= 0.0) && (zap2 <= 1.0));
    *cx1 = x1;
    *cy1 = y1;
    *cz1 = z1; // If either point is inside canvas, set clip-region crossings to be the points themselves
    *cx2 = x2;
    *cy2 = y2;
    *cz2 = z2;
    *NCrossings = 0;
    *face1 = *face2 = 0;
    *AxisPos1 = *AxisPos2 = -1;
    if ((*Inside1) && (*Inside2)) return; // If both points are inside canvas, don't need to find clip-region crossings

#define DOCLIP(XAP1, XAP2, YAP1, YAP2, ZAP1, ZAP2, POS, SGN, FACE) \
    if (XAP2 != XAP1) { \
        fr = (POS - XAP1) / (XAP2 - XAP1); \
        if ((fr >= 0) && (fr <= 1)) { \
            double yleft, zleft; \
            yleft = YAP1 + (YAP2 - YAP1) * fr; \
            zleft = ZAP1 + (ZAP2 - ZAP1) * fr; \
            if ((yleft >= 0.0) && (yleft <= 1.0) && (zleft >= 0.0) && (zleft <= 1.0)) { \
                cx = x1 + (x2 - x1) * fr; \
                cy = y1 + (y2 - y1) * fr; \
                cz = z1 + (z2 - z1) * fr; \
                if ((XAP1 * SGN) < (XAP2 * SGN)) { \
                    *cx1 = cx; \
                    *cy1 = cy; \
                    *cz1 = cz; \
                    *face1 = FACE; \
                    *AxisPos1 = yleft; \
                } else { \
                    *cx2 = cx; \
                    *cy2 = cy; \
                    *cz2 = cz; \
                    *face2 = FACE; \
                    *AxisPos2 = yleft; \
                } \
                (*NCrossings)++; \
            } \
        } \
    }

    DOCLIP(zap1, zap2, xap1, xap2, yap1, yap2, 0.0, 1.0, 0); // front
    DOCLIP(zap1, zap2, xap1, xap2, yap1, yap2, 1.0, -1.0, 0); // back
    DOCLIP(xap1, xap2, yap1, yap2, zap1, zap2, 0.0, 1.0, FACE_LEFT); // left
    DOCLIP(xap1, xap2, yap1, yap2, zap1, zap2, 1.0, -1.0, FACE_RIGHT); // right
    DOCLIP(yap1, yap2, xap1, xap2, zap1, zap2, 0.0, 1.0, FACE_BOTTOM); // bottom
    DOCLIP(yap1, yap2, xap1, xap2, zap1, zap2, 1.0, -1.0, FACE_TOP); // top
    return;
}

//! LineDraw_Init - Begin drawing a line through a series of points in data coordinates, which is clipped to the edges
//! of the graph
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] xa - The x axis
//! \param [in] ya - The y axis
//! \param [in] za - The z axis
//! \param [in] xrn - The monotonic region of the x axis in which points lie
//! \param [in] yrn - The monotonic region of the y axis in which points lie
//! \param [in] zrn - The monotonic region of the z axis in which points lie
//! \param [in] sg - The graph settings
//! \param [in] ThreeDim - Flag indicating whether the graph is three-dimensional
//! \param [in] origin_x - The x coordinate of the bottom-left corner of the graph, in postscript points
//! \param [in] origin_y - The y coordinate of the bottom-left corner of the graph, in postscript points
//! \param [in] width - The width of the graph, in postscript points
//! \param [in] height - The height of the graph, in postscript points
//! \param [in] zdepth - The depth of the graph, in postscript points, for 3D plots
//! \return A handle for the line, allocated in the current ppl_memAlloc context, or NULL on failure

LineDrawHandle *LineDraw_Init(EPSComm *x, pplset_axis *xa, pplset_axis *ya, pplset_axis *za, const int xrn,
                              const int yrn, const int zrn, pplset_graph *sg, const unsigned char ThreeDim,
                              const double origin_x, const double origin_y, const double width, const double height,
                              const double zdepth) {
    LineDrawHandle *output;
    output = (LineDrawHandle *) ppl_memAlloc(sizeof(LineDrawHandle));
    if (output == NULL) return NULL;
    output->x = x;
    output->xa = xa;
    output->ya = ya;
    output->za = za;
    output->xrn = xrn;
    output->yrn = yrn;
    output->zrn = zrn;
    output->sg = sg;
    output->ThreeDim = ThreeDim;
    output->origin_x = origin_x;
    output->origin_y = origin_y;
    output->width = width;
    output->height = height;
    output->zdepth = zdepth;
    output->x0set = 0;
    output->x1set = 0;
    return output;
}

//! LineDraw_Point - Add a point to a line, drawing the segment from the previous point (clipped to the edges of the
//! graph) if the pen is down
//! \param [in] X - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] ld - The handle of the line
//! \param [in] x - The x coordinate of the point, in data coordinates
//! \param [in] y - The y coordinate of the point, in data coordinates
//! \param [in] z - The z coordinate of the point, in data coordinates
//! \param [in] x_offset - Offset to add to the x coordinate, in data coordinates
//! \param [in] y_offset - Offset to add to the y coordinate, in data coordinates
//! \param [in] z_offset - Offset to add to the z coordinate, in data coordinates
//! \param [in] x_perpoffset - Offset to add to the x coordinate perpendicular to the line, in postscript points
//! \param [in] y_perpoffset - Offset to add to the y coordinate perpendicular to the line, in postscript points
//! \param [in] z_perpoffset - Offset to add to the depth perpendicular to the line, in postscript points
//! \param [in] linetype - The line type (dash pattern)
//! \param [in] linewidth - The line width, as a multiple of the default line width
//! \param [in] colstr - The postscript command to set the color of the line

void LineDraw_Point(EPSComm *X, LineDrawHandle *ld, const double x, const double y, const double z,
                    const double x_offset, const double y_offset, const double z_offset, const double x_perpoffset,
                    const double y_perpoffset, const double z_perpoffset, const int linetype, const double linewidth,
                    char *colstr) {
    unsigned char f1, f2;
    double ap1, ap2;
    int Inside1, Inside2, NCrossings;
    double xpos, ypos, depth, xap, yap, zap;
    double theta_x, theta_y, theta_z;
    double cx1, cy1, cz1, cx2, cy2, cz2;

    eps_plot_GetPosition(&xpos, &ypos, &depth, &xap, &yap, &zap, &theta_x, &theta_y, &theta_z, ld->ThreeDim, x, y, z,
                         ld->xa, ld->ya, ld->za, ld->xrn, ld->yrn, ld->zrn, ld->sg, ld->origin_x, ld->origin_y,
                         ld->width, ld->height, ld->zdepth, 1);

    if ((!gsl_finite(xpos)) || (!gsl_finite(ypos)) || (!gsl_finite(depth))) {
        LineDraw_PenUp(X, ld);
        return;
    }

    xpos += x_offset * M_TO_PS;
    ypos += y_offset * M_TO_PS;
    depth += z_offset * M_TO_PS;

    xap += x_offset / ld->width;
    yap += y_offset / ld->height;
    zap += z_offset / ld->zdepth;

    if (!ld->x1set) {
        ld->x1set = 1;
        ld->x1 = xpos;
        ld->y1 = ypos;
        ld->z1 = depth;
        ld->xap1 = xap;
        ld->yap1 = yap;
        ld->zap1 = zap;
        ld->xpo1 = x_perpoffset;
        ld->ypo1 = y_perpoffset;
        ld->zpo1 = z_perpoffset;
        return;
    }

    LineDraw_FindCrossingPoints(X, ld->x1, ld->y1, ld->z1, ld->xap1, ld->yap1, ld->zap1, xpos, ypos, depth, xap, yap,
                                zap, &Inside1, &Inside2, &cx1, &cy1, &cz1, &cx2, &cy2, &cz2, &f1, &ap1, &f2, &ap2,
                                &NCrossings);

    // Add in perpendicular offsets
    cx1 = cx1 + (ld->xpo1 * cos(theta_x) + ld->ypo1 * cos(theta_y) + ld->zpo1 * cos(theta_z)) * M_TO_PS;
    cy1 = cy1 + (ld->xpo1 * sin(theta_x) + ld->ypo1 * sin(theta_y) + ld->zpo1 * sin(theta_z)) * M_TO_PS;
    cx2 = cx2 + (x_perpoffset * cos(theta_x) + y_perpoffset * cos(theta_y) + z_perpoffset * cos(theta_z)) * M_TO_PS;
    cy2 = cy2 + (x_perpoffset * sin(theta_x) + y_perpoffset * sin(theta_y) + z_perpoffset * sin(theta_z)) * M_TO_PS;

    // Test whether end-points of line are within clip region
    if ((!Inside1) && (!Inside2)) // Neither point on line segment is inside clip-region
    {
        if (NCrossings >= 2) // Check that we haven't crossed clip region during the course of line segment
            ThreeDimBuffer_linesegment(ld->x, depth, linetype, linewidth, colstr, cx1, cy1, cx1, cy1, cx2, cy2, 1, 0,
                                       0.0);
        ld->x0set = 0;
    } else if ((!Inside1) && (Inside2)) // We have just entered clip region; previous point was outside
    {
        ThreeDimBuffer_linesegment(ld->x, z, linetype, linewidth, colstr, cx1, cy1, cx1, cy1, cx2, cy2, 1, 0, 0.0);
        if ((!ld->x0set) || (cx1 != cx2) || (cy1 != cy2)) {
            ld->x0 = cx1;
            ld->y0 = cy1;
        }
        ld->x0set = 1;
    } else if ((Inside1) && (!Inside2)) // We have just left clip region; previous point was inside
    {
        if (ld->x0set)
            ThreeDimBuffer_linesegment(ld->x, depth, linetype, linewidth, colstr, ld->x0, ld->y0, cx1, cy1, cx2, cy2, 0,
                                       0, 0.0);
        else
            ThreeDimBuffer_linesegment(ld->x, depth, linetype, linewidth, colstr, cx1, cy1, cx1, cy1, cx2, cy2, 1, 0,
                                       0.0);
        ld->x0set = 0;
    } else // if (( Inside1) && ( Inside2)) // We are within the clip region
    {
        if (ld->x0set)
            ThreeDimBuffer_linesegment(ld->x, depth, linetype, linewidth, colstr, ld->x0, ld->y0, cx1, cy1, cx2, cy2, 0,
                                       0, 0.0);
        else
            ThreeDimBuffer_linesegment(ld->x, depth, linetype, linewidth, colstr, cx1, cy1, cx1, cy1, cx2, cy2, 1, 0,
                                       0.0);
        if ((!ld->x0set) || (cx1 != cx2) || (cy1 != cy2)) {
            ld->x0 = cx1;
            ld->y0 = cy1;
        }
        ld->x0set = 1;
    }
    ld->x1 = xpos;
    ld->y1 = ypos;
    ld->z1 = depth;
    ld->xap1 = xap;
    ld->yap1 = yap;
    ld->zap1 = zap;
    ld->xpo1 = x_perpoffset;
    ld->ypo1 = y_perpoffset;
    ld->zpo1 = z_perpoffset;
    return;
}

//! LineDraw_PenUp - Lift the pen, so that the next point added to a line starts a new segment
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] ld - The handle of the line

void LineDraw_PenUp(const EPSComm *x, LineDrawHandle *ld) {
    ThreeDimBuffer_linepenup(ld->x);
    ld->x0set = 0;
    ld->x1set = 0;
    return;
}
