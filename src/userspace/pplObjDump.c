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

// Writes Pyxplot objects to a file as Pyxplot expressions which recreate them, e.g. vector([1,2])*unit(m). This is used
// by the save command to record the values of variables, and by the file.dump() method. Numbers are written to full
// precision, and objects which cannot be recreated, such as functions and files, are written as types.null().

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

//! pplObjDump_escaped - Write a string to a file in quoted, escaped form, followed by a suffix
//! \param [in] output - The file handle to write to
//! \param [in] str - The string to write
//! \param [in] suffix - Text to write after the escaped string

static void pplObjDump_escaped(FILE *output, const char *str, const char *suffix) {
    // Each character escapes to at most two characters, plus two quotation marks and a null terminator
    const size_t bufferLen = 2 * strlen(str) + 3;
    char *buffer = (char *) malloc(bufferLen);
    if (buffer == NULL) return;
    ppl_strEscapify(str, buffer, bufferLen);
    fprintf(output, "%s%s", buffer, suffix);
    free(buffer);
    buffer = NULL;
}

//! pplObjDump - Write an object to a file, in a format which Pyxplot can read back in to recreate the object
//! \param [in] c - The Pyxplot context
//! \param [in] o - The object to write
//! \param [in] output - The file handle to write to

void pplObjDump(ppl_context *c, pplObj *o, FILE *output) {
    // Numbers are written with 17 significant figures, enough to recreate any double exactly
    char msg[SSTR_LENGTH];
    const int NSigFigs = 17;

    // Stop if the user has pressed Ctrl-C
    if (cancellationFlag) return;
    // A missing object is written as null
    if (o == NULL) {
        fprintf(output, "types.null()");
        return;
    }
    int t = o->objType;
    // Write each type of object in the syntax which recreates it
    switch (t) {
        case PPLOBJ_NUM:
            fprintf(output, "%s", ppl_unitsNumericDisplay(c, o, 0, SW_DISPLAY_T, NSigFigs));
            break;
        case PPLOBJ_STR:
            pplObjDump_escaped(output, o->auxil, "");
            break;
        case PPLOBJ_BOOL:
            if (o->real == 0) fprintf(output, "false");
            else fprintf(output, "true");
            break;
        case PPLOBJ_DATE: {
            int status = 0;
            int year, month, day, hour, minute;
            double second;
            ppl_fromUnixTime(c, o->real, &year, &month, &day, &hour, &minute, &second, &status, msg);
            fprintf(output, "time.fromCalendar(%d,%d,%d,%d,%d,%.12f)", year, month, day, hour, minute, second);
        } break;
        case PPLOBJ_COL: {
            int n;
            const int ct = floor(o->exponent[0] + 0.01);
            // Colors are written as a call to the function for their color space, with the right number of components
            if (ct == SW_COLSPACE_CMYK) {
                fprintf(output, "cmyk(");
                n = 4;
            } else if (ct == SW_COLSPACE_RGB) {
                fprintf(output, "rgb(");
                n = 3;
            } else if (ct == SW_COLSPACE_HSB) {
                fprintf(output, "hsb(");
                n = 3;
            } else {
                ppl_warning(&c->errcontext, ERR_INTERNAL, "Unknown color space in pplObjPrint.");
                fprintf(output, "ERR(");
                n = 0;
            }

            // Write the components, separated by commas
            if (n >= 1) { fprintf(output, "%s", ppl_numericDisplay(o->exponent[8], c->numdispBuff[0], NSigFigs, 0)); }
            // Second component
            if (n >= 2) {
                fprintf(output, ",");
                fprintf(output, "%s", ppl_numericDisplay(o->exponent[9], c->numdispBuff[0], NSigFigs, 0));
            }
            // Third component
            if (n >= 3) {
                fprintf(output, ",");
                fprintf(output, "%s", ppl_numericDisplay(o->exponent[10], c->numdispBuff[0], NSigFigs, 0));
            }
            // Fourth component
            if (n >= 4) {
                fprintf(output, ",");
                fprintf(output, "%s", ppl_numericDisplay(o->exponent[11], c->numdispBuff[0], NSigFigs, 0));
            }
            fprintf(output, ")");
            break;
        }
        case PPLOBJ_DICT: {
            int first = 1;
            dictIterator *iter = ppl_dictIterateInit((dict *) o->auxil);
            char *key;
            pplObj *item;

            // Dictionaries are written as {key:value, ...}
            fprintf(output, "{");
            // Write each item, skipping variables which are merely declared global, and zombies
            while ((item = (pplObj *) ppl_dictIterate(&iter, &key)) != NULL) {
                if (cancellationFlag) break;
                if ((item->objType == PPLOBJ_GLOB) || (item->objType == PPLOBJ_ZOM))
                    continue; // Hide globals and zombies
                if (!first) fprintf(output, ",");
                pplObjDump_escaped(output, key, ":");
                pplObjDump(c, item, output);
                first = 0;
            }
            fprintf(output, "}");
            break;
        }
        case PPLOBJ_USER:
        case PPLOBJ_MOD: {
            int first = 1;
            dictIterator *iter = ppl_dictIterateInit((dict *) o->auxil);
            char *key;
            pplObj *item;
            fprintf(output, "module({");

            // Modules and class instances are written as a module made from a dictionary of their members, skipping
            // global declarations and zombies
            while ((item = (pplObj *) ppl_dictIterate(&iter, &key)) != NULL) {
                if (cancellationFlag) break;
                if ((item->objType == PPLOBJ_GLOB) || (item->objType == PPLOBJ_ZOM))
                    continue; // Hide globals and zombies
                if (!first) fprintf(output, ",");
                pplObjDump_escaped(output, key, ":");
                pplObjDump(c, item, output);
                first = 0;
            }
            fprintf(output, "})");
            break;
        }
        case PPLOBJ_LIST: {
            int first = 1;
            listIterator *iter = ppl_listIterateInit((list *) o->auxil);
            pplObj *item;
            fprintf(output, "[");
            // Write each item of the list
            while ((item = (pplObj *) ppl_listIterate(&iter)) != NULL) {
                if (cancellationFlag) break;
                if (!first) fprintf(output, ",");
                pplObjDump(c, item, output);
                first = 0;
            }
            fprintf(output, "]");
            break;
        }
        case PPLOBJ_VEC: {
            int j = 0;
            double real, imag;
            char *unit;
            o->real = 1;
            o->imag = 0;
            o->flagComplex = 0;
            unit = ppl_printUnit(c, o, &real, &imag, 1, 1, SW_DISPLAY_T);
            if (o->dimensionless) real = 1;
            gsl_vector *v = ((pplVector *) (o->auxil))->v;
            fprintf(output, "vector([");

            // Write each element, scaled into the display units found above. A vector's units are found by printing the
            // units of a value of 1 with the vector's dimensions.
            for (j = 0; j < v->size; j++) {
                if (cancellationFlag) break;
                if (j > 0) fprintf(output, ",");
                fprintf(output, "%s", ppl_numericDisplay(gsl_vector_get(v, j) * real, c->numdispBuff[0], NSigFigs, 0));
            }
            fprintf(output, "])");

            // Multiply by the units
            if (!o->dimensionless) fprintf(output, "%s", unit);
            break;
        }
        case PPLOBJ_MAT: {
            int j = 0, k = 0;
            double real, imag;
            char *unit;
            o->real = 1;
            o->imag = 0;
            o->flagComplex = 0;
            unit = ppl_printUnit(c, o, &real, &imag, 1, 1, SW_DISPLAY_T);
            if (o->dimensionless) real = 1;
            gsl_matrix *m = ((pplMatrix *) (o->auxil))->m;
            {
                fprintf(output, "matrix([");

                // Write each row of the matrix, scaled into the display units found above
                for (j = 0; j < m->size1; j++) {
                    if (cancellationFlag) break;
                    if (j > 0) fprintf(output, ",");
                    fprintf(output, "[");
                    // Write each element of the row
                    for (k = 0; k < m->size2; k++) {
                        if (cancellationFlag) break;
                        if (k > 0) fprintf(output, ",");
                        fprintf(output, "%s",
                                ppl_numericDisplay(gsl_matrix_get(m, j, k) * real, c->numdispBuff[0], NSigFigs, 0));
                    }
                    fprintf(output, "]");
                }
                fprintf(output, "])");
                if (!o->dimensionless) fprintf(output, "%s", unit);
            }
            break;
        }
        case PPLOBJ_EXC: {
            fprintf(output, "types.exception(");
            pplObjDump_escaped(output, o->auxil, "");
            fprintf(output, ")");
            break;
        }
        case PPLOBJ_FILE:
        case PPLOBJ_FUNC:
        case PPLOBJ_TYPE:
        case PPLOBJ_NULL:
        case PPLOBJ_GLOB:
        case PPLOBJ_ZOM:
        case PPLOBJ_EXP:
        case PPLOBJ_BYT:
        default:
            fprintf(output, "types.null()");
            break;
    }
    return;
}
