// datafile_rasters.c
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

// Chooses special rasters of sample points for functions created by the histogram and fft commands. These functions are
// step functions, so sampling them uniformly would misrepresent them; instead, when an expression plotted is just such
// a function of the dummy variable, it is sampled once at the centre of each bin of the histogram, or at each grid
// point of the Fourier transform. Used when plotting and tabulating functions (datafile.c and eps_plot.c).

#define _DATAFILE_RASTERS_C 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "coreUtils/errorReport.h"
#include "coreUtils/memAlloc.h"
#include "coreUtils/list.h"
#include "coreUtils/dict.h"
#include "parser/parser.h"
#include "settings/settings.h"
#include "settings/settingTypes.h"
#include "stringTools/asciidouble.h"
#include "stringTools/strConstants.h"
#include "userspace/context.h"
#include "userspace/contextVarDef.h"
#include "userspace/pplObj.h"
#include "userspace/pplObjFunc.h"
#include "userspace/unitsArithmetic.h"

//! ppldata_fromFuncs_checkSpecialRaster - When plotting a function of a histogram or FFT which was created with
//! the histogram or fft command, choose a raster of sample points at the centres of the bins of the histogram or the
//! grid points of the Fourier transform, rather than a uniform raster
//! \param [in] C - The Pyxplot context
//! \param [in] fnlist - The list of expressions being plotted
//! \param [in] fnlist_len - The number of expressions in <fnlist>
//! \param [in] dummyVar - The name of the dummy variable, e.g. "x"
//! \param [in] min - The lower limit of the plot range, or NULL if unset
//! \param [in] max - The upper limit of the plot range, or NULL if unset
//! \param [in] unit - The physical units of the dummy variable
//! \param [out] raster - Set to point to the special raster, allocated in the current ppl_memAlloc context, if one is
//! chosen; otherwise left unchanged
//! \param [out] rasterLen - Set to the number of points in the special raster, if one is chosen

void ppldata_fromFuncs_checkSpecialRaster(ppl_context *C, pplExpr **fnlist, const int fnlist_len, const char *dummyVar,
                                          const double *min, const double *max, const pplObj *unit, double **raster,
                                          int *rasterLen) {
    char msg[LSTR_LENGTH];
    int pos, commaPos = 0, bracketLevel = 0, containsOperator = 0, containsDummy = 0, NWords = 0,
                   contextRough = -1;
    int dummyArgNo = 0;
    char newWord = 1, fail = 0;
    char *buf = msg;
    char *currentFnName = "", *dummyInFunction = NULL;
    pplObj *funcObjPtr = NULL;
    const pplFunc *funcPtr = NULL;

    // Make temporary rough workspace
    const int outputContext = ppl_memAlloc_GetMemContext();
    contextRough = ppl_memAlloc_DescendIntoNewContext();
    list *bracketStack = ppl_listInit(0);
    list *bracketCommaPos = ppl_listInit(0);

    // Parse each expression, to check that the dummy variable appears only as a bare argument of a single function
    for (int i = 0; i < fnlist_len; i++) {
        int j = pos = 0;
        // Step through each character of the expression
        while (1) {
            char c = fnlist[i]->ascii[j++];
            // Accumulate the characters of a word
            if ((isalnum(c)) || (c == '_')) {
                newWord = 0;
                buf[pos++] = c;
                continue;
            }
            if ((!newWord) && (pos > 0)) NWords++;
            if (!newWord) buf[pos++] = '\0';
            newWord = 1;
            pos = 0;
            if (((unsigned char) c <= ' ') && (c != '\0')) continue;
            // The word is the dummy variable; it must be the only thing in an argument of a function, and always of the
            // same function
            if (strcmp(buf, dummyVar) == 0) {
                containsDummy = 1;
                if (containsOperator || (NWords > 1)) fail = 1;
                if ((dummyInFunction != NULL) && (currentFnName != NULL)) {
                    if ((strcmp(dummyInFunction, currentFnName) != 0) || (dummyArgNo != commaPos)) fail = 1;
                }
                if (currentFnName == NULL) fail = 1;
                // Note which function, and which argument
                else {
                    dummyInFunction = currentFnName;
                    dummyArgNo = commaPos;
                }
            }
            // An opening bracket starts the arguments of the function named by the preceding word
            if (c == '(') {
                if (containsDummy) fail = 1;
                ppl_listAppendCpy(bracketCommaPos, &commaPos, sizeof(int));
                ppl_listAppendCpy(bracketStack, buf, strlen(buf) + 1);
                bracketLevel++;
                currentFnName = (char *) ppl_listLast(bracketStack);
                containsOperator = 0;
                containsDummy = 0;
                NWords = 0;
                buf[0] = '\0';
                continue;
            }
            buf[0] = '\0';
            // A closing bracket ends the arguments of a function
            if (c == ')') {
                // Return to the enclosing function
                if (bracketLevel > 0) {
                    ppl_listPop(bracketStack);
                    currentFnName = (char *) ppl_listLast(bracketStack);
                    commaPos = *(int *) ppl_listPop(bracketCommaPos);
                    containsOperator = 1;
                    containsDummy = 0;
                    NWords = 10;
                    bracketLevel--;
                }
                continue;
            }
            // A comma starts the next argument
            if (c == ',') {
                commaPos++;
                containsOperator = 0;
                containsDummy = 0;
                NWords = 0;
                continue;
            }
            if (c == '\0') break;
            containsOperator = 1;
            if (containsDummy) fail = 1;
        }
    }

    // Give up unless the dummy variable is an argument of a single function
    if (fail || (dummyInFunction == NULL) || (dummyInFunction[0] == '\0')) goto CLEANUP;

    // Look up function which dummy variable is an argument to
    ppl_contextVarLookup(C, dummyInFunction, &funcObjPtr, 0);
    if ((funcObjPtr == NULL) || (funcObjPtr->objType != PPLOBJ_FUNC)) goto CLEANUP;
    funcPtr = (pplFunc *) funcObjPtr->auxil;
    if (funcPtr == NULL) goto CLEANUP;
    if ((funcPtr->functionType != PPL_FUNC_HISTOGRAM) && (funcPtr->functionType != PPL_FUNC_FFT)) goto CLEANUP;
    if (dummyArgNo >= funcPtr->minArgs) goto CLEANUP;

    // Make custom raster
    if (funcPtr->functionType == PPL_FUNC_HISTOGRAM) {
        const histogramDescriptor *desc = (histogramDescriptor *) funcPtr->functionPtr;
        int rasterCount = 0;
        long NInput = desc->Nbins - 1;
        if (NInput < 1) NInput = 1;

        // Allocate the raster in the caller's memory context
        double *outputRaster = (double *) ppl_memAlloc_incontext(NInput * sizeof(double), outputContext);
        if (outputRaster == NULL) goto CLEANUP;

        // Sample at the centre of each bin within the plot range, using geometric centres for logarithmic bins
        for (int i = 0; i < (desc->Nbins - 1); i++) {
            const double midpoint =
                    desc->log ? sqrt(desc->bins[i] * desc->bins[i + 1]) : ((desc->bins[i] + desc->bins[i + 1]) / 2);
            if (((min == NULL) || (*min <= midpoint)) && ((max == NULL) || (*max >= midpoint))) {
                outputRaster[rasterCount++] = midpoint;
            }
        }

        *raster = outputRaster;
        *rasterLen = rasterCount;
    } else if (funcPtr->functionType == PPL_FUNC_FFT) {
        const FFTDescriptor *desc = (FFTDescriptor *) funcPtr->functionPtr;
        int rasterCount = 0;

        // The dummy variable must be one of the transform's arguments
        if (dummyArgNo >= desc->Ndims) goto CLEANUP;
        double *outputRaster = (double *) ppl_memAlloc_incontext(desc->XSize[dummyArgNo] * sizeof(double),
                                outputContext);
        if (outputRaster == NULL) goto CLEANUP;

        // Sample at each grid point within the plot range
        for (int i = 0; i < desc->XSize[dummyArgNo]; i++) {
            const double x = (i - desc->XSize[dummyArgNo] / 2.0) / desc->range[dummyArgNo].real;
            if (((min == NULL) || (*min <= x)) && ((max == NULL) || (*max >= x))) { outputRaster[rasterCount++] = x; }
        }

        *raster = outputRaster;
        *rasterLen = rasterCount;
    }

// Free the temporary workspace
CLEANUP:
    // Delete rough workspace
    if (contextRough > 0) ppl_memAlloc_AscendOutOfContext(contextRough);
    return;
}
