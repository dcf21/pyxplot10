// moduleColor.c
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

// The colors module's functions wavelength() and spectrum(), which compute the color seen by the eye for monochromatic
// light of a given wavelength, or for a spectrum given as a function of wavelength. The light is weighted by the CIE
// color-matching functions to give XYZ color coordinates, which are converted to RGB. The named colors themselves are
// set up in defaultVars.c.

// Parts of this source file are modified from public domain code by John
// Walker, including corrections by Andrew J. S. Hamilton.

// See <http://www.fourmilab.ch/documents/specrend/>
// Source code at <http://www.fourmilab.ch/documents/specrend/specrend.c>

// Referenced from <http://stackoverflow.com/questions/1472514/convert-light-frequency-to-rgb>

// John Walker's homepage is at <http://www.fourmilab.ch/>

#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <math.h>
#include <string.h>

#include <gsl/gsl_const_mksa.h>
#include <gsl/gsl_math.h>

#include "expressions/expCompile.h"
#include "userspace/stack.h"
#include "expressions/expEval.h"
#include "expressions/fnCall.h"
#include "settings/settings.h"
#include "stringTools/asciidouble.h"
#include "userspace/context.h"
#include "userspace/garbageCollector.h"
#include "userspace/pplObj_fns.h"
#include "userspace/pplObjFunc.h"
#include "userspace/unitsArithmetic.h"

#include "defaultObjs/moduleColor.h"
#include "defaultObjs/defaultFuncs.h"
#include "defaultObjs/defaultFuncsChecks.h"

#define GAMMA_FUDGE 0.45

struct colorSystem {
    char *name; // Colour system name
    double xRed, yRed, // Red x, y
            xGreen, yGreen, // Green x, y
            xBlue, yBlue, // Blue x, y
            xWhite, yWhite, // White point x, y
            gamma; // Gamma correction for system
};

// White point chromaticities
#define IlluminantC   0.3101, 0.3162 // For NTSC television
#define IlluminantD65 0.3127, 0.3291 // For EBU and SMPTE
#define IlluminantE   0.33333333, 0.33333333 // CIE equal-energy illuminant

#define GAMMA_REC709 0 // Rec. 709

static struct colorSystem
        // Name                  xRed    yRed    xGreen  yGreen  xBlue  yBlue    White point        Gamma
        //  NTSCsystem  =  { "NTSC",               0.67,   0.33,   0.21,   0.71,   0.14,   0.08,   IlluminantC,    GAMMA_REC709 },
        //  EBUsystem   =  { "EBU (PAL/SECAM)",    0.64,   0.33,   0.29,   0.60,   0.15,   0.06,   IlluminantD65,  GAMMA_REC709 },
        SMPTEsystem = {"SMPTE", 0.630, 0.340, 0.310, 0.595, 0.155, 0.070, IlluminantD65, GAMMA_REC709};
//  HDTVsystem  =  { "HDTV",               0.670,  0.330,  0.210,  0.710,  0.150,  0.060,  IlluminantD65,  GAMMA_REC709 },
//  CIEsystem   =  { "CIE",                0.7355, 0.2645, 0.2658, 0.7243, 0.1669, 0.0085, IlluminantE,    GAMMA_REC709 },
//  Rec709system = { "CIE REC 709",        0.64,   0.33,   0.30,   0.60,   0.15,   0.06,   IlluminantD65,  GAMMA_REC709 };

#define CIE_WLEN_MIN      380
#define CIE_WLEN_STEPSIZE 5
#define N_CIE_STEPS       81

static double cie_color_match[N_CIE_STEPS][3] = {
        {0.0014, 0.0000, 0.0065}, {0.0022, 0.0001, 0.0105}, {0.0042, 0.0001, 0.0201}, {0.0076, 0.0002, 0.0362},
        {0.0143, 0.0004, 0.0679}, {0.0232, 0.0006, 0.1102}, {0.0435, 0.0012, 0.2074}, {0.0776, 0.0022, 0.3713},
        {0.1344, 0.0040, 0.6456}, {0.2148, 0.0073, 1.0391}, {0.2839, 0.0116, 1.3856}, {0.3285, 0.0168, 1.6230},
        {0.3483, 0.0230, 1.7471}, {0.3481, 0.0298, 1.7826}, {0.3362, 0.0380, 1.7721}, {0.3187, 0.0480, 1.7441},
        {0.2908, 0.0600, 1.6692}, {0.2511, 0.0739, 1.5281}, {0.1954, 0.0910, 1.2876}, {0.1421, 0.1126, 1.0419},
        {0.0956, 0.1390, 0.8130}, {0.0580, 0.1693, 0.6162}, {0.0320, 0.2080, 0.4652}, {0.0147, 0.2586, 0.3533},
        {0.0049, 0.3230, 0.2720}, {0.0024, 0.4073, 0.2123}, {0.0093, 0.5030, 0.1582}, {0.0291, 0.6082, 0.1117},
        {0.0633, 0.7100, 0.0782}, {0.1096, 0.7932, 0.0573}, {0.1655, 0.8620, 0.0422}, {0.2257, 0.9149, 0.0298},
        {0.2904, 0.9540, 0.0203}, {0.3597, 0.9803, 0.0134}, {0.4334, 0.9950, 0.0087}, {0.5121, 1.0000, 0.0057},
        {0.5945, 0.9950, 0.0039}, {0.6784, 0.9786, 0.0027}, {0.7621, 0.9520, 0.0021}, {0.8425, 0.9154, 0.0018},
        {0.9163, 0.8700, 0.0017}, {0.9786, 0.8163, 0.0014}, {1.0263, 0.7570, 0.0011}, {1.0567, 0.6949, 0.0010},
        {1.0622, 0.6310, 0.0008}, {1.0456, 0.5668, 0.0006}, {1.0026, 0.5030, 0.0003}, {0.9384, 0.4412, 0.0002},
        {0.8544, 0.3810, 0.0002}, {0.7514, 0.3210, 0.0001}, {0.6424, 0.2650, 0.0000}, {0.5419, 0.2170, 0.0000},
        {0.4479, 0.1750, 0.0000}, {0.3608, 0.1382, 0.0000}, {0.2835, 0.1070, 0.0000}, {0.2187, 0.0816, 0.0000},
        {0.1649, 0.0610, 0.0000}, {0.1212, 0.0446, 0.0000}, {0.0874, 0.0320, 0.0000}, {0.0636, 0.0232, 0.0000},
        {0.0468, 0.0170, 0.0000}, {0.0329, 0.0119, 0.0000}, {0.0227, 0.0082, 0.0000}, {0.0158, 0.0057, 0.0000},
        {0.0114, 0.0041, 0.0000}, {0.0081, 0.0029, 0.0000}, {0.0058, 0.0021, 0.0000}, {0.0041, 0.0015, 0.0000},
        {0.0029, 0.0010, 0.0000}, {0.0020, 0.0007, 0.0000}, {0.0014, 0.0005, 0.0000}, {0.0010, 0.0004, 0.0000},
        {0.0007, 0.0002, 0.0000}, {0.0005, 0.0002, 0.0000}, {0.0003, 0.0001, 0.0000}, {0.0002, 0.0001, 0.0000},
        {0.0002, 0.0001, 0.0000}, {0.0001, 0.0000, 0.0000}, {0.0001, 0.0000, 0.0000}, {0.0001, 0.0000, 0.0000},
        {0.0000, 0.0000, 0.0000}};


//! xyz_to_rgb - Convert a color from CIE xyz chromaticity coordinates into RGB components, in a specified RGB color
//! system. The RGB components may be negative if the color lies outside the system's gamut.
//! \param [in] cs - The RGB color system, defined by the chromaticities of its primaries and white point
//! \param [in] xc - The CIE x chromaticity coordinate
//! \param [in] yc - The CIE y chromaticity coordinate
//! \param [in] zc - The CIE z chromaticity coordinate
//! \param [out] r - Red component
//! \param [out] g - Green component
//! \param [out] b - Blue component

static void xyz_to_rgb(const struct colorSystem *cs, const double xc, const double yc, const double zc, double *r,
                       double *g, double *b) {

    // The chromaticity coordinates of the primaries of the color system, with z = 1 - x - y
    const double xr = cs->xRed;
    const double yr = cs->yRed;
    const double zr = 1 - (xr + yr);
    const double xg = cs->xGreen;
    const double yg = cs->yGreen;
    const double zg = 1 - (xg + yg);
    const double xb = cs->xBlue;
    const double yb = cs->yBlue;
    const double zb = 1 - (xb + yb);

    // The chromaticity of the white point
    const double xw = cs->xWhite;
    const double yw = cs->yWhite;
    const double zw = 1 - (xw + yw);

    // xyz -> rgb matrix, before scaling to white
    double rx = (yg * zb) - (yb * zg);
    double ry = (xb * zg) - (xg * zb);
    double rz = (xg * yb) - (xb * yg);
    double gx = (yb * zr) - (yr * zb);
    double gy = (xr * zb) - (xb * zr);
    double gz = (xb * yr) - (xr * yb);
    double bx = (yr * zg) - (yg * zr);
    double by = (xg * zr) - (xr * zg);
    double bz = (xr * yg) - (xg * yr);

    // White scaling factors. Dividing by yw scales the white luminance to unity, as conventional.
    const double rw = ((rx * xw) + (ry * yw) + (rz * zw)) / yw;
    const double gw = ((gx * xw) + (gy * yw) + (gz * zw)) / yw;
    const double bw = ((bx * xw) + (by * yw) + (bz * zw)) / yw;

    // xyz -> rgb matrix, correctly scaled to white
    rx = rx / rw;
    ry = ry / rw;
    rz = rz / rw;
    gx = gx / gw;
    gy = gy / gw;
    gz = gz / gw;
    bx = bx / bw;
    by = by / bw;
    bz = bz / bw;

    // rgb of the desired point
    *r = (rx * xc) + (ry * yc) + (rz * zc);
    *g = (gx * xc) + (gy * yc) + (gz * zc);
    *b = (bx * xc) + (by * yc) + (bz * zc);
    return;
}

//! inside_gamut - Test whether a set of RGB components lies inside the RGB gamut, i.e. whether none is negative
//! \param [in] r - Red component
//! \param [in] g - Green component
//! \param [in] b - Blue component
//! \return One if the color is inside the gamut, zero otherwise

static int inside_gamut(const double r, const double g, const double b) {
    return (r >= 0) && (g >= 0) && (b >= 0);
}

//! constrain_rgb - Bring a color which lies outside the RGB gamut inside it, by adding just enough white to make
//! none of its components negative
//! \param [in|out] r - Red component
//! \param [in|out] g - Green component
//! \param [in|out] b - Blue component
//! \return One if the color was modified, zero if it was already within the gamut

static int constrain_rgb(double *r, double *g, double *b) {

    // Amount of white needed is w = - min(0, *r, *g, *b)
    double w = (0 < *r) ? 0 : *r;
    w = (w < *g) ? w : *g;
    w = (w < *b) ? w : *b;
    w = -w;

    // Add just enough white to make r, g, b all positive
    if (w > 0) {
        double n = (*r + *g + *b) / (*r + *g + *b + w);
        if (!gsl_finite(n)) n = 1;
        *r = (*r + w) * n;
        *g = (*g + w) * n;
        *b = (*b + w) * n;
        return 1; // Colour modified to fit RGB gamut
    }
    return 0; // Colour within RGB gamut
}

//! pplfunc_colWavelen - Implement the function colors.wavelength(lambda,norm) in the colors module, which returns a
//! color representation of monochromatic light at wavelength lambda, normalised to brightness norm
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! xyzToColor - Convert a color from XYZ coordinates into an RGB color object, after fudging the brightness of the
//! spectrum, bringing the color within the gamut which can be displayed, and scaling it by a normalisation
//! \param [in] x - The X coordinate of the color
//! \param [in] y - The Y coordinate of the color
//! \param [in] z - The Z coordinate of the color
//! \param [in] norm - The normalisation by which to scale the brightness of the color
//! \param [out] out - The color object

static void xyzToColor(double x, double y, double z, const double norm, pplObj *out) {
    double r, g, b;
    // Fudge to brightness of spectrum
    double xyz = gsl_hypot3(x, y, z);
    if (xyz > 1) xyz = 1;
    if (xyz < 0) xyz = 0;
    const double gamma = pow(xyz, GAMMA_FUDGE) / xyz;
    x *= gamma;
    y *= gamma;
    z *= gamma;

    // Convert to RGB, bring the color within the gamut which can be displayed, and scale it by the normalisation
    xyz_to_rgb(&SMPTEsystem, x, y, z, &r, &g, &b);
    if (!inside_gamut(r, g, b)) constrain_rgb(&r, &g, &b);
    double *const rgb[3] = {&r, &g, &b};
    for (int i = 0; i < 3; i++) {
        *rgb[i] *= fabs(norm);
        if (*rgb[i] < 0) *rgb[i] = 0;
        if (*rgb[i] > 1) *rgb[i] = 1;
    }
    pplObjColor(out, 0, SW_COLSPACE_RGB, r, g, b, 0);
}

//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_colWavelen(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    // p0 is the position of the wavelength within the table of CIE color-matching functions; i and j are the table
    // entries either side of it, with weights wi and wj
    char *FunctionDescription = "wavelength(lambda,norm)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};

    // The wavelength must have units of length, and the normalisation must be dimensionless
    if (ppl_funcCheckUnit(&call, &in[0], "first", "a wavelength", UNIT_LENGTH, 1)) return;
    // Report a normalisation with units
    if (!in[1].dimensionless) {
        ppl_funcError(status, errType, errText, ERR_UNIT,
                "The %s function requires a dimensionless renormalisation constant as its second argument. Supplied value has dimensions of <%s>.",
                FunctionDescription, ppl_printUnit(c, &in[1], NULL, NULL, 0, 1, 0));
        return;
    }
    pplObjColor(&OUTPUT, 0, SW_COLSPACE_RGB, 0, 0, 0, 0);

    // Wavelengths outside the table (in nm) give black
    const double norm = in[1].real;
    const double p0 = ((in[0].real * 1e9) - CIE_WLEN_MIN) / CIE_WLEN_STEPSIZE;
    // Outside the table
    if ((!gsl_finite(p0)) || (p0 < 0) || (p0 >= N_CIE_STEPS - 1)) {
        pplObjColor(&OUTPUT, 0, SW_COLSPACE_RGB, 0, 0, 0, 0);
        return;
    } // Black
    int i = (int) floor(p0);
    if (i < 0) i = 0;
    if (i > N_CIE_STEPS - 2) i = N_CIE_STEPS - 2;
    const int j = i + 1;
    double wi = (j - p0);
    if (wi < 0) wi = 0;
    if (wi > 1) wi = 1;
    const double wj = 1 - wi;

    // Interpolate the color-matching functions at the wavelength, to get the XYZ coordinates
    const double x = cie_color_match[i][0] * wi + cie_color_match[j][0] * wj;
    const double y = cie_color_match[i][1] * wi + cie_color_match[j][1] * wj;
    const double z = cie_color_match[i][2] * wi + cie_color_match[j][2] * wj;

    xyzToColor(x, y, z, norm, &OUTPUT);
}


//! pplfunc_colSpectrum - Implement the function colors.spectrum(spec,norm) in the colors module, which returns a color
//! representation of the spectrum spec, normalised to brightness norm. spec should be a function that takes a single
//! input (wavelength) with units of length, and may return an output with arbitrary units
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_colSpectrum(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    // x, y and z accumulate the XYZ coordinates of the spectrum
    char *FunctionDescription = "spectrum(spec,norm)";
    const int stkLevelOld = c->stackPtr;
    double x = 0, y = 0, z = 0;

    // The first argument must be a function
    if (in[0].objType != PPLOBJ_FUNC) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The %s function requires a function object as its first argument. Supplied object is of type <%s>.",
                FunctionDescription, pplObjTypeNames[in[0].objType]);
        return;
    }
    pplFunc *fi = (pplFunc *) in[0].auxil;
    // Magic functions, such as diff_d, cannot be called in this way
    if ((fi == NULL) || (fi->functionType == PPL_FUNC_MAGIC)) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The %s function requires a function object as its first argument. Integration and differentiation operators are not suitable functions.",
                FunctionDescription);
        return;
    }
    // The normalisation must be dimensionless
    if (!in[1].dimensionless) {
        ppl_funcError(status, errType, errText, ERR_UNIT,
                "The %s function requires a dimensionless renormalisation constant as its second argument. Supplied value has dimensions of <%s>.",
                FunctionDescription, ppl_printUnit(c, &in[1], NULL, NULL, 0, 1, 0));
        return;
    }
    pplObjColor(&OUTPUT, 0, SW_COLSPACE_RGB, 0, 0, 0, 0);

    // The normalisation, which scales the brightness of the result
    const double norm = in[1].real;

    // Check there's enough space on the stack
    ppl_stackMustHave(c, 4);
    // Report a full stack
    if (c->stackFull) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "stack overflow in the colors.spectrum function.");
        return;
    }

    // Evaluate the spectrum at each wavelength in the table, and weight it by the color-matching functions
    for (int i = 0; i < N_CIE_STEPS; i++) {
        const double wlen = (CIE_WLEN_MIN + i * CIE_WLEN_STEPSIZE) * 1e-9;
        pplExpr dummy;

        // Dummy expression object with dummy line number information
        dummy.srcLineN = 0;
        dummy.srcId = 0;
        dummy.srcFname = "<dummy>";
        dummy.ascii = "<colors.spectrum() function call>";

        // Push the function to call, followed by its argument
        ppl_stackPushCopy(c, &in[0], 1);

        // Push wavelength
        pplObjNum(&c->stack[c->stackPtr], 0, wlen, 0);
        c->stack[c->stackPtr].dimensionless = 0;
        c->stack[c->stackPtr].exponent[UNIT_LENGTH] = 1;
        c->stack[c->stackPtr].refCount = 1;
        c->stackPtr++;

        // Call function
        c->errStat.errMsgExpr[0] = '\0';
        ppl_fnCall(c, &dummy, 0, 1, 1, 1);

        // Propagate error if function failed
        if (c->errStat.status) {
            ppl_funcError(status, errType, errText, ERR_TYPE,
                    "Error inside spectrum function supplied to the %s function: %s",
                    FunctionDescription, c->errStat.errMsgExpr);
            return;
        }

        // Return error if function didn't return a number
        if (c->stack[c->stackPtr - 1].objType != PPLOBJ_NUM) {
            ppl_funcError(status, errType, errText, ERR_TYPE,
                    "The %s function requires a spectrum function that returns a number. Supplied function returned an object of type <%s>.",
                    FunctionDescription, pplObjTypeNames[c->stack[c->stackPtr - 1].objType]);
            return;
        }

        // Get number back and clean stack
        double w = fabs(c->stack[c->stackPtr - 1].real) / N_CIE_STEPS;
        if (!gsl_finite(w)) w = 0;
        ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack

        // Add color to accumulators
        x += cie_color_match[i][0] * w;
        y += cie_color_match[i][1] * w;
        z += cie_color_match[i][2] * w;
    }

    xyzToColor(x, y, z, norm, &OUTPUT);
}
