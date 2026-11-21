// tabulate.c
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

// Implements the tabulate command, which evaluates functions or reads data files in the same way as the plot command,
// and writes the resulting columns of numbers to a text file. Each column is written in its own units, which are listed
// in a ColumnUnits header line, either in an automatically chosen format or in a format supplied by the user.

#define _TABULATE_C 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <glob.h>
#include <wordexp.h>

#include <gsl/gsl_math.h>

#include "commands/tabulate.h"
#include "coreUtils/errorReport.h"
#include "coreUtils/backup.h"
#include "coreUtils/getPasswd.h"
#include "coreUtils/memAlloc.h"
#include "expressions/traceback_fns.h"
#include "mathsTools/dcfmath.h"
#include "parser/cmdList.h"
#include "parser/parser.h"
#include "settings/settingTypes.h"
#include "stringTools/asciidouble.h"
#include "userspace/context.h"
#include "userspace/pplObj_fns.h"
#include "userspace/pplObjFunc.h"
#include "userspace/unitsArithmetic.h"
#include "userspace/unitsDisp.h"

#include "datafile.h"

//! ppl_tab_dataGridDisplay - Write a block of tabulated data to a file, applying any range restrictions and
//! formatting each column
//! \param [in] c - The Pyxplot context
//! \param [in] output - The file handle to write to
//! \param [in] data - The data to write
//! \param [in] minSet - Array of flags indicating whether a lower limit is set on each column
//! \param [in] min - Array of lower limits on each column
//! \param [in] maxSet - Array of flags indicating whether an upper limit is set on each column
//! \param [in] max - Array of upper limits on each column
//! \param [in] unit - Array of the physical units of each column
//! \param [in] format - A format string to apply to each row, or NULL to use the default format
//! \return Zero on success, or one if an error occurred

static int ppl_tab_dataGridDisplay(ppl_context *c, FILE *output, dataTable *data, const int *minSet, const double *min,
                                   const int *maxSet, const double *max, const pplObj *unit, char *format) {
    char msg[PPL_MSG_LENGTH];
    char tmpchr = '\0';
    double tmpdbl, multiplier[USING_ITEMS_MAX];
    int inRange, allInts[USING_ITEMS_MAX], allSmall[USING_ITEMS_MAX];
    long i, k;
    int l;
    double val;
    const int Ncolumns = data->Ncolumns_real;

    // Check that the firstEntries above have the same units as any supplied ranges
    for (int j = 0; j < Ncolumns; j++)
        // Only columns with a range need checking
        if (minSet[j] || maxSet[j]) {
            // The range must have the same units as the data
            if (!ppl_unitsDimEqual(&unit[j], data->firstEntries + j)) {
                ppl_snprintfTrunc(
                        msg, sizeof(msg),
                        "The minimum and maximum limits specified in range %d in the tabulate command have conflicting physical dimensions with the data returned from the data file. The limits have units of <%s>, whilst the data have units of <%s>.",
                        j + 1, ppl_printUnit(c, &unit[j], NULL, NULL, 0, 1, 0),
                        ppl_printUnit(c, data->firstEntries + j, NULL, NULL, 1, 1, 0));
                ppl_error(&c->errcontext, ERR_NUMERICAL, -1, -1, msg);
                return 1;
            }
        }

    // Output a column units line
    fprintf(output, "# ColumnUnits: ");
    // For each column, write its units and find the factor which converts its values into them
    for (int j = 0; j < Ncolumns; j++) {
        // Dimensionless columns need no conversion
        if (data->firstEntries[j].dimensionless) {
            fprintf(output, "1 "); // This column contains dimensionless data
            multiplier[j] = 1.0;
        } else {
            data->firstEntries[j].real = 1.0;
            data->firstEntries[j].imag = 0.0;
            data->firstEntries[j].flagComplex = 0;
            const char *cptr = ppl_printUnit(c, data->firstEntries + j, multiplier + j, &tmpdbl, 0, 1, SW_DISPLAY_T);
            for (i = 0; ((cptr[i] != '\0') && (cptr[i] != '(')); i++);
            i++; // Fastforward over opening bracket
            for (; ((cptr[i] != '\0') && (cptr[i] != ')')); i++) fprintf(output, "%c", cptr[i]);
            fprintf(output, " ");
        }
    }
    fprintf(output, "\n");

    // Iterate over columns of data in this data grid, working out which columns of data are all ints, and which are all %f-able data
    for (int j = 0; j < Ncolumns; j++) { allInts[j] = allSmall[j] = 1; }
    const dataBlock *blk = data->first;
    // Loop over the blocks of the data table
    while (blk != NULL) {
        // Loop over the rows in each block
        for (i = 0; i < blk->blockPosition; i++) {
            inRange = 1;
            // Skip rows with any value outside its column's range
            for (k = 0; k < Ncolumns; k++) {
                val = blk->data_real[k + Ncolumns * i];
                // This value is outside the range
                if ((minSet[k] && (val < min[k])) || (maxSet[k] && (val > max[k]))) {
                    inRange = 0;
                    break;
                } // Check that value is within range
            }
            if (inRange)
                // Note which columns contain values which are not all small integers, or not all of a size which can be
                // written with %f
                for (k = 0; k < Ncolumns; k++) {
                    val = blk->data_real[k + Ncolumns * i] * multiplier[k];
                    if (!gsl_finite(val)) allInts[k] = 0; // don't try to represent NANs as integers!
                    if ((fabs(val) > 1000) || (!ppl_dblEqual(val, floor(val + 0.5)))) allInts[k] = 0;
                    if ((fabs(val) > 1000) || (fabs(val) < 0.0099999999999))
                        allSmall[k] =
                                0; // Columns containing only numbers in this range are fprintfed using %f, rather than %e
                }
        }
        blk = blk->next;
    }

    // Write out each row of data which falls within the ranges, in the best format for each column or the user's format
    blk = data->first;
    int split = 0;
    // Loop over the blocks of the data table
    while (blk != NULL) {
        // Loop over the rows in each block; splits in the data are written as blank lines
        for (i = 0; i < blk->blockPosition; i++) {
            inRange = 1;
            if (blk->split[i]) split = 1;
            // Skip rows with any value outside its column's range
            for (k = 0; k < Ncolumns; k++) {
                val = blk->data_real[k + Ncolumns * i];
                // This value is outside the range
                if ((minSet[k] && (val < min[k])) || (maxSet[k] && (val > max[k]))) {
                    inRange = 0;
                    break;
                } // Check that value is within range
            }
            // Write this row
            if (inRange) {
                // A blank line marks a break in the data
                if (split) {
                    fprintf(output, "\n");
                    split = 0;
                }
                if (format ==
                    NULL) // User has not supplied a format string, and so we just list the contents of each column in turn using best-fit format style
                {
                    // Write each column as an integer, a fixed-point number or in exponential notation
                    for (k = 0; k < Ncolumns; k++) {
                        val = blk->data_real[k + Ncolumns * i] * multiplier[k];
                        if (allInts[k]) fprintf(output, "%10d ", (int) val);
                        else if (allSmall[k]) fprintf(output, "%17.10f ", val);
                        else fprintf(output, "%17.8e ", val);
                    }
                } else { // The user has supplied a format string, which we now substitute column values into
                    // Step through the format string; if it runs out before all the columns are used, it is repeated
                    for (int pos = l = 0;; pos++) {
                        // At the end of the format string, start again if some columns remain
                        if (format[pos] == '\0') {
                            if ((l > 0) && (l < Ncolumns)) pos = 0;
                            else break;
                        }
                        if (format[pos] != '%')
                            fprintf(output, "%c",
                                    format[pos]); // Just copy text of format string until we hit a % character
                        // Parse a % format token, and write the next column using it
                        else {
                            k = pos + 1; // k looks ahead to see experimentally if syntax is right
                            // %% writes a literal percent sign
                            if (format[k] == '%') {
                                fprintf(output, "%%");
                                pos++;
                                continue;
                            }
                            if ((format[k] == '+') || (format[k] == '-') || (format[k] == ' ') || (format[k] == '#'))
                                k++; // optional flag can be <+- #>
                            while ((format[k] >= '0') && (format[k] <= '9')) k++; // length can be some digits
                            if (format[k] == '.') // precision starts with a . and is followed by more digits
                            {
                                k++;
                                while ((format[k] >= '0') && (format[k] <= '9')) k++; // length can be some digits
                            }
                            // We do not allow user to specify optional length flag, which could potentially be <hlL>
                            if (l >= Ncolumns) val = GSL_NAN;
                            else
                                val = blk->data_real[l + Ncolumns * i] *
                                        multiplier
                                                [l]; // Set val to equal data from data table that we're about to print.
                            l++;
                            // Temporarily terminate the string after the token, so it can be passed to fprintf
                            if (format[k] != '\0') {
                                tmpchr = format[k + 1];
                                format[k + 1] = '\0';
                            } // NULL terminate format token before passing it to fprintf
                            if (format[k] ==
                                'd') // %d -- print quantity as an integer, but take care to avoid overflows
                            {
                                if ((!gsl_finite(val)) || (val > INT_MAX - 1) || (val < INT_MIN + 1))
                                    fprintf(output, "nan");
                                else fprintf(output, format + pos, (int) floor(val));
                                pos = k;
                            } else if ((format[k] == 'e') ||
                                       (format[k] == 'f')) // %f or %e -- print quantity as floating point number
                            {
                                fprintf(output, format + pos, val);
                                pos = k;
                            } else if (format[k] == 's') // %s -- print quantity in our best-fit format style
                            {
                                if (l >= Ncolumns) ppl_snprintfTrunc(msg, sizeof(msg), "nan");
                                else if (allInts[l - 1]) ppl_snprintfTrunc(msg, sizeof(msg), "%10d ", (int) val);
                                else if (allSmall[l - 1]) ppl_snprintfTrunc(msg, sizeof(msg), "%17.10f ", val);
                                else ppl_snprintfTrunc(msg, sizeof(msg), "%17.8e ", val);
                                fprintf(output, format + pos, msg);
                                pos = k;
                            } else {
                                fprintf(output, "%c", format[pos]);
                                l--;
                            } // l-- because this wasn't a valid format token, so we didn't print any data in the end
                            if (format[k] != '\0')
                                format[k + 1] =
                                        tmpchr; // Remove temporary NULL termination at the end of the format token
                        }
                    }
                }
                fprintf(output, "\n");
            } else split = 1;
        }
        blk = blk->next;
    }

    // Success
    return 0;
}


//! ppl_directive_tabulate - Implement the tabulate command, which writes data from files or functions to a text
//! file
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppl_directive_tabulate(ppl_context *c, parserLine *pl, parserOutput *in, const int interactive,
                            const int iterDepth) {
    char msg[PPL_MSG_LENGTH];
    wordexp_t wordExp;
    int pos;
    const pplObj *stk = in->stk;
    FILE *output = NULL;
    char filenameTemp[FNAME_LENGTH];
    parserLine *spool = NULL, **dataSpool = &spool;
    pplObj unit[USING_ITEMS_MAX];
    int minSet[USING_ITEMS_MAX], maxSet[USING_ITEMS_MAX];
    double min[USING_ITEMS_MAX], max[USING_ITEMS_MAX];

    // Read data range
    {
        int pos = PARSE_tabulate_0range_list, nr = 0;
        const int o1 = PARSE_tabulate_min_0range_list;
        const int o2 = PARSE_tabulate_max_0range_list;
        for (int i = 0; i < USING_ITEMS_MAX; i++) minSet[i] = maxSet[i] = 0;
        // Walk the linked list of ranges on the parser stack; each may have a minimum and a maximum
        while (stk[pos].objType == PPLOBJ_NUM) {
            pos = (int) round(stk[pos].real);
            if (pos <= 0) break;
            // There may be no more ranges than columns
            if (nr >= USING_ITEMS_MAX) {
                ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                                  "Too many ranges specified; a maximum of %d are allowed.", USING_ITEMS_MAX);
                ppl_tbAddLine(c, pl, ERR_SYNTAX, 0, "");
                return;
            }
            if (ppldata_readRange(c, pl, in, pos, o1, o2, nr, NULL, unit, min, minSet, max, maxSet)) return;
            nr++;
        }
    }

    // Work out filename for output file
    const char *filename = c->set->term_current.output;
    if ((filename == NULL) || (filename[0] == '\0')) filename = "pyxplot.txt";

    // Perform expansion of shell filename shortcuts such as ~
    if ((wordexp(filename, &wordExp, 0) != 0) || (wordExp.we_wordc <= 0)) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Could not find directory containing filename '%s'.", filename);
        ppl_error(&c->errcontext, ERR_FILE, -1, -1, msg);
        return;
    }
    // A wildcard may not match more than one file
    if (wordExp.we_wordc > 1) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Filename '%s' is ambiguous.", filename);
        ppl_error(&c->errcontext, ERR_FILE, -1, -1, msg);
        return;
    }
    strncpy(filenameTemp, wordExp.we_wordv[0], FNAME_LENGTH);
    filenameTemp[FNAME_LENGTH - 1] = '\0';
    wordfree(&wordExp);
    filename = filenameTemp;

    // Open output file and write header at the top of it
    ppl_createBackupIfRequired(c, filename);
    output = fopen(filename, "w");
    // Report a failure to open the file
    if (output == NULL) {
        ppl_snprintfTrunc(msg, sizeof(msg), "The tabulate command could not open output file '%s' for writing.",
                          filename);
        ppl_error(&c->errcontext, ERR_FILE, -1, -1, msg);
        return;
    }
    fprintf(output, "# Datafile generated by Pyxplot %s\n# Timestamp: %s\n", VERSION,
            ppl_strStrip(ppl_friendlyTimestring(), msg));
    fprintf(output, "# User: %s\n# Pyxplot command: %s\n\n", ppl_unixGetIRLName(&c->errcontext), pl->linetxt);

    // Loop over datasets being tabulated
    pos = PARSE_tabulate_0tabulate_list;
    long j = 0; // Index blocks are numbered from zero, as they are by the index modifier when the data are read back
    // Walk the linked list of the items to tabulate on the parser stack
    while (stk[pos].objType == PPLOBJ_NUM) {
        char *format, datafname[FNAME_LENGTH] = "";
        int errCount = 5;
        dataTable *data = NULL;
        pos = (int) round(stk[pos].real);
        if (pos <= 0) break;
        fprintf(output, "\n\n\n# Index %ld\n", j); // Put a heading at the top of the new data index

        // Read display format
        if (stk[pos + PARSE_tabulate_format_0tabulate_list].objType == PPLOBJ_STR)
            format = (char *) stk[pos + PARSE_tabulate_format_0tabulate_list].auxil;
        else format = NULL;

        // A filename may contain wildcards; tabulate each matching file in turn, until no more are found
        for (int w = 0;; w++) {
            // Descend into new memory context
            const int contextLocal = ppl_memAlloc_DescendIntoNewContext();
            int status = 0;

            // Fetch data
            ppldata_fromCmd(c, &data, pl, in, w, datafname, dataSpool, PARSE_TABLE_tabulate_, pos, -1, 0, min, minSet,
                            max, maxSet, unit, 0, &status, msg, &errCount, iterDepth);

            // Print data
            if ((status) || (data == NULL)) {
                if (w == 0) ppl_error(&c->errcontext, ERR_GENERIC, -1, -1, msg);
                ppl_memAlloc_AscendOutOfContext(contextLocal);
                break;
            } else {
                // For data from a file, write the filename, starting a new index block for every file after the first
                if (datafname[0] != '\0') {
                    // Each further file gets an index block of its own
                    if (w > 0) {
                        j++;
                        fprintf(output, "\n\n\n# Index %ld\n", j);
                    }
                    fprintf(output, "\n# %s\n\n", datafname);
                }
                ppl_tab_dataGridDisplay(c, output, data, minSet, min, maxSet, max, unit, format);
            }

            // Ascend out of memory content
            ppl_memAlloc_AscendOutOfContext(contextLocal);
        }
        j++;
    }

    // Close the output file
    fclose(output);
    return;
}
