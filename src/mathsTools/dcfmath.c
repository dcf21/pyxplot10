// dcfmath.c
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

// Small mathematical helper functions which the standard C maths library lacks: minima and maxima, signs, unit
// conversion between degrees and radians, linear and logarithmic rasters of sample points, sorting, and the greatest
// common divisor. They are used throughout Pyxplot's calculation and plotting code.

#define _PPL_DCFMATH_C 1

#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <math.h>

#include "dcfmath.h"

double ppl_machineEpsilon;

//! ppl_makeMachineEpsilon - Compute the machine epsilon for double-precision arithmetic, and store it in the global
//! variable <ppl_machineEpsilon>

void ppl_makeMachineEpsilon() {
    ppl_machineEpsilon = 1.0;
    // Halve epsilon until adding half of it to one makes no difference
    do { ppl_machineEpsilon /= 2.0; } while ((1.0 + (ppl_machineEpsilon / 2.0)) != 1.0);
    return;
}

//! ppl_max - Return the larger of two numbers
//! \param [in] x - First number
//! \param [in] y - Second number
//! \return The larger of <x> and <y>

double ppl_max(const double x, const double y) {
    if (x > y) return x;
    return y;
}

//! ppl_min - Return the smaller of two numbers
//! \param [in] x - First number
//! \param [in] y - Second number
//! \return The smaller of <x> and <y>

double ppl_min(const double x, const double y) {
    if (x < y) return x;
    return y;
}

//! ppl_max3 - Return the largest of three numbers
//! \param [in] x - First number
//! \param [in] y - Second number
//! \param [in] z - Third number
//! \return The largest of <x>, <y> and <z>

double ppl_max3(const double x, const double y, const double z) {
    const double o = x > y ? x : y;
    if (o > z) return o;
    return z;
}

//! ppl_min3 - Return the smallest of three numbers
//! \param [in] x - First number
//! \param [in] y - Second number
//! \param [in] z - Third number
//! \return The smallest of <x>, <y> and <z>

double ppl_min3(const double x, const double y, const double z) {
    const double o = x < y ? x : y;
    if (o < z) return o;
    return z;
}

//! ppl_sgn - Return the sign of a number
//! \param [in] x - The input number
//! \return -1 if <x> is negative, 1 if it is positive, or 0 if it is zero

int ppl_sgn(const double x) {
    if (x == 0) return 0;
    if (x < 0) return -1;
    return 1;
}

//! ppl_linRaster - Fill an array with linearly-spaced values between two limits, inclusive
//! \param [out] out - The array to fill, which must have room for at least max(Nsteps, 2) values
//! \param [in] min - The first value
//! \param [in] max - The last value
//! \param [in] Nsteps - The number of values to generate. Values below 2 are treated as 2.

void ppl_linRaster(double *out, const double min, const double max, int Nsteps) {
    int i;
    if (Nsteps < 2) Nsteps = 2; // Avoid division by zero
    for (i = 0; i < Nsteps; i++) out[i] = min + (max - min) * ((double) i / (double) (Nsteps - 1));
    return;
}

//! ppl_logRaster - Fill an array with logarithmically-spaced values between two limits, inclusive
//! \param [out] out - The array to fill, which must have room for at least max(Nsteps, 2) values
//! \param [in] min - The first value. Values below 1e-200 are treated as 1e-200.
//! \param [in] max - The last value. Values below 1e-200 are treated as 1e-200.
//! \param [in] Nsteps - The number of values to generate. Values below 2 are treated as 2.

void ppl_logRaster(double *out, double min, double max, int Nsteps) {
    int i;
    if (min < 1e-200) min = 1e-200; // Avoid log of negative numbers or zero
    if (max < 1e-200) max = 1e-200;
    if (Nsteps < 2) Nsteps = 2; // Avoid division by zero
    for (i = 0; i < Nsteps; i++) out[i] = min * pow(max / min, (double) i / (double) (Nsteps - 1));
    return;
}

//! ppl_degs - Convert an angle from radians to degrees
//! \param [in] rad - Angle in radians
//! \return Angle in degrees

double ppl_degs(const double rad) {
    return rad * 180 / M_PI;
}

//! ppl_rads - Convert an angle from degrees to radians
//! \param [in] degrees - Angle in degrees
//! \return Angle in radians

double ppl_rads(const double degrees) {
    return degrees * M_PI / 180;
}

//! ppl_dblSort - Comparison function for sorting arrays of doubles into ascending order with qsort()
//! \param [in] a - Pointer to the first double
//! \param [in] b - Pointer to the second double
//! \return -1, 0 or 1 if the first double is less than, equal to or greater than the second

int ppl_dblSort(const void *a, const void *b) {
    const double *da = (const double *) a;
    const double *db = (const double *) b;
    if (*da < *db) return -1;
    if (*da > *db) return 1;
    return 0;
}

//! ppl_gcd - Compute the greatest common divisor of two non-negative integers, using Euclid's algorithm
//! \param [in] a - First integer
//! \param [in] b - Second integer
//! \return The greatest common divisor of <a> and <b>

int ppl_gcd(int a, int b) {
    // Euclid's algorithm: replace (a, b) with (b, a mod b) until b is zero
    while (b > 0) {
        const int tmp = a % b;
        a = b;
        b = tmp;
    }
    return a;
}
