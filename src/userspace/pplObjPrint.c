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

// Renders Pyxplot objects as text, for the print command, for expressions typed at the prompt, for str(), and for "show
// variables". Depending on the setting of "set numerics display", values are shown in natural form, in a typeable form
// which Pyxplot can read back, or in LaTeX. Long containers are truncated to fit the output buffer.

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
    const int NSigFigs = c->set->term_current.SignificantFigures;
    const int typeable = c->set->term_current.NumDisplay;

    // A missing object
    if (o == NULL) {
        ppl_snprintfTrunc(out, outlen, "%s", "<null>");
        return;
    }
    int t = o->objType;
    // Render each type of object
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
            // Dates within containers, or in typeable mode, are shown as a call to time.fromCalendar(); otherwise as a
            // date string in the current timezone
            if (internal || (typeable == SW_DISPLAY_T)) {
                int status = 0;
                int year, month, day, hour, minute;
                double second;
                ppl_fromUnixTime(c, o->real, &year, &month, &day, &hour, &minute, &second, &status, msg);
                if (status) // The date cannot be expressed in the current calendar, e.g. it precedes its epoch
                    snprintf(out, outlen, "time.fromJD(%.17g)", o->real / 86400.0 + 2440587.5);
                else
                    snprintf(out, outlen, "time.fromCalendar(%d,%d,%d,%d,%d,%.4f,\"UTC\")", year, month, day, hour,
                             minute, second);
            } else {
                int status = 0;
                char timezone[FNAME_LENGTH];
                double offset;
                ppl_calendarTimezoneSet(c, 0, NULL);
                ppl_calendarTimezoneOffset(c, o->real, timezone, &offset);
                ppl_dateString(c, out, outlen, o->real + offset, NULL, timezone, &status, msg);
                ppl_calendarTimezoneUnset(c);
                // If the date cannot be expressed in the current calendar, e.g. it precedes its epoch, show its Julian Date
                if (status) snprintf(out, outlen, "time.fromJD(%.10g)", o->real / 86400.0 + 2440587.5);
            }
            break;
        case PPLOBJ_COL: {
            int i = 0, n;
            const int ct = floor(o->exponent[0] + 0.01);
            // Colors are shown as a call to the function for their color space, with the right number of components
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
            // Show the components, separated by commas
            if (n >= 1) {
                ppl_strAppendf(
                        out, outlen, &i, "%s",
                        ppl_numericDisplay(o->exponent[8], c->numdispBuff[0], NSigFigs, (typeable == SW_DISPLAY_L)));
                i += strlen(out + i);
            }
            // Second component
            if (n >= 2) {
                ppl_strAppendChar(out, outlen, &i, ',');
                ppl_strAppendf(
                        out, outlen, &i, "%s",
                        ppl_numericDisplay(o->exponent[9], c->numdispBuff[0], NSigFigs, (typeable == SW_DISPLAY_L)));
                i += strlen(out + i);
            }
            // Third component
            if (n >= 3) {
                ppl_strAppendChar(out, outlen, &i, ',');
                ppl_strAppendf(
                        out, outlen, &i, "%s",
                        ppl_numericDisplay(o->exponent[10], c->numdispBuff[0], NSigFigs, (typeable == SW_DISPLAY_L)));
                i += strlen(out + i);
            }
            // Fourth component
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
            // Dictionaries nested more than eight deep are abbreviated, to avoid infinite recursion
            else {
                ppl_strAppendChar(out, outlen, &i, '{');
                // Show each item as key:value, skipping global declarations and zombies; items after a module start on
                // a new line
                while ((item = (pplObj *) ppl_dictIterate(&iter, &key)) != NULL) {
                    if ((item->objType == PPLOBJ_GLOB) || (item->objType == PPLOBJ_ZOM))
                        continue; // Hide globals and zombies
                    // Separate items with commas
                    if (!first) {
                        ppl_strAppendChar(out, outlen, &i, ',');
                        if (prevMod) { ppl_strAppendChar(out, outlen, &i, '\n'); }
                        ppl_strAppendChar(out, outlen, &i, ' ');
                    }
                    ppl_strEscapify(key, out + i, outlen - i);
                    i += strlen(out + i);
                    ppl_strAppendf(out, outlen, &i, ":");
                    pplObjPrint(c, item, NULL, out + i, outlen - i, 1, modIterDepth + 1);
                    i += strlen(out + i);
                    // Stop when the output buffer is nearly full
                    if (i > outlen - 400) {
                        ppl_strAppendf(out, outlen, &i, "%s", ", ...");
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
            int i = 0;
            const int expand = (modIterDepth == 0);
            dictIterator *iter = ppl_dictIterateInit((dict *) o->auxil);
            char *key;
            pplObj *item;
            ppl_strAppendf(out, outlen, &i, (t == PPLOBJ_MOD) ? "module {" : "module instance {");
            if (!expand) ppl_strAppendf(out, outlen, &i, "%s", " ... }");
            // Modules are expanded only at the top level, with one member per line, indented by depth and aligned in
            // columns
            else {
                ppl_strAppendChar(out, outlen, &i, '\n');
                // Show each member, skipping global declarations and zombies
                while ((item = (pplObj *) ppl_dictIterate(&iter, &key)) != NULL) {
                    const int j = i;
                    if ((item->objType == PPLOBJ_GLOB) || (item->objType == PPLOBJ_ZOM))
                        continue; // Hide globals and zombies
                    // Indent the member by its depth
                    for (int k = 0; k <= modIterDepth; k++) {
                        ppl_strAppendChar(out, outlen, &i, ' ');
                        ppl_strAppendChar(out, outlen, &i, ' ');
                    }
                    ppl_strAppendf(out, outlen, &i, "%s", key);
                    while ((i < j + 16) && (i + 1 < outlen)) ppl_strAppendChar(out, outlen, &i, ' ');
                    snprintf(out + i, outlen - i, ": ");
                    i += strlen(out + i);
                    pplObjPrint(c, item, key, out + i, outlen - i, 1, modIterDepth + 1);
                    i += strlen(out + i);
                    ppl_strAppendChar(out, outlen, &i, '\n');
                    // Stop when the output buffer is nearly full
                    if (i > outlen - 400) {
                        ppl_strAppendf(out, outlen, &i, "%s", "  ...\n");
                        break;
                    }
                }
                if (i < outlen) ppl_strAppendChar(out, outlen, &i, ' ');
                // Indent the closing brace to match
                for (int k = 0; ((k < modIterDepth) && (i < outlen - 2)); k++) {
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
            // Lists nested more than eight deep are abbreviated, to avoid infinite recursion
            else {
                ppl_strAppendChar(out, outlen, &i, '[');
                // Show each item; items after a module start on a new line
                while ((item = (pplObj *) ppl_listIterate(&iter)) != NULL) {
                    // Separate items with commas
                    if (!first) {
                        ppl_strAppendChar(out, outlen, &i, ',');
                        if (prevMod) { ppl_strAppendChar(out, outlen, &i, '\n'); }
                        ppl_strAppendChar(out, outlen, &i, ' ');
                    }
                    pplObjPrint(c, item, NULL, out + i, outlen - i, 1, modIterDepth + 1);
                    i += strlen(out + i);
                    // Stop when the output buffer is nearly full
                    if (i > outlen - 400) {
                        ppl_strAppendf(out, outlen, &i, "%s", ", ...");
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
            // Show each element, scaled into the display units. A vector's units are found by printing the units of a
            // value of 1 with the vector's dimensions.
            for (j = 0; j < v->size; j++) {
                // Separate elements with commas
                if (j > 0) {
                    ppl_strAppendChar(out, outlen, &i, ',');
                    ppl_strAppendChar(out, outlen, &i, ' ');
                }
                ppl_strAppendf(out, outlen, &i, "%s",
                               ppl_numericDisplay(gsl_vector_get(v, j) * real, c->numdispBuff[0], NSigFigs,
                                                  (typeable == SW_DISPLAY_L)));
                // Stop when the output buffer is nearly full
                if (i > outlen - 400) {
                    ppl_strAppendf(out, outlen, &i, "%s", ", ...");
                    break;
                }
            }
            ppl_strAppendf(out, outlen, &i, "%s", ")");
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
            // In typeable form, a matrix is shown as a call to matrix() with a list of rows; otherwise it is laid out
            // one row per line, with the units beside the middle row
            if (internal || (typeable == SW_DISPLAY_T)) {
                ppl_strAppendf(out, outlen, &i, "%s", "matrix(");
                // Show each row
                for (j = 0; j < m->size1; j++) {
                    // Separate rows with commas
                    if (j > 0) {
                        ppl_strAppendChar(out, outlen, &i, ',');
                        ppl_strAppendChar(out, outlen, &i, ' ');
                    }
                    ppl_strAppendChar(out, outlen, &i, '[');
                    // Show each element of the row
                    for (k = 0; k < m->size2; k++) {
                        // Separate elements with commas
                        if (k > 0) {
                            ppl_strAppendChar(out, outlen, &i, ',');
                            ppl_strAppendChar(out, outlen, &i, ' ');
                        }
                        ppl_strAppendf(out, outlen, &i, "%s",
                                       ppl_numericDisplay(gsl_matrix_get(m, j, k) * real, c->numdispBuff[0], NSigFigs,
                                                          (typeable == SW_DISPLAY_L)));
                        // Stop when the output buffer is nearly full
                        if (i > outlen - 400) {
                            ppl_strAppendf(out, outlen, &i, "%s", ", ...");
                            break;
                        }
                    }
                    ppl_strAppendf(out, outlen, &i, "%s", "]");
                    // Stop when the output buffer is nearly full
                    if (i > outlen - 400) {
                        ppl_strAppendf(out, outlen, &i, "%s", ", ...");
                        break;
                    }
                }
                ppl_strAppendf(out, outlen, &i, "%s", ")");
                if (!o->dimensionless) ppl_strAppendf(out, outlen, &i, "%s", unit);
            } else {
                const int unitLine = m->size1 / 2;
                // Lay out each row on a line of its own
                for (j = 0; j < m->size1; j++) {
                    ppl_strAppendf(out, outlen, &i, "%s", "(");
                    // Show each element of the row
                    for (k = 0; k < m->size2; k++) {
                        // Separate elements with commas
                        if (k > 0) {
                            ppl_strAppendChar(out, outlen, &i, ',');
                            ppl_strAppendChar(out, outlen, &i, ' ');
                        }
                        ppl_strAppendf(out, outlen, &i, "%s",
                                       ppl_numericDisplay(gsl_matrix_get(m, j, k) * real, c->numdispBuff[0], NSigFigs,
                                                          (typeable == SW_DISPLAY_L)));
                        // Stop when the output buffer is nearly full
                        if (i > outlen - 400) {
                            ppl_strAppendf(out, outlen, &i, "%s", ", ...");
                            break;
                        }
                    }
                    if ((j != unitLine) || (o->dimensionless)) ppl_strAppendf(out, outlen, &i, "%s", ")\n");
                    else ppl_strAppendf(out, outlen, &i, ") %s\n", unit);
                    i += strlen(out + i);
                    // Stop when the output buffer is nearly full
                    if (i > outlen - 400) {
                        ppl_strAppendf(out, outlen, &i, "%s", "...\n");
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
            // A function may consist of a chain of definitions, each valid over a different range of its arguments;
            // show each on its own line
            for (; f != NULL; f = f->next, first = 0) {
                const int t = f->functionType;
                // Start each definition after the first on a new line, indented to line up with the first
                if (!first) {
                    ppl_strAppendChar(out, outlen, &i, '\n');
                    for (int j = 0; j < indent; j++) ppl_strAppendChar(out, outlen, &i, ' ');
                }
                // Describe each type of function
                switch (t) {
                    case PPL_FUNC_SYSTEM:
                    case PPL_FUNC_MAGIC:
                        ppl_strAppendf(out, outlen, &i, "%s.", f->description);
                        i += strlen(out + i);
                        break;
                    case PPL_FUNC_USERDEF: {
                        int j = -1, l, m;
                        for (int k = 0; k < f->maxArgs; k++)
                            if (f->minActive[k] || f->maxActive[k]) j = k;
                        ppl_strAppendf(out, outlen, &i, "%s(", fnname);
                        // The argument names are stored one after another, each null-terminated; list them separated by
                        // commas
                        for (l = 0, m = 0; l < f->maxArgs; l++, m++) {
                            for (; f->argList[m] != '\0'; m++) ppl_strAppendChar(out, outlen, &i, f->argList[m]);
                            ppl_strAppendChar(out, outlen, &i, ',');
                        }
                        if (f->maxArgs > 0) i--; // Remove final comma from list of arguments
                        ppl_strAppendChar(out, outlen, &i, ')');
                        // Show the range of values of each argument over which this definition applies, e.g.
                        // f(x)[0:1]=...; j is the last argument which has a limit
                        for (int k = 0; k <= j; k++) {
                            ppl_strAppendChar(out, outlen, &i, '[');
                            // The lower limit, if there is one
                            if (f->minActive[k]) {
                                ppl_strAppendf(out, outlen, &i, "%s", ppl_unitsNumericDisplay(c, f->min + k, 0, 0, 0));
                            }
                            ppl_strAppendChar(out, outlen, &i, ':');
                            // The upper limit, if there is one
                            if (f->maxActive[k]) {
                                ppl_strAppendf(out, outlen, &i, "%s", ppl_unitsNumericDisplay(c, f->max + k, 0, 0, 0));
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
                        break;
                    }
                    case PPL_FUNC_HISTOGRAM: {
                        ppl_strAppendf(out, outlen, &i, "%s(x)= [histogram of data from the file '%s']", fnname,
                                       ((histogramDescriptor *) f->functionPtr)->filename);
                        break;
                    }
                    case PPL_FUNC_FFT: {
                        ppl_strAppendf(out, outlen, &i, "%s(x)= [%d-dimensional fft]", fnname,
                                       ((FFTDescriptor *) f->functionPtr)->Ndims);
                        break;
                    }
                    case PPL_FUNC_SUBROUTINE: {
                        int l, m;
                        ppl_strAppendf(out, outlen, &i, "%s(", fnname);
                        // List the argument names, separated by commas
                        for (l = 0, m = 0; l < f->maxArgs; l++, m++) {
                            for (; f->argList[m] != '\0'; m++) ppl_strAppendChar(out, outlen, &i, f->argList[m]);
                            ppl_strAppendChar(out, outlen, &i, ',');
                        }
                        if (f->maxArgs > 0) i--; // Remove final comma from list of arguments
                        ppl_strAppendf(out, outlen, &i, ") = [subroutine]");
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
            // Name the type, e.g. <data type: string>
            if (((pplType *) (o->auxil))->id >= 0) {
                ppl_strAppendf(out, outlen, &i, "%s", ": ");
                ppl_strAppendf(out, outlen, &i, "%s", pplObjTypeNames[((pplType *) (o->auxil))->id]);
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
            ppl_strAppendf(out, outlen, &i, "%s", ">");
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
