// pplObjPrint.c
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

#include <gsl/gsl_matrix.h>
#include <gsl/gsl_vector.h>
#define GSL_RANGE_CHECK_OFF 1

#include "coreUtils/errorReport.h"
#include "coreUtils/list.h"
#include "coreUtils/dict.h"

#include "settings/settingTypes.h"
#include "stringTools/asciidouble.h"

#include "userspace/calendars.h"
#include "userspace/context.h"
#include "userspace/pplObj.h"
#include "userspace/pplObjFunc.h"
#include "userspace/pplObjPrint.h"
#include "userspace/unitsDisp.h"

//! pplObjPrint - Write a textual representation of an object
//! \param [in] c - The Pyxplot context
//! \param [in] o - The object to print
//! \param [in] oname - The name of the variable containing the object, used to label some object types (may be NULL)
//! \param [out] out - Character buffer into which to write the representation
//! \param [in] outlen - The size of the buffer <out>
//! \param [in] internal - If true, write strings with quotation marks and escape characters, as they would be typed
//! in Pyxplot; otherwise write them verbatim
//! \param [in] modIterDepth - The depth of recursion into compound objects, used to prevent infinite recursion

void pplObjPrint(ppl_context *c, pplObj *o, const char *oname, char *out, const int outlen, const int internal,
                 const int modIterDepth) {
    char msg[SSTR_LENGTH];
    int t;
    const int NSigFigs = c->set->term_current.SignificantFigures;
    const int typeable = c->set->term_current.NumDisplay;

    if (o == NULL) {
        ppl_snprintfTrunc(out, outlen, "%s", "<null>");
        return;
    }
    t = o->objType;
    switch (t) {
        case PPLOBJ_NUM:
            ppl_snprintfTrunc(out, outlen, "%s",
                              ppl_unitsNumericDisplay(c, o, 0, internal ? SW_DISPLAY_T : typeable, NSigFigs));
            break;
        case PPLOBJ_STR:
            if (internal || (typeable == SW_DISPLAY_T)) ppl_strEscapify(o->auxil, out, outlen);
            else ppl_snprintfTrunc(out, outlen, "%s", (char *) o->auxil);
            break;
        case PPLOBJ_BOOL:
            if (o->real == 0) ppl_snprintfTrunc(out, outlen, "%s", "false");
            else ppl_snprintfTrunc(out, outlen, "%s", "true");
            break;
        case PPLOBJ_DATE:
            if (internal || (typeable == SW_DISPLAY_T)) {
                int status = 0;
                int year, month, day, hour, minute;
                double second;
                ppl_fromUnixTime(c, o->real, &year, &month, &day, &hour, &minute, &second, &status, msg);
                snprintf(out, outlen, "time.fromCalendar(%d,%d,%d,%d,%d,%.4f,\"UTC\")", year, month, day, hour, minute,
                         second);
            } else {
                int status = 0;
                char timezone[FNAME_LENGTH];
                double offset;
                ppl_calendarTimezoneSet(c, 0, NULL);
                ppl_calendarTimezoneOffset(c, o->real, timezone, &offset);
                ppl_dateString(c, out, o->real + offset, NULL, timezone, &status, msg);
                ppl_calendarTimezoneUnset(c);
            }
            break;
        case PPLOBJ_COL: {
            int i = 0, n;
            const int ct = floor(o->exponent[0] + 0.01);
            if (ct == SW_COLSPACE_CMYK) {
                ppl_strAppendf(out, outlen, &i, "%s", "cmyk(");
                n = 4;
            } else if (ct == SW_COLSPACE_RGB) {
                ppl_strAppendf(out, outlen, &i, "%s", "rgb(");
                n = 3;
            } else if (ct == SW_COLSPACE_HSB) {
                ppl_strAppendf(out, outlen, &i, "%s", "hsb(");
                n = 3;
            } else {
                ppl_warning(&c->errcontext, ERR_INTERNAL, "Unknown color space in pplObjPrint.");
                ppl_strAppendf(out, outlen, &i, "%s", "ERR(");
                n = 0;
            }
            i += strlen(out + i);
            if (n >= 1) {
                ppl_strAppendf(
                        out, outlen, &i, "%s",
                        ppl_numericDisplay(o->exponent[8], c->numdispBuff[0], NSigFigs, (typeable == SW_DISPLAY_L)));
                i += strlen(out + i);
            }
            if (n >= 2) {
                ppl_strAppendChar(out, outlen, &i, ',');
                ppl_strAppendf(
                        out, outlen, &i, "%s",
                        ppl_numericDisplay(o->exponent[9], c->numdispBuff[0], NSigFigs, (typeable == SW_DISPLAY_L)));
                i += strlen(out + i);
            }
            if (n >= 3) {
                ppl_strAppendChar(out, outlen, &i, ',');
                ppl_strAppendf(
                        out, outlen, &i, "%s",
                        ppl_numericDisplay(o->exponent[10], c->numdispBuff[0], NSigFigs, (typeable == SW_DISPLAY_L)));
                i += strlen(out + i);
            }
            if (n >= 4) {
                ppl_strAppendChar(out, outlen, &i, ',');
                ppl_strAppendf(
                        out, outlen, &i, "%s",
                        ppl_numericDisplay(o->exponent[11], c->numdispBuff[0], NSigFigs, (typeable == SW_DISPLAY_L)));
                i += strlen(out + i);
            }
            ppl_strAppendf(out, outlen, &i, "%s", ")");
            break;
        }
        case PPLOBJ_DICT: {
            const int expand = (modIterDepth < 8);
            int i = 0, first = 1, prevMod = 0;
            dictIterator *iter = ppl_dictIterateInit((dict *) o->auxil);
            char *key;
            pplObj *item;
            if (!expand) ppl_strAppendf(out, outlen, &i, "%s", "{ ... }");
            else {
                ppl_strAppendChar(out, outlen, &i, '{');
                while ((item = (pplObj *) ppl_dictIterate(&iter, &key)) != NULL) {
                    if ((item->objType == PPLOBJ_GLOB) || (item->objType == PPLOBJ_ZOM))
                        continue; // Hide globals and zombies
                    if (!first) {
                        ppl_strAppendChar(out, outlen, &i, ',');
                        if (prevMod) { ppl_strAppendChar(out, outlen, &i, '\n'); }
                        ppl_strAppendChar(out, outlen, &i, ' ');
                    }
                    ppl_strEscapify(key, out + i, outlen - i);
                    i += strlen(out + i);
                    ppl_strAppendf(out, outlen, &i, ":");
                    i += strlen(out + i);
                    pplObjPrint(c, item, NULL, out + i, outlen - i, 1, modIterDepth + 1);
                    i += strlen(out + i);
                    if (i > outlen - 400) {
                        ppl_strAppendf(out, outlen, &i, "%s", ", ...");
                        i += strlen(out + i);
                        break;
                    }
                    first = 0;
                    prevMod = item->objType == PPLOBJ_MOD;
                }
                ppl_strAppendf(out, outlen, &i, "%s", "}");
            }
            break;
        }
        case PPLOBJ_USER:
        case PPLOBJ_MOD: {
            int i = 0, k;
            const int expand = (modIterDepth == 0);
            dictIterator *iter = ppl_dictIterateInit((dict *) o->auxil);
            char *key;
            pplObj *item;
            ppl_strAppendf(out, outlen, &i, (t == PPLOBJ_MOD) ? "module {" : "module instance {");
            i += strlen(out + i);
            if (!expand) ppl_strAppendf(out, outlen, &i, "%s", " ... }");
            else {
                ppl_strAppendChar(out, outlen, &i, '\n');
                while ((item = (pplObj *) ppl_dictIterate(&iter, &key)) != NULL) {
                    const int j = i;
                    if ((item->objType == PPLOBJ_GLOB) || (item->objType == PPLOBJ_ZOM))
                        continue; // Hide globals and zombies
                    for (k = 0; k <= modIterDepth; k++) {
                        ppl_strAppendChar(out, outlen, &i, ' ');
                        ppl_strAppendChar(out, outlen, &i, ' ');
                    }
                    ppl_strAppendf(out, outlen, &i, "%s", key);
                    i += strlen(out + i);
                    while ((i < j + 16) && (i + 1 < outlen)) ppl_strAppendChar(out, outlen, &i, ' ');
                    snprintf(out + i, outlen - i, ": ");
                    i += strlen(out + i);
                    pplObjPrint(c, item, key, out + i, outlen - i, 1, modIterDepth + 1);
                    i += strlen(out + i);
                    ppl_strAppendChar(out, outlen, &i, '\n');
                    if (i > outlen - 400) {
                        ppl_strAppendf(out, outlen, &i, "%s", "  ...\n");
                        i += strlen(out + i);
                        break;
                    }
                }
                if (i < outlen) ppl_strAppendChar(out, outlen, &i, ' ');
                for (k = 0; ((k < modIterDepth) && (i < outlen - 2)); k++) {
                    ppl_strAppendChar(out, outlen, &i, ' ');
                    ppl_strAppendChar(out, outlen, &i, ' ');
                }
                ppl_strAppendf(out, outlen, &i, "%s", "}");
            }
            break;
        }
        case PPLOBJ_LIST: {
            const int expand = (modIterDepth < 8);
            int i = 0, first = 1, prevMod = 0;
            listIterator *iter = ppl_listIterateInit((list *) o->auxil);
            pplObj *item;
            if (!expand) ppl_strAppendf(out, outlen, &i, "%s", "[ ... ]");
            else {
                ppl_strAppendChar(out, outlen, &i, '[');
                while ((item = (pplObj *) ppl_listIterate(&iter)) != NULL) {
                    if (!first) {
                        ppl_strAppendChar(out, outlen, &i, ',');
                        if (prevMod) { ppl_strAppendChar(out, outlen, &i, '\n'); }
                        ppl_strAppendChar(out, outlen, &i, ' ');
                    }
                    pplObjPrint(c, item, NULL, out + i, outlen - i, 1, modIterDepth + 1);
                    i += strlen(out + i);
                    if (i > outlen - 400) {
                        ppl_strAppendf(out, outlen, &i, "%s", ", ...");
                        i += strlen(out + i);
                        break;
                    }
                    first = 0;
                    prevMod = item->objType == PPLOBJ_MOD;
                }
                ppl_strAppendf(out, outlen, &i, "%s", "]");
            }
            break;
        }
        case PPLOBJ_VEC: {
            int i = 0, j = 0;
            double real, imag;
            char *unit;
            o->real = 1;
            o->imag = 0;
            o->flagComplex = 0;
            unit = ppl_printUnit(c, o, &real, &imag, 1, 1, SW_DISPLAY_T);
            if (o->dimensionless) real = 1;
            gsl_vector *v = ((pplVector *) (o->auxil))->v;
            ppl_strAppendf(out, outlen, &i, "%s", "vector(");
            i += strlen(out + i);
            for (j = 0; j < v->size; j++) {
                if (j > 0) {
                    ppl_strAppendChar(out, outlen, &i, ',');
                    ppl_strAppendChar(out, outlen, &i, ' ');
                }
                ppl_strAppendf(out, outlen, &i, "%s",
                               ppl_numericDisplay(gsl_vector_get(v, j) * real, c->numdispBuff[0], NSigFigs,
                                                  (typeable == SW_DISPLAY_L)));
                i += strlen(out + i);
                if (i > outlen - 400) {
                    ppl_strAppendf(out, outlen, &i, "%s", ", ...");
                    i += strlen(out + i);
                    break;
                }
            }
            ppl_strAppendf(out, outlen, &i, "%s", ")");
            i += strlen(out + i);
            if (!o->dimensionless) ppl_strAppendf(out, outlen, &i, "%s", unit);
            break;
        }
        case PPLOBJ_MAT: {
            int i = 0, j = 0, k = 0;
            double real, imag;
            char *unit;
            o->real = 1;
            o->imag = 0;
            o->flagComplex = 0;
            unit = ppl_printUnit(c, o, &real, &imag, 1, 1, SW_DISPLAY_T);
            if (o->dimensionless) real = 1;
            gsl_matrix *m = ((pplMatrix *) (o->auxil))->m;
            if (internal || (typeable == SW_DISPLAY_T)) {
                ppl_strAppendf(out, outlen, &i, "%s", "matrix(");
                i += strlen(out + i);
                for (j = 0; j < m->size1; j++) {
                    if (j > 0) {
                        ppl_strAppendChar(out, outlen, &i, ',');
                        ppl_strAppendChar(out, outlen, &i, ' ');
                    }
                    ppl_strAppendChar(out, outlen, &i, '[');
                    for (k = 0; k < m->size2; k++) {
                        if (k > 0) {
                            ppl_strAppendChar(out, outlen, &i, ',');
                            ppl_strAppendChar(out, outlen, &i, ' ');
                        }
                        ppl_strAppendf(out, outlen, &i, "%s",
                                       ppl_numericDisplay(gsl_matrix_get(m, j, k) * real, c->numdispBuff[0], NSigFigs,
                                                          (typeable == SW_DISPLAY_L)));
                        i += strlen(out + i);
                        if (i > outlen - 400) {
                            ppl_strAppendf(out, outlen, &i, "%s", ", ...");
                            i += strlen(out + i);
                            break;
                        }
                    }
                    ppl_strAppendf(out, outlen, &i, "%s", "]");
                    i += strlen(out + i);
                    if (i > outlen - 400) {
                        ppl_strAppendf(out, outlen, &i, "%s", ", ...");
                        i += strlen(out + i);
                        break;
                    }
                }
                ppl_strAppendf(out, outlen, &i, "%s", ")");
                i += strlen(out + i);
                if (!o->dimensionless) ppl_strAppendf(out, outlen, &i, "%s", unit);
            } else {
                const int unitLine = m->size1 / 2;
                for (j = 0; j < m->size1; j++) {
                    ppl_strAppendf(out, outlen, &i, "%s", "(");
                    i += strlen(out + i);
                    for (k = 0; k < m->size2; k++) {
                        if (k > 0) {
                            ppl_strAppendChar(out, outlen, &i, ',');
                            ppl_strAppendChar(out, outlen, &i, ' ');
                        }
                        ppl_strAppendf(out, outlen, &i, "%s",
                                       ppl_numericDisplay(gsl_matrix_get(m, j, k) * real, c->numdispBuff[0], NSigFigs,
                                                          (typeable == SW_DISPLAY_L)));
                        i += strlen(out + i);
                        if (i > outlen - 400) {
                            ppl_strAppendf(out, outlen, &i, "%s", ", ...");
                            i += strlen(out + i);
                            break;
                        }
                    }
                    if ((j != unitLine) || (o->dimensionless)) ppl_strAppendf(out, outlen, &i, "%s", ")\n");
                    else ppl_strAppendf(out, outlen, &i, ") %s\n", unit);
                    i += strlen(out + i);
                    if (i > outlen - 400) {
                        ppl_strAppendf(out, outlen, &i, "%s", "...\n");
                        i += strlen(out + i);
                        break;
                    }
                }
            }
            break;
        }
        case PPLOBJ_FILE: {
            ppl_snprintfTrunc(out, outlen, "%s", "<file handle>");
            break;
        }
        case PPLOBJ_FUNC: {
            const char *fnname = (oname == NULL) ? "function" : oname;
            const pplFunc *f = (pplFunc *) o->auxil;
            int first = 1, i = 0;
            const int indent = 16 * (modIterDepth > 0) + 2 * modIterDepth;
            for (; f != NULL; f = f->next, first = 0) {
                const int t = f->functionType;
                if (!first) {
                    int j;
                    ppl_strAppendChar(out, outlen, &i, '\n');
                    for (j = 0; j < indent; j++) ppl_strAppendChar(out, outlen, &i, ' ');
                }
                switch (t) {
                    case PPL_FUNC_SYSTEM:
                    case PPL_FUNC_MAGIC:
                        ppl_strAppendf(out, outlen, &i, "%s.", f->description);
                        i += strlen(out + i);
                        break;
                    case PPL_FUNC_USERDEF: {
                        int j = -1, k, l, m;
                        for (k = 0; k < f->maxArgs; k++)
                            if (f->minActive[k] || f->maxActive[k]) j = k;
                        ppl_strAppendf(out, outlen, &i, "%s(", fnname);
                        i += strlen(out + i);
                        for (l = 0, m = 0; l < f->maxArgs; l++, m++) {
                            for (; f->argList[m] != '\0'; m++) ppl_strAppendChar(out, outlen, &i, f->argList[m]);
                            ppl_strAppendChar(out, outlen, &i, ',');
                        }
                        if (f->maxArgs > 0) i--; // Remove final comma from list of arguments
                        ppl_strAppendChar(out, outlen, &i, ')');
                        for (k = 0; k <= j; k++) {
                            ppl_strAppendChar(out, outlen, &i, '[');
                            if (f->minActive[k]) {
                                ppl_strAppendf(out, outlen, &i, "%s", ppl_unitsNumericDisplay(c, f->min + k, 0, 0, 0));
                                i += strlen(out + i);
                            }
                            ppl_strAppendChar(out, outlen, &i, ':');
                            if (f->maxActive[k]) {
                                ppl_strAppendf(out, outlen, &i, "%s", ppl_unitsNumericDisplay(c, f->max + k, 0, 0, 0));
                                i += strlen(out + i);
                            }
                            ppl_strAppendChar(out, outlen, &i, ']');
                        }
                        if (f->functionPtr == NULL) ppl_strAppendf(out, outlen, &i, "=");
                        else ppl_strAppendf(out, outlen, &i, "=%s", ((pplExpr *) f->functionPtr)->ascii);
                        i += strlen(out + i);
                        break;
                    }
                    case PPL_FUNC_SPLINE:
                    case PPL_FUNC_INTERP2D:
                    case PPL_FUNC_BMPDATA: {
                        splineDescriptor *s = (splineDescriptor *) f->functionPtr;
                        ppl_strAppendf(out, outlen, &i, "%s(x)= [%s interpolation of data from the %sfile '%s']",
                                       fnname, s->splineType, (f->functionType == PPL_FUNC_BMPDATA) ? "bitmap " : "",
                                       s->filename);
                        i += strlen(out + i);
                        break;
                    }
                    case PPL_FUNC_HISTOGRAM: {
                        ppl_strAppendf(out, outlen, &i, "%s(x)= [histogram of data from the file '%s']", fnname,
                                       ((histogramDescriptor *) f->functionPtr)->filename);
                        i += strlen(out + i);
                        break;
                    }
                    case PPL_FUNC_FFT: {
                        ppl_strAppendf(out, outlen, &i, "%s(x)= [%d-dimensional fft]", fnname,
                                       ((FFTDescriptor *) f->functionPtr)->Ndims);
                        i += strlen(out + i);
                        break;
                    }
                    case PPL_FUNC_SUBROUTINE: {
                        int l, m;
                        ppl_strAppendf(out, outlen, &i, "%s(", fnname);
                        i += strlen(out + i);
                        for (l = 0, m = 0; l < f->maxArgs; l++, m++) {
                            for (; f->argList[m] != '\0'; m++) ppl_strAppendChar(out, outlen, &i, f->argList[m]);
                            ppl_strAppendChar(out, outlen, &i, ',');
                        }
                        if (f->maxArgs > 0) i--; // Remove final comma from list of arguments
                        ppl_strAppendf(out, outlen, &i, ") = [subroutine]");
                        i += strlen(out + i);
                        break;
                    }
                    default:
                        ppl_snprintfTrunc(out, outlen, "%s", "<unknown function type>");
                        break;
                }
            }
            break;
        }
        case PPLOBJ_TYPE: {
            int i = 0;
            ppl_snprintfTrunc(out, outlen, "%s", "<data type");
            i += strlen(out + i);
            if (((pplType *) (o->auxil))->id >= 0) {
                ppl_strAppendf(out, outlen, &i, "%s", ": ");
                i += strlen(out + i);
                ppl_strAppendf(out, outlen, &i, "%s", pplObjTypeNames[((pplType *) (o->auxil))->id]);
                i += strlen(out + i);
            }
            ppl_strAppendf(out, outlen, &i, "%s", ">");
            break;
        }
        case PPLOBJ_NULL: {
            ppl_snprintfTrunc(out, outlen, "%s", "null");
            break;
        }
        case PPLOBJ_EXC: {
            int i = 0;
            ppl_snprintfTrunc(out, outlen, "%s", "<exception: ");
            i += strlen(out + i);
            ppl_strAppendf(out, outlen, &i, "%s", (char *) o->auxil);
            i += strlen(out + i);
            ppl_strAppendf(out, outlen, &i, "%s", ">");
            i += strlen(out + i);
            out[outlen - 1] = '\0';
            break;
        }
        case PPLOBJ_GLOB: {
            ppl_snprintfTrunc(out, outlen, "%s", "<internal: global marker>");
            break;
        }
        case PPLOBJ_ZOM: {
            ppl_snprintfTrunc(out, outlen, "%s", "<internal: zombie>");
            break;
        }
        case PPLOBJ_EXP: {
            ppl_snprintfTrunc(out, outlen, "%s", "<internal: expression bytecode>");
            break;
        }
        case PPLOBJ_BYT: {
            ppl_snprintfTrunc(out, outlen, "%s", "<internal: command-line bytecode>");
            break;
        }
        default:
            ppl_snprintfTrunc(out, outlen, "%s", "<unknown object type>");
            break;
    }
    out[outlen - 1] = '\0';
    return;
}
