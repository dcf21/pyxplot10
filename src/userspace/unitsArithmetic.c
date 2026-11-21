// unitsArithmetic.c
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

// Arithmetic on quantities with physical units: raising to powers, multiplication, division, addition and subtraction,
// keeping track of the units of the result, and checking that the units of the operands are compatible. Temperatures
// need special care, since degrees Celsius and Fahrenheit have offsets from absolute zero. The expression evaluator
// calls these functions for each arithmetic operator.

#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <string.h>

#include <gsl/gsl_complex.h>
#include <gsl/gsl_complex_math.h>
#include <gsl/gsl_math.h>

#include "stringTools/asciidouble.h"
#include "settings/settingTypes.h"
#include "userspace/context.h"
#include "userspace/pplObj_fns.h"
#include "userspace/pplObjUnits.h"
#include "userspace/unitsDisp.h"
#include "userspace/unitsArithmetic.h"

// -------------------------------------------------------------------
// Functions for comparing the units of quantities
// -------------------------------------------------------------------

//! ppl_unitsDimCpy - Copy the physical dimensions of one object onto another
//! \param [out] o - The object whose dimensions are to be set
//! \param [in] i - The object whose dimensions are to be copied

void ppl_unitsDimCpy(pplObj *o, const pplObj *i) {
    int j;
    o->dimensionless = i->dimensionless;
    o->tempType = i->tempType;
    for (j = 0; j < UNITS_MAX_BASEUNITS; j++) o->exponent[j] = i->exponent[j];
    return;
}

//! ppl_unitsDimInverse - Set the physical dimensions of one object to be the inverse of those of another
//! \param [out] o - The object whose dimensions are to be set
//! \param [in] i - The object whose dimensions are to be inverted

void ppl_unitsDimInverse(pplObj *o, const pplObj *i) {
    int j;
    o->dimensionless = i->dimensionless;
    for (j = 0; j < UNITS_MAX_BASEUNITS; j++) o->exponent[j] = -i->exponent[j];
    o->tempType = 0; // Either input was per temperature, or we are now per temperature.
    return;
}

//! ppl_unitsDimEqual - Test whether two objects have the same physical dimensions
//! \param [in] a - The first object
//! \param [in] b - The second object
//! \return One if the objects have the same dimensions, zero otherwise

int ppl_unitsDimEqual(const pplObj *a, const pplObj *b) {
    int j;
    if (a->dimensionless && b->dimensionless) return 1;
    for (j = 0; j < UNITS_MAX_BASEUNITS; j++)
        if (ppl_dblEqual(a->exponent[j], b->exponent[j]) == 0) return 0;
    return 1;
}

//! ppl_unitsDimEqual2 - Test whether an object has the same physical dimensions as a unit
//! \param [in] a - The object
//! \param [in] b - The unit
//! \return One if the dimensions are the same, zero otherwise

int ppl_unitsDimEqual2(const pplObj *a, const unit *b) {
    int j;
    for (j = 0; j < UNITS_MAX_BASEUNITS; j++)
        if (ppl_dblEqual(a->exponent[j], b->exponent[j]) == 0) return 0;
    return 1;
}

//! ppl_unitsDimEqual3 - Test whether two units have the same physical dimensions
//! \param [in] a - The first unit
//! \param [in] b - The second unit
//! \return One if the dimensions are the same, zero otherwise

int ppl_unitsDimEqual3(const unit *a, const unit *b) {
    int j;
    for (j = 0; j < UNITS_MAX_BASEUNITS; j++)
        if (ppl_dblEqual(a->exponent[j], b->exponent[j]) == 0) return 0;
    return 1;
}

//! ppl_tempTypeMatch - Test whether two quantities are measured on compatible temperature scales. Quantities without
//! dimensions of temperature are compatible with anything.
//! \param [in] a - The temperature scale of the first quantity, or zero if it has no dimensions of temperature
//! \param [in] b - The temperature scale of the second quantity, or zero if it has no dimensions of temperature
//! \return One if the temperature scales are compatible, zero otherwise

unsigned char ppl_tempTypeMatch(const unsigned char a, const unsigned char b) {
    if ((a < 1) || (b < 1))
        return 1; // anything is compatible with something which doesn't have dimensions of temperature
    return (a == b);
}

// -------------------------------
// ARITHMETIC OPERATIONS ON VALUES
// -------------------------------

//! ppl_uaPow - Raise one quantity to the power of another, taking account of physical units and complex numbers
//! \param [in] c - The Pyxplot context
//! \param [in] a - The base
//! \param [in] b - The exponent, which must be dimensionless
//! \param [out] o - The result (may be the same object as <a> or <b>)
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void ppl_uaPow(ppl_context *c, const pplObj *a, const pplObj *b, pplObj *o, int *status, int *errType, char *errText) {
    // exponent is the power to which a is raised; DimLess is cleared if the result has any units
    int i;
    double exponent;
    gsl_complex ac, bc;
    unsigned char DimLess = 1;

    exponent = b->real; // We may overwrite this when we set o->real, so store a copy

    // An exponent with units is an error, unless errors are switched off, in which case the result is NaN. The same
    // pattern follows every check below.
    if (b->dimensionless == 0) {
        // Errors switched off: return NaN
        if (c->set->term_current.ExplicitErrors == SW_ONOFF_OFF) {
            pplObjNum(o, o->amMalloced, GSL_NAN, 0.0);
            return;
        } else {
            ppl_snprintfTrunc(errText, PPL_MSG_LENGTH,
                              "Exponent should be dimensionless, but instead has dimensions of <%s>.",
                              ppl_printUnit(c, b, NULL, NULL, 0, 1, 0));
            *errType = ERR_UNIT;
            *status = 1;
            return;
        }
    }

    if (c->set->term_current.ComplexNumbers == SW_ONOFF_OFF) // Real pow()
    {
        // Complex numbers give NaN when complex arithmetic is switched off
        if (a->flagComplex || b->flagComplex) {
            o->real = GSL_NAN;
        } else {
            o->real = pow(a->real, exponent);
            // A result which overflows, or is complex, e.g. (-1)**0.5, is an error
            if ((!gsl_finite(o->real)) && (c->set->term_current.ExplicitErrors == SW_ONOFF_ON)) {
                ppl_snprintfTrunc(
                        errText, PPL_MSG_LENGTH,
                        "Exponentiation operator produced an overflow error or a complex number result. To enable complex arithmetic, type 'set numerics complex'.");
                *errType = ERR_NUMERICAL;
                *status = 1;
                return;
            }
        }
        o->imag = 0.0;
        o->flagComplex = 0;
    } else // Complex pow()
    {
        // Quantities with units cannot be raised to complex powers, which would give complex dimensions
        if ((a->dimensionless == 0) && (b->flagComplex)) {
            // Errors switched off: return NaN
            if (c->set->term_current.ExplicitErrors == SW_ONOFF_OFF) {
                pplObjNum(o, o->amMalloced, GSL_NAN, 0.0);
                return;
            } else {
                ppl_snprintfTrunc(
                        errText, PPL_MSG_LENGTH,
                        "Raising quantities with physical units to complex powers produces quantities with complex physical dimensions, which is forbidden. The operand in question has dimensions of <%s>.",
                        ppl_printUnit(c, a, NULL, NULL, 0, 1, 0));
                *errType = ERR_UNIT;
                *status = 1;
                return;
            }
        } else {
            // Raise to a real power or a complex power
            if (!b->flagComplex) {
                GSL_SET_COMPLEX(&ac, a->real, a->imag);
                ac = gsl_complex_pow_real(ac, b->real);
            } else {
                GSL_SET_COMPLEX(&ac, a->real, a->imag);
                GSL_SET_COMPLEX(&bc, b->real, b->imag);
                ac = gsl_complex_pow(ac, bc);
            }
            o->real = GSL_REAL(ac);
            o->imag = GSL_IMAG(ac);
            o->flagComplex = fabs(o->imag) > (fabs(o->real) * 1e-15);
            if (!o->flagComplex) o->imag = 0.0; // Enforce that real numbers have positive zero imaginary components
            // An overflow is an error
            if ((!gsl_finite(o->real)) || (!gsl_finite(o->imag))) {
                // Report it, or return NaN if errors are switched off
                if (c->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
                    ppl_snprintfTrunc(errText, PPL_MSG_LENGTH, "Exponentiation operator produced an overflow error.");
                    *errType = ERR_OVERFLOW;
                    *status = 1;
                    return;
                } else {
                    pplObjNum(o, o->amMalloced, GSL_NAN, 0.0);
                    return;
                }
            }
        }
    }

    // A dimensionless number raised to a power is dimensionless
    if (a->dimensionless != 0) {
        if ((o != a) && (o != b)) ppl_unitsDimCpy(o, a);
        return;
    }

    // Otherwise multiply the exponent of each base unit by the power, checking for absurdly large exponents
    for (i = 0; i < UNITS_MAX_BASEUNITS; i++) {
        o->exponent[i] = a->exponent[i] * exponent;
        if (ppl_dblEqual(o->exponent[i], 0) == 0) DimLess = 0;
        // Exponents this large are an overflow
        if (fabs(o->exponent[i]) > 20000) {
            // Errors switched off: return NaN
            if (c->set->term_current.ExplicitErrors == SW_ONOFF_OFF) {
                pplObjNum(o, o->amMalloced, GSL_NAN, 0.0);
                return;
            } else {
                ppl_snprintfTrunc(errText, PPL_MSG_LENGTH, "Overflow of physical dimensions of argument.");
                *errType = ERR_OVERFLOW;
                *status = 1;
                return;
            }
        }
    }
    o->tempType = a->tempType;
    o->dimensionless = DimLess;
    return;
}

//! ppl_uaMul - Multiply two quantities, taking account of physical units and complex numbers
//! \param [in] c - The Pyxplot context
//! \param [in] a - The first quantity
//! \param [in] b - The second quantity
//! \param [out] o - The result (may be the same object as <a> or <b>)
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void ppl_uaMul(ppl_context *c, const pplObj *a, const pplObj *b, pplObj *o, int *status, int *errType, char *errText) {
    // tmp is workspace for complex multiplication; DimLess is cleared if the result has any units
    int i;
    double tmp, areal, breal, aimag, bimag;
    unsigned char DimLess = 1;

    // Copy the operands, since o may be the same object as a or b
    areal = a->real;
    aimag = a->imag;
    breal = b->real;
    bimag = b->imag;

    // Two inputs have conflicting temperature units. This is only allowed in the special case of oC/oF and friends, when temperature conversion happens.
    if (!ppl_tempTypeMatch(a->tempType, b->tempType)) {
        // Multiplying a temperature by an inverse temperature on a different scale, e.g. oC/oF, converts between the
        // scales by adding the difference between their offsets
        if (ppl_dblEqual(a->exponent[UNIT_TEMPERATURE], 1.0) && (ppl_dblEqual(b->exponent[UNIT_TEMPERATURE], -1.0))) {
            areal = areal + c->tempTypeOffset[a->tempType] -
                    c->tempTypeOffset[b->tempType]; // Remember, areal and breal have already had multipliers applied.
        } else if (ppl_dblEqual(a->exponent[UNIT_TEMPERATURE], -1.0) &&
                   (ppl_dblEqual(b->exponent[UNIT_TEMPERATURE], 1.0))) {
            breal = breal + c->tempTypeOffset[b->tempType] -
                    c->tempTypeOffset
                            [a->tempType]; // Imaginary part needs to conversion... multiplication already done.
        } else {
            *status = 1;
            *errType = ERR_UNIT;
            ppl_snprintfTrunc(
                    errText, PPL_MSG_LENGTH,
                    "Attempt to multiply quantities with different temperature units: left operand has units of <%s>, while right operand has units of <%s>. These must be explicitly cast onto the same temperature scale before multiplication is allowed. Type 'help units temperatures' for more details.",
                    ppl_printUnit(c, a, NULL, NULL, 0, 1, 0), ppl_printUnit(c, b, NULL, NULL, 1, 1, 0));
        }
    }

    // If one or other input has temperature dependence, we need to propagate which unit of temperature is being used.
    // If we're in one of the cases handled above, don't worry as temperature exponent is about to end up as zero after, e.g. oC/oF
    o->tempType = (a->tempType > b->tempType) ? a->tempType : b->tempType;

    // Multiply the values, as real or complex numbers
    if ((c->set->term_current.ComplexNumbers == SW_ONOFF_OFF) ||
        ((!a->flagComplex) && (!b->flagComplex))) // Real multiplication
    {
        // Complex numbers give NaN when complex arithmetic is switched off
        if (a->flagComplex || b->flagComplex) {
            o->real = GSL_NAN;
        } else {
            o->real = areal * breal;
        }
        o->imag = 0.0;
        o->flagComplex = 0;
    } else // Complex multiplication
    {
        // Complex multiplication: (a + ib)(c + id) = (ac - bd) + i(bc + ad)
        if (c->set->term_current.ComplexNumbers == SW_ONOFF_OFF) {
            o->real = GSL_NAN;
            o->imag = 0;
            o->flagComplex = 0;
        } else {
            tmp = (areal * breal - aimag * bimag);
            o->imag = (aimag * breal + areal * bimag);
            o->real = tmp;
            o->flagComplex = !ppl_dblEqual(o->imag, 0);
            if (!o->flagComplex) o->imag = 0.0; // Enforce that real numbers have positive zero imaginary components
        }
    }

    // An overflow is an error
    if ((!gsl_finite(o->real)) || (!gsl_finite(o->imag))) {
        // Report it, or return NaN if errors are switched off
        if (c->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
            ppl_snprintfTrunc(errText, PPL_MSG_LENGTH, "Multiplication produced an overflow error.");
            *errType = ERR_OVERFLOW;
            *status = 1;
            return;
        } else {
            pplObjNum(o, o->amMalloced, GSL_NAN, 0.0);
            return;
        }
    }

    // The product of dimensionless numbers is dimensionless
    if ((a->dimensionless != 0) && (b->dimensionless != 0)) {
        if ((o != a) && (o != b)) ppl_unitsDimCpy(o, a);
        return;
    }

    // Otherwise add the exponents of each base unit, checking for absurdly large exponents
    for (i = 0; i < UNITS_MAX_BASEUNITS; i++) {
        o->exponent[i] = a->exponent[i] + b->exponent[i];
        if (ppl_dblEqual(o->exponent[i], 0) == 0) DimLess = 0;
        // Exponents this large are an overflow
        if (fabs(o->exponent[i]) > 20000) {
            // Errors switched off: return NaN
            if (c->set->term_current.ExplicitErrors == SW_ONOFF_OFF) {
                pplObjNum(o, o->amMalloced, GSL_NAN, 0.0);
                return;
            } else {
                ppl_snprintfTrunc(errText, PPL_MSG_LENGTH, "Overflow of physical dimensions of argument.");
                *errType = ERR_OVERFLOW;
                *status = 1;
                return;
            }
        }
    }
    o->dimensionless = DimLess;
    if (o->exponent[UNIT_TEMPERATURE] == 0) o->tempType = 0; // We've lost our temperature dependence
    return;
}

//! ppl_uaDiv - Divide one quantity by another, taking account of physical units and complex numbers
//! \param [in] c - The Pyxplot context
//! \param [in] a - The numerator
//! \param [in] b - The denominator
//! \param [out] o - The result (may be the same object as <a> or <b>)
//! \param [out] status - Set to one if an error occurred, e.g. division by zero
//! \param [out] errType - Set to the type of error, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void ppl_uaDiv(ppl_context *c, const pplObj *a, const pplObj *b, pplObj *o, int *status, int *errType, char *errText) {
    // mag and tmp are workspace for complex division; DimLess is cleared if the result has any units
    int i;
    double mag, tmp, areal, breal, aimag, bimag;
    unsigned char DimLess = 1;

    // Copy the operands, since o may be the same object as a or b
    areal = a->real;
    aimag = a->imag;
    breal = b->real;
    bimag = b->imag;

    // Two inputs have conflicting temperature units. This is only allowed in the special case of oC/oF and friends, when temperature conversion happens.
    if (!ppl_tempTypeMatch(a->tempType, b->tempType)) {
        // Dividing temperatures on different scales, e.g. oC/oF, converts between the scales by adding the difference
        // between their offsets
        if (ppl_dblEqual(a->exponent[UNIT_TEMPERATURE], 1.0) && (ppl_dblEqual(b->exponent[UNIT_TEMPERATURE], 1.0))) {
            areal = areal + c->tempTypeOffset[a->tempType] -
                    c->tempTypeOffset[b->tempType]; // Remember, areal and breal have already had multipliers applied.
        } else if (ppl_dblEqual(a->exponent[UNIT_TEMPERATURE], -1.0) &&
                   (ppl_dblEqual(b->exponent[UNIT_TEMPERATURE], -1.0))) {
            breal = breal + c->tempTypeOffset[b->tempType] -
                    c->tempTypeOffset
                            [a->tempType]; // Imaginary part needs to conversion... multiplication already done.
        } else {
            *status = 1;
            *errType = ERR_UNIT;
            ppl_snprintfTrunc(
                    errText, PPL_MSG_LENGTH,
                    "Attempt to divide quantities with different temperature units: left operand has units of <%s>, while right operand has units of <%s>. These must be explicitly cast onto the same temperature scale before division is allowed. Type 'help units temperatures' for more details.",
                    ppl_printUnit(c, a, NULL, NULL, 0, 1, 0), ppl_printUnit(c, b, NULL, NULL, 1, 1, 0));
        }
    }

    // If one or other input has temperature dependence, we need to propagate which unit of temperature is being used.
    // If we're in one of the cases handled above, don't worry as temperature exponent is about to end up as zero after, e.g. oC/oF
    o->tempType = (a->tempType > b->tempType) ? a->tempType : b->tempType;

    // Divide the values, as real or complex numbers
    if ((c->set->term_current.ComplexNumbers == SW_ONOFF_OFF) ||
        ((!a->flagComplex) && (!b->flagComplex))) // Real division
    {
        // Complex numbers give NaN when complex arithmetic is switched off; division by zero is an error
        if (a->flagComplex || b->flagComplex) {
            o->real = GSL_NAN;
            o->imag = 0;
            o->flagComplex = 0;
        } else if (fabs(breal) < 1e-200) {
            // Errors switched off: return NaN
            if (c->set->term_current.ExplicitErrors == SW_ONOFF_OFF) {
                o->real = GSL_NAN;
                o->imag = 0;
                o->flagComplex = 0;
            } else {
                ppl_snprintfTrunc(errText, PPL_MSG_LENGTH, "Division by zero error.");
                *errType = ERR_NUMERICAL;
                *status = 1;
                return;
            }
        } else {
            // Real division
            if (a->flagComplex || b->flagComplex) {
                o->real = GSL_NAN;
            } else {
                o->real = areal / breal;
            }
            o->imag = 0.0;
            o->flagComplex = 0;
        }
    } else // Complex division
    {
        // Complex division: multiply by the complex conjugate of the divisor, and divide by the square of its
        // magnitude, mag
        if (c->set->term_current.ComplexNumbers == SW_ONOFF_OFF) {
            o->real = GSL_NAN;
            o->imag = 0;
            o->flagComplex = 0;
        } else if ((mag = pow(breal, 2) + pow(bimag, 2)) < 1e-200) {
            // Division by zero: return NaN, or report an error
            if (c->set->term_current.ExplicitErrors == SW_ONOFF_OFF) {
                o->real = GSL_NAN;
                o->imag = 0;
                o->flagComplex = 0;
            } else {
                ppl_snprintfTrunc(errText, PPL_MSG_LENGTH, "Division by zero error.");
                *errType = ERR_NUMERICAL;
                *status = 1;
                return;
            }
        } else {
            tmp = (areal * breal + aimag * bimag) / mag;
            o->imag = (aimag * breal - areal * bimag) / mag;
            o->real = tmp;
            o->flagComplex = !ppl_dblEqual(o->imag, 0);
            if (!o->flagComplex) o->imag = 0.0; // Enforce that real numbers have positive zero imaginary components
        }
    }

    // An overflow is an error
    if ((!gsl_finite(o->real)) || (!gsl_finite(o->imag))) {
        // Report it, or return NaN if errors are switched off
        if (c->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
            ppl_snprintfTrunc(errText, PPL_MSG_LENGTH, "Division produced an overflow error.");
            *errType = ERR_OVERFLOW;
            *status = 1;
            return;
        } else {
            pplObjNum(o, o->amMalloced, GSL_NAN, 0.0);
            return;
        }
    }

    // The quotient of dimensionless numbers is dimensionless
    if ((a->dimensionless != 0) && (b->dimensionless != 0)) {
        if ((o != a) && (o != b)) ppl_unitsDimCpy(o, a);
        return;
    }

    // Otherwise subtract the exponents of each base unit, checking for absurdly large exponents
    for (i = 0; i < UNITS_MAX_BASEUNITS; i++) {
        o->exponent[i] = a->exponent[i] - b->exponent[i];
        if (ppl_dblEqual(o->exponent[i], 0) == 0) DimLess = 0;
        // Exponents this large are an overflow
        if (fabs(o->exponent[i]) > 20000) {
            // Errors switched off: return NaN
            if (c->set->term_current.ExplicitErrors == SW_ONOFF_OFF) {
                pplObjNum(o, o->amMalloced, GSL_NAN, 0.0);
                return;
            } else {
                ppl_snprintfTrunc(errText, PPL_MSG_LENGTH, "Overflow of physical dimensions of argument.");
                *errType = ERR_UNIT;
                *status = 1;
                return;
            }
        }
    }
    o->dimensionless = DimLess;
    if (o->exponent[UNIT_TEMPERATURE] == 0) o->tempType = 0; // We've lost our temperature dependence
    return;
}

//! ppl_uaAdd - Add two quantities, which must have the same physical dimensions
//! \param [in] c - The Pyxplot context
//! \param [in] a - The first quantity
//! \param [in] b - The second quantity
//! \param [out] o - The result (may be the same object as <a> or <b>)
//! \param [out] status - Set to one if an error occurred, e.g. the quantities have different dimensions
//! \param [out] errType - Set to the type of error, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void ppl_uaAdd(ppl_context *c, const pplObj *a, const pplObj *b, pplObj *o, int *status, int *errType, char *errText) {
    // Add the real parts; the result takes the units of the left operand
    o->real = a->real + b->real;
    // Unless the result overwrites an operand, give it the left operand's units
    if ((o != a) && (o != b)) {
        ppl_unitsDimCpy(o, a);
        o->imag = 0.0;
        o->flagComplex = 0;
    }
    // Quantities can only be added if they have the same units
    if ((a->dimensionless == 0) || (b->dimensionless == 0)) {
        // The units differ: describe them in the error message
        if (ppl_unitsDimEqual(a, b) == 0) {
            // Describe whichever operands have units
            if (a->dimensionless) {
                ppl_snprintfTrunc(
                        errText, LSTR_LENGTH,
                        "Attempt to add quantities with conflicting dimensions: left operand is dimensionless, while right operand has units of <%s>.",
                        ppl_printUnit(c, b, NULL, NULL, 1, 1, 0));
            } else if (b->dimensionless) {
                ppl_snprintfTrunc(
                        errText, LSTR_LENGTH,
                        "Attempt to add quantities with conflicting dimensions: left operand has units of <%s>, while right operand is dimensionless.",
                        ppl_printUnit(c, a, NULL, NULL, 0, 1, 0));
            } else {
                ppl_snprintfTrunc(
                        errText, LSTR_LENGTH,
                        "Attempt to add quantities with conflicting dimensions: left operand has units of <%s>, while right operand has units of <%s>.",
                        ppl_printUnit(c, a, NULL, NULL, 0, 1, 0), ppl_printUnit(c, b, NULL, NULL, 1, 1, 0));
            }
            *errType = ERR_UNIT;
            *status = 1;
            return;
        }
    }
    // Temperatures on different scales, e.g. oC and oF, cannot be added directly
    if (!ppl_tempTypeMatch(a->tempType, b->tempType)) {
        *errType = ERR_UNIT;
        *status = 1;
        ppl_snprintfTrunc(
                errText, LSTR_LENGTH,
                "Attempt to add quantities with different temperature units: left operand has units of <%s>, while right operand has units of <%s>. These must be explicitly cast onto the same temperature scale before addition is allowed. Type 'help units temperatures' for more details.",
                ppl_printUnit(c, a, NULL, NULL, 0, 1, 0), ppl_printUnit(c, b, NULL, NULL, 1, 1, 0));
    }
    // Add the imaginary parts, if either operand is complex
    if (a->flagComplex || b->flagComplex) {
        // Complex numbers give NaN when complex arithmetic is switched off
        if (c->set->term_current.ComplexNumbers == SW_ONOFF_OFF) {
            o->real = GSL_NAN;
            o->imag = 0;
            o->flagComplex = 0;
            return;
        }
        o->imag = a->imag + b->imag;
        o->flagComplex = !ppl_dblEqual(o->imag, 0);
        if (!o->flagComplex) o->imag = 0.0; // Enforce that real numbers have positive zero imaginary components
    }

    // An overflow is an error
    if ((!gsl_finite(o->real)) || (!gsl_finite(o->imag))) {
        // Report it, or return NaN if errors are switched off
        if (c->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
            ppl_snprintfTrunc(errText, LSTR_LENGTH, "Addition produced an overflow error.");
            *errType = ERR_OVERFLOW;
            *status = 1;
            return;
        } else {
            pplObjNum(o, o->amMalloced, GSL_NAN, 0.0);
            return;
        }
    }

    // Done
    return;
}

//! ppl_uaSub - Subtract one quantity from another. They must have the same physical dimensions.
//! \param [in] c - The Pyxplot context
//! \param [in] a - The quantity to subtract from
//! \param [in] b - The quantity to subtract
//! \param [out] o - The result (may be the same object as <a> or <b>)
//! \param [out] status - Set to one if an error occurred, e.g. the quantities have different dimensions
//! \param [out] errType - Set to the type of error, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void ppl_uaSub(ppl_context *c, const pplObj *a, const pplObj *b, pplObj *o, int *status, int *errType, char *errText) {
    // Subtract the real parts; the result takes the units of the left operand
    o->real = a->real - b->real;
    // Unless the result overwrites an operand, give it the left operand's units
    if ((o != a) && (o != b)) {
        ppl_unitsDimCpy(o, a);
        o->imag = 0.0;
        o->flagComplex = 0;
    }
    // Quantities can only be subtracted if they have the same units
    if ((a->dimensionless == 0) || (b->dimensionless == 0)) {
        // The units differ: describe them in the error message
        if (ppl_unitsDimEqual(a, b) == 0) {
            // Describe whichever operands have units
            if (a->dimensionless) {
                ppl_snprintfTrunc(
                        errText, LSTR_LENGTH,
                        "Attempt to subtract quantities with conflicting dimensions: left operand is dimensionless, while right operand has units of <%s>.",
                        ppl_printUnit(c, b, NULL, NULL, 1, 1, 0));
            } else if (b->dimensionless) {
                ppl_snprintfTrunc(
                        errText, LSTR_LENGTH,
                        "Attempt to subtract quantities with conflicting dimensions: left operand has units of <%s>, while right operand is dimensionless.",
                        ppl_printUnit(c, a, NULL, NULL, 0, 1, 0));
            } else {
                ppl_snprintfTrunc(
                        errText, LSTR_LENGTH,
                        "Attempt to subtract quantities with conflicting dimensions: left operand has units of <%s>, while right operand has units of <%s>.",
                        ppl_printUnit(c, a, NULL, NULL, 0, 1, 0), ppl_printUnit(c, b, NULL, NULL, 1, 1, 0));
            }
            *errType = ERR_UNIT;
            *status = 1;
            return;
        }
    }
    // Temperatures on different scales, e.g. oC and oF, cannot be subtracted directly
    if (!ppl_tempTypeMatch(a->tempType, b->tempType)) {
        *errType = ERR_UNIT;
        *status = 1;
        ppl_snprintfTrunc(
                errText, LSTR_LENGTH,
                "Attempt to subtract quantities with different temperature units: left operand has units of <%s>, while right operand has units of <%s>. These must be explicitly cast onto the same temperature scale before subtraction is allowed. Type 'help units temperatures' for more details.",
                ppl_printUnit(c, a, NULL, NULL, 0, 1, 0), ppl_printUnit(c, b, NULL, NULL, 1, 1, 0));
    }
    // Subtract the imaginary parts, if either operand is complex
    if (a->flagComplex || b->flagComplex) {
        // Complex numbers give NaN when complex arithmetic is switched off
        if (c->set->term_current.ComplexNumbers == SW_ONOFF_OFF) {
            o->real = GSL_NAN;
            o->imag = 0;
            o->flagComplex = 0;
            return;
        }
        o->imag = a->imag - b->imag;
        o->flagComplex = !ppl_dblEqual(o->imag, 0);
        if (!o->flagComplex) o->imag = 0.0; // Enforce that real numbers have positive zero imaginary components
    }

    // An overflow is an error
    if ((!gsl_finite(o->real)) || (!gsl_finite(o->imag))) {
        // Report it, or return NaN if errors are switched off
        if (c->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
            ppl_snprintfTrunc(errText, LSTR_LENGTH, "Subtraction produced an overflow error.");
            *errType = ERR_OVERFLOW;
            *status = 1;
            return;
        } else {
            pplObjNum(o, o->amMalloced, GSL_NAN, 0.0);
            return;
        }
    }

    // Done
    return;
}

//! ppl_uaMod - Compute the remainder of dividing one quantity by another. They must have the same physical
//! dimensions, and must be real.
//! \param [in] c - The Pyxplot context
//! \param [in] a - The numerator
//! \param [in] b - The denominator
//! \param [out] o - The result (may be the same object as <a> or <b>)
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void ppl_uaMod(ppl_context *c, const pplObj *a, const pplObj *b, pplObj *o, int *status, int *errType, char *errText) {
    // Floored modulo, so that the result has the sign of b, e.g. -1 % 3 = 2; the result takes the units of the left
    // operand
    o->real = a->real - floor(a->real / b->real) * b->real;
    // Unless the result overwrites an operand, give it the left operand's units
    if ((o != a) && (o != b)) {
        ppl_unitsDimCpy(o, a);
        o->imag = 0.0;
        o->flagComplex = 0;
    }
    // Both operands must have the same units
    if ((a->dimensionless == 0) || (b->dimensionless == 0)) {
        // The units differ: describe them in the error message
        if (ppl_unitsDimEqual(a, b) == 0) {
            // Describe whichever operands have units
            if (a->dimensionless) {
                ppl_snprintfTrunc(
                        errText, LSTR_LENGTH,
                        "Attempt to apply mod operator to quantities with conflicting dimensions: left operand is dimensionless, while right operand has units of <%s>.",
                        ppl_printUnit(c, b, NULL, NULL, 1, 1, 0));
            } else if (b->dimensionless) {
                ppl_snprintfTrunc(
                        errText, LSTR_LENGTH,
                        "Attempt to apply mod operator to quantities with conflicting dimensions: left operand has units of <%s>, while right operand is dimensionless.",
                        ppl_printUnit(c, a, NULL, NULL, 0, 1, 0));
            } else {
                ppl_snprintfTrunc(
                        errText, LSTR_LENGTH,
                        "Attempt to apply mod operator to quantities with conflicting dimensions: left operand has units of <%s>, while right operand has units of <%s>.",
                        ppl_printUnit(c, a, NULL, NULL, 0, 1, 0), ppl_printUnit(c, b, NULL, NULL, 1, 1, 0));
            }
            *errType = ERR_UNIT;
            *status = 1;
            return;
        }
    }
    // Temperatures on different scales, e.g. oC and oF, are not compatible
    if (!ppl_tempTypeMatch(a->tempType, b->tempType)) {
        *errType = ERR_UNIT;
        *status = 1;
        ppl_snprintfTrunc(
                errText, LSTR_LENGTH,
                "Attempt to apply mod operator to quantities with different temperature units: left operand has units of <%s>, while right operand has units of <%s>. These must be explicitly cast onto the same temperature scale before the use of the mod operator is allowed. Type 'help units temperatures' for more details.",
                ppl_printUnit(c, a, NULL, NULL, 0, 1, 0), ppl_printUnit(c, b, NULL, NULL, 1, 1, 0));
    }
    // The modulo of complex numbers is not defined
    if (a->flagComplex || b->flagComplex) {
        // Complex numbers give NaN when complex arithmetic is switched off
        if (c->set->term_current.ComplexNumbers == SW_ONOFF_OFF) {
            o->real = GSL_NAN;
            o->imag = 0;
            o->flagComplex = 0;
            return;
        }
        ppl_snprintfTrunc(errText, LSTR_LENGTH,
                          "Mod operator can only be applied to real operands; complex operands supplied.");
        *errType = ERR_NUMERICAL;
        *status = 1;
        return;
    }

    // An overflow, including taking a number modulo zero, is an error
    if ((!gsl_finite(o->real)) || (!gsl_finite(o->imag))) {
        // Report it, or return NaN if errors are switched off
        if (c->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
            ppl_snprintfTrunc(errText, LSTR_LENGTH, "Modulo operator produced an overflow error.");
            *errType = ERR_OVERFLOW;
            *status = 1;
            return;
        } else {
            pplObjNum(o, o->amMalloced, GSL_NAN, 0.0);
            return;
        }
    }

    // Done
    return;
}
