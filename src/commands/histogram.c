// histogram.c
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

// Implements the histogram command, which reads one column of values from a data file, counts how many fall into each
// bin, and stores the result as a new function of x whose value is the number of values per unit x in the bin
// containing x. The bins may be listed explicitly, or have a regular width and origin, and are spaced logarithmically
// if the x1 axis is logarithmic. ppl_histogram_evaluate() is called by the expression evaluator when the function is
// used.

#define _HISTOGRAM_C 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <gsl/gsl_math.h>

#include "commands/core.h"
#include "commands/histogram.h"
#include "coreUtils/errorReport.h"
#include "stringTools/asciidouble.h"
#include "coreUtils/memAlloc.h"
#include "expressions/traceback_fns.h"
#include "parser/cmdList.h"
#include "parser/parser.h"
#include "settings/settings.h"
#include "settings/settingTypes.h"
#include "userspace/context.h"
#include "userspace/contextVarDef.h"
#include "userspace/garbageCollector.h"
#include "userspace/pplObj_fns.h"
#include "userspace/pplObjFunc.h"
#include "userspace/unitsArithmetic.h"
#include "userspace/unitsDisp.h"

#include "datafile.h"


//! hcompare - Comparison function for sorting arrays of doubles into ascending order with qsort()
//! \param [in] x - Pointer to the first double
//! \param [in] y - Pointer to the second double
//! \return -1, 0 or 1 if the first double is less than, equal to or greater than the second

static int hcompare(const void *x, const void *y) {
    if (*((const double *) x) > *((const double *) y)) return 1.0;
    else if (*((const double *) x) < *((const double *) y)) return -1.0;
    else return 0.0;
}

//! ppl_directive_histogram - Implement the histogram command, which bins data from a file into a histogram, and
//! stores it as a new function
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppl_directive_histogram(ppl_context *c, parserLine *pl, parserOutput *in, const int interactive,
                             const int iterDepth) {
    char msg[PPL_MSG_LENGTH];
    pplObj *stk = in->stk;
    dataTable *data;
    dataBlock *blk;
    long int i = 0, j, k;
    int contextLocalVec, contextDataTab, errCount = DATAFILE_NERRS;
    int gotBinList, logAxis;
    pplFunc *funcPtr;
    pplObj v, firstEntry;
    char filenameOut[FNAME_LENGTH] = "";
    pplObj unit[USING_ITEMS_MAX];
    int minSet[USING_ITEMS_MAX], maxSet[USING_ITEMS_MAX];
    double min[USING_ITEMS_MAX], max[USING_ITEMS_MAX];
    double *xdata;
    parserLine *spool = NULL, **dataSpool = &spool;
    histogramDescriptor *output;

    // Read ranges
    {
        int i, pos = PARSE_histogram_0range_list, nr = 0;
        const int o1 = PARSE_histogram_min_0range_list;
        const int o2 = PARSE_histogram_max_0range_list;
        for (i = 0; i < USING_ITEMS_MAX; i++) minSet[i] = maxSet[i] = 0;
        // Walk the linked list of ranges on the parser stack; only one range is allowed
        while (stk[pos].objType == PPLOBJ_NUM) {
            pos = (int) round(stk[pos].real);
            if (pos <= 0) break;
            // There is only one column of data, so only one range is allowed
            if (nr >= 1) {
                ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                                  "Too many ranges specified; only one is allowed.");
                ppl_tbAddLine(c, pl, ERR_SYNTAX, 0, "");
                return;
            }
            if (ppldata_readRange(c, pl, in, pos, o1, o2, nr, NULL, unit, min, minSet, max, maxSet)) return;
            nr++;
        }
    }

    // Allocate a new memory context for the data file we're about to read
    contextLocalVec = ppl_memAlloc_DescendIntoNewContext();
    contextDataTab = ppl_memAlloc_DescendIntoNewContext();

    // Read input data
    {
        const int NcolRequired = 1;
        int status = 0, j;
        ppldata_fromCmd(c, &data, pl, in, 0, filenameOut, dataSpool, PARSE_TABLE_histogram_, 0, NcolRequired, 0, min,
                        minSet, max, maxSet, unit, 0, &status, c->errStat.errBuff, &errCount, iterDepth);

        // Exit on error
        if ((status) || (data == NULL)) {
            ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "");
            ppl_memAlloc_AscendOutOfContext(contextLocalVec);
            return;
        }

        // Check that the firstEntries above have the same units as any supplied ranges
        for (j = 0; j < NcolRequired; j++)
            // Only columns with a range need checking
            if (minSet[j] || maxSet[j]) {
                // The range must have the same units as the data
                if (!ppl_unitsDimEqual(&unit[j], data->firstEntries + j)) {
                    ppl_snprintfTrunc(
                            c->errStat.errBuff, sizeof(c->errStat.errBuff),
                            "The minimum and maximum limits specified in range %d in the histogram command have conflicting physical dimensions with the data returned from the data file. The limits have units of <%s>, whilst the data have units of <%s>.",
                            j + 1, ppl_printUnit(c, unit + j, NULL, NULL, 0, 1, 0),
                            ppl_printUnit(c, data->firstEntries + j, NULL, NULL, 1, 1, 0));
                    ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
                    return;
                }
            }

        // Check that we have at least three datapoints
        if (data->Nrows < 3) {
            ppl_snprintfTrunc(
                    c->errStat.errBuff, sizeof(c->errStat.errBuff),
                    "Histogram construction is only possible on data sets with members at at least three values of x.");
            ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
            return;
        }

        // Remember the units of the data
        firstEntry = data->firstEntries[0];
    }

    // Transfer data from multiple data tables into a single vector
    xdata = (double *) ppl_memAlloc_incontext(data->Nrows * sizeof(double), contextLocalVec);

    // Failure to allocate memory is reported as an error
    if (xdata == NULL) {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "Out of memory.");
        ppl_tbAddLine(c, pl, ERR_MEMORY, 0, "");
        return;
    }

    // Copy data table into an array of x values
    blk = data->first;
    i = 0;
    // Loop over the blocks of the data table
    while (blk != NULL) {
        for (j = 0; j < blk->blockPosition; j++) xdata[i++] = blk->data_real[j];
        blk = blk->next;
    }

    // Free original data table which is no longer needed
    ppl_memAlloc_AscendOutOfContext(contextDataTab);

    // Sort data vector
    qsort((void *) xdata, i, sizeof(double), hcompare);

    // Work out whether we are constructing histogram in linear space or log space
    gotBinList = (stk[PARSE_histogram_bin_list].objType == PPLOBJ_NUM);
    logAxis = ((!gotBinList) && (c->set->XAxes[1].log == SW_BOOL_TRUE)); // Read from axis x1

    // Filter out any values of x which are out-of-range
    for (j = k = 0; j < i; j++) {
        if (!gsl_finite(xdata[j])) continue; // Ignore non-finite datapoints
        if ((minSet[0]) && (xdata[j] < min[0])) continue; // Ignore out-of-range datapoints
        if ((maxSet[0]) && (xdata[j] > max[0])) continue; // Ignore out-of-range datapoints

        // With logarithmic bins, work with the logarithms of the values, and reject values which are not positive
        if (logAxis) {
            // Warn about values which have no logarithm, and skip them
            if (xdata[j] <= 0.0) {
                if (ppl_errorCountTake(&errCount)) { // Only report a limited number of errors
                    v = firstEntry;
                    v.real = xdata[j];
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Negative or zero values are not allowed in the construction of histograms in log space; value of x=%s will be ignored.",
                            ppl_unitsNumericDisplay(c, &v, 0, 0, 0));
                    ppl_warning(&c->errcontext, ERR_NUMERICAL, msg);
                    ppl_errorCountCheck(&c->errcontext, errCount, filenameOut, -1);
                }
                continue;
            }
            xdata[k] = log(xdata[j]); // If we're constructing histogram in log space, log data now
            if (!gsl_finite(xdata[k])) continue;
        } else {
            xdata[k] = xdata[j];
        }
        k++;
    }

    // Check that we have at least three points to interpolate
    if (k < 3) {
        ppl_snprintfTrunc(
                c->errStat.errBuff, sizeof(c->errStat.errBuff),
                "Histogram construction is only possible on data sets with members at at least three values of x.");
        ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
        return;
    }

    // Make histogramDescriptor data structure
    output = (histogramDescriptor *) malloc(sizeof(histogramDescriptor));
    // Failure to allocate memory is reported as an error
    if (output == NULL) {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "Out of memory.");
        ppl_tbAddLine(c, pl, ERR_MEMORY, 0, "");
        return;
    }
    output->unit = firstEntry;
    output->log = logAxis;
    output->filename = (char *) malloc(strlen(filenameOut) + 1);
    // Failure to allocate memory is reported as an error
    if (output->filename == NULL) {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "Out of memory.");
        ppl_tbAddLine(c, pl, ERR_MEMORY, 0, "");
        free(output);
        output = NULL;
        return;
    }
    strcpy(output->filename, filenameOut);

    // Now need to work out what bins we're going to use
    if (gotBinList) {
        int binListLen = 0, j;
        int pos = PARSE_histogram_bin_list;
        // Count the bins in the list supplied
        while (stk[pos].objType == PPLOBJ_NUM) {
            pos = (int) round(stk[pos].real);
            if (pos <= 0) break;
            binListLen++;
        }
        output->bins = (double *) malloc(binListLen * sizeof(double));
        // Failure to allocate memory is reported as an error
        if (output->bins == NULL) {
            ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "Out of memory.");
            ppl_tbAddLine(c, pl, ERR_MEMORY, 0, "");
            free(output->filename);
            output->filename = NULL;
            free(output);
            output = NULL;
            return;
        }
        j = 0;
        pos = PARSE_histogram_bin_list;
        // Copy the bin boundaries supplied, rejecting any with the wrong units
        while (stk[pos].objType == PPLOBJ_NUM) {
            pplObj *val;
            pos = (int) round(stk[pos].real);
            if (pos <= 0) break;
            val = &stk[pos + PARSE_histogram_x_bin_list];
            // Warn about bin boundaries with the wrong units, and skip them
            if (!ppl_unitsDimEqual(&firstEntry, val)) {
                ppl_snprintfTrunc(
                        msg, sizeof(msg),
                        "The supplied bin boundary at x=%s has conflicting physical dimensions with the data supplied, which has units of <%s>. Ignoring this bin boundary.",
                        ppl_printUnit(c, val, NULL, NULL, 0, 1, 0), ppl_printUnit(c, &firstEntry, NULL, NULL, 1, 1, 0));
                ppl_warning(&c->errcontext, ERR_NUMERICAL, msg);
            } else {
                output->bins[j++] = val->real;
            }
        }
        binListLen = j; // some of the bins may have been rejected
        output->Nbins = binListLen;
        qsort((void *) output->bins, binListLen, sizeof(double),
              hcompare); // Make sure that bins are in ascending order
    } else {
        pplObj *BinWidth, *BinOrigin, tempValObj;
        int BinOriginSet;
        double BinOriginDbl = 0, BinWidthDbl, xbinmin, xbinmax;
        pplObj *inbinWidth = &stk[PARSE_histogram_binwidth];
        pplObj *inbinOrigin = &stk[PARSE_histogram_binorigin];
        xbinmin = xdata[0];
        xbinmax = xdata[k - 1];
        if (minSet[0]) xbinmin = logAxis ? log(min[0]) : min[0];
        if (maxSet[0]) xbinmax = logAxis ? log(max[0]) : max[0];
        // Make sure the range is the right way around
        if (xbinmax < xbinmin) {
            const double temp = xbinmax;
            xbinmax = xbinmin;
            xbinmin = temp;
        }
        // Log the range in debugging mode
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg),
                              "Prior to application of BinOrigin, histogram command using range [%e:%e]", xbinmin,
                              xbinmax);
            ppl_log(&c->errcontext, msg);
        }

        // The bin width is that given in the command, or a hundredth of the range of the data if set to automatic, or
        // else that set with "set binwidth"
        if (inbinWidth->objType == PPLOBJ_NUM) {
            BinWidth = inbinWidth;
        } else if (c->set->term_current.BinWidthAuto) {
            BinWidth = &tempValObj;
            tempValObj = firstEntry;
            tempValObj.flagComplex = 0;
            tempValObj.imag = 0.0;
            tempValObj.real = (xbinmax - xbinmin) / 100;
            if (logAxis) tempValObj.real = exp(tempValObj.real);
        } else {
            BinWidth = &(c->set->term_current.BinWidth);
        }

        // The bin origin is that given in the command, or else that set with "set binorigin", unless that is automatic
        if (inbinOrigin->objType == PPLOBJ_NUM) {
            BinOrigin = inbinOrigin;
            BinOriginSet = 1;
        } else {
            BinOrigin = &(c->set->term_current.BinOrigin);
            BinOriginSet = !c->set->term_current.BinOriginAuto;
        }

        // The bin width must have the same units as the data
        if ((!logAxis) && (!ppl_unitsDimEqual(&firstEntry, BinWidth))) {
            ppl_snprintfTrunc(
                    c->errStat.errBuff, sizeof(c->errStat.errBuff),
                    "The bin width supplied to the histogram command has conflicting physical dimensions with the data supplied. The former has units of <%s>, whilst the latter has units of <%s>.",
                    ppl_printUnit(c, BinWidth, NULL, NULL, 0, 1, 0),
                    ppl_printUnit(c, &firstEntry, NULL, NULL, 1, 1, 0));
            ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
            free(output->filename);
            output->filename = NULL;
            free(output);
            output = NULL;
            return;
        }
        // With logarithmic bins, the bin width is a dimensionless factor
        if ((logAxis) && (BinWidth->dimensionless == 0)) {
            ppl_snprintfTrunc(
                    c->errStat.errBuff, sizeof(c->errStat.errBuff),
                    "For logarithmically spaced bins, the multiplicative spacing between bins must be dimensionless. The supplied spacing has units of <%s>.",
                    ppl_printUnit(c, BinWidth, NULL, NULL, 0, 1, 0));
            ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
            free(output->filename);
            output->filename = NULL;
            free(output);
            output = NULL;
            return;
        }
        // The bin origin must have the same units as the data
        if ((BinOriginSet) && (!ppl_unitsDimEqual(&firstEntry, BinOrigin))) {
            ppl_snprintfTrunc(
                    c->errStat.errBuff, sizeof(c->errStat.errBuff),
                    "The bin origin supplied to the histogram command has conflicting physical dimensions with the data supplied. The former has units of <%s>, whilst the latter has units of <%s>.",
                    ppl_printUnit(c, BinOrigin, NULL, NULL, 0, 1, 0),
                    ppl_printUnit(c, &firstEntry, NULL, NULL, 1, 1, 0));
            ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
            free(output->filename);
            output->filename = NULL;
            free(output);
            output = NULL;
            return;
        }
        // With logarithmic bins, the factor between bins must be greater than one
        if ((logAxis) && (BinWidth->real <= 1.0)) {
            ppl_snprintfTrunc(
                    c->errStat.errBuff, sizeof(c->errStat.errBuff),
                    "For logarithmically spaced bins, the multiplicative spacing between bins must be greater than 1.0. Value supplied was %s.",
                    ppl_unitsNumericDisplay(c, BinWidth, 0, 0, 0));
            ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
            free(output->filename);
            output->filename = NULL;
            free(output);
            output = NULL;
            return;
        }
        // The bin width must be positive
        if (BinWidth->real <= 0.0) {
            ppl_snprintfTrunc(
                    c->errStat.errBuff, sizeof(c->errStat.errBuff),
                    "The bin width supplied to the histogram command must be greater than zero. Value supplied was %s.",
                    ppl_unitsNumericDisplay(c, BinWidth, 0, 0, 0));
            ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
            free(output->filename);
            output->filename = NULL;
            free(output);
            output = NULL;
            return;
        }
        // With logarithmic bins, the bin origin must be positive
        if ((logAxis) && (BinOriginSet) && (BinOrigin->real <= 0.0)) {
            ppl_snprintfTrunc(
                    c->errStat.errBuff, sizeof(c->errStat.errBuff),
                    "For logarithmically spaced bins, the specified bin origin must be greater than zero. Value supplied was %s.",
                    ppl_unitsNumericDisplay(c, BinOrigin, 0, 0, 0));
            ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
            free(output->filename);
            output->filename = NULL;
            free(output);
            output = NULL;
            return;
        }
        // With logarithmic bins, the minimum must be positive
        if ((logAxis) && (minSet[0]) && (min[0] <= 0.0)) {
            unit[0].real = min[0];
            ppl_snprintfTrunc(
                    c->errStat.errBuff, sizeof(c->errStat.errBuff),
                    "For logarithmically spaced bins, the specified minimum must be greater than zero. Value supplied was %s.",
                    ppl_unitsNumericDisplay(c, &unit[0], 0, 0, 0));
            ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
            free(output->filename);
            output->filename = NULL;
            free(output);
            output = NULL;
            return;
        }
        // With logarithmic bins, the maximum must be positive
        if ((logAxis) && (maxSet[0]) && (max[0] <= 0.0)) {
            unit[0].real = max[0];
            ppl_snprintfTrunc(
                    c->errStat.errBuff, sizeof(c->errStat.errBuff),
                    "For logarithmically spaced bins, the specified maximum must be greater than zero. Value supplied was %s.",
                    ppl_unitsNumericDisplay(c, &unit[0], 0, 0, 0));
            ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
            free(output->filename);
            output->filename = NULL;
            free(output);
            output = NULL;
            return;
        }
        // The range must have the same units as the data
        if ((minSet[0] || maxSet[0]) && (!ppl_unitsDimEqual(&firstEntry, &unit[0]))) {
            ppl_snprintfTrunc(
                    c->errStat.errBuff, sizeof(c->errStat.errBuff),
                    "The range supplied to the histogram command has conflicting physical dimensions with the data supplied. The former has units of <%s>, whilst the latter has units of <%s>.",
                    ppl_printUnit(c, &unit[0], NULL, NULL, 0, 1, 0),
                    ppl_printUnit(c, &firstEntry, NULL, NULL, 1, 1, 0));
            ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
            free(output->filename);
            output->filename = NULL;
            free(output);
            output = NULL;
            return;
        }
        // The bin width must be finite
        if (!gsl_finite(BinWidth->real)) {
            ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                              "The bin width specified to the histogram command is not a finite number.");
            ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
            free(output->filename);
            output->filename = NULL;
            free(output);
            output = NULL;
            return;
        }
        // The bin origin must be finite
        if (BinOriginSet && !gsl_finite(BinOrigin->real)) {
            ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                              "The bin origin specified to the histogram command is not a finite number.");
            ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
            free(output->filename);
            output->filename = NULL;
            free(output);
            output = NULL;
            return;
        }

        // Convert the bin width and origin into the (possibly logarithmic) space in which the bins are evenly spaced
        if (logAxis) {
            if (BinOriginSet) BinOriginDbl = log(BinOrigin->real);
            BinWidthDbl = log(BinWidth->real);
        } else {
            if (BinOriginSet) BinOriginDbl = BinOrigin->real;
            BinWidthDbl = BinWidth->real;
        }

        // Log the bin width and origin in debugging mode
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Histogram command using a bin width of %e and a bin origin of %e.",
                              BinWidthDbl, BinOriginDbl);
            ppl_log(&c->errcontext, msg);
        }

        // Generate a series of bins to use based on supplied BinWidth and BinOrigin
        if (BinOriginSet) BinOriginDbl = BinOriginDbl - BinWidthDbl * floor(BinOriginDbl / BinWidthDbl);
        else BinOriginDbl = 0.0;
        xbinmin = floor((xbinmin - BinOriginDbl) / BinWidthDbl) * BinWidthDbl + BinOriginDbl;
        xbinmax = ceil((xbinmax - BinOriginDbl) / BinWidthDbl) * BinWidthDbl + BinOriginDbl;
        // Log the range in debugging mode
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "After application of BinOrigin, histogram command using range [%e:%e]",
                              xbinmin, xbinmax);
            ppl_log(&c->errcontext, msg);
        }

        // Refuse to create an unreasonable number of bins
        if (((xbinmax - xbinmin) / BinWidthDbl + 1.0001) > 1e7) {
            ppl_snprintfTrunc(
                    c->errStat.errBuff, sizeof(c->errStat.errBuff),
                    "The supplied value of BinWidth produces a binning scheme with more than 1e7 bins. This is probably not sensible.");
            ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "");
            return;
        }
        output->Nbins = (long) ((xbinmax - xbinmin) / BinWidthDbl + 1.0001);
        // Log the number of bins in debugging mode
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Histogram command using %ld bins", output->Nbins);
            ppl_log(&c->errcontext, msg);
        }
        output->bins = (double *) malloc(output->Nbins * sizeof(double));
        // Failure to allocate memory is reported as an error
        if (output->bins == NULL) {
            ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "Out of memory.");
            ppl_tbAddLine(c, pl, ERR_MEMORY, 0, "");
            return;
        }

        // Compute the bin boundaries
        for (j = 0; j < output->Nbins; j++) output->bins[j] = xbinmin + j * BinWidthDbl;
    }

    // Allocate vector for storing histogram values
    output->binvals = (double *) malloc(output->Nbins * sizeof(double));
    // Failure to allocate memory is reported as an error
    if (output->binvals == NULL) {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "Out of memory.");
        ppl_tbAddLine(c, pl, ERR_MEMORY, 0, "");
        free(output->bins);
        output->bins = NULL;
        return;
    }
    for (i = 0; i < output->Nbins; i++) output->binvals[i] = 0.0;

    // Count number of data values in each histogram bin
    i = 0;
    for (j = 0; j < k; j++) // Loop over all of the data points we have read in
    {
        while ((i < output->Nbins) && (xdata[j] > output->bins[i])) i++;
        if (i >= output->Nbins) break; // We gone off the top of the last bin
        if (i == 0) continue; // We've not yet arrived in the bottom bin
        output->binvals[i - 1] += 1.0;
    }

    // If this is a logarithmic set of bins, unlog bin boundaries now
    if (logAxis)
        for (i = 0; i < output->Nbins; i++) output->bins[i] = exp(output->bins[i]);

    // Divide each bin's number of counts by its width
    for (i = 0; i < output->Nbins - 1; i++) output->binvals[i] /= output->bins[i + 1] - output->bins[i] + 1e-200;

    // Debugging lines
    if (DEBUG)
        // Log the contents of each bin
        for (i = 0; i < output->Nbins - 1; i++) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Bin %ld [%e:%e] --> %e", i + 1, output->bins[i], output->bins[i + 1],
                              output->binvals[i]);
            ppl_log(&c->errcontext, msg);
        }

    // Make a new function descriptor
    funcPtr = (pplFunc *) malloc(sizeof(pplFunc));
    // Failure to allocate memory is reported as an error
    if (funcPtr == NULL) {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                          "Out of memory whilst adding histogram object to function dictionary.");
        ppl_tbAddLine(c, pl, ERR_MEMORY, 0, "");
        return;
    }
    funcPtr->functionType = PPL_FUNC_HISTOGRAM;
    funcPtr->refCount = 1;
    funcPtr->minArgs = 1;
    funcPtr->maxArgs = 1;
    funcPtr->notNan = 1;
    funcPtr->realOnly = 1;
    funcPtr->numOnly = 1;
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
        obj = ppl_lookupAssignable(c, pl, in, PARSE_histogram_varnames, PARSE_histogram_varname_varnames);
        if (obj == NULL) return;

        // Replace the variable's old value with the function, keeping its reference count and storage
        val.refCount = 1;
        pplObjFunc(&val, 0, 1, funcPtr);
        ppl_objReplace(obj, &val);
    }

    // Free copies of data vectors defined within our local context
    ppl_memAlloc_AscendOutOfContext(contextLocalVec);
    return;
}

//! ppl_histogram_evaluate - Evaluate a function created by the histogram command, by looking up the bin which
//! contains the requested value
//! \param [in] c - The Pyxplot context
//! \param [in] FuncName - The name of the function, used in error messages
//! \param [in] desc - The descriptor of the histogram
//! \param [in] in - The argument passed to the function
//! \param [out] out - The value of the histogram
//! \param [out] status - Set to one if an error occurred
//! \param [out] errout - Character buffer into which to write an error message, if an error occurred

void ppl_histogram_evaluate(ppl_context *c, char *FuncName, histogramDescriptor *desc, pplObj *in, pplObj *out,
                            int *status, char *errout) {
    long i, Nsteps, len, ss, pos;
    double dblin;

    // The argument must have the same units as the data
    if (!ppl_unitsDimEqual(in, &desc->unit)) {
        // Report an error, or return NaN if errors are switched off
        if (c->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
            ppl_snprintfTrunc(
                    errout, LSTR_LENGTH,
                    "The %s(x) function expects an argument with dimensions of <%s>, but has instead received an argument with dimensions of <%s>.",
                    FuncName, ppl_printUnit(c, &desc->unit, NULL, NULL, 0, 1, 0),
                    ppl_printUnit(c, in, NULL, NULL, 1, 1, 0));
        } else {
            pplObjNum(out, 0, 0, 0);
            out->real = GSL_NAN;
            out->imag = 0;
        }
        *status = 1;
        return;
    }
    // The argument must be real
    if (in->flagComplex) {
        // Report an error, or return NaN if errors are switched off
        if (c->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
            ppl_snprintfTrunc(
                    errout, LSTR_LENGTH,
                    "The %s(x) function expects a real argument, but the supplied argument has an imaginary component.",
                    FuncName);
        } else {
            pplObjNum(out, 0, 0, 0);
            out->real = GSL_NAN;
            out->imag = 0;
        }
        *status = 1;
        return;
    }

    // The result has the units of one over those of the data, and is zero outside the histogram
    dblin = in->real;

    *out = desc->unit;
    out->imag = 0.0;
    out->real = 0.0;
    out->flagComplex = 0;
    for (i = 0; i < UNITS_MAX_BASEUNITS; i++)
        if (out->exponent[i] != 0.0) out->exponent[i] *= -1; // Output has units of 'per x'
    if ((desc->Nbins < 1) || (dblin < desc->bins[0]) || (dblin > desc->bins[desc->Nbins - 1]))
        return; // Query is outside range of histogram

    // Find the bin containing the argument by a binary search of the bin boundaries
    len = desc->Nbins;
    Nsteps = (long) ceil(log(len) / log(2));
    // Take steps of halving size, so long as they do not pass the argument
    for (pos = i = 0; i < Nsteps; i++) {
        ss = 1 << (Nsteps - 1 - i);
        if (pos + ss >= len) continue;
        if (desc->bins[pos + ss] <= dblin) pos += ss;
    }
    if (desc->bins[pos] > dblin) return; // Off left end
    else if (pos == len - 1) return; // Off right end

    // Return the contents of the bin
    out->real = desc->binvals[pos];
    return;
}
