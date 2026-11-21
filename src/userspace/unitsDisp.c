// unitsDisp.c
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

// Displays quantities with physical units, and parses the names of units. It chooses the units in which to show a
// quantity, e.g. 3 km/s rather than 3000 m/s, taking account of the user's preferred units, the current unit scheme
// (SI, CGS, imperial...) and SI prefixes; renders the number in natural, typeable or LaTeX form; and implements the
// unit() function, which turns strings such as "km/s**2" into quantities.

#define _PPL_UNITSDISP_C 1

#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <string.h>

#include <gsl/gsl_complex.h>
#include <gsl/gsl_complex_math.h>
#include <gsl/gsl_math.h>

#include "coreUtils/errorReport.h"
#include "settings/settings.h"
#include "settings/settingTypes.h"
#include "stringTools/asciidouble.h"
#include "userspace/context.h"
#include "userspace/pplObj_fns.h"
#include "userspace/pplObjUnits.h"
#include "userspace/unitsArithmetic.h"
#include "userspace/unitsDisp.h"

// The SI prefixes, from 10^-24 to 10^24 in steps of 1000, in full, abbreviated, and in LaTeX
const char *SIprefixes_full[] = {"yocto", "zepto", "atto", "femto", "pico", "nano", "micro", "milli", "",
                                 "kilo",  "mega",  "giga", "tera",  "peta", "exa",  "zetta", "yotta"};
const char *SIprefixes_abbrev[] = {"y", "z", "a", "f", "p", "n", "u", "m", "", "k", "M", "G", "T", "P", "E", "Z", "Y"};
const char *SIprefixes_latex[] = {"y", "z", "a", "f", "p", "n", "\\upmu ", "m", "",
                                  "k", "M", "G", "T", "P", "E", "Z",       "Y"};

// NB: The LaTeX upmu character is defined by the {upgreek} package

//! printOneNumber - Render a single number using a printf-style format string supplied by the user
//! \param [out] out - The buffer into which to write the number
//! \param [in] len - The space available in <out>; nothing is written if this is zero or less
//! \param [in] formatString - The printf-style format string, e.g. "%.3f"
//! \param [in] requiredArgs - The number of arguments required by <formatString>, including the number itself; any
//! others are integer field widths or precisions, given as *
//! \param [in] intArg - The type of number required by <formatString>: 0 for a double, 1 for an int, or 2 for an
//! unsigned int
//! \param [in] arg1i - The first integer argument required by <formatString>
//! \param [in] arg2i - The second integer argument required by <formatString>
//! \param [in] x - The number to render
//! \return The number of characters written

static int printOneNumber(char *out, const int len, const char *formatString, const int requiredArgs, const int intArg,
                          const int arg1i, const int arg2i, const double x) {
    if (len <= 0) return 0;
    out[0] = '\0';
    // printf() needs to be given an argument of the type which the format expects, after up to two integer arguments
    // (e.g. the field width and precision of %*.*f)
    if (intArg == 0) {
        // Print a double
        if (requiredArgs == 1) snprintf(out, len, formatString, x);
        if (requiredArgs == 2) snprintf(out, len, formatString, arg1i, x);
        if (requiredArgs == 3) snprintf(out, len, formatString, arg1i, arg2i, x);
    } else if (intArg == 1) {
        // Print an integer
        const int xi = (int) x;
        if (requiredArgs == 1) snprintf(out, len, formatString, xi);
        if (requiredArgs == 2) snprintf(out, len, formatString, arg1i, xi);
        if (requiredArgs == 3) snprintf(out, len, formatString, arg1i, arg2i, xi);
    } else if (intArg == 2) {
        // Print an unsigned integer
        const unsigned int xi = (unsigned int) x;
        if (requiredArgs == 1) snprintf(out, len, formatString, xi);
        if (requiredArgs == 2) snprintf(out, len, formatString, arg1i, xi);
        if (requiredArgs == 3) snprintf(out, len, formatString, arg1i, arg2i, xi);
    }
    return (int) strlen(out);
}

//! ppl_unitsNumericDisplayWithFormat - Render a quantity as a string using a printf-style format string, followed
//! by its physical units
//! \param [in] c - The Pyxplot context
//! \param [in] in - The quantity to render
//! \param [in] N - Which of two static output buffers in the context to use (0 or 1)
//! \param [in] formatString - The printf-style format string for a single number, e.g. "%.3f"
//! \param [in] formatChar - The conversion character at the end of <formatString>, e.g. 'f' or 'd'
//! \param [in] maxLen - The maximum length of the output string
//! \param [in] requiredArgs - The number of integer arguments (zero, one or two) required by <formatString>, e.g.
//! for field widths and precisions given as *
//! \param [in] arg1i - The first integer argument required by <formatString>
//! \param [in] arg2i - The second integer argument required by <formatString>
//! \return Pointer to the output buffer, which is overwritten by subsequent calls

char *ppl_unitsNumericDisplayWithFormat(ppl_context *c, const pplObj *in, const int N, const char *formatString,
                                        const char formatChar, int maxLen, const int requiredArgs, int arg1i,
                                        int arg2i) {
    double numberOutReal, numberOutImag;
    char *output;
    int i = 0, done = 0, intArg = 0;
    if (N == 0) output = c->udNumDispA;
    else output = c->udNumDispB;

    // The output is written to a fixed-size buffer in the context, so never write more than it can hold, even if the
    // caller's own buffer is larger
    if (maxLen > (int) sizeof(c->udNumDispA)) maxLen = (int) sizeof(c->udNumDispA);

    // Complex numbers are not a number when complex arithmetic is switched off
    if ((c->set->term_current.ComplexNumbers == SW_ONOFF_OFF) && (in->flagComplex != 0)) {
        strcpy(output, "nan");
        return output;
    }

    // Find the units in which to display the number, and its value in those units
    char *unitstr = ppl_printUnit(c, in, &numberOutReal, &numberOutImag, N, 1, 0);

    // Integer formats cannot show numbers beyond the range of an int; show them as infinite
    if ((formatChar == 'd') || (formatChar == 'i') || (formatChar == 'o')) {
        // Clip numbers beyond the range of a signed int
        if ((numberOutReal > INT_MAX) || (numberOutImag > INT_MAX)) {
            strcpy(output + i, "inf");
            done = 1;
        } else if ((numberOutReal < INT_MIN) || (numberOutImag < INT_MIN)) {
            strcpy(output + i, "-inf");
            done = 1;
        }
        intArg = 1;
    } else if ((formatChar == 'x') || (formatChar == 'X')) {
        // Hexadecimal formats need unsigned integers
        if ((numberOutReal > UINT_MAX) || (numberOutImag > UINT_MAX)) {
            strcpy(output + i, "inf");
            done = 1;
        } else if ((numberOutReal < 0) || (numberOutImag < 0)) {
            strcpy(output + i, "-inf");
            done = 1;
        }
        intArg = 2;
    }

    // Write the number, or nan, or a complex number in the form (a+bi)
    if (done) {
        i += strlen(output + i);
    } else if (((c->set->term_current.ComplexNumbers == SW_ONOFF_OFF) && (in->flagComplex != 0)) ||
               (!gsl_finite(numberOutReal)) || (!gsl_finite(numberOutImag))) {
        strcpy(output + i, "nan");
        i += strlen(output + i);
    } else {
        if (in->flagComplex) output[i++] = '('; // open brackets on complex number

        // The real part
        i += printOneNumber(output + i, maxLen - i - 4, formatString, requiredArgs, intArg, arg1i, arg2i,
                            numberOutReal);
        // The imaginary part, if any
        if (in->flagComplex) {
            output[i++] = '+';
            i += printOneNumber(output + i, maxLen - i - 4, formatString, requiredArgs, intArg, arg1i, arg2i,
                                numberOutImag);
            output[i++] = 'i';
            output[i++] = ')'; // close brackets on complex number
        }
    }

    // Append the units
    if (unitstr[0] != '\0') {
        snprintf(output + i, maxLen - i, " %s", unitstr);
        i += strlen(output + i); // Add unit string as required
    }

    output[i++] = '\0'; // null terminate string
    return output;
}

//! ppl_unitsNumericDisplay - Render a quantity as a string, followed by its physical units
//! \param [in] c - The Pyxplot context
//! \param [in] in - The quantity to render
//! \param [in] N - Which of two static output buffers in the context to use (0 or 1)
//! \param [in] typeable - The output style, one of the SW_DISPLAY_* constants (natural, typeable or LaTeX), or zero
//! to use the current setting
//! \param [in] NSigFigs - The number of significant figures to display, or zero or less to use the current setting
//! \return Pointer to the output buffer, which is overwritten by subsequent calls

char *ppl_unitsNumericDisplay(ppl_context *c, pplObj *in, const int N, int typeable, int NSigFigs) {
    // OoM is the smallest component which is significant at the chosen number of significant figures
    double numberOutReal, numberOutImag;
    char *output;
    int i = 0;
    if (N == 0) output = c->udNumDispA;
    else output = c->udNumDispB;

    // Use the user's chosen number of significant figures, unless the caller asked for a specific number
    if (NSigFigs <= 0)
        NSigFigs =
                c->set->term_current
                        .SignificantFigures; // If number of significant figures not specified, use user-selected number

    // Complex numbers are not a number when complex arithmetic is switched off
    if ((c->set->term_current.ComplexNumbers == SW_ONOFF_OFF) && (in->flagComplex != 0))
        return ppl_numericDisplay(GSL_NAN, c->numdispBuff[N], NSigFigs, (typeable == SW_DISPLAY_L));

    // Use the user's chosen display style, unless the caller asked for one; then find the units in which to display the
    // number, and its value in those units
    if (typeable == 0) typeable = c->set->term_current.NumDisplay;
    char *unitstr = ppl_printUnit(c, in, &numberOutReal, &numberOutImag, N, 1, typeable);

    // Values which are not finite are shown as nan
    if (((c->set->term_current.ComplexNumbers == SW_ONOFF_OFF) && (in->flagComplex != 0)) ||
        (!gsl_finite(numberOutReal)) || (!gsl_finite(numberOutImag))) {
        if (typeable == SW_DISPLAY_L) ppl_strAppendChar(output, LSTR_LENGTH, &i, '$');
        ppl_strAppendf(output, LSTR_LENGTH, &i, "%s",
                       ppl_numericDisplay(GSL_NAN, c->numdispBuff[N], NSigFigs, (typeable == SW_DISPLAY_L)));
    } else {
        const double OoM = hypot(numberOutReal, numberOutImag) * pow(10, -NSigFigs);

        // In LaTeX, the number is in math mode. A complex number with significant real and imaginary parts is
        // bracketed.
        if (typeable == SW_DISPLAY_L) ppl_strAppendChar(output, LSTR_LENGTH, &i, '$');
        if ((fabs(numberOutReal) >= OoM) && (fabs(numberOutImag) > OoM))
            ppl_strAppendChar(output, LSTR_LENGTH, &i, '('); // open brackets on complex number
        // The real part, unless it is insignificant
        if (fabs(numberOutReal) >= OoM) {
            ppl_strAppendf(output, LSTR_LENGTH, &i, "%s",
                           ppl_numericDisplay(numberOutReal, c->numdispBuff[N], NSigFigs, (typeable == SW_DISPLAY_L)));
        }
        if ((fabs(numberOutReal) >= OoM) && (fabs(numberOutImag) > OoM) && (numberOutImag > 0))
            ppl_strAppendChar(output, LSTR_LENGTH, &i, '+');
        // The imaginary part, unless it is insignificant
        if (fabs(numberOutImag) > OoM) {
            if (fabs(numberOutImag - 1.0) >= OoM) // If this is false, imaginary part is 1, so print +i, not +1i
            {
                // Show the coefficient of i, unless it is -1, which is shown as -i
                if (fabs(numberOutImag + 1.0) >= OoM) {
                    ppl_strAppendf(
                            output, LSTR_LENGTH, &i, "%s",
                            ppl_numericDisplay(numberOutImag, c->numdispBuff[N], NSigFigs, (typeable == SW_DISPLAY_L)));
                    i += strlen(output + i);
                } else {
                    ppl_strAppendChar(output, LSTR_LENGTH, &i, '-');
                } // Just print -i, not -1i
            }
            // i itself is written as i, or, in typeable form, as sqrt(-1)
            if (typeable != SW_DISPLAY_T) {
                ppl_strAppendChar(output, LSTR_LENGTH, &i, 'i');
            } else if ((fabs(numberOutImag - 1.0) >= OoM) && (fabs(numberOutImag + 1.0) >= OoM)) {
                ppl_strAppendf(output, LSTR_LENGTH, &i, "%s", "*sqrt(-1)");
            } else {
                ppl_strAppendf(output, LSTR_LENGTH, &i, "%s", "sqrt(-1)");
            } // We've not printed 1 or -1, so nothing to multiply with
        }
        if ((fabs(numberOutReal) >= OoM) && (fabs(numberOutImag) > OoM))
            ppl_strAppendChar(output, LSTR_LENGTH, &i, ')'); // close brackets on complex number
    }

    // Append the units, separated by a space, or by a thin space in LaTeX
    if (unitstr[0] != '\0') {
        if (typeable == SW_DISPLAY_N) ppl_strAppendChar(output, LSTR_LENGTH, &i, ' ');
        // A thin space in LaTeX
        else if (typeable == SW_DISPLAY_L) {
            ppl_strAppendChar(output, LSTR_LENGTH, &i, '\\');
            ppl_strAppendChar(output, LSTR_LENGTH, &i, ',');
        }
        ppl_strAppendf(output, LSTR_LENGTH, &i, "%s", unitstr);
        i += strlen(output + i); // Add unit string as required
    }

    // Close math mode in LaTeX
    if (typeable == SW_DISPLAY_L) ppl_strAppendChar(output, LSTR_LENGTH, &i, '$');
    ppl_strAppendChar(output, LSTR_LENGTH, &i, '\0'); // null terminate string
    return output;
}

// -------------------------------------------------------------------
// Routines for printing units
// -------------------------------------------------------------------

//! unitInScheme - Test whether a unit belongs to the scheme of units which the user has selected (set unit scheme)
//! \param [in] c - The Pyxplot context
//! \param [in] u - The unit
//! \return True if the unit is in the selected scheme

static int unitInScheme(const ppl_context *c, const unit *u) {
    // Each unit records which schemes it belongs to
    switch (c->set->term_current.UnitScheme) {
        case SW_UNITSCH_SI:
            return u->si;
        case SW_UNITSCH_CGS:
            return u->cgs;
        case SW_UNITSCH_IMP:
            return u->imperial;
        case SW_UNITSCH_US:
            return u->us;
        case SW_UNITSCH_PLK:
            return u->planck;
        case SW_UNITSCH_ANC:
            return u->ancient;
        default:
            return 0;
    }
}


//! ppl_udFindOptimalNextUnit - Find the unit which best matches some of the remaining physical dimensions of a
//! quantity, and divide the quantity's dimensions by it. Used to build up a list of units with which to display a
//! quantity.
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - The quantity, whose dimensions are reduced by those of the chosen unit
//! \param [in] first - Flag indicating whether this is the first unit to be chosen, in which case a unit which
//! matches all of the quantity's dimensions is preferred
//! \param [out] best - Set to point to the chosen unit, or NULL if none was found
//! \param [out] pow - Set to the power to which the chosen unit is raised

void ppl_udFindOptimalNextUnit(const ppl_context *c, pplObj *in, const unsigned char first, unit **best, double *pow) {
    // For each candidate unit, power is the power to which it must be raised to match one dimension of the quantity,
    // and score counts how many of the quantity's dimensions it then matches
    int found = 0, BestScore = 0;

    // Consider every unit in the database
    for (int i = 0; i < c->unit_pos; i++) {
        if (!ppl_tempTypeMatch(in->tempType, c->unit_database[i].tempType))
            continue; // Don't convert between different temperature units
        // Try raising the unit to the power which matches each of the quantity's dimensions in turn, and score how many
        // dimensions then match
        for (int j = 0; j < UNITS_MAX_BASEUNITS; j++) {
            if ((c->unit_database[i].exponent[j] == 0) || (in->exponent[j] == 0)) continue;
            const double power = in->exponent[j] / c->unit_database[i].exponent[j];
            int score = 0;
            for (int k = 0; k < UNITS_MAX_BASEUNITS; k++)
                if (ppl_dblEqual(in->exponent[k], power * c->unit_database[i].exponent[k])) score++;

            // Some units, e.g. the hertz, may only be used on their own, to express the whole of a quantity's
            // dimensions
            if (c->unit_database[i].notToBeCompounded && ((!first) || (score < UNITS_MAX_BASEUNITS - 1))) continue;

            if (found == 0) // This is first possible unit we've found, and we have nothing to compare it to.
            {
                *best = c->unit_database + i;
                *pow = power;
                BestScore = score;
                found = 1;
            }

            // A user-preferred unit always beats a non-user-preferred unit
            if ((score >= BestScore) && ((c->unit_database[i].userSel)) && (!((*best)->userSel))) {
                *best = c->unit_database + i;
                *pow = power;
                BestScore = score;
                continue;
            }
            if ((score <= BestScore) && (!(c->unit_database[i].userSel)) && (((*best)->userSel))) continue;

            // A unit in the current scheme always beats one not in current scheme
            if ((unitInScheme(c, &c->unit_database[i])) && (!unitInScheme(c, *best))) {
                *best = c->unit_database + i;
                *pow = power;
                BestScore = score;
                continue;
            }
            if ((!unitInScheme(c, &c->unit_database[i])) && (unitInScheme(c, *best))) continue;

            // A unit which matches more dimensions wins
            if (score > BestScore) {
                *best = c->unit_database + i;
                *pow = power;
                BestScore = score;
                continue;
            }
            // Between equal scores, prefer a unit raised to the power of 1 or -1
            if ((score == BestScore) && (ppl_dblEqual(fabs(power), 1.0) && (!ppl_dblEqual(fabs(*pow), 1.0)))) {
                *best = c->unit_database + i;
                *pow = power;
                continue;
            }
            // ...and then a unit raised to the power of 1, rather than -1
            if ((score == BestScore) && (ppl_dblEqual(power, 1.0) && (!ppl_dblEqual(*pow, 1.0)))) {
                *best = c->unit_database + i;
                *pow = power;
                continue;
            }
            if (score < BestScore) continue;
        }
    }
    // No unit matches any of the remaining dimensions; otherwise remove the dimensions accounted for by the unit chosen
    if (found == 0) {
        *pow = 0;
        *best = NULL;
        return;
    }
    for (int j = 0; j < UNITS_MAX_BASEUNITS; j++) { in->exponent[j] -= (*pow) * (*best)->exponent[j]; }
    return;
}

//! ppl_udPrefixFix - Choose SI prefixes and dimensionally-equivalent alternative units for a list of units, so as
//! to make the number displayed alongside them as close to unity as possible
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - The quantity being displayed, whose value is rescaled to take account of the units chosen
//! \param [in|out] UnitList - The list of units with which the quantity will be displayed
//! \param [in] UnitPow - The power to which each unit in <UnitList> is raised
//! \param [out] UnitPref - The SI prefix chosen for each unit in <UnitList>
//! \param [in] Nunits - The number of units in <UnitList>

void ppl_udPrefixFix(const ppl_context *c, pplObj *in, unit **UnitList, const double *UnitPow, int *UnitPref,
                     const int Nunits) {
    double NewMagnitude, OldMagnitude;
    int BestPrefix = 0;

    // Apply unit multipliers to the value we're going to display
    for (int i = 0; i < Nunits; i++) {
        in->real /= pow(UnitList[i]->multiplier, UnitPow[i]);
        in->imag /= pow(UnitList[i]->multiplier, UnitPow[i]);
        UnitPref[i] = 0;
    }

    // Search for alternative dimensionally-equivalent units which give a smaller value
    for (int i = 0; i < Nunits; i++)
        for (int j = 0; j < c->unit_pos; j++)
            if (ppl_tempTypeMatch(UnitList[i]->tempType, c->unit_database[j].tempType) &&
                (ppl_unitsDimEqual3(UnitList[i], c->unit_database + j))) {
                OldMagnitude = hypot(in->real, in->imag);
                const double NewValueReal = in->real * pow(UnitList[i]->multiplier / c->unit_database[j].multiplier,
                                                     UnitPow[i]);
                const double NewValueImag = in->imag * pow(UnitList[i]->multiplier / c->unit_database[j].multiplier,
                                                     UnitPow[i]);
                NewMagnitude = hypot(NewValueReal, NewValueImag);

                // A user-preferred unit always beats a non-user-preferred unit
                if (((c->unit_database[j].userSel)) && (!(UnitList[i]->userSel))) {
                    UnitList[i] = c->unit_database + j;
                    in->real = NewValueReal;
                    in->imag = NewValueImag;
                    continue;
                }
                if ((!(c->unit_database[j].userSel)) && ((UnitList[i]->userSel))) continue;

                // A unit in the current scheme always beats one which is not
                if ((unitInScheme(c, &c->unit_database[j])) && (!unitInScheme(c, (UnitList[i])))) {
                    UnitList[i] = c->unit_database + j;
                    in->real = NewValueReal;
                    in->imag = NewValueImag;
                    continue;
                }
                if ((!unitInScheme(c, &c->unit_database[j])) && (unitInScheme(c, (UnitList[i])))) continue;

                // Otherwise, a unit with a smaller display value wins
                if ((NewMagnitude < OldMagnitude) && (NewMagnitude >= 1)) {
                    UnitList[i] = c->unit_database + j;
                    in->real = NewValueReal;
                    in->imag = NewValueImag;
                }
            }

    // Apply unit multiplier which arise from user-preferred SI prefixes, for example, millimetres
    for (int i = 0; i < Nunits; i++)
        // Prefixes are numbered 0 (yocto) to 16 (yotta), with 8 meaning no prefix, so prefix p multiplies by
        // 10^((p-8)*3)
        if (UnitList[i]->userSel) {
            in->real /= pow(10, (UnitList[i]->userSelPrefix - 8) * 3 * UnitPow[i]);
            in->imag /= pow(10, (UnitList[i]->userSelPrefix - 8) * 3 * UnitPow[i]);
            UnitPref[i] = UnitList[i]->userSelPrefix - 8;
        }

    // Search for an SI prefix we can use to reduce the size of this number
    if (c->set->term_current.UnitDisplayPrefix == SW_ONOFF_ON) {
        OldMagnitude = hypot(in->real, in->imag);
        int PrefixBestPos = -1;
        double PrefixBestVal = OldMagnitude;
        for (int i = 0; i < Nunits; i++)
            if (ppl_dblEqual(UnitPow[i], 1))
                if (UnitList[i]->userSel == 0)
                    // Try each SI prefix which this unit may take, e.g. km but not kg/1000, in steps of a factor of
                    // 1000
                    for (int j = UnitList[i]->minPrefix; j <= UnitList[i]->maxPrefix; j += 3) {
                        NewMagnitude = OldMagnitude / pow(10, j);
                        // Choose the prefix which gives the smallest value which is at least 1
                        if ((NewMagnitude >= 1) && ((NewMagnitude < PrefixBestVal) || (PrefixBestVal < 1))) {
                            PrefixBestPos = i;
                            BestPrefix = j;
                            PrefixBestVal = NewMagnitude;
                        }
                    }
        // Apply the best prefix found, if any
        if (PrefixBestPos >= 0) {
            in->real /= pow(10, BestPrefix);
            in->imag /= pow(10, BestPrefix);
            UnitPref[PrefixBestPos] = BestPrefix / 3;
        }
    }
    return;
}

//! ppl_printUnit - Render the physical units of a quantity as a string, choosing the units which make the numerical
//! value of the quantity as close to unity as possible
//! \param [in] c - The Pyxplot context
//! \param [in] in - The quantity whose units are to be rendered
//! \param [out] numberOutReal - Set to the real part of the quantity, expressed in the chosen units (optional)
//! \param [out] numberOutImag - Set to the imaginary part of the quantity, expressed in the chosen units (optional)
//! \param [in] N - Which of two static output buffers in the context to use (0 or 1)
//! \param [in] DivAllowed - If true, units with negative powers may be written after a /, e.g. m/s; otherwise they
//! are written with negative powers, e.g. m*s**-1
//! \param [in] typeable - The output style, one of the SW_DISPLAY_* constants, or zero to use the current setting
//! \return Pointer to the output buffer, which is overwritten by subsequent calls

char *ppl_printUnit(ppl_context *c, const pplObj *in, double *numberOutReal, double *numberOutImag, int N,
                    const int DivAllowed, int typeable) {
    char *output, *temp;
    pplObj residual = *in;
    unit *UnitList[UNITS_MAX_BASEUNITS];
    double UnitPow[UNITS_MAX_BASEUNITS];
    int UnitPref[UNITS_MAX_BASEUNITS];
    unsigned char UnitDisp[UNITS_MAX_BASEUNITS];
    double ExpMax = 0;
    int pos = 0, OutputPos = 0;
    int j = 0, l;
    const PreferredUnit *pu;

    // Use the user's display style unless the caller asked for one, and choose which of three static output buffers to
    // use, so that up to three units can be rendered at once
    if (typeable == 0) typeable = c->set->term_current.NumDisplay;

    // Choose the output buffer
    if (N == 0) output = c->udBuffA;
    else if (N == 1) output = c->udBuffB;
    else output = c->udBuffC;

    // Check whether input value is dimensionally equal to any preferred units
    listIterator *listiter = ppl_listIterateInit(c->unit_PreferredUnits);
    // Look for a unit, set with "set unit preferred", which matches the quantity's dimensions exactly
    while (listiter != NULL) {
        pu = (PreferredUnit *) listiter->data;
        if (ppl_unitsDimEqual(&pu->value, in) && (in->tempType == pu->value.tempType))
            break; // Preferred unit matches dimensions
        ppl_listIterate(&listiter);
    }

    if (listiter != NULL) // Found a preferred unit to use
    {
        // Use the preferred unit's list of units, powers and prefixes; the residual value is the number to display in
        // front of them
        for (int i = 0; i < pu->NUnits; i++) {
            UnitList[i] = c->unit_database + pu->UnitID[i];
            UnitPow[i] = pu->exponent[i];
            UnitPref[i] = (pu->prefix[i] >= 0) ? pu->prefix[i] - 8 : 0;
            UnitDisp[i] = 0;
        }
        pos = pu->NUnits;
        residual.real /= pu->value.real;
        residual.imag /= pu->value.real;
        for (int i = 0; i < UNITS_MAX_BASEUNITS; i++) residual.exponent[i] = 0;
        residual.dimensionless = 1;
    } else // Not using a preferred unit
    {
        // Dimensionless quantities need no units
        if (in->dimensionless != 0) {
            output[0] = '\0';
            if (numberOutReal != NULL) *numberOutReal = in->real;
            if (numberOutImag != NULL) *numberOutImag = in->imag;
            return output;
        }

        // Find a list of units which multiply together to match dimensions of quantity to display
        while (1) {
            // Add units one at a time until every dimension is accounted for
            if (pos >= UNITS_MAX_BASEUNITS) {
                ppl_error(&c->errcontext, ERR_INTERNAL, -1, -1, "Overflow whilst trying to display a unit.");
                break;
            }
            ppl_udFindOptimalNextUnit(c, &residual, pos == 0, UnitList + pos, UnitPow + pos);
            UnitDisp[pos] = 0;
            if (ppl_dblEqual(UnitPow[pos], 0) != 0) break;
            pos++;
        }

        // Go through list of units and fix prefixes / unit choice to minimise displayed number
        ppl_udPrefixFix(c, &residual, UnitList, UnitPow, UnitPref, pos);
    }

    // Display units one by one
    int first = 1;
    // Write the units in order of decreasing power, so that those with positive powers come first, and those with
    // negative powers can follow a /
    while (1) {
        int found = 0;
        for (int i = 0; i < pos; i++)
            // Find the unit with the highest power which has not yet been written
            if ((UnitDisp[i] == 0) && ((found == 0) || (UnitPow[i] > ExpMax))) {
                ExpMax = UnitPow[i];
                found = 1;
                j = i;
            }
        if (found == 0) break;
        // In typeable form, units are written as *unit(...)
        if ((typeable == SW_DISPLAY_T) && first) {
            ppl_strAppendf(output, LSTR_LENGTH, &OutputPos, "%s", "*unit(");
        }
        if (!first) // Print * or /
        {
            // Units after the first are separated by * (a thin space in LaTeX), or by / for negative powers if division
            // is allowed
            if ((!DivAllowed) || (UnitPow[j] > 0) || (ppl_dblEqual(UnitPow[j], 0))) {
                // A thin space in LaTeX
                if (typeable == SW_DISPLAY_L) {
                    ppl_strAppendChar(output, LSTR_LENGTH, &OutputPos, '\\');
                    ppl_strAppendChar(output, LSTR_LENGTH, &OutputPos, ',');
                } else ppl_strAppendChar(output, LSTR_LENGTH, &OutputPos, '*');
            } else {
                ppl_strAppendChar(output, LSTR_LENGTH, &OutputPos, '/');
            }
        }
        // In LaTeX, unit names are set in roman type
        if (typeable == SW_DISPLAY_L) {
            ppl_strAppendf(output, LSTR_LENGTH, &OutputPos, "%s", "\\mathrm{");
        }
        if (UnitPref[j] != 0) // Print SI prefix
        {
            // The SI prefix, abbreviated or in full
            if (c->set->term_current.UnitDisplayAbbrev == SW_ONOFF_ON) {
                if (typeable != SW_DISPLAY_L)
                    ppl_strAppendf(output, LSTR_LENGTH, &OutputPos, "%s", SIprefixes_abbrev[UnitPref[j] + 8]);
                else ppl_strAppendf(output, LSTR_LENGTH, &OutputPos, "%s", SIprefixes_latex[UnitPref[j] + 8]);
            } else {
                ppl_strAppendf(output, LSTR_LENGTH, &OutputPos, "%s", SIprefixes_full[UnitPref[j] + 8]);
            }
            OutputPos += strlen(output + OutputPos);
        }
        // The name of the unit, abbreviated or in full, and singular or plural; units with negative powers are
        // singular, e.g. metres per second
        if (c->set->term_current.UnitDisplayAbbrev == SW_ONOFF_ON) {
            // Abbreviated names, in plain text or in LaTeX
            if (typeable != SW_DISPLAY_L) {
                if (UnitPow[j] >= 0)
                    ppl_strAppendf(output, LSTR_LENGTH, &OutputPos, "%s",
                                   UnitList[j]->nameAp); // Use abbreviated name for unit
                else ppl_strAppendf(output, LSTR_LENGTH, &OutputPos, "%s", UnitList[j]->nameAs);
            } else {
                if (UnitPow[j] >= 0)
                    ppl_strAppendf(output, LSTR_LENGTH, &OutputPos, "%s",
                                   UnitList[j]->nameLp); // Use abbreviated LaTeX name for unit
                else ppl_strAppendf(output, LSTR_LENGTH, &OutputPos, "%s", UnitList[j]->nameLs);
            }
        } else {
            if (UnitPow[j] >= 0) temp = UnitList[j]->nameFp; // Use full name for unit...
            else temp = UnitList[j]->nameFs;

            // Full names, in plain text or in LaTeX
            if (typeable != SW_DISPLAY_L) {
                ppl_strAppendf(output, LSTR_LENGTH, &OutputPos, "%s", temp);
            } // ... either as it comes
            // In LaTeX, underscores in names must be escaped
            else {
                // Copy the name, escaping underscores
                for (int k = l = 0; temp[k] != '\0'; k++) {
                    if (temp[k] == '_')
                        ppl_strAppendChar(output, LSTR_LENGTH, &OutputPos,
                                          '\\'); // ... or with escaped underscores for LaTeX
                    ppl_strAppendChar(output, LSTR_LENGTH, &OutputPos, temp[k]);
                }
            }
        }
        OutputPos += strlen(output + OutputPos);
        if (typeable == SW_DISPLAY_L) { ppl_strAppendChar(output, LSTR_LENGTH, &OutputPos, '}'); }
        if (((first || (!DivAllowed)) && (!ppl_dblEqual(UnitPow[j], 1))) ||
            (((!first) || DivAllowed) && (!ppl_dblEqual(fabs(UnitPow[j]), 1)))) // Print power
        {
            // Write the power, unless it is 1 (or -1 after a /), as ^{n} in LaTeX or **n otherwise
            if (typeable == SW_DISPLAY_L) {
                ppl_strAppendChar(output, LSTR_LENGTH, &OutputPos, '^');
                ppl_strAppendChar(output, LSTR_LENGTH, &OutputPos, '{');
            } else {
                ppl_strAppendChar(output, LSTR_LENGTH, &OutputPos, '*');
                ppl_strAppendChar(output, LSTR_LENGTH, &OutputPos, '*');
            }
            if ((first) || (!DivAllowed))
                ppl_strAppendf(output, LSTR_LENGTH, &OutputPos, "%s",
                               ppl_numericDisplay(UnitPow[j], c->numdispBuff[N],
                                                  c->set->term_current.SignificantFigures, (typeable == SW_DISPLAY_L)));
            else
                ppl_strAppendf(output, LSTR_LENGTH, &OutputPos, "%s",
                               ppl_numericDisplay(fabs(UnitPow[j]), c->numdispBuff[N],
                                                  c->set->term_current.SignificantFigures, (typeable == SW_DISPLAY_L)));
            OutputPos += strlen(output + OutputPos);
            if (typeable == SW_DISPLAY_L) { ppl_strAppendChar(output, LSTR_LENGTH, &OutputPos, '}'); }
        }
        UnitDisp[j] = 1;
        first = 0;
    }

    // Clean up and return
    if (typeable == SW_DISPLAY_T) ppl_strAppendChar(output, LSTR_LENGTH, &OutputPos, ')');
    output[OutputPos] = '\0';
    if (numberOutReal != NULL) *numberOutReal = residual.real;
    if (numberOutImag != NULL) *numberOutImag = residual.imag;
    return output;
}

// ------------------------------------------------
// Function to evaluate strings of the form "m/s"
// ------------------------------------------------

//! ppl_unitNameCmp - Test whether a string begins with the name of a unit, followed by a non-alphanumeric character
//! \param [in] in - The string to test
//! \param [in] unit - The name of the unit (may be NULL)
//! \param [in] caseSensitive - If true, the comparison is case-sensitive
//! \return The length of the unit name if it matches, or zero otherwise

int ppl_unitNameCmp(const char *in, const char *unit, const unsigned char caseSensitive) {
    int k;
    if (unit == NULL) return 0;
    if (caseSensitive)
        for (k = 0; ((unit[k] != '\0') && (unit[k] == in[k])); k++);
    else
        for (k = 0; ((unit[k] != '\0') && (toupper(unit[k]) == toupper(in[k]))); k++);
    if ((unit[k] == '\0') && (!(isalnum(in[k]) || (in[k] == '_')))) return k;
    return 0;
}

//! ppl_unitsStringEvaluate - Evaluate a string describing a physical unit, such as "m/s**2", into a quantity
//! \param [in] c - The Pyxplot context
//! \param [in] in - The string to evaluate
//! \param [out] out - The quantity described by the string
//! \param [out] end - Set to the number of characters of <in> which were read
//! \param [out] errpos - Set to the position in <in> of any error, or -1 if there was no error
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void ppl_unitsStringEvaluate(const ppl_context *c, const char *in, pplObj *out, int *end, int *errpos, char *errText) {
    // i is the position reached in the string; power is the power of the unit being read; powerneg is -1 after a / and
    // 0 when the string has ended; p is set when a unit name has been matched
    int i = 0, j = 0, k, m;
    double power = 1.0, powerneg = 1.0;
    pplObjNum(out, out->amMalloced, 0.0, 0.0);

    // The string may begin with a number, e.g. "1000 m", optionally followed by * or /
    while (((unsigned char) in[i] <= ' ') && (in[i] != '\0')) i++;
    out->real = ppl_getFloat(in + i, &j); // Unit strings can have numbers out the front
    if (j < 0) j = 0;
    i += j;
    // Without a number, the multiplier is 1
    if (j == 0) {
        out->real = 1.0;
    } else {
        while (((unsigned char) in[i] <= ' ') && (in[i] != '\0')) i++;
        if (in[i] == '*') i++;
        // A / after the number makes the following unit an inverse
        else if (in[i] == '/') {
            i++;
            powerneg = -1.0;
        }
    }

    // Read units one at a time, each optionally followed by a power and then * or /
    while (powerneg != 0.0) {
        int p = 0;
        while (((unsigned char) in[i] <= ' ') && (in[i] != '\0')) i++;
        // Try to match each unit in the database: abbreviated names are case-sensitive, and full names and alternative
        // names case-insensitive
        for (j = 0; j < c->unit_pos; j++) {
            double multiplier = 1.0;
            if ((k = ppl_unitNameCmp(in + i, c->unit_database[j].nameAp, 1)) != 0) p = 1;
            else if ((k = ppl_unitNameCmp(in + i, c->unit_database[j].nameAs, 1)) != 0) p = 1;
            else if ((k = ppl_unitNameCmp(in + i, c->unit_database[j].nameFp, 0)) != 0) p = 1;
            else if ((k = ppl_unitNameCmp(in + i, c->unit_database[j].nameFs, 0)) != 0) p = 1;
            else if ((k = ppl_unitNameCmp(in + i, c->unit_database[j].nameFp, 0)) != 0) p = 1;
            else if ((k = ppl_unitNameCmp(in + i, c->unit_database[j].alt1, 0)) != 0) p = 1;
            else if ((k = ppl_unitNameCmp(in + i, c->unit_database[j].alt2, 0)) != 0) p = 1;
            else if ((k = ppl_unitNameCmp(in + i, c->unit_database[j].alt3, 0)) != 0) p = 1;
            else if ((k = ppl_unitNameCmp(in + i, c->unit_database[j].alt4, 0)) != 0) p = 1;
            // Otherwise try each SI prefix which the unit can take, followed by its name
            else {
                // Prefixes are numbered 0 (yocto) to 16 (yotta), with 8 meaning no prefix
                for (int l = c->unit_database[j].minPrefix / 3 + 8; l <= c->unit_database[j].maxPrefix / 3 + 8; l++) {
                    if (l == 8) continue;
                    for (k = 0;
                         ((SIprefixes_full[l][k] != '\0') && (toupper(SIprefixes_full[l][k]) == toupper(in[i + k])));
                         k++);
                    // A full prefix, e.g. kilo, must be followed by a full or alternative name, e.g. kilometres
                    if (SIprefixes_full[l][k] == '\0') {
                        // Try each of the full and alternative names
                        if ((m = ppl_unitNameCmp(in + i + k, c->unit_database[j].nameFp, 0)) != 0) {
                            p = 1;
                            k += m;
                            multiplier = pow(10, (l - 8) * 3);
                            break;
                        } else if ((m = ppl_unitNameCmp(in + i + k, c->unit_database[j].nameFs, 0)) != 0) {
                            p = 1;
                            k += m;
                            multiplier = pow(10, (l - 8) * 3);
                            break;
                        } else if ((m = ppl_unitNameCmp(in + i + k, c->unit_database[j].alt1, 0)) != 0) {
                            p = 1;
                            k += m;
                            multiplier = pow(10, (l - 8) * 3);
                            break;
                        } else if ((m = ppl_unitNameCmp(in + i + k, c->unit_database[j].alt2, 0)) != 0) {
                            p = 1;
                            k += m;
                            multiplier = pow(10, (l - 8) * 3);
                            break;
                        } else if ((m = ppl_unitNameCmp(in + i + k, c->unit_database[j].alt3, 0)) != 0) {
                            p = 1;
                            k += m;
                            multiplier = pow(10, (l - 8) * 3);
                            break;
                        } else if ((m = ppl_unitNameCmp(in + i + k, c->unit_database[j].alt4, 0)) != 0) {
                            p = 1;
                            k += m;
                            multiplier = pow(10, (l - 8) * 3);
                            break;
                        }
                    }
                    for (k = 0; ((SIprefixes_abbrev[l][k] != '\0') && (SIprefixes_abbrev[l][k] == in[i + k])); k++);
                    // An abbreviated prefix, e.g. k, must be followed by an abbreviated name, e.g. km
                    if (SIprefixes_abbrev[l][k] == '\0') {
                        // Try the plural and singular abbreviations
                        if ((m = ppl_unitNameCmp(in + i + k, c->unit_database[j].nameAp, 1)) != 0) {
                            p = 1;
                            k += m;
                            multiplier = pow(10, (l - 8) * 3);
                            break;
                        } else if ((m = ppl_unitNameCmp(in + i + k, c->unit_database[j].nameAs, 1)) != 0) {
                            p = 1;
                            k += m;
                            multiplier = pow(10, (l - 8) * 3);
                            break;
                        }
                    }
                }
            }
            if (p == 0) continue;
            i += k;
            while (((unsigned char) in[i] <= ' ') && (in[i] != '\0')) i++;
            // A power may follow the unit, written as ^n or **n; the comma expressions skip over the operator once it
            // has been recognised
            if (((in[i] == '^') && (i++, 1)) || (((in[i] == '*') && (in[i + 1] == '*')) && (i += 2, 1))) {
                power = ppl_getFloat(in + i, &k);
                // The power must be a number
                if (k <= 0) {
                    *errpos = i;
                    ppl_snprintfTrunc(errText, PPL_MSG_LENGTH, "%s",
                                      "Syntax Error: Was expecting a numerical constant here.");
                    return;
                }
                i += k;
                while (((unsigned char) in[i] <= ' ') && (in[i] != '\0')) i++;
            }

            // Units of temperature on different scales, e.g. oC and oF, cannot be mixed in one quantity
            if (c->unit_database[j].tempType != 0) {
                // A second, different temperature scale is an error
                if ((out->tempType > 0) && (out->tempType != c->unit_database[j].tempType)) {
                    *errpos = i;
                    ppl_snprintfTrunc(
                            errText, PPL_MSG_LENGTH, "%s",
                            "Unit Error: Attempt to mix Kelvin, oC and oF in the units of a single quantity. Try again with all quantities converted into Kelvin. Type 'help units temperatures' for more details.");
                    return;
                }
                out->tempType = c->unit_database[j].tempType;
            }

            // Multiply the quantity by the unit raised to its power, which is negative after a /
            for (k = 0; k < UNITS_MAX_BASEUNITS; k++)
                out->exponent[k] += c->unit_database[j].exponent[k] * power * powerneg;
            if (ppl_dblEqual(out->exponent[UNIT_TEMPERATURE], 0))
                out->tempType = 0; // We've lost our temperature dependence
            out->real *= pow(multiplier * c->unit_database[j].multiplier, power * powerneg);
            power = 1.0;
            // A * or / introduces another unit; anything else ends the string
            if (in[i] == '*') {
                powerneg = 1.0;
                i++;
            } else if (in[i] == '/') {
                powerneg = -1.0;
                i++;
            } else {
                powerneg = 0.0;
            }
            break;
        }
        // No unit matched: the string may end here, or with a closing bracket, otherwise the unit is unknown
        if (p == 0) {
            // The end of the string
            if ((in[i] == ')') || (in[i] == '\0')) {
                powerneg = 0.0;
            } else {
                *errpos = i;
                ppl_snprintfTrunc(errText, PPL_MSG_LENGTH, "%s", "No such unit.");
                return;
            }
        }
    }
    j = 1;
    for (k = 0; k < UNITS_MAX_BASEUNITS; k++)
        if (ppl_dblEqual(out->exponent[k], 0) == 0) j = 0;
    out->dimensionless = j;
    if (end != NULL) *end = i;
    return;
}

//! ppl_newPreferredUnit - Create a description of a preferred unit, from a string such as "km/s", for use by the
//! "set unit preferred" command
//! \param [in] c - The Pyxplot context
//! \param [out] output - Set to point to a newly malloced preferred-unit structure, or NULL on failure
//! \param [in] instr - The string describing the preferred unit
//! \param [in] OutputContext - Unused
//! \param [in|out] errpos - The position in <instr> of any error, or -1 if there was no error. If this is already
//! non-negative on entry, the function returns immediately.
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void ppl_newPreferredUnit(ppl_context *c, PreferredUnit **output, char *instr, const int OutputContext, int *errpos,
                          char *errText) {
    int i, k, m;
    double power = 1.0, powerneg = 1.0;
    pplObj UnitVal;

    *output = NULL;
    *errpos = -1;
    int end = strlen(instr);
    ppl_unitsStringEvaluate(c, instr, &UnitVal, &end, errpos, errText);
    if (*errpos >= 0) return; // Error in parsing unit expression
    *output = (PreferredUnit *) malloc(sizeof(PreferredUnit));
    if (*output == NULL) {
        *errpos = 0;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Out of memory.");
        return;
    }
    (*output)->value = UnitVal;
    (*output)->modified = 1;

    // Read list of units from preferred unit string
    int outpos = i = 0;
    // Read the string again, this time recording each unit, its prefix and its power, so that quantities with these
    // dimensions can be displayed in these units
    while ((powerneg != 0.0) && (outpos < UNITS_MAX_BASEUNITS)) {
        int PrefixOut = -1;
        int p = 0;
        while (((unsigned char) instr[i] <= ' ') && (instr[i] != '\0')) i++;
        // Try to match each unit in the database, by its names, or by an SI prefix followed by its names, as in
        // ppl_unitsStringEvaluate()
        for (int j = 0; j < c->unit_pos; j++) {
            if ((k = ppl_unitNameCmp(instr + i, c->unit_database[j].nameAp, 1)) != 0) p = 1;
            else if ((k = ppl_unitNameCmp(instr + i, c->unit_database[j].nameAs, 1)) != 0) p = 1;
            else if ((k = ppl_unitNameCmp(instr + i, c->unit_database[j].nameFp, 0)) != 0) p = 1;
            else if ((k = ppl_unitNameCmp(instr + i, c->unit_database[j].nameFs, 0)) != 0) p = 1;
            else if ((k = ppl_unitNameCmp(instr + i, c->unit_database[j].nameFp, 0)) != 0) p = 1;
            else if ((k = ppl_unitNameCmp(instr + i, c->unit_database[j].alt1, 0)) != 0) p = 1;
            else if ((k = ppl_unitNameCmp(instr + i, c->unit_database[j].alt2, 0)) != 0) p = 1;
            else if ((k = ppl_unitNameCmp(instr + i, c->unit_database[j].alt3, 0)) != 0) p = 1;
            else if ((k = ppl_unitNameCmp(instr + i, c->unit_database[j].alt4, 0)) != 0) p = 1;
            // Otherwise try each SI prefix which the unit can take, followed by its name
            else {
                // Prefixes are numbered 0 (yocto) to 16 (yotta), with 8 meaning no prefix
                for (int l = c->unit_database[j].minPrefix / 3 + 8; l <= c->unit_database[j].maxPrefix / 3 + 8; l++) {
                    if (l == 8) continue;
                    for (k = 0;
                         ((SIprefixes_full[l][k] != '\0') && (toupper(SIprefixes_full[l][k]) == toupper(instr[i + k])));
                         k++);
                    // A full prefix, e.g. kilo, must be followed by a full or alternative name
                    if (SIprefixes_full[l][k] == '\0') {
                        // Try each of the full and alternative names
                        if ((m = ppl_unitNameCmp(instr + i + k, c->unit_database[j].nameFp, 0)) != 0) {
                            p = 1;
                            k += m;
                            PrefixOut = l;
                            break;
                        } else if ((m = ppl_unitNameCmp(instr + i + k, c->unit_database[j].nameFs, 0)) != 0) {
                            p = 1;
                            k += m;
                            PrefixOut = l;
                            break;
                        } else if ((m = ppl_unitNameCmp(instr + i + k, c->unit_database[j].alt1, 0)) != 0) {
                            p = 1;
                            k += m;
                            PrefixOut = l;
                            break;
                        } else if ((m = ppl_unitNameCmp(instr + i + k, c->unit_database[j].alt2, 0)) != 0) {
                            p = 1;
                            k += m;
                            PrefixOut = l;
                            break;
                        } else if ((m = ppl_unitNameCmp(instr + i + k, c->unit_database[j].alt3, 0)) != 0) {
                            p = 1;
                            k += m;
                            PrefixOut = l;
                            break;
                        } else if ((m = ppl_unitNameCmp(instr + i + k, c->unit_database[j].alt4, 0)) != 0) {
                            p = 1;
                            k += m;
                            PrefixOut = l;
                            break;
                        }
                    }
                    for (k = 0; ((SIprefixes_abbrev[l][k] != '\0') && (SIprefixes_abbrev[l][k] == instr[i + k])); k++);
                    // An abbreviated prefix, e.g. k, must be followed by an abbreviated name
                    if (SIprefixes_abbrev[l][k] == '\0') {
                        // Try the plural and singular abbreviations
                        if ((m = ppl_unitNameCmp(instr + i + k, c->unit_database[j].nameAp, 1)) != 0) {
                            p = 1;
                            k += m;
                            PrefixOut = l;
                            break;
                        } else if ((m = ppl_unitNameCmp(instr + i + k, c->unit_database[j].nameAs, 1)) != 0) {
                            p = 1;
                            k += m;
                            PrefixOut = l;
                            break;
                        }
                    }
                }
            }
            if (p == 0) continue;
            i += k;
            while (((unsigned char) instr[i] <= ' ') && (instr[i] != '\0')) i++;
            // A power may follow the unit, written as ^n or **n
            if (((instr[i] == '^') && (i++, 1)) || (((instr[i] == '*') && (instr[i + 1] == '*')) && (i += 2, 1))) {
                power = ppl_getFloat(instr + i, &k);
                // The power must be a number
                if (k <= 0) {
                    *errpos = i;
                    ppl_snprintfTrunc(errText, LSTR_LENGTH, "%s",
                                      "Syntax Error: Was expecting a numerical constant here.");
                    return;
                }
                i += k;
                while (((unsigned char) instr[i] <= ' ') && (instr[i] != '\0')) i++;
            }
            (*output)->UnitID[outpos] = j;
            (*output)->prefix[outpos] = PrefixOut;
            (*output)->exponent[outpos] = power * powerneg;
            outpos++;
            power = 1.0;
            // A * or / introduces another unit; anything else ends the string
            if (instr[i] == '*') {
                powerneg = 1.0;
                i++;
            } else if (instr[i] == '/') {
                powerneg = -1.0;
                i++;
            } else {
                powerneg = 0.0;
            }
            break;
        }
        // No unit matched: the string may end here, or with a closing bracket, otherwise the unit is unknown
        if (p == 0) {
            // The end of the string
            if ((instr[i] == ')') || (instr[i] == '\0')) {
                powerneg = 0.0;
            } else {
                *errpos = i;
                ppl_snprintfTrunc(errText, LSTR_LENGTH, "%s", "No such unit.");
                return;
            }
        }
    }

    // Record how many units were read
    (*output)->NUnits = outpos;
    return;
}

//! ppl_numericDisplayDefault - Render a number as a string, using the number of significant figures and the display
//! style (natural or LaTeX) which the user has selected
//! \param [in] c - The Pyxplot context
//! \param [in] x - The number
//! \param [in] N - Which of the context's four numeric display buffers (0-3) to write the string into
//! \return Pointer to the string, which is overwritten by later calls using the same buffer

char *ppl_numericDisplayDefault(ppl_context *c, const double x, const int N) {
    return ppl_numericDisplay(x, c->numdispBuff[N], c->set->term_current.SignificantFigures,
                              (c->set->term_current.NumDisplay == SW_DISPLAY_L));
}
