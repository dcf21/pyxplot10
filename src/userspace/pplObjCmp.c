// pplObjCmp.c
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

#include <stdlib.h>
#include <math.h>
#include <string.h>

#include <gsl/gsl_math.h>

#include "mathsTools/dcfmath.h"
#include "stringTools/asciidouble.h"
#include "settings/settingTypes.h"
#include "userspace/context.h"
#include "userspace/pplObj.h"
#include "userspace/pplObjCmp.h"
#include "userspace/unitsArithmetic.h"
#include "userspace/unitsDisp.h"

//! pplcol_RGBtoHSB - Convert a color from RGB to HSB components
//! \param [in] ri - Red component, 0-1
//! \param [in] gi - Green component, 0-1
//! \param [in] bi - Blue component, 0-1
//! \param [out] ho - Hue component, 0-1
//! \param [out] so - Saturation component, 0-1
//! \param [out] bo - Brightness component, 0-1

void pplcol_RGBtoHSB(const double ri, const double gi, const double bi, double *ho, double *so, double *bo) {
    const double M = ppl_max3(ri, gi, bi);
    const double m = ppl_min3(ri, gi, bi);
    const double C = M - m;
    if (C == 0) *ho = 0;
    else if (M == ri) *ho = fmod((gi - bi) / C, 6);
    else if (M == gi) *ho = (bi - ri) / C + 2;
    else *ho = (ri - gi) / C + 4;
    *ho /= 6.0;
    *so = C / M;
    *bo = M;
    if (!gsl_finite(*so)) *so = 0;
    if (*ho < 0) *ho = 0;
    if (*ho > 1) *ho = 1;
    if (*so < 0) *so = 0;
    if (*so > 1) *so = 1;
    if (*bo < 0) *bo = 0;
    if (*bo > 1) *bo = 1;
}

//! pplcol_CMYKtoRGB - Convert a color from CMYK to RGB components
//! \param [in] ci - Cyan component, 0-1
//! \param [in] mi - Magenta component, 0-1
//! \param [in] yi - Yellow component, 0-1
//! \param [in] ki - Key (black) component, 0-1
//! \param [out] ro - Red component, 0-1
//! \param [out] go - Green component, 0-1
//! \param [out] bo - Blue component, 0-1

void pplcol_CMYKtoRGB(const double ci, const double mi, const double yi, const double ki, double *ro, double *go,
                      double *bo) {
    *ro = 1.0 - (ci + ki);
    *go = 1.0 - (mi + ki);
    *bo = 1.0 - (yi + ki);
    if (*ro < 0) *ro = 0;
    if (*ro > 1) *ro = 1;
    if (*go < 0) *go = 0;
    if (*go > 1) *go = 1;
    if (*bo < 0) *bo = 0;
    if (*bo > 1) *bo = 1;
}

//! pplcol_HSBtoRGB - Convert a color from HSB to RGB components
//! \param [in] hi - Hue component, 0-1
//! \param [in] si - Saturation component, 0-1
//! \param [in] bi - Brightness component, 0-1
//! \param [out] ro - Red component, 0-1
//! \param [out] go - Green component, 0-1
//! \param [out] bo - Blue component, 0-1

void pplcol_HSBtoRGB(double hi, const double si, const double bi, double *ro, double *go, double *bo) {
    const double ch = si * bi;
    double h2;
    const int h2i = (int) (h2 = hi * 6);
    const double x = ch * (1.0 - fabs(fmod(h2, 2) - 1.0));
    const double m = bi - ch;
    switch (h2i) {
        case 0:
            *ro = ch;
            *go = x;
            *bo = 0;
            break;
        case 1:
            *ro = x;
            *go = ch;
            *bo = 0;
            break;
        case 2:
            *ro = 0;
            *go = ch;
            *bo = x;
            break;
        case 3:
            *ro = 0;
            *go = x;
            *bo = ch;
            break;
        case 4:
            *ro = x;
            *go = 0;
            *bo = ch;
            break;
        case 5:
        case 6:
            *ro = ch;
            *go = 0;
            *bo = x;
            break; // case 6 is for hue=1.0 only
        default:
            *ro = 0;
            *go = 0;
            *bo = 0;
            break;
    }
    *ro += m;
    *go += m;
    *bo += m;

    if (*ro < 0) *ro = 0;
    if (*ro > 1) *ro = 1;
    if (*go < 0) *go = 0;
    if (*go > 1) *go = 1;
    if (*bo < 0) *bo = 0;
    if (*bo > 1) *bo = 1;
}

//! pplcol_CMYKtoHSB - Convert a color from CMYK to HSB components
//! \param [in] ci - Cyan component, 0-1
//! \param [in] mi - Magenta component, 0-1
//! \param [in] yi - Yellow component, 0-1
//! \param [in] ki - Key (black) component, 0-1
//! \param [out] ho - Hue component, 0-1
//! \param [out] so - Saturation component, 0-1
//! \param [out] bo - Brightness component, 0-1

void pplcol_CMYKtoHSB(const double ci, const double mi, const double yi, const double ki, double *ho, double *so,
                      double *bo) {
    double r, g, b;
    pplcol_CMYKtoRGB(ci, mi, yi, ki, &r, &g, &b);
    pplcol_RGBtoHSB(r, g, b, ho, so, bo);
}

//! pplcol_RGBtoCMYK - Convert a color from RGB to CMYK components
//! \param [in] ri - Red component, 0-1
//! \param [in] gi - Green component, 0-1
//! \param [in] bi - Blue component, 0-1
//! \param [out] co - Cyan component, 0-1
//! \param [out] mo - Magenta component, 0-1
//! \param [out] yo - Yellow component, 0-1
//! \param [out] ko - Key (black) component, 0-1

void pplcol_RGBtoCMYK(const double ri, const double gi, const double bi, double *co, double *mo, double *yo,
                      double *ko) {
    *ko = 1.0 - ppl_max3(ri, gi, bi);
    *co = 1.0 - ri - *ko;
    *mo = 1.0 - gi - *ko;
    *yo = 1.0 - bi - *ko;
    if (*co < 0) *co = 0;
    if (*co > 1) *co = 1;
    if (*mo < 0) *mo = 0;
    if (*mo > 1) *mo = 1;
    if (*yo < 0) *yo = 0;
    if (*yo > 1) *yo = 1;
    if (*ko < 0) *ko = 0;
    if (*ko > 1) *ko = 1;
}

//! pplcol_HSBtoCMYK - Convert a color from HSB to CMYK components
//! \param [in] hi - Hue component, 0-1
//! \param [in] si - Saturation component, 0-1
//! \param [in] bi - Brightness component, 0-1
//! \param [out] co - Cyan component, 0-1
//! \param [out] mo - Magenta component, 0-1
//! \param [out] yo - Yellow component, 0-1
//! \param [out] ko - Key (black) component, 0-1

void pplcol_HSBtoCMYK(const double hi, const double si, const double bi, double *co, double *mo, double *yo,
                      double *ko) {
    double r, g, b;
    pplcol_HSBtoRGB(hi, si, bi, &r, &g, &b);
    pplcol_RGBtoCMYK(r, g, b, co, mo, yo, ko);
}

//! SGN - Return the sign of an integer
//! \param [in] x - The input integer
//! \return -1 if <x> is negative, 1 if it is positive, or 0 if it is zero

static int SGN(const int x) {
    if (x == 0) return 0;
    if (x < 0) return -1;
    return 1;
}

//! pplObjCmpQuiet - Comparison function for sorting arrays of pointers to objects with qsort()
//! \param [in] a - Pointer to a pointer to the first object
//! \param [in] b - Pointer to a pointer to the second object
//! \return -1, 0 or 1 if the first object is less than, equal to or greater than the second, or -2 if they cannot
//! be compared

int pplObjCmpQuiet(const void *a, const void *b) {
    const pplObj *pa = *(pplObj **) a;
    const pplObj *pb = *(pplObj **) b;
    return pplObjCmp(NULL, pa, pb, NULL, NULL, NULL, 0);
}

//! pplObjCmp - Compare two objects. Objects of different types are ordered by type; objects of the same type are
//! compared by value, recursing into the members of compound objects.
//! \param [in] c - The Pyxplot context (may be NULL if <status> is NULL)
//! \param [in] a - The first object
//! \param [in] b - The second object
//! \param [out] status - Set to one if the objects could not be compared, e.g. numbers with different physical
//! dimensions (optional)
//! \param [out] errType - Set to the type of error, if the objects could not be compared
//! \param [out] errText - Character buffer into which to write an error message, if the objects could not be compared
//! \param [in] iterDepth - The depth of recursion into compound objects, used to prevent infinite recursion
//! \return -1, 0 or 1 if <a> is less than, equal to or greater than <b>, or -2 if they cannot be compared

int pplObjCmp(ppl_context *c, const pplObj *a, const pplObj *b, int *status, int *errType, char *errText,
              const int iterDepth) {
    int t1 = a->objType;
    int t2 = b->objType;
    const int t1o = pplObjTypeOrder[t1];
    const int t2o = pplObjTypeOrder[t2];
    if (cancellationFlag) return 0;
    if (iterDepth > 250) return 0;
    if ((c != NULL) && (t1o != t2o)) {
        *status = 1;
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(errText, LSTR_LENGTH,
                          "Attempt to compare an object of type <%s> with an object of type <%s>.", pplObjTypeNames[t1],
                          pplObjTypeNames[t2]);
        return -2;
    }
    if (t1o < t2o) return -1;
    if (t1o > t2o) return 1;
    if (t1o == 0) return -2; // 0 - nulls are never equal
    if (t1o == 2) // 2 - numbers or booleans
    {
        if (!ppl_unitsDimEqual(a, b)) {
            if (c == NULL) {
                int i;
                for (i = 0; i < UNITS_MAX_BASEUNITS; i++) {
                    if (a->exponent[i] < b->exponent[i]) return -1;
                    if (a->exponent[i] > b->exponent[i]) return 1;
                }
            } else {
                *status = 1;
                *errType = ERR_UNIT;
                if (a->dimensionless)
                    ppl_snprintfTrunc(
                            errText, LSTR_LENGTH,
                            "Attempt to compare a quantity which is dimensionless with one with dimensions of <%s>.",
                            ppl_printUnit(c, b, NULL, NULL, 1, 1, 0));
                else if (b->dimensionless)
                    ppl_snprintfTrunc(
                            errText, LSTR_LENGTH,
                            "Attempt to compare a quantity with dimensions of <%s> with one which is dimensionless.",
                            ppl_printUnit(c, a, NULL, NULL, 0, 1, 0));
                else
                    ppl_snprintfTrunc(
                            errText, LSTR_LENGTH,
                            "Attempt to compare a quantity with dimensions of <%s> with one with dimensions of <%s>.",
                            ppl_printUnit(c, a, NULL, NULL, 0, 1, 0), ppl_printUnit(c, b, NULL, NULL, 1, 1, 0));
                return -2;
            }
        }
        if ((c != NULL) && (c->set->term_current.ComplexNumbers == SW_ONOFF_OFF) &&
            ((a->flagComplex) || (b->flagComplex)))
            return -2;
        if ((!gsl_finite(a->real)) || (!gsl_finite(a->imag)) || (!gsl_finite(b->real)) || (!gsl_finite(b->imag)))
            return -2;
        if (a->real < b->real) return -1;
        if (a->real > b->real) return 1;
        if (a->imag < b->imag) return -1;
        if (a->imag > b->imag) return 1;
        return 0;
    } else if (t1o == 3) // 3 - dates
    {
        if (a->real < b->real) return -1;
        if (a->real > b->real) return 1;
        if (a->real == b->real) return 0;
        return -2;
    } else if ((t1o == 4) || (t1o == 15)) // 4 - strings; 15 - exception
        return SGN(strcmp((const char *) a->auxil, (const char *) b->auxil));
    else if (t1o == 14) // 14 - data types, which are compared by their identity numbers (auxil is not a string)
    {
        const int id1 = ((const pplType *) a->auxil)->id, id2 = ((const pplType *) b->auxil)->id;
        return (id1 < id2) ? -1 : ((id1 > id2) ? 1 : 0);
    }
    else if (t1o == 5) // 5 - colors
    {
        double h1, s1, b1, h2, s2, b2;
        if (round(a->exponent[0]) == SW_COLSPACE_RGB)
            pplcol_RGBtoHSB(a->exponent[8], a->exponent[9], a->exponent[10], &h1, &s1, &b1);
        else if (round(a->exponent[0]) == SW_COLSPACE_CMYK)
            pplcol_CMYKtoHSB(a->exponent[8], a->exponent[9], a->exponent[10], a->exponent[11], &h1, &s1, &b1);
        else {
            h1 = a->exponent[8];
            s1 = a->exponent[9];
            b1 = a->exponent[10];
        }
        if (round(b->exponent[0]) == SW_COLSPACE_RGB)
            pplcol_RGBtoHSB(b->exponent[8], b->exponent[9], b->exponent[10], &h2, &s2, &b2);
        else if (round(b->exponent[0]) == SW_COLSPACE_CMYK)
            pplcol_CMYKtoHSB(b->exponent[8], b->exponent[9], b->exponent[10], b->exponent[11], &h2, &s2, &b2);
        else {
            h2 = b->exponent[8];
            s2 = b->exponent[9];
            b2 = b->exponent[10];
        }
        if ((!gsl_finite(h1)) || (!gsl_finite(s1)) || (!gsl_finite(b1)) || (!gsl_finite(h2)) || (!gsl_finite(s2)) ||
            (!gsl_finite(b2)))
            return -2;
        if (h1 < h2) return -1;
        if (h1 > h2) return 1;
        if (b1 < b2) return -1;
        if (b1 > b2) return 1;
        if (s1 < s2) return -1;
        if (s1 > s2) return 1;
        return 0;
    } else if (t1o == 6) // 6 - vector
    {
        int i;
        gsl_vector *av = ((pplVector *) a->auxil)->v;
        gsl_vector *bv = ((pplVector *) b->auxil)->v;
        if (av->size < bv->size) return -1;
        if (av->size > bv->size) return 1;
        for (i = 0; i < UNITS_MAX_BASEUNITS; i++) {
            if (a->exponent[i] < b->exponent[i]) return -1;
            if (a->exponent[i] > b->exponent[i]) return 1;
        }
        for (i = 0; i < av->size; i++) {
            if (gsl_vector_get(av, i) < gsl_vector_get(bv, i)) return -1;
            if (gsl_vector_get(av, i) > gsl_vector_get(bv, i)) return 1;
        }
        return 0;
    } else if (t1o == 7) // 7 - list
    {
        int out;
        listIterator *lia, *lib;
        pplObj *oba, *obb;
        list *la = (list *) a->auxil;
        list *lb = (list *) b->auxil;
        if (la->length < lb->length) return -1;
        if (la->length > lb->length) return 1;
        lia = ppl_listIterateInit(la);
        lib = ppl_listIterateInit(lb);
        out = 0;
        while (((oba = ppl_listIterate(&lia)) != NULL) && ((obb = ppl_listIterate(&lib)) != NULL) &&
               ((out = pplObjCmp(NULL, oba, obb, NULL, NULL, NULL, iterDepth + 1)) == 0));
        return out;
    } else if (t1o == 8) // 8 - matrix
    {
        int i, j;
        gsl_matrix *am = ((pplMatrix *) a->auxil)->m;
        gsl_matrix *bm = ((pplMatrix *) b->auxil)->m;
        if (am->size1 * am->size2 < bm->size1 * bm->size2) return -1;
        if (am->size1 * am->size2 > bm->size1 * bm->size2) return 1;
        for (i = 0; i < UNITS_MAX_BASEUNITS; i++) {
            if (a->exponent[i] < b->exponent[i]) return -1;
            if (a->exponent[i] > b->exponent[i]) return 1;
        }
        for (i = 0; i < am->size1; i++)
            for (j = 0; j < am->size2; j++) {
                if (gsl_matrix_get(am, i, j) < gsl_matrix_get(bm, i, j)) return -1;
                if (gsl_matrix_get(am, i, j) > gsl_matrix_get(bm, i, j)) return 1;
            }
        return 0;
    } else if ((t1o == 9) || (t1o == 10) || (t1o == 11)) // 9 - dictionary; 10 - instance; 11 - module
    {
        int out;
        dictIterator *dia, *dib;
        pplObj *oba, *obb;
        char *keya, *keyb;
        dict *da = (dict *) a->auxil;
        dict *db = (dict *) b->auxil;
        if (da->length < db->length) return -1;
        if (da->length > db->length) return 1;
        dia = ppl_dictIterateInit(da);
        dib = ppl_dictIterateInit(db);
        out = 0;
        while (((oba = ppl_dictIterate(&dia, &keya)) != NULL) && ((obb = ppl_dictIterate(&dib, &keyb)) != NULL) &&
               ((out = SGN(strcmp(keya, keyb))) == 0) &&
               ((out = pplObjCmp(NULL, oba, obb, NULL, NULL, NULL, iterDepth + 1)) == 0));
        return out;
    } else if ((t1o == 12) || (t1o == 13)) // 12 - function; 13 - file handle
    {
        if (a->auxil < b->auxil) return -1;
        if (a->auxil > b->auxil) return -1;
        return 0;
    }
    return -2;
}
