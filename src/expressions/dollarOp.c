// dollarOp.c
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

#define _DOLLAROP_C 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>

#include <gsl/gsl_math.h>

#include "expressions/dollarOp.h"
#include "expressions/expEval.h"
#include "expressions/traceback_fns.h"
#include "settings/settingTypes.h"
#include "stringTools/asciidouble.h"
#include "userspace/context.h"
#include "userspace/pplObj_fns.h"
#include "userspace/unitsArithmetic.h"

#include "datafile.h"
#include "pplConstants.h"


//! ppl_dollarOp_config - Make a row of data from a data file available to the $ operator, which expressions in
//! "using" modifiers use to refer to columns of data
//! \param [in] c - The Pyxplot context
//! \param [in] columns_str - Array of the textual contents of each column
//! \param [in] columns_val - Array of the numerical values of each column
//! \param [in] Ncols - The number of columns
//! \param [in] filename - The name of the data file, used in warning messages
//! \param [in] file_linenumber - The line number in the data file of the current row
//! \param [in] file_linenumbers - Array of the line numbers in the data file from which each column was read, when
//! reading data in rows rather than columns
//! \param [in] linenumber_count - The number of the current data point within the current block
//! \param [in] block_count - The number of the current block of data within the current index
//! \param [in] index_number - The number of the current index within the data file
//! \param [in] usingRowCol - DATAFILE_COL if data is being read in columns, or DATAFILE_ROW if in rows
//! \param [in] usingExpr - The text of the "using" expression being evaluated, used in warning messages
//! \param [in] colHeads - Array of the names of each column, taken from the file's header
//! \param [in] NcolHeads - The number of entries in <colHeads>
//! \param [in] colUnits - Array of the physical units of each column, taken from the file's header
//! \param [in] NcolUnits - The number of entries in <colUnits>

void ppl_dollarOp_config(ppl_context *c, char **columns_str, pplObj *columns_val, const int Ncols, char *filename,
                         const long file_linenumber, long *file_linenumbers, const long linenumber_count,
                         const long block_count, const long index_number, const int usingRowCol, char *usingExpr,
                         char **colHeads, const int NcolHeads, pplObj *colUnits, const int NcolUnits) {
    c->dollarStat.columns_str = columns_str;
    c->dollarStat.columns_val = columns_val;
    c->dollarStat.Ncols = Ncols;
    c->dollarStat.filename = filename;
    c->dollarStat.file_linenumber = file_linenumber;
    c->dollarStat.file_linenumbers = file_linenumbers;
    c->dollarStat.linenumber_count = linenumber_count;
    c->dollarStat.block_count = block_count;
    c->dollarStat.index_number = index_number;
    c->dollarStat.usingRowCol = usingRowCol;
    c->dollarStat.usingExpr = usingExpr;
    c->dollarStat.colHeads = colHeads;
    c->dollarStat.NcolHeads = NcolHeads;
    c->dollarStat.colUnits = colUnits;
    c->dollarStat.NcolUnits = NcolUnits;
    c->dollarStat.warntxt[0] = '\0';
    return;
}

//! ppl_dollarOp_deconfig - Clear the row of data made available to the $ operator by ppl_dollarOp_config()
//! \param [in] c - The Pyxplot context

void ppl_dollarOp_deconfig(ppl_context *c) {
    c->dollarStat.columns_str = NULL;
    c->dollarStat.columns_val = NULL;
    c->dollarStat.Ncols = -1;
    c->dollarStat.filename = NULL;
    c->dollarStat.file_linenumber = -1;
    c->dollarStat.file_linenumbers = NULL;
    c->dollarStat.linenumber_count = -1;
    c->dollarStat.block_count = -1;
    c->dollarStat.index_number = -1;
    c->dollarStat.usingRowCol = -1;
    c->dollarStat.usingExpr = NULL;
    c->dollarStat.colHeads = NULL;
    c->dollarStat.NcolHeads = -1;
    c->dollarStat.colUnits = NULL;
    c->dollarStat.NcolUnits = -1;
    c->dollarStat.warntxt[0] = '\0';
    return;
}

//! ppl_dollarOp_fetchColByNum - Evaluate $N, placing the value of column N of the current row of data onto the top
//! of the stack. If the column does not exist, a warning is stored and NaN is returned.
//! \param [in] c - The Pyxplot context
//! \param [in] inExpr - The expression being evaluated
//! \param [in] inExprCharPos - The position of the $ operator within the expression
//! \param [in] colNum - The number of the column to fetch. Column 0 is the data point number within the current
//! block, -1 the block number, -2 the index number, and -3 the line number in the file.

void ppl_dollarOp_fetchColByNum(ppl_context *c, const pplExpr *inExpr, const int inExprCharPos, int colNum) {
    pplObj *out = &c->stack[c->stackPtr - 1];
    pplObjNum(out, 0, 0, 0);
    out->refCount = 1;

    if ((colNum < -3) || (colNum > c->dollarStat.Ncols)) {
        ppl_snprintfTrunc(c->dollarStat.warntxt, sizeof(c->dollarStat.warntxt),
                          "%s:%ld: In the expression <%s>, the requested %s number %d does not exist %son line %ld.",
                          c->dollarStat.filename, c->dollarStat.file_linenumber, c->dollarStat.usingExpr,
                          (c->dollarStat.usingRowCol == DATAFILE_COL) ? "column" : "row", colNum,
                          (c->dollarStat.usingRowCol == DATAFILE_COL) ? "" : "in the block commencing ",
                          c->dollarStat.file_linenumber);
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "No %s with number %d.",
                          (c->dollarStat.usingRowCol == DATAFILE_COL) ? "column" : "row", colNum);
        ppl_tbAddExpr(c, inExpr, ERR_RANGE, inExprCharPos, "");
        return;
    }

    if (colNum == -3) out->real = c->dollarStat.file_linenumber;
    else if (colNum == -2) out->real = c->dollarStat.index_number;
    else if (colNum == -1) out->real = c->dollarStat.block_count;
    else if (c->dollarStat.columns_val != NULL) pplObjCpy(out, &c->dollarStat.columns_val[colNum], 0, 0, 1);
    else if (colNum == 0) out->real = c->dollarStat.linenumber_count;
    else {
        char *s = c->dollarStat.columns_str[colNum - 1];
        int j = -1;
        if ((!ppl_validFloat(s, &j)) || (j < 1) ||
            ((s[j] != '\0') && (s[j] != ',') && ((unsigned char) s[j - 1] > ' '))) {
            char *str;
            int j = 0;
            while (((unsigned char) s[j] > ' ') && (s[j] != ',')) j++;
            str = (char *) malloc(j + 1);
            if (str == NULL) return;
            strncpy(str, s, j);
            str[j] = '\0';
            pplObjStr(out, 0, 1, str);
        } else {
            if ((colNum > 0) && (colNum <= c->dollarStat.NcolUnits)) {
                ppl_unitsDimCpy(out, &c->dollarStat.colUnits[colNum - 1]);
                out->real = ppl_getFloat(s, &j) * c->dollarStat.colUnits[colNum - 1].real;
            } else {
                out->real = ppl_getFloat(s, &j);
            }
        }
    }
    return;
}

//! ppl_dollarOp_fetchColByName - Evaluate $name, placing the value of the column with a given name onto the top of
//! the stack
//! \param [in] c - The Pyxplot context
//! \param [in] inExpr - The expression being evaluated
//! \param [in] inExprCharPos - The position of the $ operator within the expression
//! \param [in] colName - The name of the column to fetch, as given in the file's header

void ppl_dollarOp_fetchColByName(ppl_context *c, pplExpr *inExpr, const int inExprCharPos, char *colName) {
    int i;
    for (i = 0; i < c->dollarStat.NcolHeads; i++)
        if (strcmp(colName, c->dollarStat.colHeads[i]) == 0) {
            ppl_dollarOp_fetchColByNum(c, inExpr, inExprCharPos, i);
            return;
        }
    ppl_snprintfTrunc(c->dollarStat.warntxt, sizeof(c->dollarStat.warntxt),
                      "%s:%ld: In the expression <%s>, the requested %s named '%s' does not exist %son line %ld.",
                      c->dollarStat.filename, c->dollarStat.file_linenumber, c->dollarStat.usingExpr,
                      (c->dollarStat.usingRowCol == DATAFILE_COL) ? "column" : "row", colName,
                      (c->dollarStat.usingRowCol == DATAFILE_COL) ? "" : "in the block commencing ",
                      c->dollarStat.file_linenumber);
    ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "No %s with name '%s'.",
                      (c->dollarStat.usingRowCol == DATAFILE_COL) ? "column" : "row", colName);
    ppl_tbAddExpr(c, inExpr, ERR_DICTKEY, inExprCharPos, "");
    return;
}
