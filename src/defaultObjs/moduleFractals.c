// moduleFractals.c
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

// The fractals module, whose functions julia() and mandelbrot() count how many iterations of z -> z^2 + c a point takes
// to escape, for drawing images of the Julia and Mandelbrot sets with the colormap plot style.

#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <math.h>

#include <gsl/gsl_cdf.h>
#include <gsl/gsl_complex.h>
#include <gsl/gsl_complex_math.h>
#include <gsl/gsl_const_mksa.h>
#include <gsl/gsl_const_num.h>
#include <gsl/gsl_math.h>

#include "coreUtils/dict.h"

#include "settings/settings.h"

#include "stringTools/asciidouble.h"

#include "userspace/pplObj.h"
#include "userspace/pplObjFunc.h"
#include "userspace/unitsArithmetic.h"

#include "defaultObjs/moduleFractals.h"
#include "defaultObjs/defaultFuncs.h"
#include "defaultObjs/defaultFuncsChecks.h"

//! pplfunc_julia - Implement the function fractals.julia(z,cz,MaxIter) in the fractals module, which returns the number
//! of iterations required before the Julia set iterator diverges outside the circle |z'|<2
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_julia(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "julia(z,cz,MaxIter)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    if (ppl_funcCheckLong(&call, &in[2], "MaxIter", "function's third argument must be")) return;
    {
        double y = in[0].imag, x = in[0].real, x2;
        double cy = in[1].imag, cx = in[1].real;
        long MaxIter = (long) (in[2].real), iter;

        // Iterate z -> z^2 + c, with z = x + iy, until |z| exceeds 2 (when the point has escaped) or MaxIter is reached
        for (iter = 0; ((iter < MaxIter) && ((x * x + y * y) < 4)); iter++) {
            x2 = x * x - y * y + cx;
            y = 2 * x * y + cy;
            x = x2;
        }
        OUTPUT.real = (double) iter;
    }
    if (ppl_funcCheckOutput(&call)) return;
}

//! pplfunc_mandelbrot - Implement the function fractals.mandelbrot(z,MaxIter) in the fractals module, which returns the
//! number of iterations required before the Mandelbrot set iterator diverges outside the circle |z'|<2
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_mandelbrot(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "mandelbrot(z,MaxIter)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    if (ppl_funcCheckLong(&call, &in[1], "MaxIter", "function's second argument must be")) return;
    {
        double y = in[0].imag, x = in[0].real, x2;
        double cy = in[0].imag, cx = in[0].real;
        long MaxIter = (long) (in[1].real), iter;

        // Iterate z -> z^2 + c, with z = x + iy starting from zero, until |z| exceeds 2 or MaxIter is reached
        for (iter = 0; ((iter < MaxIter) && ((x * x + y * y) < 4)); iter++) {
            x2 = x * x - y * y + cx;
            y = 2 * x * y + cy;
            x = x2;
        }
        OUTPUT.real = (double) iter;
    }
    if (ppl_funcCheckOutput(&call)) return;
}
