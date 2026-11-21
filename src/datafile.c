// datafile.c
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

// Reads the data plotted, fitted or tabulated by commands such as plot, fit, histogram and tabulate, from data files
// (optionally through input filters), from functions sampled on a raster, or from vectors, into data tables. Applies
// the modifiers which select and transform the data: using, every, index, select, label and sort. Data files may
// contain blank lines, which separate blocks and indices, and comment lines giving the headings and units of columns or
// rows. Large numbers of function samples are evaluated in parallel with OpenMP when the functions allow it.

#define _DATAFILE_C 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <signal.h>
#include <glob.h>
#include <wordexp.h>

#include <gsl/gsl_math.h>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "coreUtils/errorReport.h"
#include "userspace/stack.h"
#include "coreUtils/memAlloc.h"
#include "expressions/dollarOp.h"
#include "expressions/expCompile_fns.h"
#include "expressions/expEval.h"
#include "expressions/expThreadSafe.h"
#include "expressions/traceback_fns.h"
#include "parser/cmdList.h"
#include "parser/parser.h"
#include "mathsTools/dcfmath.h"
#include "settings/settings.h"
#include "settings/settingTypes.h"
#include "stringTools/asciidouble.h"
#include "stringTools/strConstants.h"
#include "userspace/context.h"
#include "userspace/contextVarDef.h"
#include "userspace/garbageCollector.h"
#include "userspace/pplObj_fns.h"
#include "userspace/pplObjCmp.h"
#include "userspace/unitsDisp.h"
#include "userspace/unitsArithmetic.h"

#include "children.h"
#include "datafile.h"
#include "datafile_rasters.h"
#include "pplConstants.h"

//! ppldata_NewDataBlock - Allocate a new block of storage for rows of data, to be linked into a data table
//! \param [in] Ncolumns_real - The number of columns of numerical data
//! \param [in] Ncolumns_obj - The number of columns of objects other than numbers
//! \param [in] memContext - The ppl_memAlloc context in which to allocate the storage
//! \param [in] length - The number of rows of storage to allocate, or -1 to use the default size
//! \return The new data block, or NULL if memory allocation failed

dataBlock *ppldata_NewDataBlock(const int Ncolumns_real, const int Ncolumns_obj, const int memContext,
                                const int length) {
    dataBlock *output;
    int blockLength = 1 +
            DATAFILE_DATABLOCK_BYTES /
                    ((sizeof(double) + sizeof(long int)) * Ncolumns_real +
                     (sizeof(pplObj) + sizeof(long int)) * Ncolumns_obj); // automatic length

    if (length > 0) blockLength = length; // overriden when manually specified length > 0

    // Allocate the block, and its arrays of real values, objects, line numbers, split flags and text labels
    output = (dataBlock *) ppl_memAlloc_incontext(sizeof(dataBlock), memContext);
    if (output == NULL) return NULL;
    output->data_real = (double *) ppl_memAlloc_incontext(blockLength * Ncolumns_real * sizeof(double), memContext);
    output->data_obj = (pplObj *) ppl_memAlloc_incontext(blockLength * Ncolumns_obj * sizeof(pplObj), memContext);
    output->text = (char **) ppl_memAlloc_incontext(blockLength * sizeof(char *), memContext);
    output->fileLine_real =
            (long int *) ppl_memAlloc_incontext(blockLength * Ncolumns_real * sizeof(long int), memContext);
    output->fileLine_obj =
            (long int *) ppl_memAlloc_incontext(blockLength * Ncolumns_obj * sizeof(long int), memContext);
    output->split = (unsigned char *) ppl_memAlloc_incontext(blockLength * sizeof(unsigned char), memContext);
    output->blockLength = blockLength;
    output->blockPosition = 0;
    output->next = NULL;
    output->prev = NULL;
    if ((output->data_real == NULL) || (output->text == NULL) || (output->fileLine_real == NULL) ||
        (output->fileLine_obj == NULL) || (output->split == NULL))
        return NULL;
    return output;
}

//! ppldata_NewDataTable - Create a new, empty data table, with an initial block of storage for rows of data
//! \param [in] Ncolumns_real - The number of columns of numerical data
//! \param [in] Ncolumns_obj - The number of columns of objects other than numbers
//! \param [in] memContext - The ppl_memAlloc context in which to allocate the storage
//! \param [in] length - The number of rows of storage to allocate, or -1 to use the default size
//! \return The new data table, or NULL if memory allocation failed

dataTable *ppldata_NewDataTable(const int Ncolumns_real, const int Ncolumns_obj, const int memContext,
                                const int length) {
    dataTable *output;
    int i;

    // Allocate the table, with its first block
    output = (dataTable *) ppl_memAlloc_incontext(sizeof(dataTable), memContext);
    if (output == NULL) return NULL;
    output->Ncolumns_real = Ncolumns_real;
    output->Ncolumns_obj = Ncolumns_obj;
    output->Nrows = 0;
    output->memContext = memContext;
    output->firstEntries = (pplObj *) ppl_memAlloc_incontext(Ncolumns_real * sizeof(pplObj), memContext);
    if (output->firstEntries == NULL) return NULL;
    for (i = 0; i < Ncolumns_real; i++) pplObjNum(output->firstEntries + i, 0, 0, 0);
    output->first = ppldata_NewDataBlock(Ncolumns_real, Ncolumns_obj, memContext, length);
    output->current = output->first;
    if (output->first == NULL) return NULL;
    return output;
}

//! ppldata_NewRawDataBlock - Allocate a new block of storage for lines of raw text from a data file, to be linked into
//! a raw data table. Raw data tables are used when reading data in rows, which must be rotated before the "using"
//! expressions can be applied.
//! \param [in] memContext - The ppl_memAlloc context in which to allocate the storage
//! \return The new raw data block, or NULL if memory allocation failed

rawDataBlock *ppldata_NewRawDataBlock(const int memContext) {
    rawDataBlock *output;
    int blockLength = 1 + DATAFILE_DATABLOCK_BYTES / (sizeof(char *) + sizeof(long int));

    // Allocate the block, and its arrays of lines and line numbers
    output = (rawDataBlock *) ppl_memAlloc_incontext(sizeof(rawDataBlock), memContext);
    if (output == NULL) return NULL;
    output->text = (char **) ppl_memAlloc_incontext(blockLength * sizeof(char *), memContext);
    output->fileLine = (long int *) ppl_memAlloc_incontext(blockLength * sizeof(long int), memContext);
    output->blockLength = blockLength;
    output->blockPosition = 0;
    output->next = NULL;
    output->prev = NULL;
    if ((output->text == NULL) || (output->fileLine == NULL)) return NULL;
    return output;
}

//! ppldata_NewRawDataTable - Create a new, empty raw data table, with an initial block of storage for lines of raw text
//! \param [in] memContext - The ppl_memAlloc context in which to allocate the storage
//! \return The new raw data table, or NULL if memory allocation failed

rawDataTable *ppldata_NewRawDataTable(const int memContext) {
    rawDataTable *output;

    // Allocate the table, with its first block
    output = (rawDataTable *) ppl_memAlloc_incontext(sizeof(rawDataTable), memContext);
    if (output == NULL) return NULL;
    output->Nrows = 0;
    output->memContext = memContext;
    output->first = ppldata_NewRawDataBlock(memContext);
    output->current = output->first;
    if (output->first == NULL) return NULL;
    return output;
}

//! ppldata_DataTable_AddRow - Advance a data table to its next row, allocating a new block of storage if the current
//! one is full
//! \param [in] i - The data table
//! \return Zero on success, or one if memory allocation failed

int ppldata_DataTable_AddRow(dataTable *i) {
    if (i == NULL) return 1;
    if (i->current == NULL) return 1;
    i->Nrows++;
    // Move to the next row of the current block, or start a new block if it is full
    if (i->current->blockPosition < (i->current->blockLength - 1)) {
        i->current->blockPosition++;
        return 0;
    }
    i->current->next = ppldata_NewDataBlock(i->Ncolumns_real, i->Ncolumns_obj, i->memContext, -1);
    if (i->current == NULL) return 1;
    i->current->blockPosition = i->current->blockLength;
    i->current->next->prev = i->current;
    i->current = i->current->next;
    return 0;
}

//! ppldata_RawDataTable_AddRow - Advance a raw data table to its next line, allocating a new block of storage if the
//! current one is full
//! \param [in] i - The raw data table
//! \return Zero on success, or one if memory allocation failed

int ppldata_RawDataTable_AddRow(rawDataTable *i) {
    if (i == NULL) return 1;
    if (i->current == NULL) return 1;
    i->Nrows++;
    // Move to the next line of the current block, or start a new block if it is full
    if (i->current->blockPosition < (i->current->blockLength - 1)) {
        i->current->blockPosition++;
        return 0;
    }
    i->current->next = ppldata_NewRawDataBlock(i->memContext);
    if (i->current == NULL) return 1;
    i->current->blockPosition = i->current->blockLength;
    i->current->next->prev = i->current;
    i->current = i->current->next;
    if (i->current == NULL) return 1;
    return 0;
}

//! ppldata_DataTable_List - Print the contents of a data table, for debugging
//! \param [in] c - The Pyxplot context
//! \param [in] i - The data table to print

void ppldata_DataTable_List(ppl_context *c, dataTable *i) {
    // Variables for printing the table
    dataBlock *blk;
    pplObj v;
    int j, k, Ncolumns;

    // Report an empty table
    if (i == NULL) {
        printf("<NULL data table>\n");
        return;
    }
    printf("Table size: %d x %ld\n", i->Ncolumns_real, i->Nrows);
    printf("Memory context: %d\n", i->memContext);
    blk = i->first;
    Ncolumns = i->Ncolumns_real;
    // Print each block of the table
    while (blk != NULL) {
        // Print each row, with blank lines where the data are split
        for (j = 0; j < blk->blockPosition; j++) {
            if (blk->split[j]) printf("\n\n");
            // Print each value in the row, with its units
            for (k = 0; k < Ncolumns; k++) {
                v = i->firstEntries[k];
                v.real = blk->data_real[j * Ncolumns + k];
                v.imag = 0.0;
                v.flagComplex = 0;
                printf("%15s [line %6ld]  ", ppl_unitsNumericDisplay(c, &v, 0, 0, 0),
                       blk->fileLine_real[j * Ncolumns + k]);
            }
            if (blk->text[j] != NULL) printf("Label: <%s>", blk->text[j]);
            printf("\n");
        }
        blk = blk->next;
    }
}

//! ppldata_LaunchCoProcess - Open a data file for reading. If its filename matches one of the input filters set with
//! "set filter", the filter is launched as a helper process and its output is read instead. (The default filters
//! decompress .gz files and fetch URLs.)
//! \param [in] c - The Pyxplot context
//! \param [in] filename - The filename of the data file, which may contain wildcards
//! \param [in] wildcardMatchNumber - For filenames containing wildcards, the number of the matching file to read,
//! counting from zero
//! \param [out] filenameOut - Character buffer into which to write the filename which was opened, after wildcard
//! expansion
//! \param [out] errtext - Character buffer into which to write an error message, if an error occurred
//! \return A file handle from which to read the data, or NULL on error, with an error message in <errtext>

FILE *ppldata_LaunchCoProcess(ppl_context *c, char *filename, int wildcardMatchNumber, char *filenameOut,
                              char *errtext) {
    char msg[PPL_MSG_LENGTH];
    FILE *infile;
    dictIterator *dictIter;
    char *filter, *filterArgs, **argList;
    int i, j, k;
    sigset_t sigs;

    // SIGCHLD is unblocked again once the input filter has been launched
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGCHLD);

    // Implement the magic filename '' to refer to the last used filename
    if (filename[0] == '\0') filename = c->dollarStat.lastFilename;
    // An empty filename means the last file used; otherwise remember this one
    else {
        strncpy(c->dollarStat.lastFilename, filename, FNAME_LENGTH);
        c->dollarStat.lastFilename[FNAME_LENGTH - 1] = '\0';
    }

    // glob filename
    {
        int i, done = 0, C;
        wordexp_t wordExp;
        glob_t globData;
        char fName[FNAME_LENGTH];
        {
            int j, k;
            // Escape spaces in the filename before passing it to wordexp
            for (j = k = 0; ((filename[j] != '\0') && (k < FNAME_LENGTH - 1));) {
                if (filename[j] == ' ') fName[k++] = '\\';
                fName[k++] = filename[j++];
            }
            fName[k++] = '\0';
        }
        if (wildcardMatchNumber < 0) wildcardMatchNumber = 0;
        C = wildcardMatchNumber;
        // Expand ~ and environment variables in the filename
        if ((wordexp(fName, &wordExp, 0) != 0) || (wordExp.we_wordc <= 0)) {
            ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "Could not open file '%s'.", fName);
            if (DEBUG) ppl_log(&c->errcontext, errtext);
            return NULL;
        };
        // Find the requested match of the wildcards among the files matching each word of the expanded filename
        for (i = 0; i < wordExp.we_wordc; i++) {
            // Report a word which matches no files
            if ((glob(wordExp.we_wordv[i], 0, NULL, &globData) != 0) || ((i == 0) && (globData.gl_pathc == 0))) {
                if (wildcardMatchNumber == 0)
                    ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "Could not open file '%s'.", fName);
                else ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "glob produced zero hits.");
                if (DEBUG) ppl_log(&c->errcontext, errtext);
                return NULL;
            }
            // Count past the matches of this word
            if (C >= globData.gl_pathc) {
                C -= globData.gl_pathc;
                globfree(&globData);
                continue;
            }
            filename = (char *) ppl_memAlloc(strlen(globData.gl_pathv[C]) + 1);
            // Failure to allocate memory is reported as an error
            if (filename == NULL) {
                ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "Out of memory (00).");
                globfree(&globData);
                wordfree(&wordExp);
                return NULL;
            }
            strcpy(filename, globData.gl_pathv[C]);
            globfree(&globData);
            done = 1;
            break;
        }
        wordfree(&wordExp);
        // Report that fewer files matched than the number requested
        if (!done) {
            if (wildcardMatchNumber == 0)
                ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "Could not open file '%s'.", fName);
            else ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "glob produced too few hits.");
            if (DEBUG) ppl_log(&c->errcontext, errtext);
            return NULL;
        }
        // Return the filename to the caller, if requested
        if (filenameOut != NULL) {
            strncpy(filenameOut, filename, FNAME_LENGTH);
            filenameOut[FNAME_LENGTH - 1] = '\0';
        }
    }

    // Check whether we have a specified coprocessor to work on this filetype
    dictIter = ppl_dictIterateInit(c->set->filters);
    // Look for an input filter whose wildcard pattern matches the filename
    while (dictIter != NULL) {
        char *dkey = NULL;
        // This filter matches
        if (ppl_strWildcardTest(filename, dictIter->key)) {
            filter = (char *) ((pplObj *) dictIter->data)->auxil;
            // Log the filter in debugging mode
            if (DEBUG) {
                ppl_snprintfTrunc(msg, sizeof(msg), "Using input filter '%s'.", filter);
                ppl_log(&c->errcontext, msg);
            }
            filterArgs = (char *) malloc(strlen(filter) + 1);
            argList = (char **) malloc((strlen(filter) / 2 + 3) * sizeof(char *));
            // Failure to allocate memory is reported as an error
            if ((filterArgs == NULL) || (argList == NULL)) {
                ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "Out of memory (01).");
                if (DEBUG) ppl_log(&c->errcontext, errtext);
                free(filterArgs);
                filterArgs = NULL;
                free(argList);
                argList = NULL;
                return NULL;
            };
            strcpy(filterArgs, filter);
            // Split the filter command into words, with the filename as the last argument
            for (i = j = k = 0; filterArgs[i] != '\0'; i++) {
                // The start of a word
                if ((k == 0) && ((unsigned char) filterArgs[i] > ' ')) {
                    k = 1;
                    argList[j++] = filterArgs + i;
                } else if ((k == 1) && ((unsigned char) filterArgs[i] <= ' ')) {
                    k = 0;
                    filterArgs[i] = '\0';
                }
            }
            argList[j++] = filename;
            argList[j++] = NULL;
            pplcsp_forkInputFilter(
                    c, argList,
                    &i); // Fork process for input filter, and runned piped output through the standard IO library using fdopen()
            sigprocmask(SIG_UNBLOCK, &sigs, NULL);
            // Report a failure to open the filter's output
            if ((infile = fdopen(i, "r")) == NULL) {
                ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "Could not open connection to input filter '%s'.",
                                  argList[0]);
                if (DEBUG) ppl_log(&c->errcontext, errtext);
                free(filterArgs);
                filterArgs = NULL;
                free(argList);
                argList = NULL;
                return NULL;
            };
            free(filterArgs);
            filterArgs = NULL;
            free(argList);
            argList = NULL;
            return infile;
        }
        ppl_dictIterate(&dictIter, &dkey);
    }

    // If not, then we just open the file and return a file-handle to it
    if ((infile = fopen(filename, "r")) == NULL) {
        ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "Could not open input file '%s'.", filename);
        if (DEBUG) ppl_log(&c->errcontext, errtext);
        return NULL;
    };
    return infile;
}

//! ppldata_UsingConvert - Evaluate a single "using" expression for the current row of data, placing the result on the
//! top of the stack
//! \param [in] c - The Pyxplot context
//! \param [in] input - The "using" expression to evaluate
//! \param [in] columns_str - Array of the textual contents of each column of the current row of data
//! \param [in] columns_val - Array of the numerical values of each column of the current row of data
//! \param [in] Ncols - The number of output columns
//! \param [in] filename - The filename of the data file
//! \param [in] file_linenumber - The line number in the data file of the current row of data
//! \param [in] file_linenumbers - Array of the line numbers in the data file from which each column was read, when
//! reading data in rows
//! \param [in] linenumber_count - The number of the current data point within the current block
//! \param [in] block_count - The number of the current block of data within the current index
//! \param [in] index_number - The number of the current index within the data file
//! \param [in] usingRowCol - DATAFILE_COL to read data in columns, or DATAFILE_ROW to read it in rows
//! \param [in] colHeads - Array of the names of each column, taken from the data file's header (may be NULL)
//! \param [in] NcolHeads - The number of entries in <colHeads>
//! \param [in] colUnits - Array of the physical units of each column, taken from the data file's header (may be NULL)
//! \param [in] NcolUnits - The number of entries in <colUnits>
//! \param [out] status - Set to one if the expression could not be evaluated, or evaluated to an invalid value
//! \param [out] errtext - Character buffer into which to write an error message, if an error occurred
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppldata_UsingConvert(ppl_context *c, pplExpr *input, char **columns_str, pplObj *columns_val, const int Ncols,
                          char *filename, long file_linenumber, long *file_linenumbers, const long linenumber_count,
                          const long block_count, const long index_number, const int usingRowCol, char **colHeads,
                          const int NcolHeads, pplObj *colUnits, const int NcolUnits, int *status, char *errtext,
                          const int iterDepth) {
    double dbl = 0;
    int i = -1;
    int l = strlen(input->ascii);
    int namedColumn = 0;

    *status = 0;
    ppl_dollarOp_config(c, columns_str, columns_val, Ncols, filename, file_linenumber, file_linenumbers,
                        linenumber_count, block_count, index_number, usingRowCol, input->ascii, colHeads, NcolHeads,
                        colUnits, NcolUnits);

    // Strip trailing whitespace from the expression
    while ((l > 0) && (input->ascii[l] >= '\0') && ((unsigned char) input->ascii[l] <= ' ')) l--;
    if (ppl_validFloat(input->ascii, &i) &&
        (i > l)) // using 1:2 -- number is column number -- "1" means "the contents of column 1", i.e. "$1".
    {
        dbl = ppl_getFloat(input->ascii, NULL);
        if ((dbl > -4) && (dbl < MAX_DATACOLS)) namedColumn = 1;
    } else {
        for (i = 0; i < NcolHeads; i++)
            // A using item which names a column heading selects that column
            if (strcmp(colHeads[i], input->ascii) == 0) {
                dbl = i + 1.01;
                namedColumn = 1;
                break;
            }
    } // using ColumnName

    // Evaluate the expression, or fetch the named or numbered column directly
    c->dollarStat.warntxt[0] = '\0';
    if (namedColumn) // Clean up these cases
    {
        c->stackPtr++; // Function below assumes it is over-writing the top item on the stack
        ppl_dollarOp_fetchColByNum(c, input, 0, (int) round(dbl));
    } else {
        int lOp = 0;
        ppl_expEval(c, input, &lOp, 1, iterDepth + 1);
    }
    // Report a failure to evaluate the expression
    if ((c->errStat.status) || (c->dollarStat.warntxt[0] != '\0')) {
        int errp = c->errStat.errPosExpr, prefix = 0;
        char *errt = NULL;
        if (errp < 0) errp = 0;
        // Warnings from the $ operator take precedence
        if (c->dollarStat.warntxt[0] != '\0') {
            errt = c->dollarStat.warntxt;
            prefix = 0;
        } else if (c->errStat.errMsgExpr[0] != '\0') errt = c->errStat.errMsgExpr;
        else if (c->errStat.errMsgCmd[0] != '\0') errt = c->errStat.errMsgCmd;
        else errt = "Fail occured.";
        if (prefix)
            ppl_snprintfTrunc(
                    errtext, PPL_MSG_LENGTH,
                    "%s:%ld: Could not evaluate expression <%s>. The error, encountered at character position %d, was: '%s'",
                    filename, file_linenumber, input->ascii, errp, errt);
        else ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "%s", errt);
        ppl_tbClear(c);
        *status = 1;
    }

    ppl_dollarOp_deconfig(c); // Disable dollar operator
    return;
}


//! applyUsingFail - Report an error evaluating the "using" expressions for a row of a data file, unless too many errors
//! have already been reported, and mark the row as having failed
//! \param [in] c - The Pyxplot context
//! \param [in] errtext - The error message
//! \param [in,out] errCount - The number of errors which may still be reported
//! \param [in] filename - The name of the data file
//! \param [in] file_linenumber - The line number of the row in the data file
//! \param [out] status - Set to one, to indicate failure
//! \param [out] discontinuity - Set to one, so that lines drawn through the data are broken at this row

static void applyUsingFail(ppl_context *c, const char *errtext, int *errCount, const char *filename,
                           const long file_linenumber, int *status, int *discontinuity) {
    if (ppl_errorCountTake(errCount)) { // Only report a limited number of errors
        ppl_warning(&c->errcontext, ERR_STACKED, (char *) errtext);
        ppl_errorCountCheck(&c->errcontext, *errCount, filename, file_linenumber);
    }
    *status = 1;
    *discontinuity = 1;
    if (DEBUG) ppl_log(&c->errcontext, (char *) errtext);
}

//! stackTop - Return a pointer to the object on the top of the stack, which is where expressions leave their results
//! \param [in] c - The Pyxplot context
//! \return Pointer to the top object on the stack

static pplObj *stackTop(ppl_context *c) { return &c->stack[c->stackPtr - 1]; }

//! ppldata_ApplyUsingList - Evaluate all of the "using" expressions, the label expression and the select expression for
//! the current row of data, and append the result as a new row of the output data table
//! \param [in] c - The Pyxplot context
//! \param [in] out - The data table to append the row to
//! \param [in] usingExprs - Array of the expressions in the "using" modifier, one per output column
//! \param [in] labelExpr - An expression to evaluate to give a text label for each data point, or NULL
//! \param [in] selectExpr - An expression to evaluate for each data point, which must be true for the data point to be
//! included, or NULL
//! \param [in] continuity - DATAFILE_CONTINUOUS if lines should be drawn between data points in different blocks, or
//! DATAFILE_DISCONTINUOUS if each block should be drawn separately
//! \param [in|out] discontinuity - Flag indicating whether a break should be drawn in lines before the next data point.
//! Set when a data point is rejected by the select expression, if <continuity> is DATAFILE_DISCONTINUOUS.
//! \param [in] columns_str - Array of the textual contents of each column of the current row of data
//! \param [in] columns_val - Array of the numerical values of each column of the current row of data
//! \param [in] Ncols - The number of output columns
//! \param [in] filename - The filename of the data file
//! \param [in] file_linenumber - The line number in the data file of the current row of data
//! \param [in] file_linenumbers - Array of the line numbers in the data file from which each column was read, when
//! reading data in rows
//! \param [in] linenumber_count - The number of the current data point within the current block
//! \param [in] block_count - The number of the current block of data within the current index
//! \param [in] index_number - The number of the current index within the data file
//! \param [in] usingRowCol - DATAFILE_COL to read data in columns, or DATAFILE_ROW to read it in rows
//! \param [in] colHeads - Array of the names of each column, taken from the data file's header (may be NULL)
//! \param [in] NcolHeads - The number of entries in <colHeads>
//! \param [in] colUnits - Array of the physical units of each column, taken from the data file's header (may be NULL)
//! \param [in] NcolUnits - The number of entries in <colUnits>
//! \param [out] status - Set to a non-zero value if an error occurred
//! \param [out] errtext - Character buffer into which to write an error message, if an error occurred
//! \param [in|out] errCount - The number of errors reported so far; once this exceeds a limit, further errors are
//! suppressed
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppldata_ApplyUsingList(ppl_context *c, dataTable *out, pplExpr **usingExprs, pplExpr *labelExpr,
                            pplExpr *selectExpr, const int continuity, int *discontinuity, char **columns_str,
                            pplObj *columns_val, const int Ncols, char *filename, long file_linenumber,
                            long *file_linenumbers, const long linenumber_count, const long block_count,
                            const long index_number, const int usingRowCol, char **colHeads, const int NcolHeads,
                            pplObj *colUnits, const int NcolUnits, int *status, char *errtext, int *errCount,
                            const int iterDepth) {
    const int stkLevelOld = c->stackPtr;
    const int nUsing = out->Ncolumns_real + out->Ncolumns_obj;
    char **labOut = &out->current->text[out->current->blockPosition];
    const int memContext = out->memContext;
    int i;


    *status = 0;
    *labOut = NULL;

    // select criterion
    if (selectExpr != NULL) {
        int fail = 0;
        pplExpr *ex = selectExpr;
        ppldata_UsingConvert(c, ex, columns_str, columns_val, Ncols, filename, file_linenumber, file_linenumbers,
                             linenumber_count, block_count, index_number, usingRowCol, colHeads, NcolHeads, colUnits,
                             NcolUnits, &fail, errtext, iterDepth);
        // Give up on this row if the label could not be evaluated
        if (fail) {
            ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack before giving up
            {
                applyUsingFail(c, errtext, errCount, filename, file_linenumber, status, discontinuity);
                return;
            }
        }
        ppl_castToBool(c, stackTop(c));
        // Rows which fail the select criterion are skipped, leaving a break in the line if the data are discontinuous
        if (stackTop(c)->real == 0) {
            ppl_stackClean(c, stkLevelOld); // Discard the condition's value, now that it has been tested
            *discontinuity = (continuity == DATAFILE_DISCONTINUOUS);
            *status = 0;
            return;
        }
        ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack
    }

    // label
    if (labelExpr != NULL) {
        int fail = 0;
        char *labIn = NULL;
        pplExpr *ex = labelExpr;
        ppldata_UsingConvert(c, ex, columns_str, columns_val, Ncols, filename, file_linenumber, file_linenumbers,
                             linenumber_count, block_count, index_number, usingRowCol, colHeads, NcolHeads, colUnits,
                             NcolUnits, &fail, errtext, iterDepth);
        // Give up on this row if the expression could not be evaluated
        if (fail) {
            ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack before giving up
            {
                applyUsingFail(c, errtext, errCount, filename, file_linenumber, status, discontinuity);
                return;
            }
        }
        // The label must be a string
        if (stackTop(c)->objType != PPLOBJ_STR) {
            ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH,
                              "%s:%ld: Label expression '%s' did not evaluate to a string, but to type <%s>.", filename,
                              file_linenumber, ex->ascii, pplObjTypeNames[stackTop(c)->objType]);
            ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack before giving up
            {
                applyUsingFail(c, errtext, errCount, filename, file_linenumber, status, discontinuity);
                return;
            }
        }
        labIn = (char *) stackTop(c)->auxil;
        *labOut = (char *) ppl_memAlloc_incontext(strlen(labIn) + 1, memContext);
        if (*labOut != NULL) strcpy(*labOut, labIn);
        ppl_stackClean(c, stkLevelOld); // Discard the expression's value, now that it has been copied
    }

    // using expressions
    for (i = 0; i < nUsing; i++) {
        int fail = 0;
        const int outObj = i >= out->Ncolumns_real;
        const int idx = outObj ? (i - out->Ncolumns_real) : i;
        pplExpr *ex = usingExprs[i];
        ppldata_UsingConvert(c, ex, columns_str, columns_val, Ncols, filename, file_linenumber, file_linenumbers,
                             linenumber_count, block_count, index_number, usingRowCol, colHeads, NcolHeads, colUnits,
                             NcolUnits, &fail, errtext, iterDepth);
        // Give up on this row if the expression could not be evaluated
        if (fail) {
            ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack before giving up
            {
                applyUsingFail(c, errtext, errCount, filename, file_linenumber, status, discontinuity);
                return;
            }
        }
        // Columns of real values must be numbers, dates or booleans; other columns hold objects, such as colors
        if (!outObj) {
            if ((stackTop(c)->objType != PPLOBJ_NUM) && (stackTop(c)->objType != PPLOBJ_BOOL) &&
                (stackTop(c)->objType != PPLOBJ_DATE)) {
                ppl_snprintfTrunc(
                        errtext, PPL_MSG_LENGTH,
                        "%s:%ld: Data item calculated from expression '%s' was not a number, but had type <%s>.",
                        filename, file_linenumber, ex->ascii, pplObjTypeNames[stackTop(c)->objType]);
                ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack before giving up
                {
                    applyUsingFail(c, errtext, errCount, filename, file_linenumber, status, discontinuity);
                    return;
                }
            }
            // Values must be real
            if ((stackTop(c)->objType == PPLOBJ_NUM) && (stackTop(c)->flagComplex)) {
                ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH,
                                  "%s:%ld: Data item calculated from expression '%s' was a complex number.", filename,
                                  file_linenumber, ex->ascii);
                ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack before giving up
                {
                    applyUsingFail(c, errtext, errCount, filename, file_linenumber, status, discontinuity);
                    return;
                }
            }
            // The first row sets the type and units of each column; later rows must match them
            if (out->Nrows == 0) {
                out->firstEntries[i] = *stackTop(c);
                out->firstEntries[i].refCount = 1; // no memory leak here, as all allowed types have no auxilary data
                out->firstEntries[i].amMalloced = 0;
            } else {
                // The value must have the same type as the first row's
                if (out->firstEntries[i].objType != stackTop(c)->objType) {
                    ppl_snprintfTrunc(
                            errtext, PPL_MSG_LENGTH,
                            "%s:%ld: Data item calculated from expression '%s' has inconsistent types, including <%s> and <%s>.",
                            filename, file_linenumber, ex->ascii, pplObjTypeNames[out->firstEntries[i].objType],
                            pplObjTypeNames[stackTop(c)->objType]);
                    ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack before giving up
                    {
                        applyUsingFail(c, errtext, errCount, filename, file_linenumber, status, discontinuity);
                        return;
                    }
                }
                if ((out->firstEntries[i].objType == PPLOBJ_NUM) &&
                    (!ppl_unitsDimEqual(&out->firstEntries[i], stackTop(c)))) {
                    ppl_snprintfTrunc(
                            errtext, PPL_MSG_LENGTH,
                            "%s:%ld: Data item calculated from expression '%s' has inconsistent physical units, including <%s> and <%s>.",
                            filename, file_linenumber, ex->ascii,
                            ppl_printUnit(c, &out->firstEntries[i], NULL, NULL, 0, 1, 0),
                            ppl_printUnit(c, stackTop(c), NULL, NULL, 1, 1, 0));
                    ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack before giving up
                    {
                        applyUsingFail(c, errtext, errCount, filename, file_linenumber, status, discontinuity);
                        return;
                    }
                }
            }
            out->current->data_real[idx + out->current->blockPosition * out->Ncolumns_real] = stackTop(c)->real;
        } else {
            pplObj *oo = &out->current->data_obj[idx + out->current->blockPosition * out->Ncolumns_obj];
            oo->refCount = 1;
            pplObjCpy(oo, stackTop(c), 0, 0, 1);
        }
        ppl_stackClean(c, stkLevelOld); // Discard the expression's value, now that it has been copied
    }

    // Mark any break in the data before this row, and move on to the next row
    out->current->split[out->current->blockPosition] = *discontinuity;
    i = ppldata_DataTable_AddRow(out);
    // Failure to allocate memory is reported as an error
    if (i) {
        ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "%s:%ld: Out of memory storing data table.", filename,
                          file_linenumber);
        *errCount = -1;
        {
            applyUsingFail(c, errtext, errCount, filename, file_linenumber, status, discontinuity);
            return;
        }
    }
    *discontinuity = 0;
    return;
}

//! ppldata_RotateRawData - Convert a table of raw data read in rows into columns, apply the "using" expressions to each
//! column in turn, and append the results to an output data table. The raw data table is then emptied.
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Pointer to the raw data table, which is reset to an empty table
//! \param [in] out - The data table to append the results to
//! \param [in] usingExprs - Array of the expressions in the "using" modifier, one per output column
//! \param [in] labelExpr - An expression to evaluate to give a text label for each data point, or NULL
//! \param [in] selectExpr - An expression to evaluate for each data point, which must be true for the data point to be
//! included, or NULL
//! \param [in] continuity - DATAFILE_CONTINUOUS if lines should be drawn between data points in different blocks, or
//! DATAFILE_DISCONTINUOUS if each block should be drawn separately
//! \param [in] filename - The filename of the data file
//! \param [in] block_count - The number of the current block of data within the current index
//! \param [in] index_number - The number of the current index within the data file
//! \param [in] colHeads - Array of the names of each column, taken from the data file's header (may be NULL)
//! \param [in] NcolHeads - The number of entries in <colHeads>
//! \param [in] colUnits - Array of the physical units of each column, taken from the data file's header (may be NULL)
//! \param [in] NcolUnits - The number of entries in <colUnits>
//! \param [out] status - Set to a non-zero value if an error occurred
//! \param [out] errtext - Character buffer into which to write an error message, if an error occurred
//! \param [in|out] errCount - The number of errors reported so far; once this exceeds a limit, further errors are
//! suppressed
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppldata_RotateRawData(ppl_context *c, rawDataTable **in, dataTable *out, pplExpr **usingExprs, pplExpr *labelExpr,
                           pplExpr *selectExpr, const int continuity, char *filename, const long block_count,
                           const long index_number, char **colHeads, const int NcolHeads, pplObj *colUnits,
                           const int NcolUnits, int *status, char *errtext, int *errCount, const int iterDepth) {
    int i;
    long linenumber_count = 0;
    int discontinuity = 1;
    const int contextRaw = (*in)->memContext;
    unsigned char hadspace[MAX_DATACOLS];
    unsigned char hadcomma[MAX_DATACOLS];

    // Each column starts at a word boundary
    for (i = 0; i < MAX_DATACOLS; i++) hadspace[i] = 1;
    for (i = 0; i < MAX_DATACOLS; i++) hadcomma[i] = 0;

    // Make a row of data from the next word of each line read, until the lines run out
    while (!cancellationFlag) {
        int gotData = 0, Ncols = 0;
        rawDataBlock *blk = (*in)->first;
        char *rowData[MAX_DATACOLS];
        long file_linenumber[MAX_DATACOLS];

        // Take the next word of each stored line as one column of the row
        while ((blk != NULL) && (Ncols < MAX_DATACOLS)) {
            file_linenumber[0] = -1;
            // Each stored line
            for (i = 0; ((i < blk->blockPosition) && (Ncols < MAX_DATACOLS)); i++) {
                char *s = blk->text[i];
                // A line which has run out supplies a blank value
                if (s == NULL) {
                    rowData[Ncols] = " ";
                    file_linenumber[Ncols] = -1;
                    Ncols++;
                } else {
                    int caught = 0, p = 0;
                    // Find the next word of the line, separated by whitespace or commas
                    for (; (s[p] != '\0'); p++) {
                        // Whitespace separates words
                        if ((unsigned char) s[p] <= ' ') {
                            hadspace[Ncols] = 1;
                        } else if (s[p] == ',') {
                            p++;
                            while ((s[p] != '\0') && ((unsigned char) s[p] <= ' ')) { p++; }
                            caught = 1;
                            hadspace[Ncols] = hadcomma[Ncols] = 1;
                        } else {
                            if (hadspace[Ncols] && !hadcomma[Ncols]) { caught = 1; }
                            hadspace[Ncols] = hadcomma[Ncols] = 0;
                        }
                        if (caught) break;
                    }

                    // Move past the word
                    blk->text[i] += p;
                    // A line with no more words supplies a blank value
                    if (!caught) {
                        rowData[Ncols] = " ";
                        file_linenumber[Ncols] = -1;
                        Ncols++;
                    } else {
                        rowData[Ncols] = blk->text[i];
                        file_linenumber[Ncols] = blk->fileLine[i];
                        Ncols++;
                        gotData = 1;
                    }
                }
            }
            blk = blk->next;
        }
        if (!gotData) break;

        // Apply the using expressions to the row
        ppldata_ApplyUsingList(c, out, usingExprs, labelExpr, selectExpr, continuity, &discontinuity, rowData, NULL,
                               Ncols, filename, file_linenumber[0], file_linenumber, linenumber_count, block_count,
                               index_number, DATAFILE_ROW, colHeads, NcolHeads, colUnits, NcolUnits, status, errtext,
                               errCount, iterDepth);
        linenumber_count++;
    }

    // Discard the stored lines, and start a new table for the next block
    ppl_memAlloc_Free(contextRaw);
    *in = ppldata_NewRawDataTable(contextRaw);
    return;
}

// Routines for sorting data tables

typedef struct ppldata_sorter {
    double *data_real;
    pplObj *data_obj;
    char *text;
    long *fileLine_real;
    long *fileLine_obj;
    unsigned char split;
} ppldata_sorter;

static int ppldata_sort_sortCol;

//! ppldata_sort_cmp - Comparison function for qsort(), used to sort the rows of a data table by the value in the sort
//! column
//! \param [in] xv - Pointer to the first row
//! \param [in] yv - Pointer to the second row
//! \return -1, 0 or 1 if the first row's sort value is less than, equal to or greater than the second's

static int ppldata_sort_cmp(const void *xv, const void *yv) {
    ppldata_sorter *x = (ppldata_sorter *) xv;
    ppldata_sorter *y = (ppldata_sorter *) yv;
    return pplObjCmpQuiet((void *) &x->data_obj[ppldata_sort_sortCol], (void *) &y->data_obj[ppldata_sort_sortCol]);
}

//! ppldata_sort - Sort the rows of a data table by the values in one of its columns. Each block of data is sorted
//! separately, unless the data are continuous between blocks.
//! \param [in] c - The Pyxplot context
//! \param [in] in - The data table to sort
//! \param [in] sortCol - The number of the column containing the values to sort by
//! \param [in] ignoreContinuity - If true, sort all of the blocks together as a single list of data points
//! \return A new, sorted data table, or <in> if it could not be sorted

dataTable *ppldata_sort(const ppl_context *c, const dataTable *in, const int sortCol, const int ignoreContinuity) {
    // Variables for sorting the table
    int i, io, Nc, Nd, Nr;
    long ji, jo;
    dataBlock *blk;
    ppldata_sorter *sorter;
    dataTable *output;

    // There is nothing to sort without data
    if (in == NULL) return NULL;

    // The numbers of columns, and of rows
    Nc = in->Ncolumns_obj;
    Nd = in->Ncolumns_real;
    Nr = in->Nrows;
    sorter = (ppldata_sorter *) malloc(Nr * sizeof(ppldata_sorter));
    if (sorter == NULL) return NULL;

    // Transfer data from DataTable into a DataTable_sorter array which is in a format qsort() can sort
    blk = in->first;
    jo = 0;
    // Make an array of pointers to the rows
    while (blk != NULL) {
        long ji;
        // Each row of the block
        for (ji = 0; ji < blk->blockPosition; ji++, jo++) {
            sorter[jo].data_real = &(blk->data_real[Nd * ji]);
            sorter[jo].data_obj = &(blk->data_obj[Nc * ji]);
            sorter[jo].text = blk->text[ji];
            sorter[jo].fileLine_real = &(blk->fileLine_real[Nc * ji]);
            sorter[jo].fileLine_obj = &(blk->fileLine_obj[Nc * ji]);
            if (ignoreContinuity) sorter[jo].split = 0;
            else sorter[jo].split = blk->split[ji];
        }
        blk = blk->next;
    }

    // Sort the DataTable_sorter array
    ppldata_sort_sortCol = sortCol - in->Ncolumns_real;
    for (i = 0, io = 0; io <= jo; io++)
        // Sort each run of rows between breaks separately, unless the data are continuous
        if ((io == jo) || sorter[io].split) {
            qsort((void *) (sorter + i), io - i, sizeof(ppldata_sorter), ppldata_sort_cmp);
            sorter[i].split = (i != 0);
            for (ji = i + 1; ji < io; ji++) sorter[ji].split = 0;
            i = io;
        }

    // Copy sorted data into a new dataTable, removing the sort column
    output = ppldata_NewDataTable(Nc, Nd - 1, in->memContext, Nr);
    // Copy the rows into a new table, in sorted order, without the sort column
    if (output != NULL) {
        const int sc = sortCol - in->Ncolumns_real;
        int i, io;
        long jo;
        for (i = 0, io = 0; i < Nc; i++) output->firstEntries[io++] = in->firstEntries[i];
        for (jo = 0; jo < Nr; jo++)
            for (i = 0, io = 0; i < Nc; i++) output->first->data_real[(io++) + Nc * jo] = sorter[jo].data_real[i];
        for (jo = 0; jo < Nr; jo++)
            for (i = 0, io = 0; i < Nd; i++)
                if (i != sc) output->first->data_obj[(io++) + (Nd - 1) * jo] = sorter[jo].data_obj[i];
        for (jo = 0; jo < Nr; jo++) output->first->text[jo] = sorter[jo].text;
        for (jo = 0; jo < Nr; jo++)
            for (i = 0, io = 0; i < Nc; i++)
                output->first->fileLine_real[(io++) + Nc * jo] = sorter[jo].fileLine_real[i];
        for (jo = 0; jo < Nr; jo++)
            for (i = 0, io = 0; i < Nd; i++)
                if (i != sc) output->first->fileLine_obj[(io++) + (Nd - 1) * jo] = sorter[jo].fileLine_obj[i];
        for (jo = 0; jo < Nr; jo++) output->first->split[jo] = sorter[jo].split;
        output->Nrows = Nr;
        output->first->blockPosition = Nr;
    }
    free(sorter);
    sorter = NULL;
    return output;
}

//! ppldata_fetchFromSpool - Fetch the next line of data supplied inline after a command
//! \param [in|out] dataSpool - Pointer to the next line of inline data, which is advanced to the following line
//! \return The text of the line, or NULL if there are no more lines

static char *ppldata_fetchFromSpool(parserLine **dataSpool) {
    char *out;
    if ((dataSpool == NULL) || (*dataSpool == NULL)) return NULL;
    out = (*dataSpool)->linetxt;
    *dataSpool = (*dataSpool)->next;
    return out;
}

//! ppldata_autoUsingList - Generate the default "using" expressions 1:2:3:..., for when the user has not supplied any
//! \param [in] c - The Pyxplot context
//! \param [out] usingExprs - Array into which to write the compiled expressions
//! \param [in] Ncols - The number of output columns
//! \param [out] errtext - Character buffer into which to write an error message, if an error occurred
//! \return Zero on success, or one if the expressions could not be compiled

int ppldata_autoUsingList(ppl_context *c, pplExpr **usingExprs, const int Ncols, char *errtext) {
    int i;
    // Compile the expression for each column, $1, $2, ...
    for (i = 0; i < Ncols; i++) {
        int end = 0, ep = 0, es = 0;
        char ascii[16];
        pplExpr *exptmp = NULL;
        ppl_snprintfTrunc(ascii, sizeof(ascii), "%d", i + 1);
        ppl_expCompile(c, 0, 0, "", ascii, &end, 0, 0, 0, &exptmp, &ep, &es, errtext);
        // Report a failure to compile the expression
        if (es || c->errStat.status) {
            ppl_tbClear(c);
            ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "Out of memory (03).");
            if (DEBUG) ppl_log(&c->errcontext, errtext);
            return 1;
        }
        usingExprs[i] = pplExpr_tmpcpy(exptmp);
        pplExpr_free(exptmp);
        exptmp = NULL;
        // Failure to allocate memory is reported as an error
        if (usingExprs[i] == NULL) {
            ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "Out of memory (04).");
            if (DEBUG) ppl_log(&c->errcontext, errtext);
            return 1;
        }
    }
    return 0;
}

//! closeDataInput - Close the stream from which a data file was being read, unless it is stdin or data was being
//! read from the command line
//! \param [in] input - The stream
//! \param [in] readFromCommandLine - True if data was being read from lines following the command, not a file

static void closeDataInput(FILE *input, const int readFromCommandLine) {
    if ((!readFromCommandLine) && (input != stdin)) fclose(input);
}

//! ppldata_fromFile - Read data from a file, applying the "using", "every", "index", "select" and "sort" modifiers,
//! into a data table
//! \param [in] c - The Pyxplot context
//! \param [out] out - Set to point to the output data table
//! \param [in] filename - The filename of the data file, which may contain wildcards, or "--" for data supplied inline
//! \param [in] wildcardMatchNumber - For filenames containing wildcards, the number of the matching file to read,
//! counting from zero
//! \param [out] filenameOut - Character buffer into which to write the filename which was actually read, after wildcard
//! expansion (optional)
//! \param [in|out] dataSpool - Lines of data supplied inline after the command, if the filename is "--"
//! \param [in] indexNo - The index within the data file to read, or -1 to read all indices
//! \param [in] usingExprs - Array of the expressions in the "using" modifier, one per output column
//! \param [in] autoUsingExprs - If true, the "using" expressions were not supplied by the user, but generated
//! automatically
//! \param [in] Ncols - The number of output columns
//! \param [in] NusingObjs - The number of output columns which contain objects other than numbers, e.g. strings for
//! labels
//! \param [in] labelExpr - An expression to evaluate to give a text label for each data point, or NULL
//! \param [in] selectExpr - An expression to evaluate for each data point, which must be true for the data point to be
//! included, or NULL
//! \param [in] sortBy - An expression by which to sort the data points, or NULL to leave them unsorted
//! \param [in] usingRowCol - DATAFILE_COL to read data in columns, or DATAFILE_ROW to read it in rows
//! \param [in] everyList - The six parameters of the "every" modifier: the step between data points, the step between
//! blocks, the first data point and block, and the last data point and block (-1 for no limit)
//! \param [in] continuity - DATAFILE_CONTINUOUS if lines should be drawn between data points in different blocks, or
//! DATAFILE_DISCONTINUOUS if each block should be drawn separately
//! \param [in] persistent - If true, allocate the output data table in the lowest ppl_memAlloc context, so that it
//! persists after the current command; otherwise allocate it in the current context
//! \param [out] status - Set to a non-zero value if an error occurred
//! \param [out] errtext - Character buffer into which to write an error message, if an error occurred
//! \param [in|out] errCount - The number of errors reported so far; once this exceeds a limit, further errors are
//! suppressed
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppldata_fromFile(ppl_context *c, dataTable **out, char *filename, const int wildcardMatchNumber, char *filenameOut,
                      parserLine **dataSpool, const int indexNo, pplExpr **usingExprs, int autoUsingExprs, int Ncols,
                      int NusingObjs, pplExpr *labelExpr, pplExpr *selectExpr, pplExpr *sortBy, const int usingRowCol,
                      const long *everyList, const int continuity, const int persistent, int *status, char *errtext,
                      int *errCount, const int iterDepth) {
    char msg[PPL_MSG_LENGTH];
    int readFromCommandLine = 0, discontinuity = 0, hadwhitespace, hadcomma, oneColumnInput = 1;
    int contextOutput, contextRough, contextRaw;
    char lineNumberStr[32];
    const long linestep = everyList[0], blockstep = everyList[1], linefirst = everyList[2], blockfirst = everyList[3],
               linelast = everyList[4], blocklast = everyList[5];
    long index_number = 0;
    long linenumber_count = 0;
    long linenumber_stepcnt = 0;
    long block_count = 0;
    long block_stepcnt = 0;
    long prev_blanklines = 10;
    long file_linenumber = 0;
    long itemsOnLine;
    FILE *filtered_input = NULL;
    char linebuffer[LSTR_LENGTH], *lineptr = linebuffer, *linebufferPtr = &linebuffer[0], *cptr;

    // Variables for reading the data file
    int i, j, k, l, m;

    // Column and row headings and units, read from comment lines in the file
    char **columnHeadings = NULL;
    int NcolumnHeadings = 0;
    char **rowHeadings = NULL;
    int NrowHeadings = 0;
    pplObj *columnUnits = NULL;
    int NcolumnUnits = 0;
    pplObj *rowUnits = NULL;
    int NrowUnits = 0;

    // The words of the current line
    char *colData[MAX_DATACOLS];
    rawDataTable *rawDataTab = NULL;

    // Init
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Opening datafile '%s'.", filename);
        ppl_log(&c->errcontext, msg);
    }
    // Unless the number of columns is given, assume two
    if (Ncols < 0) {
        // If number of columns unspecified, assume two
        autoUsingExprs = 1;
        if ((*status = ppldata_autoUsingList(c, usingExprs, Ncols = 2, errtext))) return;
    }
    if (Ncols != 2)
        oneColumnInput = 0; // Only have special handling for one-column datafiles when looking for two columns

    // If sortBy is not null, add it to using list
    if (sortBy != NULL) {
        usingExprs[Ncols] = sortBy;
        NusingObjs++;
        Ncols++;
    }

    // Open requested datafile
    if (strcmp(filename, "-") == 0) {
        // A filename of - means standard input, which cannot contain several files
        if (wildcardMatchNumber > 0) {
            *status = 1;
            return;
        }
        filtered_input = stdin;
        if (DEBUG) ppl_log(&c->errcontext, "Reading from stdin.");
    } else if (strcmp(filename, "--") == 0) {
        // A filename of -- means data supplied inline after the command
        if (wildcardMatchNumber > 0) {
            *status = 1;
            return;
        }
        readFromCommandLine = 1;
        if (DEBUG) ppl_log(&c->errcontext, "Reading from command line.");
    } else {
        filtered_input = ppldata_LaunchCoProcess(c, filename, wildcardMatchNumber, filenameOut, errtext);
        // Report a file which cannot be opened
        if (filtered_input == NULL) {
            *status = 1;
            return;
        }
    }


    // Keep a record of the memory context we're going to output into, and then make a scratchpad context
    contextOutput = persistent ? 0 : ppl_memAlloc_GetMemContext();
    contextRough = ppl_memAlloc_DescendIntoNewContext(); // Rough mallocs inside this subroutine happen in this context
    contextRaw = ppl_memAlloc_DescendIntoNewContext(); // Raw data goes into here when plotting with rows

    *out = ppldata_NewDataTable(Ncols - NusingObjs, NusingObjs, contextOutput, -1);
    if (*out == NULL) {
        ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "%s",
                          "Out of memory whilst trying to allocate data table to read data from file.");
        *status = 1;
        if (DEBUG) ppl_log(&c->errcontext, errtext);
        closeDataInput(filtered_input, readFromCommandLine);
        return;
    }
    // When reading data in rows, the lines are stored and turned into columns later
    if (usingRowCol == DATAFILE_ROW) {
        rawDataTab = ppldata_NewRawDataTable(contextRaw);
        // Failure to allocate memory is reported as an error
        if (rawDataTab == NULL) {
            ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "%s",
                              "Out of memory whilst trying to allocate data table to read data from file.");
            *status = 1;
            if (DEBUG) ppl_log(&c->errcontext, errtext);
            closeDataInput(filtered_input, readFromCommandLine);
            return;
        }
    }

    // Read input file, line by line
    while (readFromCommandLine || ((!ferror(filtered_input)) && (!feof(filtered_input)))) {
        if (cancellationFlag) break;
        if (!readFromCommandLine) ppl_file_readline(filtered_input, &linebufferPtr, NULL, LSTR_LENGTH);
        else lineptr = ppldata_fetchFromSpool(dataSpool);

        if (readFromCommandLine && (lineptr == NULL)) break; // End of file reached

        // Lines of data supplied inline after the command may be of any length, so truncate them to fit linebuffer
        if (lineptr != linebuffer) ppl_snprintfTrunc(linebuffer, sizeof(linebuffer), "%s", lineptr);
        ppl_strStrip(linebuffer, linebuffer);
        if (readFromCommandLine && (strcmp(linebuffer, "END") == 0)) break;

        // Count lines, blocks and indices: one blank line starts a new block, two start a new index
        file_linenumber++;

        // Skip leading whitespace
        for (j = 0; ((linebuffer[j] != '\0') && ((unsigned char) linebuffer[j] <= ' ')); j++);
        if (linebuffer[j] == '\0') // We have a blank line
        {
            if (prev_blanklines > 1) continue; // A very long gap in the datafile
            prev_blanklines++;
            // Count consecutive blank lines
            if (prev_blanklines == 1) {
                block_count++; // First newline gives us a new block
                block_stepcnt = ((block_stepcnt - 1) % blockstep);
                discontinuity = 1; // We want to break the line here
                linenumber_count = 0; // Zero linecounter within block
                linenumber_stepcnt = 0;
            } else {
                index_number++; // Second newline gives us a new index
                block_count = 0; // Zero block counter
                block_stepcnt = 0;
                // Log the new index in debugging mode
                if (DEBUG) {
                    ppl_snprintfTrunc(msg, sizeof(msg), "Index %ld begins on line %ld of datafile.", index_number,
                                      file_linenumber);
                    ppl_log(&c->errcontext, msg);
                }
                if ((indexNo >= 0) && (index_number > indexNo))
                    break; // We'll never find any more data once we've gone past the specified index
            }
            continue;
        }

        // Ignore comment lines
        if (linebuffer[j] == '#') {
            for (i = j + 1; ((linebuffer[i] != '\0') && ((unsigned char) linebuffer[i] <= ' ')); i++);
            if ((strncmp(linebuffer + i, "Columns:", 8) == 0) &&
                (usingRowCol == DATAFILE_COL)) // '# Columns:' means we have a list of column headings
            {
                i += 8;
                // Log progress in debugging mode
                if (DEBUG) {
                    ppl_snprintfTrunc(msg, sizeof(msg), "Reading column headings as specified on line %ld of datafile",
                                      file_linenumber);
                    ppl_log(&c->errcontext, msg);
                }
                while ((linebuffer[i] != '\0') && ((unsigned char) linebuffer[i] <= ' ')) i++;
                itemsOnLine = 0;
                hadwhitespace = 1;
                // Count the column headings on the line
                for (j = i; linebuffer[j] != '\0'; j++) {
                    // Whitespace separates headings
                    if ((unsigned char) linebuffer[j] <= ' ') {
                        hadwhitespace = 1;
                    } else if (hadwhitespace) {
                        itemsOnLine++;
                        hadwhitespace = 0;
                    }
                }
                cptr = (char *) ppl_memAlloc_incontext(j - i + 1, contextRough);
                strcpy(cptr, linebuffer + i);
                columnHeadings = (char **) ppl_memAlloc_incontext(itemsOnLine * sizeof(char *), contextRough);
                NcolumnHeadings = itemsOnLine;
                // Failure to allocate memory is reported as an error
                if ((cptr == NULL) || (columnHeadings == NULL)) {
                    ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "%s", "Out of memory (05).");
                    *status = 1;
                    if (DEBUG) ppl_log(&c->errcontext, errtext);
                    closeDataInput(filtered_input, readFromCommandLine);
                    return;
                }
                itemsOnLine = 0;
                hadwhitespace = 1;
                // Split the line into headings
                for (j = 0; cptr[j] != '\0'; j++) {
                    // Terminate each heading at whitespace
                    if ((unsigned char) cptr[j] <= ' ') {
                        cptr[j] = '\0';
                        hadwhitespace = 1;
                    } else if (hadwhitespace) {
                        columnHeadings[itemsOnLine] = cptr + j;
                        itemsOnLine++;
                        hadwhitespace = 0;
                    }
                }
                // Log the headings in debugging mode
                if (DEBUG) {
                    ppl_snprintfTrunc(msg, sizeof(msg), "Total of %ld column headings read.", itemsOnLine);
                    ppl_log(&c->errcontext, msg);
                    // Log each heading
                    for (k = 0; k < itemsOnLine; k++) {
                        ppl_snprintfTrunc(msg, sizeof(msg), "Column heading %d: %s", k, columnHeadings[k]);
                        ppl_log(&c->errcontext, msg);
                    }
                }
            } else if ((strncmp(linebuffer + i, "Rows:", 5) == 0) &&
                       (usingRowCol == DATAFILE_ROW)) // '# Rows:' means we have a list of row headings
            {
                i += 5;
                // Log progress in debugging mode
                if (DEBUG) {
                    ppl_snprintfTrunc(msg, sizeof(msg), "Reading row headings as specified on line %ld of datafile",
                                      file_linenumber);
                    ppl_log(&c->errcontext, msg);
                }
                while ((linebuffer[i] != '\0') && ((unsigned char) linebuffer[i] <= ' ')) i++;
                itemsOnLine = 0;
                hadwhitespace = 1;
                // Count the row headings on the line
                for (j = i; linebuffer[j] != '\0'; j++) {
                    // Whitespace separates headings
                    if ((unsigned char) linebuffer[j] <= ' ') {
                        hadwhitespace = 1;
                    } else if (hadwhitespace) {
                        itemsOnLine++;
                        hadwhitespace = 0;
                    }
                }
                cptr = (char *) ppl_memAlloc_incontext(j - i + 1, contextRough);
                strcpy(cptr, linebuffer + i);
                rowHeadings = (char **) ppl_memAlloc_incontext(itemsOnLine * sizeof(char *), contextRough);
                NrowHeadings = itemsOnLine;
                // Failure to allocate memory is reported as an error
                if ((cptr == NULL) || (rowHeadings == NULL)) {
                    ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "%s", "Out of memory (06).");
                    *status = 1;
                    if (DEBUG) ppl_log(&c->errcontext, errtext);
                    closeDataInput(filtered_input, readFromCommandLine);
                    return;
                }
                itemsOnLine = 0;
                hadwhitespace = 1;
                // Split the line into headings
                for (j = 0; cptr[j] != '\0'; j++) {
                    // Terminate each heading at whitespace
                    if ((unsigned char) cptr[j] <= ' ') {
                        cptr[j] = '\0';
                        hadwhitespace = 1;
                    } else if (hadwhitespace) {
                        rowHeadings[itemsOnLine] = cptr + j;
                        itemsOnLine++;
                        hadwhitespace = 0;
                    }
                }
                // Log the headings in debugging mode
                if (DEBUG) {
                    ppl_snprintfTrunc(msg, sizeof(msg), "Total of %ld row headings read.", itemsOnLine);
                    ppl_log(&c->errcontext, msg);
                    // Log each heading
                    for (k = 0; k < itemsOnLine; k++) {
                        ppl_snprintfTrunc(msg, sizeof(msg), "Row heading %d: %s", k, rowHeadings[k]);
                        ppl_log(&c->errcontext, msg);
                    }
                }
            } else if (strncmp(linebuffer + i, "ColumnUnits:", 12) ==
                       0) // '# ColumnUnits:' means we have a list of column units
            {
                i += 12;
                // Log progress in debugging mode
                if (DEBUG) {
                    ppl_snprintfTrunc(msg, sizeof(msg), "Reading column units as specified on line %ld of datafile",
                                      file_linenumber);
                    ppl_log(&c->errcontext, msg);
                }
                while ((linebuffer[i] != '\0') && ((unsigned char) linebuffer[i] <= ' ')) i++;
                itemsOnLine = 0;
                hadwhitespace = 1;
                // Count the column units on the line
                for (j = i; linebuffer[j] != '\0'; j++) {
                    // Whitespace separates units
                    if ((unsigned char) linebuffer[j] <= ' ') {
                        hadwhitespace = 1;
                    } else if (hadwhitespace) {
                        itemsOnLine++;
                        hadwhitespace = 0;
                    }
                }
                cptr = (char *) ppl_memAlloc_incontext(j - i + 2, contextRough);
                strncpy(cptr, linebuffer + i, j - i + 2);
                columnUnits = (pplObj *) ppl_memAlloc_incontext(itemsOnLine * sizeof(pplObj), contextRough);
                NcolumnUnits = itemsOnLine;
                // Failure to allocate memory is reported as an error
                if ((cptr == NULL) || (columnUnits == NULL)) {
                    ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "%s", "Out of memory (07).");
                    *status = 1;
                    if (DEBUG) ppl_log(&c->errcontext, errtext);
                    closeDataInput(filtered_input, readFromCommandLine);
                    return;
                }
                itemsOnLine = 0;
                hadwhitespace = 1;
                // Evaluate each unit in turn
                for (k = 0; cptr[k] != '\0'; k++) {
                    // Skip whitespace between units
                    if ((unsigned char) cptr[k] <= ' ') {
                        continue;
                    } else {
                        for (l = 0; (unsigned char) cptr[k + l] > ' '; l++);
                        cptr[k + l] = '\0';
                        m = -1;
                        columnUnits[itemsOnLine].refCount = 1;
                        pplObjNum(columnUnits + itemsOnLine, 0, 1, 0);
                        ppl_unitsStringEvaluate(c, cptr + k, columnUnits + itemsOnLine, &l, &m, errtext);
                        // Warn about a unit which cannot be evaluated, and treat the column as dimensionless
                        if (m >= 0) {
                            pplObjNum(columnUnits + itemsOnLine, 0, 1, 0);
                            if (ppl_errorCountTake(errCount)) { // Only report a limited number of errors
                                ppl_snprintfTrunc(msg, sizeof(msg), "%s:%ld:%d: %s", filename, file_linenumber, i + k,
                                                  errtext);
                                ppl_warning(&c->errcontext, ERR_STACKED, msg);
                                ppl_errorCountCheck(&c->errcontext, *errCount, filename, file_linenumber);
                            }
                        }
                        itemsOnLine++;
                        k += l;
                    }
                }
                // Log the units in debugging mode
                if (DEBUG) {
                    ppl_snprintfTrunc(msg, sizeof(msg), "Total of %ld column units read.", itemsOnLine);
                    ppl_log(&c->errcontext, msg);
                    // Log each unit
                    for (k = 0; k < itemsOnLine; k++) {
                        ppl_snprintfTrunc(msg, sizeof(msg), "Column unit %d: %s", k,
                                          ppl_unitsNumericDisplay(c, columnUnits + k, 0, 0, 0));
                        ppl_log(&c->errcontext, msg);
                    }
                }
            } else if (strncmp(linebuffer + i, "RowUnits:", 9) == 0) // '# RowUnits:' means we have a list of row units
            {
                i += 9;
                // Log progress in debugging mode
                if (DEBUG) {
                    ppl_snprintfTrunc(msg, sizeof(msg), "Reading row units as specified on line %ld of datafile",
                                      file_linenumber);
                    ppl_log(&c->errcontext, msg);
                }
                while ((linebuffer[i] != '\0') && ((unsigned char) linebuffer[i] <= ' ')) i++;
                itemsOnLine = 0;
                hadwhitespace = 1;
                // Count the row units on the line
                for (j = i; linebuffer[j] != '\0'; j++) {
                    // Whitespace separates units
                    if ((unsigned char) linebuffer[j] <= ' ') {
                        hadwhitespace = 1;
                    } else if (hadwhitespace) {
                        itemsOnLine++;
                        hadwhitespace = 0;
                    }
                }
                cptr = (char *) ppl_memAlloc_incontext(j - i + 2, contextRough);
                strncpy(cptr, linebuffer + i, j - i + 2);
                rowUnits = (pplObj *) ppl_memAlloc_incontext(itemsOnLine * sizeof(pplObj), contextRough);
                NrowUnits = itemsOnLine;
                // Failure to allocate memory is reported as an error
                if ((cptr == NULL) || (rowUnits == NULL)) {
                    ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "%s", "Out of memory (08).");
                    *status = 1;
                    if (DEBUG) ppl_log(&c->errcontext, errtext);
                    closeDataInput(filtered_input, readFromCommandLine);
                    return;
                }
                itemsOnLine = 0;
                hadwhitespace = 1;
                // Evaluate each unit in turn
                for (k = 0; cptr[k] != '\0'; k++) {
                    // Skip whitespace between units
                    if ((unsigned char) cptr[k] <= ' ') {
                        continue;
                    } else {
                        for (l = 0; (unsigned char) cptr[k + l] > ' '; l++);
                        cptr[k + l] = '\0';
                        m = -1;
                        rowUnits[itemsOnLine].refCount = 1;
                        pplObjNum(rowUnits + itemsOnLine, 0, 1, 0);
                        ppl_unitsStringEvaluate(c, cptr + k, rowUnits + itemsOnLine, &l, &m, errtext);
                        // Warn about a unit which cannot be evaluated, and treat the row as dimensionless
                        if (m >= 0) {
                            pplObjNum(rowUnits + itemsOnLine, 0, 1, 0);
                            if (ppl_errorCountTake(errCount)) { // Only report a limited number of errors
                                ppl_snprintfTrunc(msg, sizeof(msg), "%s:%ld:%d: %s", filename, file_linenumber, i + k,
                                                  errtext);
                                ppl_warning(&c->errcontext, ERR_STACKED, msg);
                                ppl_errorCountCheck(&c->errcontext, *errCount, filename, file_linenumber);
                            }
                        }
                        itemsOnLine++;
                        k += l;
                    }
                }
                // Log the units in debugging mode
                if (DEBUG) {
                    ppl_snprintfTrunc(msg, sizeof(msg), "Total of %ld row units read.", itemsOnLine);
                    ppl_log(&c->errcontext, msg);
                    // Log each unit
                    for (k = 0; k < itemsOnLine; k++) {
                        ppl_snprintfTrunc(msg, sizeof(msg), "Row unit %d: %s", k,
                                          ppl_unitsNumericDisplay(c, rowUnits + k, 0, 0, 0));
                        ppl_log(&c->errcontext, msg);
                    }
                }
            }
            continue; // Ignore comment lines
        }
        prev_blanklines = 0;

        // If we're in an index we're ignoring, don't do anything with this line
        if ((indexNo >= 0) && (index_number != indexNo)) continue;

        // If we're in a block that we're ignoring, don't do anything with this line
        if ((block_stepcnt != 0) || ((blockfirst >= 0) && (block_count < blockfirst)) ||
            ((blocklast >= 0) && (block_count > blocklast)))
            continue;

        // Fetch this line if linenumber is within range, or if we are using rows and we need everything
        if (usingRowCol == DATAFILE_ROW) // Store the whole line into a raw text spool
        {
            if (discontinuity)
                ppldata_RotateRawData(c, &rawDataTab, *out, usingExprs, labelExpr, selectExpr, continuity, filename,
                                      block_count, index_number, rowHeadings, NrowHeadings, rowUnits, NrowUnits, status,
                                      errtext, errCount, iterDepth);
            // Give up if the stored rows could not be processed
            if (*status) {
                closeDataInput(filtered_input, readFromCommandLine);
                return;
            }
            cptr = rawDataTab->current->text[rawDataTab->current->blockPosition] =
                    (char *) ppl_memAlloc_incontext(strlen(linebuffer) + 1, contextRaw);
            // Failure to allocate memory is reported as an error
            if (cptr == NULL) {
                ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "%s", "Out of memory whilst placing data into text spool.");
                *status = 1;
                if (DEBUG) ppl_log(&c->errcontext, errtext);
                closeDataInput(filtered_input, readFromCommandLine);
                return;
            }
            strcpy(cptr, linebuffer);
            rawDataTab->current->fileLine[rawDataTab->current->blockPosition] = file_linenumber;
            // Failure to allocate memory is reported as an error
            if (ppldata_RawDataTable_AddRow(rawDataTab)) {
                ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "%s", "Out of memory whilst placing data into text spool.");
                *status = 1;
                if (DEBUG) ppl_log(&c->errcontext, errtext);
                closeDataInput(filtered_input, readFromCommandLine);
                return;
            }
            discontinuity = 0;
        } else if ((linenumber_stepcnt == 0) && ((linefirst < 0) || (linenumber_count >= linefirst)) &&
                   ((linelast < 0) || (linenumber_count <= linelast))) {
            // Count the number of data items on this line
            itemsOnLine = 0;
            hadwhitespace = 1;
            hadcomma = 0;
            // Split the line into words at whitespace or commas
            for (i = 0; linebuffer[i] != '\0'; i++) {
                // Whitespace separates words
                if ((unsigned char) linebuffer[i] <= ' ') {
                    hadwhitespace = 1;
                } else if (linebuffer[i] == ',') {
                    int j;
                    for (j = i + 1; (((unsigned char) linebuffer[j] <= ' ') && (linebuffer[j] != '\0')); j++);
                    colData[itemsOnLine++] = linebuffer + j;
                    hadwhitespace = hadcomma = 1;
                } else {
                    if (hadwhitespace && !hadcomma) { colData[itemsOnLine++] = linebuffer + i; }
                    hadwhitespace = hadcomma = 0;
                }
                if (itemsOnLine == MAX_DATACOLS) break; // Don't allow ColumnData array to overflow
            }

            // Add line numbers as first column to one-column datafiles
            if (itemsOnLine > 1) oneColumnInput = 0;
            // One-column data files are plotted against line number
            if ((itemsOnLine == 1) && autoUsingExprs && oneColumnInput) {
                colData[itemsOnLine++] = colData[0];
                colData[0] = lineNumberStr;
                ppl_snprintfTrunc(lineNumberStr, sizeof(lineNumberStr), "%ld", file_linenumber);
            }

            // Apply the using expressions to the line
            ppldata_ApplyUsingList(c, *out, usingExprs, labelExpr, selectExpr, continuity, &discontinuity, colData,
                                   NULL, itemsOnLine, filename, file_linenumber, NULL, linenumber_count, block_count,
                                   index_number, DATAFILE_COL, columnHeadings, NcolumnHeadings, columnUnits,
                                   NcolumnUnits, status, errtext, errCount, iterDepth);
            if (*status) { *status = 0; /* It was just a warning... */ }
        }
        linenumber_count++;
        linenumber_stepcnt = ((linenumber_stepcnt - 1) % linestep);
    }

    // Close input file
    closeDataInput(filtered_input, readFromCommandLine);

    // If we are reading rows, go through all of the data that we've read and rotate it by 90 degrees
    if (usingRowCol == DATAFILE_ROW) {
        ppldata_RotateRawData(c, &rawDataTab, *out, usingExprs, labelExpr, selectExpr, continuity, filename,
                              block_count, index_number, rowHeadings, NrowHeadings, rowUnits, NrowUnits, status,
                              errtext, errCount, iterDepth);
        if (*status) return;
    }

    // If data is to be sorted, sort it now
    if (sortBy != NULL) { *out = ppldata_sort(c, *out, Ncols - 1, continuity == DATAFILE_CONTINUOUS); }

    // Debugging line
    // ppldata_DataTable_List(*out);

    // Delete rough workspace
    ppl_memAlloc_AscendOutOfContext(contextRough);
    return;
}

// Functions are only sampled in parallel if there are at least this many samples to share between threads
#define PARALLEL_MIN_SAMPLES 1000

// The number of samples evaluated in parallel at a time, which bounds the memory needed to store their values
#define PARALLEL_BLOCK_SAMPLES 32768

//! parallelSampler - The state of the parallel evaluation of a list of functions at a raster of points, the first
//! stage of ppldata_fromFuncs(). The functions are evaluated in blocks of samples, on several threads at once. The
//! values are then turned into a data table serially, in order, so that the result (including any error messages) is
//! identical to evaluating the functions serially. Samples at which any function cannot be evaluated are flagged, and
//! evaluated again serially, which reports the error.
typedef struct parallelSampler {
    ppl_context *c; // The Pyxplot context
    pplExpr **fnlist; // The functions to evaluate
    int fnlist_len; // The number of functions
    char *dummyNames[2]; // The names of the dummy variables, e.g. {"x"} or {"x","y"}
    int nDummy; // The number of dummy variables: 1, or 2 when sampling a grid
    const double *rasterX, *rasterY; // The values of the dummy variables
    int rasterXlen; // The number of values of the first dummy variable
    const pplObj *unitX, *unitY; // The physical units of the dummy variables
    long nSamples; // The total number of samples
    int iterDepth; // The depth of recursion, used to prevent infinite recursion
    long blockStart, blockLen; // The range of samples whose values are currently held
    pplObj *results; // The value of each function at each sample in the block, indexed [sample * fnlist_len + f]
    unsigned char *failed; // Flags of the samples in the block at which any function could not be evaluated
} parallelSampler;

//! parallelSamplerInit - Decide whether a list of functions can be evaluated in parallel, and if so, prepare to do so
//! \param [out] s - The state of the parallel evaluation
//! \param [in] c - The Pyxplot context
//! \param [in] fnlist - The functions to evaluate
//! \param [in] fnlist_len - The number of functions in <fnlist>
//! \param [in] dummyNames - The names of the dummy variables
//! \param [in] nDummy - The number of dummy variables: 1, or 2 when sampling a grid
//! \param [in] rasterX - The values of the first dummy variable
//! \param [in] rasterXlen - The number of values in <rasterX>
//! \param [in] unitX - The physical units of the first dummy variable
//! \param [in] rasterY - The values of the second dummy variable, if sampling a grid
//! \param [in] rasterYlen - The number of values in <rasterY>
//! \param [in] unitY - The physical units of the second dummy variable
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion
//! \return True if the functions are to be evaluated in parallel; false if they must be evaluated serially

static int parallelSamplerInit(parallelSampler *s, ppl_context *c, pplExpr **fnlist, const int fnlist_len,
                               char *const *dummyNames, const int nDummy, const double *rasterX, const int rasterXlen,
                               const pplObj *unitX, const double *rasterY, const int rasterYlen, const pplObj *unitY,
                               const int iterDepth) {
    long p;
    int j;
    s->results = NULL;
    s->failed = NULL;
#ifndef _OPENMP
    return 0; // Pyxplot was compiled without OpenMP
#else
    s->c = c;
    s->fnlist = fnlist;
    s->fnlist_len = fnlist_len;
    s->dummyNames[0] = dummyNames[0];
    s->dummyNames[1] = dummyNames[1];
    s->nDummy = nDummy;
    s->rasterX = rasterX;
    s->rasterY = rasterY;
    s->rasterXlen = rasterXlen;
    s->unitX = unitX;
    s->unitY = unitY;
    s->nSamples = (long) rasterXlen * ((nDummy > 1) ? rasterYlen : 1);
    s->iterDepth = iterDepth;
    s->blockStart = s->blockLen = 0;

    // Only large numbers of samples are worth sharing between threads, and only if every function is safe to evaluate
    // in parallel
    if ((s->nSamples < PARALLEL_MIN_SAMPLES) || (omp_get_max_threads() < 2)) return 0;
    for (j = 0; j < fnlist_len; j++)
        if (!ppl_expThreadSafe(c, fnlist[j], (const char *const *) dummyNames, nDummy)) return 0;

    // Allocate space for the values of the functions at a block of samples
    s->results = (pplObj *) malloc(PARALLEL_BLOCK_SAMPLES * fnlist_len * sizeof(pplObj));
    s->failed = (unsigned char *) malloc(PARALLEL_BLOCK_SAMPLES);
    // Failure to allocate memory means the functions are evaluated serially
    if ((s->results == NULL) || (s->failed == NULL)) {
        free(s->results);
        free(s->failed);
        s->results = NULL;
        s->failed = NULL;
        return 0; // Fall back to evaluating the functions serially
    }
    // Initialise each value
    for (p = 0; p < PARALLEL_BLOCK_SAMPLES * fnlist_len; p++) {
        s->results[p].refCount = 1;
        pplObjZom(&s->results[p], 0);
    }
    return 1;
#endif
}

//! discardSample - Free the values of the functions evaluated so far at a sample, and flag the sample as failed, so
//! that it is evaluated again serially
//! \param [in,out] s - The state of the parallel evaluation
//! \param [in] q - The number of the sample within the current block
//! \param [in] nEvaluated - The number of functions which were evaluated at the sample

static void discardSample(parallelSampler *s, const long q, const int nEvaluated) {
    int g;
    // Free the values of the functions already evaluated
    for (g = 0; g < nEvaluated; g++) {
        pplObj *r = &s->results[q * s->fnlist_len + g];
        ppl_garbageObject(r);
        r->refCount = 1;
        pplObjZom(r, 0);
    }
    s->failed[q] = 1;
}

//! parallelSamplerFill - Evaluate the functions at a block of samples, on several threads at once
//! \param [in,out] s - The state of the parallel evaluation
//! \param [in] start - The number of the first sample in the block

static void parallelSamplerFill(parallelSampler *s, const long start) {
    // Evaluate the functions at the samples of this block
    s->blockStart = start;
    s->blockLen = ppl_min(PARALLEL_BLOCK_SAMPLES, s->nSamples - start);
    memset(s->failed, 0, s->blockLen);

#ifdef _OPENMP
#pragma omp parallel
    {
        // Each thread evaluates the functions in a worker context of its own, with its own dummy variables
        ppl_context *w = ppl_contextWorkerNew(s->c);
        pplObj *dummy[2] = {NULL, NULL};
        long q;
        int a, k;
        // Each thread gets its own copies of the dummy variables
        if (w != NULL) {
            // Copy each dummy variable into the thread's own namespace
            for (a = 0; a < s->nDummy; a++) {
                pplObj temp;
                ppl_contextGetVarPointer(w, s->dummyNames[a], &dummy[a], &temp); // Creates the variable
                pplObjCpy(dummy[a], (pplObj *) ((a == 0) ? s->unitX : s->unitY), 0, dummy[a]->amMalloced, 1);
                dummy[a]->imag = 0;
                dummy[a]->flagComplex = 0;
            }
            // Give the worker private copies of the variables the functions read, so that threads do not contend
            // to update their reference counts. If this fails, the worker reads the shared variables instead.
            ppl_contextWorkerPrivatise(w, s->c, s->fnlist, s->fnlist_len, (const char *const *) s->dummyNames,
                                       s->nDummy);
        }

#pragma omp for schedule(dynamic, 256)
        // Share the samples of the block between the threads
        for (q = 0; q < s->blockLen; q++) {
            const long sample = s->blockStart + q;
            int f;
            // Samples which a thread cannot evaluate are left to be evaluated serially
            if ((w == NULL) || cancellationFlag) {
                s->failed[q] = 1; // Evaluated serially later, if at all
                continue;
            }
            dummy[0]->real = s->rasterX[sample % s->rasterXlen];
            if (s->nDummy > 1) dummy[1]->real = s->rasterY[sample / s->rasterXlen];
            // Evaluate each function at this sample
            for (f = 0; f < s->fnlist_len; f++) {
                const int stkLevelOld = w->stackPtr; // Stack: note its level, to discard the value afterwards
                const pplObj *ob = ppl_expEval(w, s->fnlist[f], &k, 0, s->iterDepth);
                // Only plain values may leave the worker; other objects (e.g. functions) could refer to its private
                // copies of shared variables. Values which are errors, or of other types, are evaluated serially.
                if (w->errStat.status ||
                    ((ob->objType != PPLOBJ_NUM) && (ob->objType != PPLOBJ_BOOL) && (ob->objType != PPLOBJ_STR) &&
                     (ob->objType != PPLOBJ_DATE) && (ob->objType != PPLOBJ_COL))) {
                    ppl_stackClean(w, stkLevelOld); // Stack: discard anything left by the evaluation
                    ppl_tbClear(w);
                    discardSample(s, q, f);
                    break;
                }
                pplObjCpy(&s->results[q * s->fnlist_len + f], (pplObj *) ob, 0, 0, 1);
                ppl_stackClean(w, stkLevelOld); // Stack: discard the value, now that it has been copied
            }
        }
        ppl_contextWorkerFree(w);
    }
#endif
}

//! parallelSamplerFree - Free the storage used to evaluate functions in parallel, including any values not used
//! \param [in,out] s - The state of the parallel evaluation

static void parallelSamplerFree(parallelSampler *s) {
    long q;
    if (s->results == NULL) return;
    for (q = 0; q < PARALLEL_BLOCK_SAMPLES * s->fnlist_len; q++) ppl_garbageObject(&s->results[q]);
    free(s->results);
    free(s->failed);
    s->results = NULL;
    s->failed = NULL;
}

//! ppldata_fromFuncs - Sample a list of functions at a raster of points, applying the "using", "select" and "sort"
//! modifiers, into a data table
//! \param [in] c - The Pyxplot context
//! \param [out] out - Set to point to the output data table
//! \param [in] fnlist - The list of functions (expressions) to sample
//! \param [in] fnlist_len - The number of functions in <fnlist>
//! \param [in] rasterX - The values of the first dummy variable at which to sample the functions
//! \param [in] rasterXlen - The number of values in <rasterX>
//! \param [in] parametric - If true, the functions are parametric functions of t (or of u and v, if <rasterY> is
//! given); otherwise they are functions of x (or x and y)
//! \param [in] unitX - The physical units of the first dummy variable
//! \param [in] rasterY - The values of the second dummy variable at which to sample the functions, or NULL for
//! functions of one variable
//! \param [in] rasterYlen - The number of values in <rasterY>
//! \param [in] unitY - The physical units of the second dummy variable
//! \param [in] usingExprs - Array of the expressions in the "using" modifier, one per output column
//! \param [in] autoUsingExprs - If true, the "using" expressions were not supplied by the user, but generated
//! automatically
//! \param [in] Ncols - The number of output columns
//! \param [in] NusingObjs - The number of output columns which contain objects other than numbers, e.g. strings for
//! labels
//! \param [in] labelExpr - An expression to evaluate to give a text label for each data point, or NULL
//! \param [in] selectExpr - An expression to evaluate for each data point, which must be true for the data point to be
//! included, or NULL
//! \param [in] sortBy - An expression by which to sort the data points, or NULL to leave them unsorted
//! \param [in] continuity - DATAFILE_CONTINUOUS if lines should be drawn between data points in different blocks, or
//! DATAFILE_DISCONTINUOUS if each block should be drawn separately
//! \param [out] status - Set to a non-zero value if an error occurred
//! \param [out] errtext - Character buffer into which to write an error message, if an error occurred
//! \param [in|out] errCount - The number of errors reported so far; once this exceeds a limit, further errors are
//! suppressed
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppldata_fromFuncs(ppl_context *c, dataTable **out, pplExpr **fnlist, const int fnlist_len, const double *rasterX,
                       const int rasterXlen, int parametric, pplObj *unitX, const double *rasterY, const int rasterYlen,
                       pplObj *unitY, pplExpr **usingExprs, int autoUsingExprs, int Ncols, int NusingObjs,
                       pplExpr *labelExpr, pplExpr *selectExpr, pplExpr *sortBy, const int continuity, int *status,
                       char *errtext, int *errCount, const int iterDepth) {
    char msg[PPL_MSG_LENGTH];
    int discontinuity2 = 0;
    const int sampleGrid = (rasterY != NULL);
    int a, j, k, contextOutput;
    long expectedNrows;
    char buffer[FNAME_LENGTH];
    pplObj colData[USING_ITEMS_MAX + 2];
    pplObj *ordinateVar[2], dummyTemp[2];
    parallelSampler parallel; // The state of the evaluation of the functions on several threads at once
    int inParallel = 0; // True if the functions are being evaluated in parallel

    // Init
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Evaluated supplied set of functions.");
        ppl_log(&c->errcontext, msg);
    }
    // There is a fixed limit on the number of functions
    if (fnlist_len > USING_ITEMS_MAX) {
        ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "Too many functions supplied. A maximum of %d are allowed.",
                          USING_ITEMS_MAX);
        *status = 1;
        if (DEBUG) ppl_log(&c->errcontext, errtext);
        return;
    }
    // Unless the number of columns is given, assume two
    if (Ncols < 0) {
        // If number of columns unspecified, assume two
        autoUsingExprs = 1;
        if ((*status = ppldata_autoUsingList(
                     c, usingExprs, Ncols = fnlist_len + (!parametric) + ((!parametric) && sampleGrid), errtext)))
            return;
    }
    // Initialise the values of the functions
    for (a = 0; a < USING_ITEMS_MAX + 2; a++) {
        colData[a].refCount = 1;
        colData[a].objType = PPLOBJ_ZOM;
        colData[a].amMalloced = 0;
    }

    // If sortBy is not null, add it to using list
    if (sortBy != NULL) {
        usingExprs[Ncols] = sortBy;
        NusingObjs++;
        Ncols++;
    }

    // Keep a record of the memory context we're going to output into
    contextOutput = ppl_memAlloc_GetMemContext();
    *out = ppldata_NewDataTable(Ncols - NusingObjs, NusingObjs, contextOutput,
                                sampleGrid ? (rasterXlen * rasterYlen) : rasterXlen);
    // Failure to allocate memory is reported as an error
    if (*out == NULL) {
        ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "%s",
                          "Out of memory whilst trying to allocate data table to read data from file.");
        *status = 1;
        if (DEBUG) ppl_log(&c->errcontext, errtext);
        return;
    }

    // Loop over ordinate values
    {
        int i, i2;
        const int ilen = rasterXlen, i2len = sampleGrid ? rasterYlen : 1;
        int offset = 1 + (!parametric) + ((!parametric) && sampleGrid);
        long p = 0;

        // If possible, the functions are evaluated in blocks of samples on several threads at once. The values are
        // turned into a data table below, in order, exactly as if they had been evaluated there.
        {
            char *dummyNames[2];
            dummyNames[0] = parametric ? (sampleGrid ? "u" : "t") : "x";
            dummyNames[1] = parametric ? "v" : "y";
            inParallel = parallelSamplerInit(&parallel, c, fnlist, fnlist_len, dummyNames, sampleGrid ? 2 : 1, rasterX,
                                             rasterXlen, unitX, rasterY, rasterYlen, unitY, iterDepth);
        }

        // Sample each point of the raster, or of the grid
        for (i2 = 0; i2 < i2len; i2++, discontinuity2 = 1)
            // Each point along x
            for (i = 0; i < ilen; i++, p++) {
                if (cancellationFlag) goto CANCEL;

                // Get a pointer to the value of the variable 'x' in the user's variable space
                ordinateVar[1] = NULL;
                // Set the dummy variable, or variables, to the position of this sample
                for (a = 0; a <= sampleGrid; a++) {
                    int am, rc;
                    char *v, *v2;
                    pplObj **ru;
                    // The first dummy variable is x, or t or u for parametric plots; the second is y, or v
                    if (!a) {
                        v = "x";
                        v2 = sampleGrid ? "u" : "t";
                        ru = &unitX;
                    } else {
                        v = "y";
                        v2 = "v";
                        ru = &unitY;
                    }
                    ppl_contextGetVarPointer(c, parametric ? v2 : v, &ordinateVar[a], &dummyTemp[a]);
                    am = ordinateVar[a]->amMalloced;
                    rc = ordinateVar[a]->refCount;
                    ordinateVar[a]->amMalloced = 0;
                    ppl_garbageObject(ordinateVar[a]);
                    *(ordinateVar[a]) = **ru;
                    ordinateVar[a]->amMalloced = am;
                    ordinateVar[a]->refCount = rc;
                    ordinateVar[a]->self_lval = NULL;
                    ordinateVar[a]->self_this = NULL;
                    ordinateVar[a]->imag = 0.0;
                    ordinateVar[a]->flagComplex = 0;
                }

                // Set the dummy variables to this sample's position
                ordinateVar[0]->real = rasterX[i];
                if (sampleGrid) ordinateVar[1]->real = rasterY[i2];
                if (sampleGrid)
                    ppl_snprintfTrunc(buffer, sizeof(buffer), "%c=%s; %c=%s", (parametric ? 'u' : 'x'),
                                      ppl_unitsNumericDisplay(c, ordinateVar[0], 0, 0, 0), (parametric ? 'v' : 'y'),
                                      ppl_unitsNumericDisplay(c, ordinateVar[1], 1, 0, 0));
                else
                    ppl_snprintfTrunc(buffer, sizeof(buffer), "%c=%s", (parametric ? 't' : 'x'),
                                      ppl_unitsNumericDisplay(c, ordinateVar[0], 0, 0, 0));
                pplObjNum(colData + 0, 0, p, 0);
                if (!parametric) colData[1] = *ordinateVar[0];
                if ((!parametric) && sampleGrid) colData[2] = *ordinateVar[1];
                if (inParallel && (p >= parallel.blockStart + parallel.blockLen))
                    parallelSamplerFill(&parallel, p); // Evaluate the functions at the next block of samples
                // Use values already evaluated in parallel, if there are any; otherwise evaluate the functions now
                if (inParallel && (!parallel.failed[p - parallel.blockStart])) {
                    // The functions have already been evaluated in parallel: take ownership of their values
                    pplObj *r = &parallel.results[(p - parallel.blockStart) * fnlist_len];
                    // Take each value
                    for (j = 0; j < fnlist_len; j++) {
                        memcpy(&colData[j + offset], &r[j], sizeof(pplObj));
                        pplObjZom(&r[j], 0);
                    }
                } else
                    // Evaluate each function
                    for (j = 0; j < fnlist_len; j++) {
                        const int stkLevelOld = c->stackPtr;
                        pplObj *ob;
                        ob = ppl_expEval(c, fnlist[j], &k, 0, iterDepth);
                        // Report a failure to evaluate the function, and skip this sample
                        if (c->errStat.status) {
                            int errp = c->errStat.errPosExpr;
                            char *errt = NULL;
                            if (errp < 0) errp = 0;
                            if (c->errStat.errMsgExpr[0] != '\0') errt = c->errStat.errMsgExpr;
                            else if (c->errStat.errMsgCmd[0] != '\0') errt = c->errStat.errMsgCmd;
                            else errt = "Fail occurred.";
                            if (ppl_errorCountTake(errCount)) { // Only report a limited number of errors
                                ppl_snprintfTrunc(
                                        msg, sizeof(msg),
                                        "%s: Could not evaluate expression <%s>. The error, encountered at character position %d, was: '%s'",
                                        buffer, fnlist[j]->ascii, errp, errt);
                                ppl_error(&c->errcontext, ERR_NUMERICAL, -1, -1, msg);
                                ppl_errorCountCheck(&c->errcontext, *errCount, buffer, 0);
                            }
                            ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack before giving up
                            ppl_tbClear(c);
                            *status = 1;
                            break;
                        }
                        pplObjCpy(&colData[j + offset], ob, 0, 0, 1);
                        ppl_stackClean(c, stkLevelOld); // Discard the expression's value, now that it has been copied
                    }
                expectedNrows = (*out)->Nrows + 1;
                // Apply the using expressions to the values
                if (!(*status)) {
                    ppldata_ApplyUsingList(c, *out, usingExprs, labelExpr, selectExpr, continuity, &discontinuity2,
                                           NULL, colData, fnlist_len + offset - 1, buffer, 0, NULL, i, 0, 0,
                                           DATAFILE_COL, NULL, 0, NULL, 0, status, errtext, errCount, iterDepth);
                } else {
                    if (!sampleGrid) discontinuity2 = 1;
                }
                if (sampleGrid &&
                    ((*out)->Nrows < expectedNrows)) // If sampling a grid, MUST have entry for every point
                {
                    int i;
                    for (i = 0; i < (*out)->Ncolumns_real; i++)
                        (*out)->current->data_real[i + (*out)->current->blockPosition * (*out)->Ncolumns_real] =
                                GSL_NAN;
                    for (i = 0; i < (*out)->Ncolumns_obj; i++)
                        pplObjNum(&(*out)->current->data_obj[i + (*out)->current->blockPosition * (*out)->Ncolumns_obj],
                                  0, GSL_NAN, 0);
                    (*out)->current->text[(*out)->current->blockPosition] = NULL;
                    ppldata_DataTable_AddRow(*out);
                }
                for (j = 0; j < fnlist_len; j++) ppl_garbageObject(&colData[j + offset]);
                *status = 0;

                // Reset the variable x (and maybe y or t) to its old value
                ppl_contextRestoreVarPointer(c, parametric ? (sampleGrid ? "u" : "t") : "x", &dummyTemp[0]);
                if (ordinateVar[1] != NULL) ppl_contextRestoreVarPointer(c, parametric ? ("v") : "y", &dummyTemp[1]);
            }
    }

// If the user pressed control-C, free any values which were evaluated in parallel but not used
CANCEL:
    // Free the values of any functions evaluated in parallel which were not used (e.g. if the user pressed Ctrl-C)
    if (inParallel) parallelSamplerFree(&parallel);

    // If data is to be sorted, sort it now
    if ((!sampleGrid) && (sortBy != NULL)) {
        *out = ppldata_sort(c, *out, Ncols - 1, continuity == DATAFILE_CONTINUOUS);
    }

    // Debugging line
    // ppldata_DataTable_List(*out);
    return;
}

//! ppldata_fromVectors - Read data from a list of vectors, one per column, applying the "using", "select" and "sort"
//! modifiers, into a data table
//! \param [in] c - The Pyxplot context
//! \param [out] out - Set to point to the output data table
//! \param [in] objList - Array of the vectors
//! \param [in] objListLen - The number of vectors in <objList>
//! \param [in] usingExprs - Array of the expressions in the "using" modifier, one per output column
//! \param [in] autoUsingExprs - If true, the "using" expressions were not supplied by the user, but generated
//! automatically
//! \param [in] Ncols - The number of output columns
//! \param [in] NusingObjs - The number of output columns which contain objects other than numbers, e.g. strings for
//! labels
//! \param [in] labelExpr - An expression to evaluate to give a text label for each data point, or NULL
//! \param [in] selectExpr - An expression to evaluate for each data point, which must be true for the data point to be
//! included, or NULL
//! \param [in] sortBy - An expression by which to sort the data points, or NULL to leave them unsorted
//! \param [in] continuity - DATAFILE_CONTINUOUS if lines should be drawn between data points in different blocks, or
//! DATAFILE_DISCONTINUOUS if each block should be drawn separately
//! \param [out] status - Set to a non-zero value if an error occurred
//! \param [out] errtext - Character buffer into which to write an error message, if an error occurred
//! \param [in|out] errCount - The number of errors reported so far; once this exceeds a limit, further errors are
//! suppressed
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppldata_fromVectors(ppl_context *c, dataTable **out, pplObj *objList, const int objListLen, pplExpr **usingExprs,
                         int autoUsingExprs, int Ncols, int NusingObjs, pplExpr *labelExpr, pplExpr *selectExpr,
                         pplExpr *sortBy, const int continuity, int *status, char *errtext, int *errCount,
                         const int iterDepth) {
    char msg[PPL_MSG_LENGTH];
    int contextOutput, discontinuity = 0, i, j;
    const int vlen = ((pplVector *) objList[0].auxil)->v->size;
    pplObj colData[USING_ITEMS_MAX + 2];
    gsl_vector *v[USING_ITEMS_MAX + 2];

    // Init
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Evaluated supplied set of functions.");
        ppl_log(&c->errcontext, msg);
    }
    // There is a fixed limit on the number of vectors
    if (objListLen > USING_ITEMS_MAX) {
        ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "Too many functions supplied. A maximum of %d are allowed.",
                          USING_ITEMS_MAX);
        *status = 1;
        if (DEBUG) ppl_log(&c->errcontext, errtext);
        return;
    }
    // Unless the number of columns is given, assume two
    if (Ncols < 0) {
        // If number of columns unspecified, assume two
        autoUsingExprs = 1;
        if ((*status = ppldata_autoUsingList(c, usingExprs, Ncols = objListLen, errtext))) return;
    }
    for (i = 0; i < objListLen; i++) v[i] = ((pplVector *) objList[i].auxil)->v;
    for (i = 0; i < objListLen; i++) pplObjNum(colData + i + 1, 0, 0, 0);
    for (i = 0; i < objListLen; i++) colData[i + 1].refCount = 1;
    for (i = 0; i < objListLen; i++) ppl_unitsDimCpy(colData + i + 1, objList + i);

    // If sortBy is not null, add it to using list
    if (sortBy != NULL) {
        usingExprs[Ncols] = sortBy;
        NusingObjs++;
        Ncols++;
    }

    // Keep a record of the memory context we're going to output into
    contextOutput = ppl_memAlloc_GetMemContext();
    *out = ppldata_NewDataTable(Ncols - NusingObjs, NusingObjs, contextOutput, vlen);
    if (*out == NULL) {
        ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "%s",
                          "Out of memory whilst trying to allocate data table to read data from file.");
        *status = 1;
        if (DEBUG) ppl_log(&c->errcontext, errtext);
        return;
    }

    // Loop over data
    for (i = 0; i < vlen; i++) {
        char *buffer = "vector data";
        if (cancellationFlag) break;
        pplObjNum(colData + 0, 0, i, 0);
        for (j = 0; j < objListLen; j++) colData[j + 1].real = gsl_vector_get(v[j], i);
        ppldata_ApplyUsingList(c, *out, usingExprs, labelExpr, selectExpr, continuity, &discontinuity, NULL, colData,
                               objListLen, buffer, 0, NULL, i, 0, 0, DATAFILE_COL, NULL, 0, NULL, 0, status, errtext,
                               errCount, iterDepth);
    }

    // If data is to be sorted, sort it now
    if (sortBy != NULL) { *out = ppldata_sort(c, *out, Ncols - 1, continuity == DATAFILE_CONTINUOUS); }

    // Debugging line
    // ppldata_DataTable_List(*out);
    return;
}

//! ppldata_fromCmd - Read the data source specified in a command such as plot, fit or tabulate (a data file, a list of
//! functions or a list of vectors), together with its modifiers, into a data table
//! \param [in] c - The Pyxplot context
//! \param [out] out - Set to point to the output data table
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command
//! \param [in] wildcardMatchNumber - For filenames containing wildcards, the number of the matching file to read,
//! counting from zero
//! \param [out] filenameOut - Character buffer into which to write the filename which was actually read, after wildcard
//! expansion (optional)
//! \param [in|out] dataSpool - Lines of data supplied inline after the command, if the filename is "--"
//! \param [in] ptab - Table mapping parser variable indices onto positions in the parser output stack
//! \param [in] stkbase - Offset to add to positions in the parser output stack
//! \param [in] Ncols - The number of output columns expected
//! \param [in] NusingObjs - The number of output columns which contain objects other than numbers
//! \param [in] min - Array of the lower limits of the ranges specified for each column
//! \param [in] minSet - Array of flags indicating which lower limits are set
//! \param [in] max - Array of the upper limits of the ranges specified for each column
//! \param [in] maxSet - Array of flags indicating which upper limits are set
//! \param [in] unitRange - Array of the physical units of the ranges specified for each column
//! \param [in] persistent - If true, allocate the output data table in the lowest ppl_memAlloc context, so that it
//! persists after the current command; otherwise allocate it in the current context
//! \param [out] status - Set to a non-zero value if an error occurred
//! \param [out] errtext - Character buffer into which to write an error message, if an error occurred
//! \param [in|out] errCount - The number of errors reported so far; once this exceeds a limit, further errors are
//! suppressed
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppldata_fromCmd(ppl_context *c, dataTable **out, const parserLine *pl, const parserOutput *in,
                     const int wildcardMatchNumber, char *filenameOut, parserLine **dataSpool, const int *ptab,
                     const int stkbase, int Ncols, const int NusingObjs, const double *min, const int *minSet,
                     const double *max, const int *maxSet, pplObj *unitRange, const int persistent, int *status,
                     char *errtext, int *errCount, const int iterDepth) {
    char msg[PPL_MSG_LENGTH];
    pplObj *stk = in->stk;
    pplObj *stko = stk + stkbase;
    pplExpr *usingExprs[USING_ITEMS_MAX], *selectExpr = NULL, *labelExpr = NULL, *sortBy = NULL;
    long everyList[6] = {1, 1, -1, -1, -1, -1};
    int Nusing = 0, autoUsingList = 0, continuity = DATAFILE_CONTINUOUS, usingRowCol = DATAFILE_COL, indexNo = -1;
    int parametric = 0, sampleGrid = 0, spacingSet = 0, rasterXlen = 0, rasterYlen = 0;
    int XminSet = 0, XmaxSet = 0;
    double *rasterX = NULL, *rasterY = NULL;
    pplObj unitX, unitY;
    double rasterXmin = 0, rasterXmax = 0, rasterYmin = 0, rasterYmax = 0;
    pplObj rasterXunits, rasterYunits;

    *status = 0;
    *out = NULL;
    if (filenameOut != NULL) filenameOut[0] = '\0';

    // Test if parametric
    {
        const int pos1 = ptab[PARSE_INDEX_parametric];
        const int pos2 = ptab[PARSE_INDEX_vmin];
        const int pos3 = ptab[PARSE_INDEX_tmin];
        const int gotV = (pos2 > 0) && (stko[pos2].objType != PPLOBJ_ZOM);
        const int gotT = (pos3 > 0) && (stko[pos3].objType != PPLOBJ_ZOM);
        if ((pos1 > 0) && (stko[pos1].objType == PPLOBJ_STR)) parametric = 1;
        // A parametric plot samples a (u,v) grid if a v range is given, or if no ranges are given and the most recent
        // of set trange and set urange/vrange was the latter
        if (parametric && (gotV || ((!gotT) && (!c->set->graph_current.USE_T_or_uv)))) sampleGrid = 1;
    }

    // Devise range for functions
    {
        const int poss = ptab[PARSE_INDEX_spacing];
        const int post1 = ptab[PARSE_INDEX_tmin];
        const int post2 = ptab[PARSE_INDEX_tmax];
        const int posv1 = ptab[PARSE_INDEX_vmin];
        const int posv2 = ptab[PARSE_INDEX_vmax];
        int rasterXlog = 0, rasterYlog = 0;
        // The raster of x values is taken from the plot command's range, or from the x1 axis, or from defaults;
        // parametric plots use the range of t, or of u and v
        if (!parametric) {
            pplset_axis *X = &c->set->XAxes[1];
            rasterXlog = (X->log == SW_BOOL_TRUE); // Read from axis x1
            // The lower limit from the command, or from the axis; otherwise a default relative to the upper limit
            if (minSet[0]) {
                rasterXmin = min[0];
                rasterXunits = unitRange[0];
                XminSet = 1;
            } else if (X->MinSet == SW_BOOL_TRUE) {
                rasterXmin = X->min;
                rasterXunits = X->unit;
                XminSet = 1;
            } else if (maxSet[0]) {
                rasterXmin = rasterXlog ? (max[0] / 100) : (max[0] - 20);
                rasterXunits = unitRange[0];
            } else if (X->MaxSet == SW_BOOL_TRUE) {
                rasterXmin = rasterXlog ? (X->max / 100) : (X->max - 20);
                rasterXunits = X->unit;
            } else {
                rasterXmin = rasterXlog ? 1.0 : -10.0;
                pplObjNum(&rasterXunits, 0, 0, 0);
            }
            // The upper limit from the command, or from the axis, if it has the same units; otherwise a default
            // relative to the lower limit
            if ((maxSet[0]) && (ppl_unitsDimEqual(&rasterXunits, &unitRange[0]))) {
                rasterXmax = max[0];
                XmaxSet = 1;
            } else if ((X->MaxSet == SW_BOOL_TRUE) && (ppl_unitsDimEqual(&rasterXunits, &(X->unit)))) {
                rasterXmax = X->max;
                XmaxSet = 1;
            } else {
                rasterXmax = rasterXlog ? (rasterXmin * 100) : (rasterXmin + 20);
            }
        } else if (!sampleGrid) {
            XminSet = 1;
            XmaxSet = 1;
            rasterXlog = (c->set->graph_current.Tlog == SW_BOOL_TRUE);
            rasterXmin = c->set->graph_current.Tmin.real;
            rasterXmax = c->set->graph_current.Tmax.real;
            rasterXunits = c->set->graph_current.Tmin;
        } else {
            rasterXlog = (c->set->graph_current.Ulog == SW_BOOL_TRUE);
            rasterXmin = c->set->graph_current.Umin.real;
            rasterXmax = c->set->graph_current.Umax.real;
            rasterXunits = c->set->graph_current.Umin;
            rasterYlog = (c->set->graph_current.Vlog == SW_BOOL_TRUE);
            rasterYmin = c->set->graph_current.Vmin.real;
            rasterYmax = c->set->graph_current.Vmax.real;
            rasterYunits = c->set->graph_current.Vmin;
        }

        // A range of t, or u, given in the command overrides the default
        if ((post1 > 0) && (stko[post1].objType != PPLOBJ_ZOM)) {
            rasterXmin = stko[post1].real;
            // A lower limit alone must have the same type and units as the default range
            if ((post2 <= 0) || (stko[post2].objType == PPLOBJ_ZOM)) {
                // The types must match
                if (stko[post1].objType != rasterXunits.objType) {
                    *status = 1;
                    ppl_snprintfTrunc(
                            errtext, PPL_MSG_LENGTH,
                            "Mismatched data types between lower and upper limits of first parametric variable.");
                    if (DEBUG) ppl_log(&c->errcontext, errtext);
                    return;
                }
                // The units must match
                if (!ppl_unitsDimEqual(&stko[post1], &rasterXunits)) {
                    *status = 1;
                    ppl_snprintfTrunc(
                            errtext, PPL_MSG_LENGTH,
                            "Mismatched physical units between lower and upper limits of first parametric variable.");
                    if (DEBUG) ppl_log(&c->errcontext, errtext);
                    return;
                }
            }
            rasterXunits = stko[post1];
        }
        // An upper limit given in the command
        if ((post2 > 0) && (stko[post2].objType != PPLOBJ_ZOM)) {
            rasterXmax = stko[post2].real;
            // The types must match
            if (stko[post2].objType != rasterXunits.objType) {
                *status = 1;
                ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH,
                                  "Mismatched data types between lower and upper limits of first parametric variable.");
                if (DEBUG) ppl_log(&c->errcontext, errtext);
                return;
            }
            // The units must match
            if (!ppl_unitsDimEqual(&stko[post2], &rasterXunits)) {
                *status = 1;
                ppl_snprintfTrunc(
                        errtext, PPL_MSG_LENGTH,
                        "Mismatched physical units between lower and upper limits of first parametric variable.");
                if (DEBUG) ppl_log(&c->errcontext, errtext);
                return;
            }
        }
        // A range of v given in the command overrides the default
        if ((posv1 > 0) && (stko[posv1].objType != PPLOBJ_ZOM)) {
            rasterYmin = stko[posv1].real;
            // A lower limit alone must have the same type and units as the default range
            if ((posv2 <= 0) || (stko[posv2].objType == PPLOBJ_ZOM)) {
                // The types must match
                if (stko[posv1].objType != rasterYunits.objType) {
                    *status = 1;
                    ppl_snprintfTrunc(
                            errtext, PPL_MSG_LENGTH,
                            "Mismatched data types between lower and upper limits of second parametric variable.");
                    if (DEBUG) ppl_log(&c->errcontext, errtext);
                    return;
                }
                // The units must match
                if (!ppl_unitsDimEqual(&stko[posv1], &rasterYunits)) {
                    *status = 1;
                    ppl_snprintfTrunc(
                            errtext, PPL_MSG_LENGTH,
                            "Mismatched physical units between lower and upper limits of second parametric variable.");
                    if (DEBUG) ppl_log(&c->errcontext, errtext);
                    return;
                }
            }
            rasterYunits = stko[posv1];
        }
        // An upper limit given in the command
        if ((posv2 > 0) && (stko[posv2].objType != PPLOBJ_ZOM)) {
            rasterYmax = stko[posv2].real;
            // The types must match
            if (stko[posv2].objType != rasterYunits.objType) {
                *status = 1;
                ppl_snprintfTrunc(
                        errtext, PPL_MSG_LENGTH,
                        "Mismatched data types between lower and upper limits of second parametric variable.");
                if (DEBUG) ppl_log(&c->errcontext, errtext);
                return;
            }
            // The units must match
            if (!ppl_unitsDimEqual(&stko[posv2], &rasterYunits)) {
                *status = 1;
                ppl_snprintfTrunc(
                        errtext, PPL_MSG_LENGTH,
                        "Mismatched physical units between lower and upper limits of second parametric variable.");
                if (DEBUG) ppl_log(&c->errcontext, errtext);
                return;
            }
        }

        // Warn about logarithmic rasters with limits which are not positive
        if (rasterXlog && ((rasterXmin <= 0) || (rasterXmax <= 0)))
            ppl_warning(
                    &c->errcontext, ERR_NUMERICAL,
                    "Attempt to tabulate data using a logarithmic ordinate axis with negative or zero limits set. Reverting limits to finite positive values with well-defined logarithms.");
        if (sampleGrid && rasterYlog && ((rasterYmin <= 0) || (rasterYmax <= 0)))
            ppl_warning(
                    &c->errcontext, ERR_NUMERICAL,
                    "Attempt to tabulate data using a logarithmic ordinate axis with negative or zero limits set. Reverting limits to finite positive values with well-defined logarithms.");

        // See if spacing has been specified
        if ((poss > 0) && (stko[poss].objType != PPLOBJ_ZOM)) {
            int i;
            pplObj *spacing = &stko[poss];
            spacingSet = 1;
            // Work out the number of samples from the spacing, for each raster
            for (i = 0; i <= sampleGrid; i++) {
                double SpacingDbl, NumberOfSamplesDbl;
                int *rast_log = i ? &rasterYlog : &rasterXlog;
                double *rast_min = i ? &rasterYmin : &rasterXmin;
                double *rast_max = i ? &rasterYmax : &rasterXmax;
                pplObj *rast_unit = i ? &rasterYunits : &rasterXunits;
                int *out = i ? &rasterYlen : &rasterXlen;
                // On logarithmic rasters, the spacing is a factor
                if (*rast_log) {
                    // The factor must be dimensionless
                    if (!spacing->dimensionless) {
                        *status = 1;
                        ppl_snprintfTrunc(
                                errtext, PPL_MSG_LENGTH,
                                "Specified spacing has units of <%s>. However, for a logarithmic ordinate axis, the spacing should be a dimensionless multiplicative factor.",
                                ppl_printUnit(c, spacing, NULL, NULL, 0, 1, 0));
                        if (DEBUG) ppl_log(&c->errcontext, errtext);
                        return;
                    }
                    // The factor must be reasonable
                    if ((spacing->real < 1e-200) || (spacing->real > 1e100)) {
                        *status = 1;
                        ppl_snprintfTrunc(
                                errtext, PPL_MSG_LENGTH,
                                "The spacing specified must be a positive multiplicative factor for logarithmic ordinate axes.");
                        if (DEBUG) ppl_log(&c->errcontext, errtext);
                        return;
                    }
                    SpacingDbl = spacing->real;
                    if ((*rast_max > *rast_min) && (SpacingDbl < 1.0)) SpacingDbl = 1.0 / SpacingDbl;
                    if ((*rast_min > *rast_max) && (SpacingDbl > 1.0)) SpacingDbl = 1.0 / SpacingDbl;
                    NumberOfSamplesDbl = 1.0 + floor(log(*rast_max / *rast_min) / log(SpacingDbl));
                    // There must be at least two samples
                    if (NumberOfSamplesDbl < 2) {
                        *status = 1;
                        ppl_snprintfTrunc(
                                errtext, PPL_MSG_LENGTH,
                                "The spacing specified produced fewer than two samples; this does not seem sensible.");
                        if (DEBUG) ppl_log(&c->errcontext, errtext);
                        return;
                    }
                    // There may be no more than 1e7 samples
                    if (NumberOfSamplesDbl > 1e7) {
                        *status = 1;
                        ppl_snprintfTrunc(
                                errtext, PPL_MSG_LENGTH,
                                "The spacing specified produced more than 1e7 samples. If you really want to do this, use 'set samples'.");
                        if (DEBUG) ppl_log(&c->errcontext, errtext);
                        return;
                    }
                    *rast_max = *rast_min * pow(SpacingDbl, NumberOfSamplesDbl - 1);
                    *out = (int) NumberOfSamplesDbl;
                } else {
                    // The spacing must have the same units as the raster
                    if (!ppl_unitsDimEqual(rast_unit, spacing)) {
                        *status = 1;
                        ppl_snprintfTrunc(
                                errtext, PPL_MSG_LENGTH,
                                "Specified spacing has units of <%s>, for an ordinate axis which has units of <%s>,",
                                ppl_printUnit(c, spacing, NULL, NULL, 0, 1, 0),
                                ppl_printUnit(c, rast_unit, NULL, NULL, 1, 1, 0));
                        if (DEBUG) ppl_log(&c->errcontext, errtext);
                        return;
                    }
                    SpacingDbl = spacing->real;
                    if ((*rast_max > *rast_min) && (SpacingDbl < 0.0)) SpacingDbl = -SpacingDbl;
                    if ((*rast_min > *rast_max) && (SpacingDbl > 0.0)) SpacingDbl = -SpacingDbl;
                    NumberOfSamplesDbl = 1.0 + (*rast_max - *rast_min) / SpacingDbl;
                    // There must be at least two samples
                    if (NumberOfSamplesDbl < 2) {
                        *status = 1;
                        ppl_snprintfTrunc(
                                errtext, PPL_MSG_LENGTH,
                                "The spacing specified produced fewer than two samples; this does not seem sensible.");
                        if (DEBUG) ppl_log(&c->errcontext, errtext);
                        return;
                    }
                    // There may be no more than 1e6 samples
                    if (NumberOfSamplesDbl > 1e6) {
                        *status = 1;
                        ppl_snprintfTrunc(
                                errtext, PPL_MSG_LENGTH,
                                "The spacing specified produced more than 1e6 samples. If you really want to do this, use 'set samples'.");
                        if (DEBUG) ppl_log(&c->errcontext, errtext);
                        return;
                    }
                    *rast_max = *rast_min + SpacingDbl * (NumberOfSamplesDbl + 1);
                    *out = (int) NumberOfSamplesDbl;
                }
            }
        } else {
            rasterXlen = c->set->graph_current.samples;
            // Without a spacing, the number of samples is set with "set samples"
            if (sampleGrid) {
                rasterXlen = (c->set->graph_current.SamplesXAuto == SW_BOOL_TRUE) ? c->set->graph_current.samples
                                                                                  : c->set->graph_current.SamplesX;
                rasterYlen = (c->set->graph_current.SamplesYAuto == SW_BOOL_TRUE) ? c->set->graph_current.samples
                                                                                  : c->set->graph_current.SamplesY;
            }
        }

        // Make raster
        unitX = rasterXunits;
        unitX.refCount = 1;
        unitY = rasterYunits;
        unitY.refCount = 1;
        rasterX = (double *) ppl_memAlloc(rasterXlen * sizeof(double));
        // Failure to allocate memory is reported as an error
        if (rasterX == NULL) {
            *status = 1;
            ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "Out of memory (09).");
            if (DEBUG) ppl_log(&c->errcontext, errtext);
            return;
        }
        if (!rasterXlog) ppl_linRaster(rasterX, rasterXmin, rasterXmax, rasterXlen);
        else ppl_logRaster(rasterX, rasterXmin, rasterXmax, rasterXlen);

        // Make the raster of y values, when sampling a grid
        if (sampleGrid) {
            rasterY = (double *) ppl_memAlloc(rasterYlen * sizeof(double));
            // Failure to allocate memory is reported as an error
            if (rasterY == NULL) {
                *status = 1;
                ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "Out of memory (10).");
                if (DEBUG) ppl_log(&c->errcontext, errtext);
                return;
            }
            if (!rasterYlog) ppl_linRaster(rasterY, rasterYmin, rasterYmax, rasterYlen);
            else ppl_logRaster(rasterY, rasterYmin, rasterYmax, rasterYlen);
        }
    }

    // Read index number
    {
        const int pos = ptab[PARSE_INDEX_index];
        if ((pos > 0) && (stko[pos].objType == PPLOBJ_NUM)) indexNo = (int) round(stko[pos].real);
    }

    // Read data spool (on first pass only)
    if ((dataSpool != NULL) && (*dataSpool == NULL)) {
        const int pos = ptab[PARSE_INDEX_data];
        if ((pos > 0) && (stk[pos].objType == PPLOBJ_BYT)) *dataSpool = (parserLine *) stk[pos].auxil;
        if (*dataSpool != NULL) *dataSpool = (*dataSpool)->next; // first line is command line
    }

    // Read select criterion
    {
        const int pos = ptab[PARSE_INDEX_select_criterion];
        if ((pos > 0) && (stko[pos].objType == PPLOBJ_EXP)) selectExpr = (pplExpr *) stko[pos].auxil;
    }

    // Read sort criterion
    {
        const int pos = ptab[PARSE_INDEX_sort_expression];
        if ((pos > 0) && (stko[pos].objType == PPLOBJ_EXP)) sortBy = (pplExpr *) stko[pos].auxil;
    }

    // Read select / sort continuity
    {
        const int pos1 = ptab[PARSE_INDEX_continuous];
        const int pos2 = ptab[PARSE_INDEX_discontinuous];
        if ((pos1 > 0) && (stko[pos1].objType == PPLOBJ_STR)) continuity = DATAFILE_CONTINUOUS;
        if ((pos2 > 0) && (stko[pos2].objType == PPLOBJ_STR)) continuity = DATAFILE_DISCONTINUOUS;
    }

    // Read label expression
    {
        const int pos = ptab[PARSE_INDEX_label];
        if (pos > 0) labelExpr = (pplExpr *) stko[pos].auxil;
    }

    // Read every item list
    {
        int Nevery = 0;
        int pos = ptab[PARSE_INDEX_every_list] + stkbase;
        const int o = ptab[PARSE_INDEX_every_item];
        if (o > 0)
            // Read each item of the every modifier
            while (stk[pos].objType == PPLOBJ_NUM) {
                long x;
                pos = (int) round(stk[pos].real);
                if (pos <= 0) break;
                // There may be no more than six items
                if (Nevery >= 6) {
                    ppl_warning(&c->errcontext, ERR_SYNTAX,
                                "More than six numbers supplied to the every modifier -- trailing entries ignored.");
                    break;
                }
                x = (long) round(stk[pos + o].real);
                if (x > everyList[Nevery]) everyList[Nevery] = x;
                Nevery++;
            }
    }

    // Print every list to log file
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Every %ld:%ld:%ld:%ld:%ld:%ld", everyList[0], everyList[1], everyList[2],
                          everyList[3], everyList[4], everyList[5]);
        ppl_log(&c->errcontext, msg);
    }

    // Read using rows or columns
    {
        const int pos1 = ptab[PARSE_INDEX_use_columns];
        const int pos2 = ptab[PARSE_INDEX_use_rows];
        if ((pos1 > 0) && (stko[pos1].objType == PPLOBJ_STR)) usingRowCol = DATAFILE_COL;
        if ((pos2 > 0) && (stko[pos2].objType == PPLOBJ_STR)) usingRowCol = DATAFILE_ROW;
    }

    // Read list of using items into an array
    {
        int hadNonNullUsingItem = 0;
        int pos = ptab[PARSE_INDEX_using_list] + stkbase;
        const int o = ptab[PARSE_INDEX_using_item];

        // Walk the linked list of using items on the parser stack
        if (o > 0)
            // Each using item
            while (stk[pos].objType == PPLOBJ_NUM) {
                pos = (int) round(stk[pos].real);
                if (pos <= 0) break;
                // There is a fixed limit on the number of using items
                if (Nusing >= USING_ITEMS_MAX) {
                    *status = 1;
                    ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "Too many using items; maximum of %d are allowed.",
                                      USING_ITEMS_MAX);
                    if (DEBUG) ppl_log(&c->errcontext, errtext);
                    return;
                }
                // Each item is an expression, or blank for the default
                if (stk[pos + o].objType == PPLOBJ_EXP) {
                    usingExprs[Nusing] = (pplExpr *) stk[pos + o].auxil;
                    hadNonNullUsingItem = 1;
                } else usingExprs[Nusing] = NULL;
                Nusing++;
            }
        if ((!hadNonNullUsingItem) && (Nusing == 1))
            Nusing = 0; // If we've only had one using item, and it was blank, this is a parser abberation
        if ((Nusing == 0) && (Ncols >= 0)) // If no using items were specified, generate an auto-generated list
        {
            autoUsingList = 1;
            if (ppldata_autoUsingList(c, usingExprs, Ncols, errtext)) return;
            Nusing = Ncols;
        } else if ((Nusing == 1) && (Ncols == 2)) // Need two columns; one supplied
        {
            int end = 0, ep = 0, es = 0;
            char ascii[16];
            pplExpr *exptmp = NULL;
            usingExprs[1] = usingExprs[0];
            ppl_snprintfTrunc(ascii, sizeof(ascii), "%d", 0);
            ppl_expCompile(c, pl->srcLineN, pl->srcId, pl->srcFname, ascii, &end, 0, 0, 0, &exptmp, &ep, &es, errtext);
            // Report a failure to compile the default using expression
            if (es || c->errStat.status) {
                ppl_tbClear(c);
                *status = 1;
                ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "Out of memory (11).");
                if (DEBUG) ppl_log(&c->errcontext, errtext);
                return;
            }
            usingExprs[0] = pplExpr_tmpcpy(exptmp);
            pplExpr_free(exptmp);
            exptmp = NULL;
            // Failure to allocate memory is reported as an error
            if (usingExprs[0] == NULL) {
                *status = 1;
                ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "Out of memory (12).");
                if (DEBUG) ppl_log(&c->errcontext, errtext);
                return;
            }
            Nusing = 2;
        }
    }

    // Print using items to log file
    if (DEBUG) {
        int logi = 0, logj;
        ppl_snprintfTrunc(msg + logi, sizeof(msg) - (logi), "Using ");
        logi += strlen(msg + logi);
        // Log each using item
        for (logj = 0; logj < Nusing; logj++) {
            if (logj == 0) msg[logi++] = ':';
            ppl_snprintfTrunc(msg + logi, sizeof(msg) - (logi), "%s", usingExprs[logj]->ascii);
        }
        ppl_log(&c->errcontext, msg);
    }

    // Check that number of using items matches number required
    if ((Ncols < 1) && (Nusing > 0))
        Ncols = Nusing; // If number of columns is specified as -1, any number of columns will do
    // The number of using items must match the number of columns needed
    if ((Ncols >= 0) && (Nusing != Ncols)) {
        *status = 1;
        ppl_snprintfTrunc(
                errtext, PPL_MSG_LENGTH,
                "The supplied using ... clause contains the wrong number of items. We need %d columns of data, but %d have been supplied.",
                Ncols, Nusing);
        if (DEBUG) ppl_log(&c->errcontext, errtext);
        return;
    }

    // Test for expression list or filename
    {
        const int pos1 = ptab[PARSE_INDEX_expression_list] + stkbase;
        const int pos2 = ptab[PARSE_INDEX_filename] + stkbase;
        if ((pos1 > 0) && (stk[pos1].objType == PPLOBJ_NUM)) // we have been passed a list of expressions
        {
            const int stkLevelOld = c->stackPtr;
            int pos = pos1;
            int Nexprs = 0, i;
            pplExpr **exprList;
            pplObj *first;
            while (stk[pos].objType == PPLOBJ_NUM) // count number of expressions
            {
                pos = (int) round(stk[pos].real);
                if (pos <= 0) break;
                Nexprs++;
            }
            // There must be at least one expression
            if (Nexprs < 1) {
                *status = 1;
                ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "Fewer than one expression was supplied to evaluate.");
                if (DEBUG) ppl_log(&c->errcontext, errtext);
                return;
            }
            exprList = (pplExpr **) ppl_memAlloc(Nexprs * sizeof(pplExpr *));
            // Failure to allocate memory is reported as an error
            if (exprList == NULL) {
                *status = 1;
                ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "Out of memory (13).");
                if (DEBUG) ppl_log(&c->errcontext, errtext);
                return;
            }
            // Collect the expressions from the linked list on the parser stack
            for (i = 0, pos = pos1; stk[pos].objType == PPLOBJ_NUM; i++) {
                pos = (int) round(stk[pos].real);
                if (pos <= 0) break;
                exprList[i] = (pplExpr *) stk[pos + ptab[PARSE_INDEX_expression]].auxil;
            }

            // A single expression which evaluates to a string is a filename; expressions which evaluate to vectors
            // supply vectors of data; others are functions to sample
            first = ppl_expEval(c, exprList[0], &i, 0, iterDepth);
            if ((!c->errStat.status) && (first->objType == PPLOBJ_STR) && (Nexprs == 1) &&
                (rasterY == NULL)) // If we have a single expression that evaluates to a string, it's a filename
            {
                char *datafile = (char *) first->auxil;
                ppldata_fromFile(c, out, datafile, wildcardMatchNumber, filenameOut, dataSpool, indexNo, usingExprs,
                                 autoUsingList, Ncols, NusingObjs, labelExpr, selectExpr, sortBy, usingRowCol,
                                 everyList, continuity, persistent, status, errtext, errCount, iterDepth);
                ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack
            } else if ((!c->errStat.status) && (first->objType == PPLOBJ_VEC) && (rasterY != NULL)) {
                const int stkLevelOld = c->stackPtr;
                pplObj *vecs;
                gsl_vector *v = ((pplVector *) first->auxil)->v;
                const int l = v->size;
                int j;
                vecs = (pplObj *) ppl_memAlloc(Nexprs * sizeof(pplObj));
                // Failure to allocate memory is reported as an error
                if (vecs == NULL) {
                    *status = 1;
                    ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "Out of memory (14).");
                    if (DEBUG) ppl_log(&c->errcontext, errtext);
                    return;
                }
                // Evaluate each expression, which must be a vector of the same length as the first
                for (i = 0; i < Nexprs; i++) {
                    int l2;
                    pplObj *obj = ppl_expEval(c, exprList[i], &j, 0, iterDepth);
                    // Report a failure to evaluate the expression
                    if (c->errStat.status) {
                        *status = 1;
                        ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "Could not evaluate vector expressions.");
                        for (j = 0; j < i; j++) ppl_garbageObject(vecs + j);
                        ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack before giving up
                        if (DEBUG) ppl_log(&c->errcontext, errtext);
                        return;
                    }
                    // The expression must evaluate to a vector
                    if (obj->objType != PPLOBJ_VEC) {
                        *status = 1;
                        ppl_snprintfTrunc(
                                errtext, PPL_MSG_LENGTH,
                                "Vector data supplied to other columns, but columns %d evaluated to an object of type <%s>.",
                                i + 1, pplObjTypeNames[obj->objType]);
                        for (j = 0; j < i; j++) ppl_garbageObject(vecs + j);
                        ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack before giving up
                        if (DEBUG) ppl_log(&c->errcontext, errtext);
                        return;
                    }
                    l2 = ((pplVector *) first->auxil)->v->size;
                    // All the vectors must have the same length
                    if (l != l2) {
                        *status = 1;
                        ppl_snprintfTrunc(
                                errtext, PPL_MSG_LENGTH,
                                "Data supplied as a list of vectors, but they have varying lengths, including %d (vector %d) and %d (vector %d).",
                                l, 1, l2, i + 1);
                        for (j = 0; j < i; j++) ppl_garbageObject(vecs + j);
                        ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack before giving up
                        if (DEBUG) ppl_log(&c->errcontext, errtext);
                        return;
                    }
                    pplObjCpy(vecs + i, obj, 0, 0, 1);
                    vecs[i].refCount = 1;
                    ppl_stackClean(c, stkLevelOld); // Discard the expression's value, now that it has been copied
                }
                if (wildcardMatchNumber <= 0)
                    ppldata_fromVectors(c, out, vecs, Nexprs, usingExprs, autoUsingList, Ncols, NusingObjs, labelExpr,
                                        selectExpr, sortBy, continuity, status, errtext, errCount, iterDepth);
                for (i = 0; i < Nexprs; i++) ppl_garbageObject(vecs + i);
            } else {
                double *rX = rasterX;
                double *rY = rasterY;
                int rXl = rasterXlen;
                const int rYl = rasterYlen;
                pplObj *uX = &unitX;
                pplObj *uY = &unitY;
                ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack
                ppl_tbClear(c);

                // See if this set of functions want a special raster
                if ((!sampleGrid) && (!spacingSet)) {
                    ppldata_fromFuncs_checkSpecialRaster(c, exprList, Nexprs, parametric ? "t" : "x",
                                                         (parametric || XminSet) ? &rasterXmin : NULL,
                                                         (parametric || XmaxSet) ? &rasterXmax : NULL, uX, &rX, &rXl);
                }

                // Sample the functions on the first pass only; later passes look for further files matching wildcards
                if (wildcardMatchNumber <= 0)
                    ppldata_fromFuncs(c, out, exprList, Nexprs, rX, rXl, parametric, uX, rY, rYl, uY, usingExprs,
                                      autoUsingList, Ncols, NusingObjs, labelExpr, selectExpr, sortBy, continuity,
                                      status, errtext, errCount, iterDepth);
            }
        } else if ((pos2 > 0) && (stk[pos2].objType == PPLOBJ_STR)) // we have been passed a filename
        {
            char *filename = (char *) stk[pos2].auxil;
            ppldata_fromFile(c, out, filename, wildcardMatchNumber, filenameOut, dataSpool, indexNo, usingExprs,
                             autoUsingList, Ncols, NusingObjs, labelExpr, selectExpr, sortBy, usingRowCol, everyList,
                             continuity, persistent, status, errtext, errCount, iterDepth);
        } else {
            *status = 1;
            ppl_snprintfTrunc(errtext, PPL_MSG_LENGTH, "Could not find any expressions to evaluate.");
            if (DEBUG) ppl_log(&c->errcontext, errtext);
            return;
        }
    }

    // Done
    return;
}
