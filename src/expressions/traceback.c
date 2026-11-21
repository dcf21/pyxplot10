// traceback.c
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

// Records errors as they propagate up through nested expressions, function calls, subroutines and commands, building a
// traceback, and prints it with arrows pointing to where in each line the error occurred. Errors in Pyxplot are not
// reported where they occur: the code which detects them writes a message into the context and adds a traceback level,
// and each caller adds its own level as the error unwinds, until the top level prints it.

#include <stdlib.h>
#include <string.h>

#include "expressions/traceback_fns.h"
#include "coreUtils/errorReport.h"
#include "stringTools/asciidouble.h"
#include "stringTools/strConstants.h"
#include "userspace/context.h"

//! ppl_tbClear - Clear any error which has been recorded, together with its traceback
//! \param [in] c - The Pyxplot context

void ppl_tbClear(ppl_context *c) {
    // Free the copy of the line of input stored at each level of the traceback
    for (int i = c->errStat.tracebackDepth - 1; i >= 0; i--) {
        traceback *t = &c->errStat.tbLevel[i];
        // Free the copy of the line
        free(t->linetext);
        t->linetext = NULL;
    }
    c->errStat.status = c->errStat.tracebackDepth = 0;
    c->errStat.errType = -1;
    if (c->errStat.errPosCmd != -1) c->errStat.oldErrPosCmd = c->errStat.errPosCmd;
    if (c->errStat.errPosExpr != -1) c->errStat.oldErrPosExpr = c->errStat.errPosExpr;
    c->errStat.errPosCmd = c->errStat.errPosExpr = -1;
    c->errStat.errBuff[0] = '\0';
    c->errStat.sourceIdExpr = c->errStat.sourceIdCmd = -1;
    return;
}

//! ppl_tbAdd - Record an error, or add a level to the traceback of an error which has already been recorded. The
//! error message is taken from <c->errStat.errBuff>.
//! \param [in] c - The Pyxplot context
//! \param [in] srcLineN - The line number of the source of the command or expression where the error occurred
//! \param [in] srcId - The ID of the source of the command or expression
//! \param [in] srcFname - The filename of the source of the command or expression
//! \param [in] cmdOrExpr - One if the error occurred in a command, zero if it occurred in an expression
//! \param [in] errType - The type of error, one of the ERR_* constants
//! \param [in] errPos - The position of the error within <linetext>, or -1 if unknown
//! \param [in] linetext - The text of the command or expression where the error occurred
//! \param [in] context - A description of the context in which the error occurred, e.g. the name of the function
//! being called

void ppl_tbAdd(ppl_context *c, const int srcLineN, const long srcId, const char *srcFname, const int cmdOrExpr,
               const int errType, const int errPos, const char *linetext, const char *context) {
    const int i = c->errStat.tracebackDepth;

    // Add a new level to the traceback, unless this is a command which failed because of an error already recorded
    // within it
    if ((!cmdOrExpr) || (i == 0)) {
        traceback *t = &c->errStat.tbLevel[i];
        if ((i < 0) || (i >= TB_MAXLEVEL)) return;
        c->errStat.tracebackDepth++;
        t->errPos = errPos;
        t->errLine = srcLineN;
        t->sourceId = srcId;
        strncpy(t->source, srcFname, FNAME_LENGTH);
        t->amErrMsgExpr = t->amErrMsgCmd = 0;
        t->source[FNAME_LENGTH - 1] = '\0';

        // Record the context of the error, e.g. the name of the function being called
        if (context == NULL) {
            t->context[0] = '\0';
        } else {
            strncpy(t->context, context, FNAME_LENGTH);
            t->context[FNAME_LENGTH - 1] = '\0';
        }

        // Keep a copy of the line in which the error occurred
        if (linetext == NULL) {
            t->linetext = NULL;
        } else {
            t->linetext = malloc(strlen(linetext) + 1);
            if (t->linetext != NULL) strcpy(t->linetext, linetext);
        }

        // Flag that an error has occurred. The first error in an expression is the one reported, with its position.
        c->errStat.status = 1;
        if (c->errStat.errType < 0) c->errStat.errType = errType;
        // The first error in an expression is the one reported, with its position
        if ((!cmdOrExpr) && (c->errStat.sourceIdExpr < 0) && (strlen(c->errStat.errBuff) > 0)) {
            t->amErrMsgExpr = 1;
            c->errStat.sourceIdExpr = srcId;
            c->errStat.errPosExpr = errPos;
            ppl_snprintfTrunc(c->errStat.errMsgExpr, sizeof(c->errStat.errMsgExpr), "%s", c->errStat.errBuff);
        }
        if ((cmdOrExpr) && (c->errStat.sourceIdCmd < 0)) t->amErrMsgCmd = 1;
    }
    // The first error in a command, likewise
    if ((cmdOrExpr) && (c->errStat.sourceIdCmd < 0) && (strlen(c->errStat.errBuff) > 0)) {
        c->errStat.sourceIdCmd = srcId;
        c->errStat.errPosCmd = errPos;
        ppl_snprintfTrunc(c->errStat.errMsgCmd, sizeof(c->errStat.errMsgCmd), "%s", c->errStat.errBuff);
    }
    return;
}

//! ppl_tbAddLine - Add a level to the error traceback, for an error in a command
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command in which the error occurred
//! \param [in] errType - The type of error, one of the ERR_* constants
//! \param [in] errPos - The position of the error within the command, or -1 if unknown
//! \param [in] context - A description of the context in which the error occurred, or NULL

void ppl_tbAddLine(ppl_context *c, const parserLine *pl, const int errType, const int errPos, const char *context) {
    ppl_tbAdd(c, pl->srcLineN, pl->srcId, pl->srcFname, 0, errType, errPos, pl->linetxt, context);
}

//! ppl_tbAddExpr - Add a level to the error traceback, for an error in an algebraic expression
//! \param [in] c - The Pyxplot context
//! \param [in] expr - The compiled expression in which the error occurred
//! \param [in] errType - The type of error, one of the ERR_* constants
//! \param [in] errPos - The position of the error within the expression, or -1 if unknown
//! \param [in] context - A description of the context in which the error occurred, or NULL

void ppl_tbAddExpr(ppl_context *c, const pplExpr *expr, const int errType, const int errPos, const char *context) {
    ppl_tbAdd(c, expr->srcLineN, expr->srcId, expr->srcFname, 0, errType, errPos, expr->ascii, context);
}

//! ppl_tbWrite - Report the error which has been recorded to the user, together with its traceback
//! \param [in] c - The Pyxplot context

void ppl_tbWrite(ppl_context *c) {
    // h1 and h2 are the positions in the message of characters to highlight
    char msg[LSTR_LENGTH];
    int i = 0;
    char *out = msg;
    int outLen = sizeof(msg);
    int h1 = -1, h2 = -1;
    out[i] = out[outLen - 1] = '\0';
    outLen--;

    if (!c->errStat.status) return; // No error!

    // Start with a line naming the type of error
    snprintf(out + i, outLen - i, "\n");
    i += strlen(out + i);

    // Name the type of error
    switch (c->errStat.errType) {
        case ERR_INTERNAL:
            snprintf(out + i, outLen - i, "Internal error:");
            break;
        case ERR_SYNTAX:
            snprintf(out + i, outLen - i, "Syntax error:");
            break;
        case ERR_NUMERICAL:
            snprintf(out + i, outLen - i, "Numerical error:");
            break;
        case ERR_FILE:
            snprintf(out + i, outLen - i, "File error:");
            break;
        case ERR_RANGE:
            snprintf(out + i, outLen - i, "Range error:");
            break;
        case ERR_UNIT:
            snprintf(out + i, outLen - i, "Unit error:");
            break;
        case ERR_OVERFLOW:
            snprintf(out + i, outLen - i, "Overflow error:");
            break;
        case ERR_NAMESPACE:
            snprintf(out + i, outLen - i, "Namespace error:");
            break;
        case ERR_TYPE:
            snprintf(out + i, outLen - i, "Type error:");
            break;
        case ERR_INTERRUPT:
            snprintf(out + i, outLen - i, "Interrupt error:");
            break;
        case ERR_DICTKEY:
            snprintf(out + i, outLen - i, "Key error:");
            break;
        case ERR_ASSERT:
            snprintf(out + i, outLen - i, "Assertion error:");
            break;
        case ERR_MEMORY:
        case ERR_GENERIC:
        default:
            snprintf(out + i, outLen - i, "Error:");
            break;
    }
    i += strlen(out + i);
    snprintf(out + i, outLen - i, "\n");
    ppl_error(&c->errcontext, ERR_PREFORMED, -1, -1, msg);

    // Print each level of the traceback, from the outermost inwards: where the error occurred, and the line in which it
    // did
    for (int j = c->errStat.tracebackDepth - 1; j >= 0; j--) {
        i = 0;
        h1 = -1;
        traceback *t = &c->errStat.tbLevel[j];
        snprintf(out + i, outLen - i, "In %s", (t->source[0] == '\0') ? "keyboard entry" : t->source);
        i += strlen(out + i);
        // The line number, position and context, if known
        if (t->errLine >= 0) {
            snprintf(out + i, outLen - i, ", line %d", t->errLine);
            i += strlen(out + i);
        }
        // The position in the line
        if (t->errPos >= 0) {
            snprintf(out + i, outLen - i, ", position %d below", t->errPos);
            i += strlen(out + i);
        }
        // The context, e.g. the function being called
        if (t->context[0] != '\0') {
            snprintf(out + i, outLen - i, " (%s)", t->context);
            i += strlen(out + i);
        }
        // The line itself, for levels other than the innermost, with the position of the error highlighted
        if ((t->linetext != NULL) && (j > 0)) {
            snprintf(out + i, outLen - i, ":\n %s", t->linetext);
            h1 = i + 3 + t->errPos;
        } else snprintf(out + i, outLen - i, (j == 0) ? ":" : ".");
        ppl_error(&c->errcontext, ERR_PREFORMED, h1, -1, msg);
    }

    // Finally print the innermost line, with an arrow pointing up to the position of the error in the command and an
    // arrow pointing down to the position in the expression, followed by the message itself
    i = 1;
    strcpy(out, "\n");
    // If the innermost line is known, print it with arrows
    if ((c->errStat.tracebackDepth > 0) && (c->errStat.tbLevel[0].linetext != NULL)) {
        const int lp = c->errStat.errPosCmd;
        const int lp2 = c->errStat.errPosExpr;
        traceback *t = &c->errStat.tbLevel[0];
        // The message about the error in the command, and an arrow pointing up from it to its position in the line
        if ((t->sourceId == c->errStat.sourceIdCmd) && (lp >= 0)) {
            ppl_snprintfTrunc(out + i, outLen - i, "%s\n", c->errStat.errMsgCmd);
            i += strlen(out + i);
            for (int k = 0; ((k < lp) && (i < outLen - 8)); k++, i++) out[i] = ' ';
            snprintf(out + i, outLen - i, " |\n");
            i += strlen(out + i);
            for (int k = 0; ((k < lp) && (i < outLen - 8)); k++, i++) out[i] = ' ';
            snprintf(out + i, outLen - i, "\\|/\n");
            i += strlen(out + i);
            h1 = i + 1 + lp;
        }
        h2 = i + 1 + lp2;
        snprintf(out + i, outLen - i, " %s\n", t->linetext);
        i += strlen(out + i);
        // An arrow pointing to the position of the error in the expression
        if ((t->sourceId == c->errStat.sourceIdExpr) && (lp2 >= 0)) {
            for (int k = 0; ((k < lp2) && (i < outLen - 8)); k++, i++) out[i] = ' ';
            snprintf(out + i, outLen - i, "/|\\\n");
            i += strlen(out + i);
            for (int k = 0; ((k < lp2) && (i < outLen - 8)); k++, i++) out[i] = ' ';
            snprintf(out + i, outLen - i, " |\n");
            i += strlen(out + i);
            snprintf(out + i, outLen - i, "%s\n", c->errStat.errMsgExpr);
            i += strlen(out + i);
        } else {
            h2 = -1;
        }
    }
    ppl_error(&c->errcontext, ERR_PREFORMED, h1, h2, msg);
    return;
}
