// eps_plot_canvas.c
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

// Maps between values on a graph's axes and positions on the postscript page, in postscript points. A value is first
// converted to a fractional position along its axis (0 at one end, 1 at the other), allowing for logarithmic axes and
// for axes linked to others through a using expression, whose mapping is tabulated; the fractions are then placed on
// the page, in two dimensions or by projecting the three-dimensional cube of the graph onto the page.

#define _PPL_EPS_PLOT_CANVAS_C 1

#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include <gsl/gsl_math.h>

#include "settings/axes_fns.h"
#include "settings/settings.h"
#include "settings/settingTypes.h"

//! eps_plot_axis_InRange - Test whether a value lies within the range of an axis
//! \param [in] xa - The axis
//! \param [in] xin - The value to test
//! \return One if the value lies within the range of the axis, zero otherwise

int eps_plot_axis_InRange(pplset_axis *xa, const double xin) {
    // Variables for the range of the axis
    int xminset = 0, xmaxset = 0;
    double xmin = 0, xmax = 0;

    // An axis whose mapping is tabulated: the value is in range if it lies on any monotonic section of the table
    if (xa->AxisLinearInterpolation != NULL) {
        for (int xrn = 0; xrn <= xa->AxisValueTurnings; xrn++)
            if (gsl_finite(pplaxis_GetPosition(xin, xa, xrn, 0))) return 1;
        return 0;
    }

    // The minimum set by the user, if any
    if (xa->HardMinSet) {
        xminset = 1;
        xmin = xa->HardMin;
    }
    // The maximum set by the user, if any
    if (xa->HardMaxSet) {
        xmaxset = 1;
        xmax = xa->HardMax;
    }
    if (xa->HardAutoMinSet) { xminset = 0; }
    if (xa->HardAutoMaxSet) { xmaxset = 0; }

    // Reversed ranges are written with the larger value first
    if (xa->RangeReversed) {
        const int swapI = xminset;
        xminset = xmaxset;
        xmaxset = swapI;
        const double swapD = xmin;
        xmin = xmax;
        xmax = swapD;
    }

    // Test the value against the limits which are set
    if (xminset && xmaxset) return (((xin >= xmin) && (xin <= xmax)) || ((xin <= xmin) && (xin >= xmax)));
    if (xminset) return (xin > xmin);
    if (xmaxset) return (xin < xmax);
    return 1; // Axis range is not fixed
}

//! eps_plot_ThreeDimProject - Project a point in the unit cube of a 3D plot onto the page, using the viewing angles set
//! in the graph settings
//! \param [in] xap - The fractional position of the point along the x axis
//! \param [in] yap - The fractional position of the point along the y axis
//! \param [in] zap - The fractional position of the point along the z axis
//! \param [in] sg - The graph settings
//! \param [in] origin_x - The x coordinate of the bottom-left corner of the graph, in postscript points
//! \param [in] origin_y - The y coordinate of the bottom-left corner of the graph, in postscript points
//! \param [in] width - The width of the graph, in postscript points
//! \param [in] height - The height of the graph, in postscript points
//! \param [in] zdepth - The depth of the graph, in postscript points, for 3D plots
//! \param [out] xpos - Set to the x coordinate of the point on the page, in postscript points
//! \param [out] ypos - Set to the y coordinate of the point on the page, in postscript points
//! \param [out] depth - Set to the depth of the point, in postscript points, used to sort items into drawing order

void eps_plot_ThreeDimProject(const double xap, const double yap, const double zap, const pplset_graph *sg,
                              const double origin_x, const double origin_y, const double width, const double height,
                              const double zdepth, double *xpos, double *ypos, double *depth) {

    // Position relative to the centre of the cube
    const double x = width * (xap - 0.5);
    const double y = height * (yap - 0.5);
    const double z = zdepth * (zap - 0.5);

    // Rotate about the vertical axis by the xy view angle
    const double x2 = x * cos(sg->XYview.real) + y * sin(sg->XYview.real);
    const double y2 = -x * sin(sg->XYview.real) + y * cos(sg->XYview.real);
    const double z2 = z;

    // Tilt about the horizontal axis by the yz view angle; the page's vertical is z3, and y3 is the depth
    const double x3 = x2;
    const double y3 = y2 * cos(sg->YZview.real) - z2 * sin(sg->YZview.real);
    const double z3 = y2 * sin(sg->YZview.real) + z2 * cos(sg->YZview.real);

    *xpos = origin_x + x3;
    *ypos = origin_y + z3;
    *depth = y3;
    return;
}

//! eps_plot_GetPosition - Find the position on the page at which a data point should be plotted, and the directions of
//! the axes at that point
//! \param [out] xpos - Set to the x coordinate of the point on the page, in postscript points
//! \param [out] ypos - Set to the y coordinate of the point on the page, in postscript points
//! \param [out] depth - Set to the depth of the point, for 3D plots
//! \param [out] xap - Set to the fractional position of the point along the x axis
//! \param [out] yap - Set to the fractional position of the point along the y axis
//! \param [out] zap - Set to the fractional position of the point along the z axis
//! \param [out] theta_x - Set to the direction of the x axis at the point, in radians
//! \param [out] theta_y - Set to the direction of the y axis at the point, in radians
//! \param [out] theta_z - Set to the direction of the z axis at the point, in radians
//! \param [in] ThreeDim - Flag indicating whether the graph is three-dimensional
//! \param [in] xin - The x coordinate of the data point
//! \param [in] yin - The y coordinate of the data point
//! \param [in] zin - The z coordinate of the data point
//! \param [in] xa - The x axis
//! \param [in] ya - The y axis
//! \param [in] za - The z axis
//! \param [in] xrn - The monotonic region of the x axis in which to look for <xin>
//! \param [in] yrn - The monotonic region of the y axis in which to look for <yin>
//! \param [in] zrn - The monotonic region of the z axis in which to look for <zin>
//! \param [in] sg - The graph settings
//! \param [in] origin_x - The x coordinate of the bottom-left corner of the graph, in postscript points
//! \param [in] origin_y - The y coordinate of the bottom-left corner of the graph, in postscript points
//! \param [in] width - The width of the graph, in postscript points
//! \param [in] height - The height of the graph, in postscript points
//! \param [in] zdepth - The depth of the graph, in postscript points, for 3D plots
//! \param [in] AllowOffBounds - If true, points outside the range of the axes are given positions off the edge of the
//! graph; otherwise they return NaN

void eps_plot_GetPosition(double *xpos, double *ypos, double *depth, double *xap, double *yap, double *zap,
                          double *theta_x, double *theta_y, double *theta_z, const unsigned char ThreeDim,
                          const double xin, const double yin, const double zin, pplset_axis *xa, pplset_axis *ya,
                          pplset_axis *za, const int xrn, const int yrn, const int zrn, pplset_graph *sg,
                          const double origin_x, const double origin_y, const double width, const double height,
                          const double zdepth, const unsigned char AllowOffBounds) {
    // Convert (xin,yin,zin) to axis positions on the range of 0-1
    *xap = pplaxis_GetPosition(xin, xa, xrn, 1);
    *yap = pplaxis_GetPosition(yin, ya, yrn, 1);
    *zap = 0.5;
    if (ThreeDim) *zap = pplaxis_GetPosition(zin, za, zrn, 1);

    // Values which cannot be placed on an axis have no position
    if ((!gsl_finite(*xap)) || (!gsl_finite(*yap)) || (!gsl_finite(*zap))) {
        *xpos = *ypos = *depth = GSL_NAN;
        return;
    }

    // Apply non-flat projections to (xap,yap,zap)
    if (sg->projection == SW_PROJ_GNOM) {}

    // Crop axis positions to range 0-1
    if ((!AllowOffBounds) &&
        ((*xap < 0.0) || (*xap > 1.0) || (*yap < 0.0) || (*yap > 1.0) || (*zap < 0.0) || (*zap > 1.0))) {
        *xpos = *ypos = *depth = GSL_NAN;
        return;
    }

    // 3D plots
    if (ThreeDim) {
        eps_plot_ThreeDimProject(*xap, *yap, *zap, sg, origin_x, origin_y, width, height, zdepth, xpos, ypos, depth);
        if (theta_x != NULL) *theta_x = atan2(cos(sg->XYview.real), -sin(sg->XYview.real) * sin(sg->YZview.real));
        if (theta_y != NULL) *theta_y = atan2(sin(sg->XYview.real), cos(sg->XYview.real) * sin(sg->YZview.real));
        if (theta_z != NULL) *theta_z = atan2(0, cos(sg->YZview.real));

        // The directions are zero if undefined
        if ((theta_x != NULL) && (!gsl_finite(*theta_x))) *theta_x = 0.0;
        if ((theta_y != NULL) && (!gsl_finite(*theta_y))) *theta_y = 0.0;
        if ((theta_z != NULL) && (!gsl_finite(*theta_z))) *theta_z = 0.0;
    } else // 2D plots
    {
        *xpos = origin_x + width * (*xap);
        *ypos = origin_y + height * (*yap);
        *depth = 0.0;

        // In two dimensions, x points right, and y and z point up
        if (theta_x != NULL) *theta_x = M_PI / 2;
        if (theta_y != NULL) *theta_y = 0.0;
        if (theta_z != NULL) *theta_z = 0.0;
    }

    // Done
    return;
}
