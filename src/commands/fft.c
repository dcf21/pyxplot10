// fft.c
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

// Implements the fft and ifft commands, which take the discrete Fourier transform of data on a regular grid in one or
// more dimensions, read from a file or sampled from a function, and store it as a new function of frequency. The data
// are optionally multiplied by a window function first, transformed with FFTW, and normalised and phase-shifted so that
// the result approximates the continuous Fourier transform. ppl_fft_evaluate() is called by the expression evaluator
// when the function is used.

#define _FFT_C 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <gsl/gsl_math.h>

#ifdef HAVE_FFTW3
#include <fftw3.h>
#else
#include <fftw.h>
#endif

#include "commands/core.h"
#include "commands/fft.h"
#include "userspace/stack.h"
#include "coreUtils/errorReport.h"
#include "coreUtils/memAlloc.h"
#include "expressions/expCompile_fns.h"
#include "expressions/expEval.h"
#include "expressions/traceback_fns.h"
#include "parser/cmdList.h"
#include "parser/parser.h"
#include "settings/settings.h"
#include "settings/settingTypes.h"
#include "stringTools/asciidouble.h"
#include "userspace/context.h"
#include "userspace/contextVarDef.h"
#include "userspace/garbageCollector.h"
#include "userspace/pplObj_fns.h"
#include "userspace/pplObjFunc.h"
#include "userspace/unitsArithmetic.h"
#include "userspace/unitsDisp.h"

#include "datafile.h"


//! fftwindow_rectangular - Apply a rectangular window function (i.e. no windowing) to a sample of data before
//! Fourier transforming it
//! \param [in|out] x - The data sample, which is multiplied by the window function
//! \param [in] Ndim - The number of dimensions of the data grid
//! \param [in] Npos - The position of the sample along each dimension of the data grid
//! \param [in] Nstep - The number of samples along each dimension of the data grid

static void fftwindow_rectangular(pplObj *x, const int Ndim, int *Npos, int *Nstep) {
}

//! fftwindow_hamming - Apply a Hamming window function to a sample of data before Fourier transforming it
//! \param [in|out] x - The data sample, which is multiplied by the window function
//! \param [in] Ndim - The number of dimensions of the data grid
//! \param [in] Npos - The position of the sample along each dimension of the data grid
//! \param [in] Nstep - The number of samples along each dimension of the data grid

static void fftwindow_hamming(pplObj *x, const int Ndim, int *Npos, int *Nstep) {
    double y = 1.0;
    for (int i = 0; i < Ndim; i++) {
        y *= 0.54 - 0.46 * cos(2 * M_PI * ((double) Npos[i]) / ((double) (Nstep[i] - 1)));
    }
    x->real *= y;
    x->imag *= y;
    // A sample with no imaginary part is marked as real
    if (x->imag == 0.0) {
        x->imag = 0.0;
        x->flagComplex = 0;
    }
}

//! fftwindow_hann - Apply a Hann window function to a sample of data before Fourier transforming it
//! \param [in|out] x - The data sample, which is multiplied by the window function
//! \param [in] Ndim - The number of dimensions of the data grid
//! \param [in] Npos - The position of the sample along each dimension of the data grid
//! \param [in] Nstep - The number of samples along each dimension of the data grid

static void fftwindow_hann(pplObj *x, const int Ndim, int *Npos, int *Nstep) {
    double y = 1.0;
    for (int i = 0; i < Ndim; i++) {
        y *= 0.5 * (1.0 - cos(2 * M_PI * ((double) Npos[i]) / ((double) (Nstep[i] - 1))));
    }
    x->real *= y;
    x->imag *= y;
    // A sample with no imaginary part is marked as real
    if (x->imag == 0.0) {
        x->imag = 0.0;
        x->flagComplex = 0;
    }
}

//! fftwindow_cosine - Apply a cosine window function to a sample of data before Fourier transforming it
//! \param [in|out] x - The data sample, which is multiplied by the window function
//! \param [in] Ndim - The number of dimensions of the data grid
//! \param [in] Npos - The position of the sample along each dimension of the data grid
//! \param [in] Nstep - The number of samples along each dimension of the data grid

static void fftwindow_cosine(pplObj *x, const int Ndim, int *Npos, int *Nstep) {
    double y = 1.0;
    for (int i = 0; i < Ndim; i++) { y *= sin(M_PI * ((double) Npos[i]) / ((double) (Nstep[i] - 1))); }
    x->real *= y;
    x->imag *= y;
    // A sample with no imaginary part is marked as real
    if (x->imag == 0.0) {
        x->imag = 0.0;
        x->flagComplex = 0;
    }
}

//! fftwindow_lanczos - Apply a Lanczos window function to a sample of data before Fourier transforming it
//! \param [in|out] x - The data sample, which is multiplied by the window function
//! \param [in] Ndim - The number of dimensions of the data grid
//! \param [in] Npos - The position of the sample along each dimension of the data grid
//! \param [in] Nstep - The number of samples along each dimension of the data grid

static void fftwindow_lanczos(pplObj *x, const int Ndim, int *Npos, int *Nstep) {
    double y = 1.0;
    // The window is the product of the window function in each dimension
    for (int i = 0; i < Ndim; i++) {
        const double z = 2 * M_PI * ((double) Npos[i]) / ((double) (Nstep[i] - 1)) - M_PI;
        // sin(z)/z tends to one at the central sample, where z is zero
        if (z != 0.0) y *= sin(z) / z;
    }
    x->real *= y;
    x->imag *= y;
    // A sample with no imaginary part is marked as real
    if (x->imag == 0.0) {
        x->imag = 0.0;
        x->flagComplex = 0;
    }
}

//! fftwindow_bartlett - Apply a Bartlett window function to a sample of data before Fourier transforming it
//! \param [in|out] x - The data sample, which is multiplied by the window function
//! \param [in] Ndim - The number of dimensions of the data grid
//! \param [in] Npos - The position of the sample along each dimension of the data grid
//! \param [in] Nstep - The number of samples along each dimension of the data grid

static void fftwindow_bartlett(pplObj *x, const int Ndim, int *Npos, int *Nstep) {
    double y = 1.0;
    for (int i = 0; i < Ndim; i++) {
        y *= (2.0 / (Nstep[i] - 1)) * ((Nstep[i] - 1) / 2.0 - fabs(Npos[i] - (Nstep[i] - 1) / 2.0));
    }
    x->real *= y;
    x->imag *= y;
    // A sample with no imaginary part is marked as real
    if (x->imag == 0.0) {
        x->imag = 0.0;
        x->flagComplex = 0;
    }
}

//! fftwindow_triangular - Apply a triangular window function to a sample of data before Fourier transforming it
//! \param [in|out] x - The data sample, which is multiplied by the window function
//! \param [in] Ndim - The number of dimensions of the data grid
//! \param [in] Npos - The position of the sample along each dimension of the data grid
//! \param [in] Nstep - The number of samples along each dimension of the data grid

static void fftwindow_triangular(pplObj *x, const int Ndim, int *Npos, int *Nstep) {
    double y = 1.0;
    for (int i = 0; i < Ndim; i++) {
        y *= (2.0 / (Nstep[i])) * ((Nstep[i]) / 2.0 - fabs(Npos[i] - (Nstep[i] - 1) / 2.0));
    }
    x->real *= y;
    x->imag *= y;
    // A sample with no imaginary part is marked as real
    if (x->imag == 0.0) {
        x->imag = 0.0;
        x->flagComplex = 0;
    }
}

//! fftwindow_gauss - Apply a Gaussian window function to a sample of data before Fourier transforming it
//! \param [in|out] x - The data sample, which is multiplied by the window function
//! \param [in] Ndim - The number of dimensions of the data grid
//! \param [in] Npos - The position of the sample along each dimension of the data grid
//! \param [in] Nstep - The number of samples along each dimension of the data grid

static void fftwindow_gauss(pplObj *x, const int Ndim, int *Npos, int *Nstep) {
    double y = 1.0;
    const double sigma = 0.5;
    for (int i = 0; i < Ndim; i++) {
        y *= exp(-0.5 * pow((Npos[i] - ((Nstep[i] - 1) / 2.0)) / (sigma * (Nstep[i] - 1) / 2.0), 2.0));
    }
    x->real *= y;
    x->imag *= y;
    // A sample with no imaginary part is marked as real
    if (x->imag == 0.0) {
        x->imag = 0.0;
        x->flagComplex = 0;
    }
}

//! fftwindow_bartletthann - Apply a Bartlett-Hann window function to a sample of data before Fourier transforming
//! it
//! \param [in|out] x - The data sample, which is multiplied by the window function
//! \param [in] Ndim - The number of dimensions of the data grid
//! \param [in] Npos - The position of the sample along each dimension of the data grid
//! \param [in] Nstep - The number of samples along each dimension of the data grid

static void fftwindow_bartletthann(pplObj *x, const int Ndim, int *Npos, int *Nstep) {
    double y = 1.0;
    // The window is the product of the window function in each dimension
    for (int i = 0; i < Ndim; i++) {
        y *= 0.62 - 0.48 * (((double) Npos[i]) / ((double) (Nstep[i] - 1)) - 0.5) -
                0.38 * cos(2 * M_PI * ((double) Npos[i]) / ((double) (Nstep[i] - 1)));
    }
    x->real *= y;
    x->imag *= y;
    // A sample with no imaginary part is marked as real
    if (x->imag == 0.0) {
        x->imag = 0.0;
        x->flagComplex = 0;
    }
}

//! fftwindow_blackman - Apply a Blackman window function to a sample of data before Fourier transforming it
//! \param [in|out] x - The data sample, which is multiplied by the window function
//! \param [in] Ndim - The number of dimensions of the data grid
//! \param [in] Npos - The position of the sample along each dimension of the data grid
//! \param [in] Nstep - The number of samples along each dimension of the data grid

static void fftwindow_blackman(pplObj *x, const int Ndim, int *Npos, int *Nstep) {
    double y = 1.0;
    const double alpha = 0.16;
    // The window is the product of the window function in each dimension
    for (int i = 0; i < Ndim; i++) {
        y *= (1.0 - alpha) / 2.0 - 0.5 * cos(2 * M_PI * ((double) Npos[i]) / ((double) (Nstep[i] - 1))) +
                alpha / 2.0 * cos(4 * M_PI * ((double) Npos[i]) / ((double) (Nstep[i] - 1)));
    }
    x->real *= y;
    x->imag *= y;
    // A sample with no imaginary part is marked as real
    if (x->imag == 0.0) {
        x->imag = 0.0;
        x->flagComplex = 0;
    }
}

//! ppl_directive_fft - Implement the fft and ifft commands, which compute the Fourier transform of data from a file
//! or of a function sampled on a regular grid, and store the result as a new function
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppl_directive_fft(ppl_context *c, parserLine *pl, parserOutput *in, const int interactive, const int iterDepth) {
    // The variables used to read the data, transform it and store the result
    pplObj *stk = in->stk;
    dataTable *data;
    dataBlock *blk;
    long int i = 0, k;
    int contextLocalVec, contextDataTab, errCount = DATAFILE_NERRS;
    int Ndims = 0, inverse, Nsamples, Nsteps[USING_ITEMS_MAX];
    pplFunc *funcPtr;
    pplObj firstEntry;
    char filenameOut[FNAME_LENGTH] = "";
    pplObj unit[USING_ITEMS_MAX];
    int minSet[USING_ITEMS_MAX], maxSet[USING_ITEMS_MAX];
    double min[USING_ITEMS_MAX], max[USING_ITEMS_MAX], step[USING_ITEMS_MAX];
    parserLine *spool = NULL, **dataSpool = &spool;
    fftw_complex *datagrid;
    FFTDescriptor *output;
    void (*WindowType)(pplObj *, int, int *, int *);

#ifdef HAVE_FFTW3
    fftw_plan fftwplan; // FFTW 3.x
#else
    fftwnd_plan fftwplan; // FFTW 2.x
#endif

    // Check whether we are doing a forward or a backward FFT
    if (strcmp((char *) stk[PARSE_ifft_directive].auxil, "ifft") == 0) inverse = 1;
    else inverse = 0;

    // Read ranges
    {
        int pos = PARSE_ifft_range_list, nr = 0;
        const int o1 = PARSE_ifft_min_range_list;
        const int o2 = PARSE_ifft_max_range_list;
        const int o3 = PARSE_ifft_step_range_list;
        for (int i = 0; i < USING_ITEMS_MAX; i++) minSet[i] = maxSet[i] = 0;
        // Walk the linked list of ranges on the parser stack; each gives the minimum, maximum and step of one dimension
        // of the grid
        while (stk[pos].objType == PPLOBJ_NUM) {
            pos = (int) round(stk[pos].real);
            if (pos <= 0) break;
            // There is a fixed limit on the number of dimensions
            if (nr >= USING_ITEMS_MAX) {
                ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                                  "Too many ranges specified; a maximum of %d are allowed.", USING_ITEMS_MAX);
                ppl_tbAddLine(c, pl, ERR_SYNTAX, 0, "");
                return;
            }
            if (ppldata_readRange(c, pl, in, pos, o1, o2, nr, NULL, unit, min, minSet, max, maxSet)) return;
            if ((stk[pos + o3].objType == PPLOBJ_NUM) || (stk[pos + o3].objType == PPLOBJ_DATE) ||
                (stk[pos + o3].objType == PPLOBJ_BOOL)) {
                // The step must have the same type as the limits
                if (unit[nr].objType != stk[pos + o3].objType) {
                    ppl_snprintfTrunc(
                            c->errStat.errBuff, sizeof(c->errStat.errBuff),
                            "Minimum and maximum limits specified for variable %d have conflicting types of <%s> and <%s>.",
                            nr + 1, pplObjTypeNames[unit[nr].objType], pplObjTypeNames[stk[pos + o3].objType]);
                    ppl_tbAddLine(c, pl, ERR_TYPE, in->stkCharPos[pos + PARSE_ifft_step_range_list], "");
                    return;
                }
                // The step must have the same units as the limits
                if (!ppl_unitsDimEqual(&unit[nr], &stk[pos + o3])) {
                    ppl_snprintfTrunc(
                            c->errStat.errBuff, sizeof(c->errStat.errBuff),
                            "Minimum and maximum limits specified for variable %d have conflicting physical units of <%s> and <%s>.",
                            nr + 1, ppl_printUnit(c, &unit[nr], NULL, NULL, 0, 0, 0),
                            ppl_printUnit(c, &stk[pos + o3], NULL, NULL, 1, 0, 0));
                    ppl_tbAddLine(c, pl, ERR_UNIT, in->stkCharPos[pos + PARSE_ifft_step_range_list], "");
                    return;
                }
                step[nr] = stk[pos + o3].real;
                const double tempDbl = 1.0 +
                        floor((max[nr] - min[nr]) / step[nr] + 0.5); // Add one because this is a fencepost problem
                // There must be at least two samples in each dimension
                if (tempDbl < 2.0) {
                    ppl_snprintfTrunc(
                            c->errStat.errBuff, sizeof(c->errStat.errBuff),
                            "The number of samples produced by the range and step size specified for dimension %d to the fft command is fewer than two; a single data sample cannot be FFTed.",
                            nr + 1);
                    ppl_tbAddLine(c, pl, ERR_RANGE, in->stkCharPos[pos + PARSE_ifft_step_range_list], "");
                    return;
                }
                // The number of samples must be finite and not unreasonably large
                if ((!gsl_finite(tempDbl) || (tempDbl > 1e8))) {
                    ppl_snprintfTrunc(
                            c->errStat.errBuff, sizeof(c->errStat.errBuff),
                            "The number of samples produced by the range and step size specified for dimension %d to the fft command is in excess of 1e8; Pyxplot is not the right tool to do this FFT in.",
                            nr + 1);
                    ppl_tbAddLine(c, pl, ERR_RANGE, in->stkCharPos[pos + PARSE_ifft_step_range_list], "");
                    return;
                }
                Nsteps[nr] = (int) tempDbl;
            } else {
                Nsteps[nr] = 100; // shouldn't be here
                step[nr] = floor((max[nr] - min[nr]) / (Nsteps[nr] - 1));
            }
            nr++;
        }
        Ndims = nr;
    }

    // Work out total size of FFT data grid
    {
        double tempDbl = 1.0;
        for (i = 0; i < Ndims; i++) tempDbl *= Nsteps[i];
        // Refuse to perform unreasonably large transforms
        if (tempDbl > 1e8) {
            ppl_snprintfTrunc(
                    c->errStat.errBuff, sizeof(c->errStat.errBuff),
                    "The total number of samples in the requested %d-dimensional FFT is in excess of 1e8; Pyxplot is not the right tool to do this FFT in.",
                    Ndims);
            ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
            return;
        }
        Nsamples = (int) tempDbl;
    }

    // Allocate a new memory context for the data file we're about to read
    contextLocalVec = ppl_memAlloc_DescendIntoNewContext();
    contextDataTab = ppl_memAlloc_DescendIntoNewContext();

    // Work out what type of window we're going to use
    {
        const int gotWindow = (stk[PARSE_ifft_window].objType == PPLOBJ_STR);
        char *cptr = (char *) stk[PARSE_ifft_window].auxil;
        WindowType = fftwindow_rectangular;
        // Select the window function named in the command
        if (gotWindow) {
            if (strcmp(cptr, "hamming") == 0) WindowType = fftwindow_hamming;
            else if (strcmp(cptr, "hann") == 0) WindowType = fftwindow_hann;
            else if (strcmp(cptr, "cosine") == 0) WindowType = fftwindow_cosine;
            else if (strcmp(cptr, "lanczos") == 0) WindowType = fftwindow_lanczos;
            else if (strcmp(cptr, "bartlett") == 0) WindowType = fftwindow_bartlett;
            else if (strcmp(cptr, "triangular") == 0) WindowType = fftwindow_triangular;
            else if (strcmp(cptr, "gauss") == 0) WindowType = fftwindow_gauss;
            else if (strcmp(cptr, "bartletthann") == 0) WindowType = fftwindow_bartletthann;
            else if (strcmp(cptr, "blackman") == 0) WindowType = fftwindow_blackman;
        }
    }

    // Read input data
    if (stk[PARSE_ifft_filename].objType == PPLOBJ_STR) {
        const int NcolRequired = Ndims + 2;
        int status = 0, j;
        ppldata_fromCmd(c, &data, pl, in, 0, filenameOut, dataSpool, PARSE_TABLE_ifft_, 0, NcolRequired, 0, min, minSet,
                        max, maxSet, unit, 0, &status, c->errStat.errBuff, &errCount, iterDepth);

        // Exit on error
        if ((status) || (data == NULL)) {
            ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "");
            ppl_memAlloc_AscendOutOfContext(contextLocalVec);
            return;
        }

        // Check that the firstEntries above have the same units as any supplied ranges
        for (j = 0; j < Ndims; j++)
            // Only columns with a range need checking
            if (minSet[j] || maxSet[j]) {
                // The range must have the same units as the data
                if (!ppl_unitsDimEqual(&unit[j], data->firstEntries + j)) {
                    ppl_snprintfTrunc(
                            c->errStat.errBuff, sizeof(c->errStat.errBuff),
                            "The minimum and maximum limits specified in range %d in the fft command have conflicting physical dimensions with the data returned from the data file. The limits have units of <%s>, whilst the data have units of <%s>.",
                            j + 1, ppl_printUnit(c, unit + j, NULL, NULL, 0, 1, 0),
                            ppl_printUnit(c, data->firstEntries + j, NULL, NULL, 1, 1, 0));
                    ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
                    return;
                }
            }

        // Read unit of f(x) in final two columns of data table (real and imaginary components of f(x)
        firstEntry = data->firstEntries[Ndims];
        firstEntry.real = 1.0;
        firstEntry.imag = 0.0;
        firstEntry.flagComplex = 0;
        // The real and imaginary parts must have the same units
        if (!ppl_unitsDimEqual(&data->firstEntries[Ndims], &data->firstEntries[Ndims + 1])) {
            ppl_snprintfTrunc(
                    c->errStat.errBuff, sizeof(c->errStat.errBuff),
                    "Data in columns %d and %d of the data table supplied to the fft command have conflicting units of <%s> and <%s> respectively. These represent the real and imaginary components of an input sample, and must have the same units.",
                    Ndims + 1, Ndims + 2, ppl_printUnit(c, &data->firstEntries[Ndims], NULL, NULL, 0, 1, 0),
                    ppl_printUnit(c, &data->firstEntries[Ndims + 1], NULL, NULL, 1, 1, 0));
            ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
            return;
        }


        // Check that we have at least two datapoints
        if (data->Nrows < 2) {
            ppl_snprintfTrunc(
                    c->errStat.errBuff, sizeof(c->errStat.errBuff),
                    "FFT construction is only possible on data sets with members at at least two values of x.");
            ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
            return;
        }

        // Allocate workspace in which to do FFT
        datagrid = (fftw_complex *) fftw_malloc(Nsamples * sizeof(fftw_complex));
        // Failure to allocate memory is reported as an error
        if (datagrid == NULL) {
            ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "Out of memory.");
            ppl_tbAddLine(c, pl, ERR_MEMORY, 0, "");
            return;
        }

        // Loop through data table
        blk = data->first;
        j = 0;
        // Copy each sample from the data table into the grid, checking that the data lie on the expected grid in
        // row-major order
        for (i = 0; i < Nsamples; i++) {
            int l, Npos[USING_ITEMS_MAX];
            double pos[USING_ITEMS_MAX];
            // Report the data running out before the grid is full
            if ((blk == NULL) || (j == blk->blockPosition)) {
                ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                                  "Premature end to data table supplied to the fft command. To perform a ");
                k = strlen(c->errStat.errBuff);
                // Write the dimensions of the grid as e.g. 10x20
                for (l = 0; l < Ndims; l++) {
                    sprintf(c->errStat.errBuff + k, "%dx", Nsteps[l]);
                    k += strlen(c->errStat.errBuff + k);
                }
                k -= (Ndims > 0);
                sprintf(c->errStat.errBuff + k,
                        " Fourier transform, need a grid of %d samples. Only received %ld samples.", Nsamples, i);
                ppl_tbAddLine(c, pl, ERR_FILE, 0, "");
                return;
            }

            // Work out what position we're expecting this data point to represent
            for (k = i, l = Ndims - 1; l >= 0; l--) {
                Npos[l] = (k % Nsteps[l]);
                pos[l] = min[l] + step[l] * Npos[l];
                k /= Nsteps[l];
            }

            // Check that first Ndims columns indeed represent this point
            for (k = 0; k < Ndims; k++)
                // Report a data point which is not where it should be on the grid
                if (!ppl_dblEqual(blk->data_real[k + (Ndims + 2) * j], pos[k])) {
                    ppl_snprintfTrunc(
                            c->errStat.errBuff, sizeof(c->errStat.errBuff),
                            "Data supplied to fft command must be on a regular rectangular grid and in row-major ordering. Row %ld should represent a data point at position (",
                            i + 1);
                    int m = strlen(c->errStat.errBuff);
                    // Write the expected position
                    for (l = 0; l < Ndims; l++) {
                        pplObj x = unit[l];
                        x.real = pos[l];
                        sprintf(c->errStat.errBuff + m, "%s,", ppl_unitsNumericDisplay(c, &x, 0, 1, -1));
                        m += strlen(c->errStat.errBuff + m);
                    }
                    m -= (Ndims > 0);
                    sprintf(c->errStat.errBuff + m, "). In fact, it contained a data point at position (");
                    m += strlen(c->errStat.errBuff + m);
                    // Write the position found
                    for (l = 0; l < Ndims; l++) {
                        pplObj x = unit[l];
                        x.real = blk->data_real[l + (Ndims + 2) * j];
                        sprintf(c->errStat.errBuff + m, "%s,", ppl_unitsNumericDisplay(c, &x, 0, 1, -1));
                        m += strlen(c->errStat.errBuff + m);
                    }
                    m -= (Ndims > 0);
                    sprintf(c->errStat.errBuff + m, ").");
                    j = strlen(c->errStat.errBuff);
                    ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
                    return;
                }

            {
                pplObj x;
                x.refCount = 1;
                pplObjNum(&x, 0, 0, 0);
                x.real = blk->data_real[(Ndims + 0) + (Ndims + 2) * j];
                x.imag = blk->data_real[(Ndims + 1) + (Ndims + 2) * j];
                // A sample with no imaginary part is marked as real
                if (x.imag == 0) {
                    x.flagComplex = 0;
                    x.imag = 0.0;
                } else {
                    x.flagComplex = 1;
                }
                (*WindowType)(&x, Ndims, Npos, Nsteps); // Apply window function to data
#ifdef HAVE_FFTW3
                datagrid[i][0] = x.real;
                datagrid[i][1] = x.imag;
#else
                datagrid[i].re = x.real;
                datagrid[i].im = x.imag;
#endif
            }
            j++;
            // Move on to the next block of the data table
            if (j == blk->blockPosition) {
                j = 0;
                blk = blk->next;
            }
        }
    } else // Fetch data from a function
    {
        int pos = PARSE_ifft_fnnames;
        char *scratchpad = (char *) malloc(LSTR_LENGTH);

        // Allocate workspace in which to do FFT
        datagrid = (fftw_complex *) fftw_malloc(Nsamples * sizeof(fftw_complex));
        // Failure to allocate memory is reported as an error
        if ((datagrid == NULL) || (scratchpad == NULL)) {
            // Free the grid if it was allocated
            if (datagrid != NULL) {
                fftw_free(datagrid);
                datagrid = NULL;
            }
            free(scratchpad);
            scratchpad = NULL;
            ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "Out of memory.");
            ppl_tbAddLine(c, pl, ERR_MEMORY, 0, "");
            return;
        }

        // Print function name
        int fnlen = 0;
        // The function's name may have several parts, separated by dots
        while (stk[pos].objType == PPLOBJ_NUM) {
            pos = (int) round(stk[pos].real);
            if (pos <= 0) break;
            if (fnlen != 0) ppl_strAppendChar(scratchpad, LSTR_LENGTH, &fnlen, '.');
            ppl_strAppendf(scratchpad, LSTR_LENGTH, &fnlen, "%s", (char *) stk[pos + PARSE_ifft_fnname_fnnames].auxil);
        }
        ppl_strAppendChar(scratchpad, LSTR_LENGTH, &fnlen, '(');

        // Sample the function at each point of the grid, by compiling and evaluating an expression which calls it there
        for (int i = 0; i < Nsamples; i++) {
            const int stkLevelOld = c->stackPtr;
            int j = fnlen;
            int k = i, explen2, errPos = -1, errType = -1, lastOpAssign;
            int Npos[USING_ITEMS_MAX];
            double pos[USING_ITEMS_MAX];
            pplExpr *e;
            // Work out the position of this sample on the grid, in row-major order
            for (int l = Ndims - 1; l >= 0; l--) {
                Npos[l] = (k % Nsteps[l]);
                pos[l] = min[l] + step[l] * Npos[l];
                k /= Nsteps[l];
            }
            // Write the position as the arguments of the function call
            for (int l = 0; l < Ndims; l++) {
                pplObj x = unit[l];
                x.real = pos[l];
                ppl_strAppendf(scratchpad, LSTR_LENGTH, &j, "%s,", ppl_unitsNumericDisplay(c, &x, 0, 1, 20));
            }
            // Replace the final comma with a closing bracket
            j -= (Ndims > 0);
            scratchpad[j] = '\0';
            ppl_strAppendChar(scratchpad, LSTR_LENGTH, &j, ')');

            const int explen1 = explen2 = j; // The length of the expression, up to and including the closing bracket
            ppl_error_setstreaminfo(&c->errcontext, 1, "fitting expression");
            ppl_expCompile(c, pl->srcLineN, pl->srcId, pl->srcFname, scratchpad, &explen2, 1, 1, 1, &e, &errPos,
                           &errType, c->errStat.errBuff);
            // Give up if the expression could not be compiled
            if (c->errStat.status || (errPos >= 0)) {
                fftw_free(datagrid);
                datagrid = NULL;
                free(scratchpad);
                scratchpad = NULL;
                return;
            }
            // The whole expression must be compiled
            if (explen2 < explen1) {
                fftw_free(datagrid);
                datagrid = NULL;
                free(scratchpad);
                scratchpad = NULL;
                strcpy(c->errStat.errBuff, "Unexpected trailing matter at the end of expression.");
                pplExpr_free(e);
                e = NULL;
                ppl_tbAddLine(c, pl, ERR_SYNTAX, 0, "");
                return;
            }
            pplObj *out = ppl_expEval(c, e, &lastOpAssign, 1, 1);
            pplExpr_free(e);
            e = NULL;
            // Give up if the function could not be evaluated
            if (c->errStat.status) {
                fftw_free(datagrid);
                datagrid = NULL;
                free(scratchpad);
                scratchpad = NULL;
                return;
            }
            // The function must return a number
            if (out->objType != PPLOBJ_NUM) {
                fftw_free(datagrid);
                datagrid = NULL;
                free(scratchpad);
                scratchpad = NULL;
                ppl_snprintfTrunc(
                        c->errStat.errBuff, sizeof(c->errStat.errBuff),
                        "The supplied function to fit produces a value which is not a number but has type <%s>.",
                        pplObjTypeNames[out->objType]);
                ppl_tbAddLine(c, pl, ERR_TYPE, 0, "");
                ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack before giving up
                return;
            }
            // The function must return a finite value
            if ((!gsl_finite(out->real)) || (!gsl_finite(out->imag))) {
                int j = 0;
                ppl_strAppendf(c->errStat.errBuff, sizeof(c->errStat.errBuff), &j,
                               "Could not evaluate input function at position %.*s", fnlen, scratchpad);
                // Write the position at which the function failed
                for (int l = 0; l < Ndims; l++) {
                    pplObj x = unit[l];
                    x.real = pos[l];
                    ppl_strAppendf(c->errStat.errBuff, sizeof(c->errStat.errBuff), &j, "%s,",
                                   ppl_unitsNumericDisplay(c, &x, 0, 1, -1));
                }
                ppl_strAppendf(c->errStat.errBuff, sizeof(c->errStat.errBuff), &j, ")");
                ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
                ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack
                fftw_free(datagrid);
                datagrid = NULL;
                free(scratchpad);
                scratchpad = NULL;
                return;
            }

            // The first sample sets the units of the function; all others must match
            if (i == 0) {
                firstEntry = *out;
                firstEntry.real = 1.0;
                firstEntry.imag = 0.0;
                firstEntry.flagComplex = 0;
            } else if (!ppl_unitsDimEqual(out, &firstEntry)) {
                ppl_snprintfTrunc(
                        c->errStat.errBuff, sizeof(c->errStat.errBuff),
                        "The supplied function to FFT does not produce values with consistent units; has produced values with units of <%s> and of <%s>.",
                        ppl_printUnit(c, &firstEntry, NULL, NULL, 0, 1, 0), ppl_printUnit(c, out, NULL, NULL, 1, 1, 0));
                ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
                ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack
                fftw_free(datagrid);
                datagrid = NULL;
                free(scratchpad);
                scratchpad = NULL;
                return;
            }
            (*WindowType)(out, Ndims, Npos, Nsteps); // Apply window function to data
#ifdef HAVE_FFTW3
            datagrid[i][0] = out->real;
            datagrid[i][1] = out->imag;
#else
            datagrid[i].re = out->real;
            datagrid[i].im = out->imag;
#endif
            ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack
        }
        free(scratchpad);
        scratchpad = NULL;
    }

    // We're finished... can now free DataTable
    ppl_memAlloc_AscendOutOfContext(contextLocalVec);

// FFT data
#ifdef HAVE_FFTW3
    fftwplan = fftw_plan_dft(Ndims, Nsteps, datagrid, datagrid, inverse ? FFTW_BACKWARD : FFTW_FORWARD,
                             FFTW_ESTIMATE); // FFTW 3.x
    fftw_execute(fftwplan);
    fftw_destroy_plan(fftwplan);
#else
    fftwplan = fftwnd_create_plan(Ndims, Nsteps, inverse ? FFTW_BACKWARD : FFTW_FORWARD, FFTW_ESTIMATE); // FFTW 2.x
    fftwnd_one(fftwplan, datagrid, datagrid);
    fftwnd_destroy_plan(fftwplan);
#endif

    // Make FFTDescriptor data structure
    output = (FFTDescriptor *) malloc(sizeof(FFTDescriptor));
    // Failure to allocate memory is reported as an error
    if (output == NULL) {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "Out of memory.");
        ppl_tbAddLine(c, pl, ERR_MEMORY, 0, "");
        fftw_free(datagrid);
        datagrid = NULL;
        return;
    }
    output->Ndims = Ndims;
    for (i = 0; i < Ndims; i++) { output->XSize[i] = Nsteps[i]; }
    // Store the span of the grid in each dimension, with its units
    for (i = 0; i < Ndims; i++) {
        pplObjNum(&output->range[i], 0, max[i] - min[i], 0);
        output->range[i].refCount = 1;
        ppl_unitsDimCpy(&output->range[i], &unit[i]);
    }
    // The arguments of the transform have the inverse units of the span
    for (i = 0; i < Ndims; i++) {
        pplObjNum(&output->invRange[i], 0, 0, 0);
        ppl_unitsDimInverse(&output->invRange[i], &output->range[i]);
    }
    output->datagrid = datagrid;

    // Apply normalisation to data and phase-shift it t put zero in the right place
    {
        int Npos[USING_ITEMS_MAX];
        double norm = 1.0;
        for (i = 0; i < Ndims; i++) norm *= output->range[i].real / Nsteps[i];
        for (i = 0; i < Ndims; i++) Npos[i] = -min[i] * Nsteps[i] / (max[i] - min[i]); // Position of zero
        // Multiply each sample by the normalisation and by the phase factor for the grid's origin
        for (i = 0; i < Nsamples; i++) {
            double normR = norm;
            double normI = 0.0;
            k = i;
            // Accumulate the phase factor for each dimension
            for (int l = Ndims - 1; l >= 0; l--) {
                const double pos = (k % Nsteps[l]);
                k /= Nsteps[l];
                const double angle = (inverse ? -1 : 1) * 2 * M_PI * pos * Npos[l] / Nsteps[l];
                const double normR2 = normR * cos(angle) - normI * sin(angle);
                const double normI2 = normR * sin(angle) + normI * cos(angle);
                normR = normR2;
                normI = normI2;
            }
#ifdef HAVE_FFTW3
            {
                // Multiply by the phase factor, keeping the old real part until the imaginary part has used it
                const double re = datagrid[i][0], im = datagrid[i][1];
                datagrid[i][0] = re * normR - im * normI;
                datagrid[i][1] = re * normI + im * normR;
            }
#else
            {
                // Multiply by the phase factor, keeping the old real part until the imaginary part has used it
                const double re = datagrid[i].re, im = datagrid[i].im;
                datagrid[i].re = re * normR - im * normI;
                datagrid[i].im = re * normI + im * normR;
            }
#endif
        }
    }

    // Make output unit
    {
        int status = 0, et = 0;
        output->outputUnit =
                firstEntry; // Output of an FFT has units of fn being FFTed, multiplied by the units of all of the arguments being FFTed
        // Multiply the units of the function by the units of each dimension
        for (i = 0; i < Ndims; i++) {
            ppl_uaMul(c, &output->outputUnit, &output->range[i], &output->outputUnit, &status, &et, c->errStat.errBuff);
            if (status) break;
        }
        // Give up if the units could not be multiplied
        if (status) {
            ppl_tbAddLine(c, pl, ERR_INTERNAL, 0, "");
            fftw_free(datagrid);
            datagrid = NULL;
            free(output);
            output = NULL;
            return;
        }
        output->outputUnit.real = output->outputUnit.imag = 0.0;
        output->outputUnit.flagComplex = 0; // Output unit has zero magnitude
    }

    // Make a new function descriptor
    funcPtr = (pplFunc *) malloc(sizeof(pplFunc));
    // Failure to allocate memory is reported as an error
    if (funcPtr == NULL) {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                          "Out of memory whilst adding FFT object to function dictionary.");
        ppl_tbAddLine(c, pl, ERR_MEMORY, 0, "");
        return;
    }
    funcPtr->functionType = PPL_FUNC_FFT;
    funcPtr->refCount = 1;
    funcPtr->minArgs = Ndims;
    funcPtr->maxArgs = Ndims;
    funcPtr->notNan = 1;
    funcPtr->numOnly = 1;
    funcPtr->realOnly = 0;
    funcPtr->dimlessOnly = 0;
    funcPtr->needSelfThis = 0;
    funcPtr->functionPtr = (void *) output;
    funcPtr->argList = NULL;
    funcPtr->min = funcPtr->max = NULL;
    funcPtr->minActive = funcPtr->maxActive = NULL;
    funcPtr->next = NULL;
    funcPtr->description = funcPtr->LaTeX = funcPtr->descriptionShort = NULL;

    // Supersede any previous function descriptor
    {
        pplObj val;
        pplObj *obj = NULL;

        // Look up the variable which is to hold the new function
        obj = ppl_lookupAssignable(c, pl, in, PARSE_ifft_varnames, PARSE_ifft_varname_varnames);
        if (obj == NULL) return;

        // Replace the variable's old value with the function, keeping its reference count and storage
        val.refCount = 1;
        pplObjFunc(&val, 0, 1, funcPtr);
        ppl_objReplace(obj, &val);
    }

    // Free copies of data vectors defined within our local context
    ppl_memAlloc_AscendOutOfContext(contextDataTab);
    return;
}

//! ppl_fft_evaluate - Evaluate a function created by the fft command, by looking up the value of the Fourier
//! transform at the requested point in its data grid
//! \param [in] c - The Pyxplot context
//! \param [in] FuncName - The name of the function, used in error messages
//! \param [in] desc - The descriptor of the Fourier transform
//! \param [in] in - Array of the arguments passed to the function
//! \param [out] out - The value of the Fourier transform
//! \param [out] status - Set to one if an error occurred
//! \param [out] errout - Character buffer into which to write an error message, if an error occurred

void ppl_fft_evaluate(ppl_context *c, char *FuncName, FFTDescriptor *desc, pplObj *in, pplObj *out, int *status,
                      char *errout) {

    *out = desc->outputUnit;

    // Issue user with a warning if complex arithmetic is not enabled
    if (c->set->term_current.ComplexNumbers != SW_ONOFF_ON)
        ppl_warning(
                &c->errcontext, ERR_NUMERICAL,
                "Attempt to evaluate a Fourier transform function whilst complex arithmetic is disabled. Fourier transforms are almost invariably complex and so this is unlikely to work.");

    // Check dimensions of input arguments and ensure that they are all real
    for (int i = 0; i < desc->Ndims; i++) {
        // Each argument must have the inverse units of the corresponding dimension of the grid
        if (!ppl_unitsDimEqual(in + i, &desc->invRange[i])) {
            // Report an error, or return NaN if errors are switched off
            if (c->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
                ppl_snprintfTrunc(
                        errout, LSTR_LENGTH,
                        "The %s(x) function expects argument %d to have dimensions of <%s>, but has instead received an argument with dimensions of <%s>.",
                        FuncName, i + 1, ppl_printUnit(c, &desc->invRange[i], NULL, NULL, 0, 1, 0),
                        ppl_printUnit(c, in + i, NULL, NULL, 1, 1, 0));
            } else {
                pplObjNum(out, 0, 0, 0);
                out->real = GSL_NAN;
                out->imag = 0;
            }
            *status = 1;
            return;
        }
        // Each argument must be real
        if ((in + i)->flagComplex) {
            // Report an error, or return NaN if errors are switched off
            if (c->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
                ppl_snprintfTrunc(
                        errout, LSTR_LENGTH,
                        "The %s(x) function expects argument %d to be real, but the supplied argument has an imaginary component.",
                        FuncName, i + 1);
            } else {
                pplObjNum(out, 0, 0, 0);
                out->real = GSL_NAN;
                out->imag = 0;
            }
            *status = 1;
            return;
        }
    }

    // Work out closest datapoint in FFT datagrid to the one we want
    int j = 0;
    // Find the grid cell for each argument; FFTW stores positive frequencies first, followed by negative ones
    for (int i = 0; i < desc->Ndims; i++) {
        double tempDbl = floor((in + i)->real * desc->range[i].real + 0.5);
        if ((tempDbl >= 0.0) && (tempDbl <= desc->XSize[i] / 2)) {
        } // Positive frequencies stored in lower half of array
        else if ((tempDbl < 0.0) && (tempDbl >= -desc->XSize[i] / 2)) {
            tempDbl += desc->XSize[i];
        } // Negative frequencies stored in upper half of array
        else {
            return;
        } // Query out of range; return zero with appropriate output unit
        j *= desc->XSize[i];
        j += (int) tempDbl;
    }

// Write output value to out
#ifdef HAVE_FFTW3
    out->real = desc->datagrid[j][0];
    // A value with no imaginary part is marked as real
    if (desc->datagrid[j][1] == 0.0) {
        out->flagComplex = 0;
        out->imag = 0.0;
    } else {
        out->flagComplex = 1;
        out->imag = desc->datagrid[j][1];
    }
#else
    out->real = desc->datagrid[j].re;
    // A value with no imaginary part is marked as real
    if (desc->datagrid[j].im == 0.0) {
        out->flagComplex = 0;
        out->imag = 0.0;
    } else {
        out->flagComplex = 1;
        out->imag = desc->datagrid[j].im;
    }
#endif
    return;
}
