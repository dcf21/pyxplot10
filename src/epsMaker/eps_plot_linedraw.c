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

// Draws lines through series of data points on graphs, used by plot styles such as lines and by function plots. Each
// point is converted to a position on the page; the segment from the previous point is clipped to the faces of the
// graph, and the visible part is passed to the depth-sorting buffer in eps_plot_threedimbuff.c, which writes it as
// postscript immediately in two-dimensional plots. Points with non-finite positions break the line.

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

//! lineClip - A line segment being clipped to the faces of a graph, and pointers to where to store the points at which
//! it crosses them. The first crossing found is written to element 0 of each array, and the second to element 1.
typedef struct lineClip {
    double x1, y1, z1; // The first end of the line segment, in postscript points
    double x2, y2, z2; // The second end of the line segment, in postscript points
    double *cx[2], *cy[2], *cz[2]; // Set to the positions of the crossing points
    unsigned char *face[2]; // Set to the faces of the graph crossed at the crossing points
    double *AxisPos[2]; // Set to the fractional positions along those faces of the crossing points
    int *NCrossings; // Incremented for each crossing point found
} lineClip;

//! clipAgainstFace - Find whether a line segment crosses one face of a graph, and if so, record the crossing point.
//! The face is the plane where the axis-fractional coordinate <a> equals <pos>, within the square where the other two
//! coordinates <b> and <c> lie between 0 and 1.
//! \param [in,out] lc - The line segment, and where to store the crossing point
//! \param [in] a1 - The coordinate normal to the face, at the first end of the segment, as a fraction along its axis
//! \param [in] a2 - The coordinate normal to the face, at the second end of the segment
//! \param [in] b1 - The first coordinate in the plane of the face, at the first end of the segment
//! \param [in] b2 - The first coordinate in the plane of the face, at the second end of the segment
//! \param [in] c1 - The second coordinate in the plane of the face, at the first end of the segment
//! \param [in] c2 - The second coordinate in the plane of the face, at the second end of the segment
//! \param [in] pos - The value of <a> at the face: 0 or 1
//! \param [in] sgn - 1 if the inside of the graph lies in the direction of increasing <a> from the face, or -1
//! \param [in] face - The FACE_* constant to record for the face

static void clipAgainstFace(lineClip *lc, const double a1, const double a2, const double b1, const double b2,
                            const double c1, const double c2, const double pos, const double sgn,
                            const unsigned char face) {
    double fr, bCross, cCross;
    int n;
    if (a2 == a1) return; // The segment is parallel to the face
    fr = (pos - a1) / (a2 - a1); // Fractional position along the segment where it meets the plane of the face
    if (!((fr >= 0) && (fr <= 1))) return; // Written this way so that NaN is rejected
    bCross = b1 + (b2 - b1) * fr;
    cCross = c1 + (c2 - c1) * fr;
    if (!((bCross >= 0.0) && (bCross <= 1.0) && (cCross >= 0.0) && (cCross <= 1.0))) return;
    // A segment entering the graph through this face records the crossing as its first; one leaving, as its second
    n = ((a1 * sgn) < (a2 * sgn)) ? 0 : 1;
    *lc->cx[n] = lc->x1 + (lc->x2 - lc->x1) * fr;
    *lc->cy[n] = lc->y1 + (lc->y2 - lc->y1) * fr;
    *lc->cz[n] = lc->z1 + (lc->z2 - lc->z1) * fr;
    *lc->face[n] = face;
    *lc->AxisPos[n] = bCross;
    (*lc->NCrossings)++;
}

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
    lineClip lc = {
            x1,        y1, z1, x2, y2, z2, {cx1, cx2}, {cy1, cy2}, {cz1, cz2}, {face1, face2}, {AxisPos1, AxisPos2},
            NCrossings};

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

    clipAgainstFace(&lc, zap1, zap2, xap1, xap2, yap1, yap2, 0.0, 1.0, 0); // front
    clipAgainstFace(&lc, zap1, zap2, xap1, xap2, yap1, yap2, 1.0, -1.0, 0); // back
    clipAgainstFace(&lc, xap1, xap2, yap1, yap2, zap1, zap2, 0.0, 1.0, FACE_LEFT); // left
    clipAgainstFace(&lc, xap1, xap2, yap1, yap2, zap1, zap2, 1.0, -1.0, FACE_RIGHT); // right
    clipAgainstFace(&lc, yap1, yap2, xap1, xap2, zap1, zap2, 0.0, 1.0, FACE_BOTTOM); // bottom
    clipAgainstFace(&lc, yap1, yap2, xap1, xap2, zap1, zap2, 1.0, -1.0, FACE_TOP); // top
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

    // Find where the point lies on the page, and as fractions of the axes
    eps_plot_GetPosition(&xpos, &ypos, &depth, &xap, &yap, &zap, &theta_x, &theta_y, &theta_z, ld->ThreeDim, x, y, z,
                         ld->xa, ld->ya, ld->za, ld->xrn, ld->yrn, ld->zrn, ld->sg, ld->origin_x, ld->origin_y,
                         ld->width, ld->height, ld->zdepth, 1);

    // A point which cannot be placed breaks the line
    if ((!gsl_finite(xpos)) || (!gsl_finite(ypos)) || (!gsl_finite(depth))) {
        LineDraw_PenUp(X, ld);
        return;
    }

    // Apply any offset to the position on the page
    xpos += x_offset * M_TO_PS;
    ypos += y_offset * M_TO_PS;
    depth += z_offset * M_TO_PS;

    // Apply the same offset to the fractional positions along the axes
    xap += x_offset / ld->width;
    yap += y_offset / ld->height;
    zap += z_offset / ld->zdepth;

    // The first point of a line only records where the line starts
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

    // Clip the segment from the previous point to the faces of the graph
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
        // Remember where this run of the line started
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
        // Remember where this run of the line started
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
