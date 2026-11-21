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

#include "settings/settings.h"
#include "settings/settingTypes.h"

//! eps_plot_axis_GetPosition - Return the fractional position along an axis at which a value should be plotted. This is
//! a wrapper for pplaxis_GetPosition().
//! \param [in] xin - The value to locate on the axis
//! \param [in] xa - The axis, or NULL to return <xin> unchanged
//! \param [in] xrn - For axes whose values are linearly interpolated from a non-monotonic function, the number of the
//! monotonic region of the axis in which to look for <xin>
//! \param [in] AllowOffBounds - If true, values outside the range of the axis return positions outside the range 0-1;
//! otherwise they return NaN
//! \return The position along the axis, in the range 0 to 1, or NaN if the value cannot be placed on the axis

double eps_plot_axis_GetPosition(const double xin, pplset_axis *xa, const int xrn, const unsigned char AllowOffBounds) {
    // With no axis, the value is already a fractional position
    int imin, imax, i;
    if (xa == NULL) return xin;
    if (xa->AxisLinearInterpolation != NULL) // Axis is linearly interpolated
    {
        imin = xa->AxisTurnings[xrn];
        imax = xa->AxisTurnings[xrn + 1];
        // An axis whose mapping is tabulated: find the interval of the table containing the value, within the monotonic
        // section xrn, and interpolate within it
        for (i = imin; i < imax; i++) {
            if (((xa->AxisLinearInterpolation[i] < xin) && (xa->AxisLinearInterpolation[i + 1] >= xin)) ||
                ((xa->AxisLinearInterpolation[i] > xin) && (xa->AxisLinearInterpolation[i + 1] <= xin)))
                return (i +
                        (xin - xa->AxisLinearInterpolation[i]) /
                                (xa->AxisLinearInterpolation[i + 1] - xa->AxisLinearInterpolation[i])) /
                        (AXISLINEARINTERPOLATION_NPOINTS - 1);
        }
        return GSL_NAN;
    }
    // Values outside the axis's range have no position, unless positions off the ends are allowed
    if (!AllowOffBounds) {
        // The axis may run in either direction
        if (xa->MaxFinal > xa->MinFinal) {
            if ((xin < xa->MinFinal) || (xin > xa->MaxFinal)) return GSL_NAN;
        } else {
            if ((xin > xa->MinFinal) || (xin < xa->MaxFinal)) return GSL_NAN;
        }
    }
    if ((xa->LogFinal == SW_BOOL_TRUE) && (xin <= 0)) return GSL_NAN;
    if (xa->LogFinal != SW_BOOL_TRUE) return (xin - xa->MinFinal) / (xa->MaxFinal - xa->MinFinal); // Either linear...
    else return (log(xin) - log(xa->MinFinal)) / (log(xa->MaxFinal) - log(xa->MinFinal)); // ... or logarithmic
}

//! eps_plot_axis_InvGetPosition - Return the value of an axis at a fractional position along it. This is a wrapper for
//! pplaxis_InvGetPosition().
//! \param [in] xin - The position along the axis, in the range 0 to 1
//! \param [in] xa - The axis
//! \return The value of the axis at position <xin>

double eps_plot_axis_InvGetPosition(const double xin, pplset_axis *xa) {
    if (xa->AxisLinearInterpolation != NULL) // Axis is linearly interpolated
    {
        const int i = floor(xin * (AXISLINEARINTERPOLATION_NPOINTS - 1));
        const double x = xin * (AXISLINEARINTERPOLATION_NPOINTS - 1) - i;
        if (i >= AXISLINEARINTERPOLATION_NPOINTS - 1)
            return xa->AxisLinearInterpolation[AXISLINEARINTERPOLATION_NPOINTS - 1];
        if (i < 0) return xa->AxisLinearInterpolation[0];
        return xa->AxisLinearInterpolation[i] * (1 - x) + xa->AxisLinearInterpolation[i + 1] * x;
    }
    if (xa->LogFinal != SW_BOOL_TRUE) return xa->MinFinal + xin * (xa->MaxFinal - xa->MinFinal); // Either linear...
    else return xa->MinFinal * pow(xa->MaxFinal / xa->MinFinal, xin); // ... or logarithmic
}

//! eps_plot_axis_InRange - Test whether a value lies within the range of an axis
//! \param [in] xa - The axis
//! \param [in] xin - The value to test
//! \return One if the value lies within the range of the axis, zero otherwise

int eps_plot_axis_InRange(pplset_axis *xa, const double xin) {
    // Variables for the range of the axis
    int xrn, swapI, xminset = 0, xmaxset = 0;
    double xmin = 0, xmax = 0, swapD;

    // An axis whose mapping is tabulated: the value is in range if it lies on any monotonic section of the table
    if (xa->AxisLinearInterpolation != NULL) {
        for (xrn = 0; xrn <= xa->AxisValueTurnings; xrn++)
            if (gsl_finite(eps_plot_axis_GetPosition(xin, xa, xrn, 0))) return 1;
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
        swapI = xminset;
        xminset = xmaxset;
        xmaxset = swapI;
        swapD = xmin;
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
    double x, y, z, x2, y2, z2, x3, y3, z3;

    // Position relative to the centre of the cube
    x = width * (xap - 0.5);
    y = height * (yap - 0.5);
    z = zdepth * (zap - 0.5);

    // Rotate about the vertical axis by the xy view angle
    x2 = x * cos(sg->XYview.real) + y * sin(sg->XYview.real);
    y2 = -x * sin(sg->XYview.real) + y * cos(sg->XYview.real);
    z2 = z;

    // Tilt about the horizontal axis by the yz view angle; the page's vertical is z3, and y3 is the depth
    x3 = x2;
    y3 = y2 * cos(sg->YZview.real) - z2 * sin(sg->YZview.real);
    z3 = y2 * sin(sg->YZview.real) + z2 * cos(sg->YZview.real);

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
    *xap = eps_plot_axis_GetPosition(xin, xa, xrn, 1);
    *yap = eps_plot_axis_GetPosition(yin, ya, yrn, 1);
    *zap = 0.5;
    if (ThreeDim) *zap = eps_plot_axis_GetPosition(zin, za, zrn, 1);

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
