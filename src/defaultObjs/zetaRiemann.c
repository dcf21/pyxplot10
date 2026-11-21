// zetaRiemann.c
//
// The code in this file is part of Pyxplot
// <http://www.pyxplot.org.uk>
//
// Copyright (C) 2006-2026 Dominic Ford <coders@pyxplot.org.uk>
//               2008-2013 Ross Church
//
//               2009-2010 Matthew Smith
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

// The Riemann zeta function for complex arguments, used by the zeta() function when its argument is complex (GSL only
// handles real arguments). It uses Peter Borwein's algorithm, the Laurent series near the pole at z=1, and the
// reflection formula for arguments with real part below one half. The implementation was provided courtesy of Matthew
// Smith.

#define DECIMAL_PLACES 18

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include <gsl/gsl_math.h>
#include <gsl/gsl_complex.h>
#include <gsl/gsl_complex_math.h>
#include <gsl/gsl_sf_result.h>
#include <gsl/gsl_sf_gamma.h>

#include "defaultObjs/zetaRiemann.h"
#include "stringTools/asciidouble.h"
#include "stringTools/strConstants.h"

//! riemann_zeta_complex__ - Evaluate the Riemann zeta function for complex arguments with real part of at least
//! one half, using Peter Borwein's algorithm (Algorithm 3 of his 2000 Canadian Math. Soc. Proc. paper), or a Laurent
//! series close to the pole at z=1
//! \param [in] x - The real part of the argument
//! \param [in] y - The imaginary part of the argument
//! \param [out] output - The value of the zeta function
//! \param [in] status - Unused
//! \param [in] errText - Unused

void riemann_zeta_complex__(double x, double y, gsl_complex *output, const int *status, const char *errText) {
    // a is the argument; b is a - 1; g is |a - 1|
    gsl_complex a, am, b, c, d, e;
    double g, h;
    int j, n, m, sign;

    a = gsl_complex_rect(x, y); /* the argument to zeta */
    b = gsl_complex_sub_real(a, 1);
    g = gsl_complex_abs(b);

    // Near the pole at z = 1, use the Laurent series about the pole, whose coefficients are the Stieltjes constants
    if (g < 0.01) {
        e = gsl_complex_inverse(b); /* build up the Laurent sum as we are near the pole */
        e = gsl_complex_add_real(e, 0.57721566490153286061);
        d = gsl_complex_mul_real(b, -0.07281584548367672486);
        e = gsl_complex_sub(e, d);
        c = gsl_complex_mul(b, b);
        d = gsl_complex_mul_real(c, -0.00484518159643615924);
        e = gsl_complex_add(e, d);
        c = gsl_complex_mul(c, b);
        d = gsl_complex_mul_real(c, 0.00034230573671722431);
        e = gsl_complex_sub(e, d);
        c = gsl_complex_mul(c, b);
        d = gsl_complex_mul_real(c, 0.00009689041939447083);
        e = gsl_complex_add(e, d);
        c = gsl_complex_mul(c, b);
        d = gsl_complex_mul_real(c, 0.00000661103181084219);
        e = gsl_complex_sub(e, d);
        c = gsl_complex_mul(c, b);
        d = gsl_complex_mul_real(c, -0.00000033162409087528);
        e = gsl_complex_add(e, d);
        *output = e;
        return;
    }

    // Use Peter Borwein's method.
    // This is Algorithm 3 from his 2000 Canadian Math. Soc. Proc. paper.
    n = (int) (2.1 + 1.2 * DECIMAL_PLACES + 0.76 * fabs(y) + 0.25 * log(1 + (y * y / (x * x))));
    m = n;
    am = gsl_complex_negative(a);
    e = GSL_COMPLEX_ZERO;
    sign = 1;
    g = 1.0;
    h = 0.0;

    // Accumulate the first sum of Borwein's method; h accumulates the binomial coefficients g, and the sum and
    // coefficients are halved whenever they grow large, to avoid overflow, with m counting the halvings left
    for (j = 0; j <= (n - 1); j++) {
        sign = -sign;
        d = gsl_complex_pow(gsl_complex_rect(((double) (2 * n - j)), 0.0), am);
        h += g;
        d = gsl_complex_mul_real(d, h);

        // The terms alternate in sign
        if (sign == 1) e = gsl_complex_add(e, d);
        else e = gsl_complex_sub(e, d);

        // The next binomial coefficient, (n choose j+1)
        g *= (double) (n - j);
        g /= (double) (j + 1);

        // Rescale to keep the numbers small
        while ((h > 0.5) && (m > 0)) {
            g /= 2.0;
            h /= 2.0;
            e = gsl_complex_div_real(e, 2.0);
            m--;
        }
    }

    // Add the second, simpler, alternating sum
    e = gsl_complex_add_real(e, 1.0);
    sign = 1;

    // Add each term of the second sum
    for (j = 1; j <= (n - 1); j++) {
        sign = -sign;
        d = gsl_complex_pow(gsl_complex_rect(((double) (j + 1)), 0.0), am);

        // The terms alternate in sign
        if (sign == 1) e = gsl_complex_add(e, d);
        else e = gsl_complex_sub(e, d);
    }

    // Divide by (1 - 2^(1-a)), which turns the alternating eta function into the zeta function
    d = gsl_complex_pow(gsl_complex_rect(2.0, 0.0), gsl_complex_negative(b));
    d = gsl_complex_add_real(gsl_complex_negative(d), 1.0);
    e = gsl_complex_div(e, d);
    *output = e;
    return;
}

//! riemann_zeta_complex - Evaluate the Riemann zeta function for complex arguments. Arguments with real part less
//! than one half are evaluated using the reflection formula.
//! \param [in] in - The argument
//! \param [out] output - The value of the zeta function
//! \param [out] status - Set to one if the function could not be evaluated, e.g. at the pole at z=1, or where it
//! would overflow or take too long to evaluate
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void riemann_zeta_complex(gsl_complex in, gsl_complex *output, int *status, char *errText) {
    // x and y are the real and imaginary parts of the argument
    double x, y;
    gsl_complex b, c;
    gsl_sf_result dr, dt;

    // Deal first with the special points, and with arguments which would take too long or overflow
    x = GSL_REAL(in);
    y = GSL_IMAG(in);

    // The pole at z = 1
    if ((x == 1.0) && (y == 0.0)) {
        *status = 1; /* pole at z=1 */
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "%s",
                          "The Riemann zeta function has a pole at z=1 and cannot be evaluated here.");
        return;
    } else if ((x == 0.0) && (y == 0.0)) {
        *status = 0;
        *output = gsl_complex_rect(-0.5, 0.0);
        return;
    } else if (fabs(y) > 1000000.0) {
        *status = 1; /* would take too long to evaluate */
        ppl_snprintfTrunc(
                errText, LSTR_LENGTH, "%s",
                "The Riemann zeta function takes a long time to evaluate for inputs with large complex components; operation cancelled.");
        return;
    } else if (x < -300.0) {
        *status = 1; /* risk of overflow */
        ppl_snprintfTrunc(
                errText, LSTR_LENGTH, "%s",
                "The Riemann zeta function cannot be evaluated for inputs with real parts below -300 due to numerical overflows.");
        return;
    } else if ((y == 0.0) && ((x / 2) == ((double) ((int) (x / 2))))) {
        *status = 0;
        *output = GSL_COMPLEX_ZERO;
        return;
    } else if (x >= 0.5) {
        *status = 0;
        riemann_zeta_complex__(x, y, output, status, errText);
        return;
    } else {
        b = gsl_complex_mul_real(in, M_LN2);
        c = gsl_complex_mul_real(gsl_complex_sub_real(in, 1.0), log(M_PI));
        b = gsl_complex_add(b, c);
        // At the trivial zeros (negative even integers), the sine term is exactly zero
        if (fabs(y) < 41) {
            c = gsl_complex_sin(gsl_complex_mul_real(in, M_PI_2));
            // A trivial zero
            if ((GSL_REAL(c) == 0) && (GSL_IMAG(c) == 0)) {
                *status = 0;
                *output = GSL_COMPLEX_ZERO;
                return;
            }
            c = gsl_complex_log(c);
        } else {
            if (y > 0) c = gsl_complex_rect(y * M_PI_2 - M_LN2, -x * M_PI_2 - M_PI_2);
            else c = gsl_complex_rect(-y * M_PI_2 - M_LN2, x * M_PI_2 - M_PI_2);
        }
        b = gsl_complex_add(b, c);
        gsl_sf_lngamma_complex_e((1 - x), (-y), &dr, &dt);
        c = gsl_complex_rect(dr.val, dt.val);
        b = gsl_complex_add(b, c);

        // The gamma function term would overflow
        if (GSL_REAL(b) > 660) {
            *status = 1; /* risk of overflow */
            ppl_snprintfTrunc(errText, LSTR_LENGTH,
                              "The Riemann zeta function could not be evaluated due to a numerical overflow.");
            return;
        }
        b = gsl_complex_exp(b);
        riemann_zeta_complex__(1.0 - x, -y, &c, status, errText);
        b = gsl_complex_mul(b, c);
        *status = 0;
        *output = b;
        return;
    }
}
