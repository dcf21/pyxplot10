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
    int i;
    for (i = c->errStat.tracebackDepth - 1; i >= 0; i--) {
        traceback *t = &c->errStat.tbLevel[i];
        if (t->linetext != NULL) {
            free(t->linetext);
            t->linetext = NULL;
        }
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
    int i = c->errStat.tracebackDepth;

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

        if (context == NULL) {
            t->context[0] = '\0';
        } else {
            strncpy(t->context, context, FNAME_LENGTH);
            t->context[FNAME_LENGTH - 1] = '\0';
        }

        if (linetext == NULL) {
            t->linetext = NULL;
        } else {
            t->linetext = malloc(strlen(linetext) + 1);
            if (t->linetext != NULL) strcpy(t->linetext, linetext);
        }

        c->errStat.status = 1;
        if (c->errStat.errType < 0) c->errStat.errType = errType;
        if ((!cmdOrExpr) && (c->errStat.sourceIdExpr < 0) && (strlen(c->errStat.errBuff) > 0)) {
            t->amErrMsgExpr = 1;
            c->errStat.sourceIdExpr = srcId;
            c->errStat.errPosExpr = errPos;
            ppl_snprintfTrunc(c->errStat.errMsgExpr, sizeof(c->errStat.errMsgExpr), "%s", c->errStat.errBuff);
        }
        if ((cmdOrExpr) && (c->errStat.sourceIdCmd < 0)) t->amErrMsgCmd = 1;
    }
    if ((cmdOrExpr) && (c->errStat.sourceIdCmd < 0) && (strlen(c->errStat.errBuff) > 0)) {
        c->errStat.sourceIdCmd = srcId;
        c->errStat.errPosCmd = errPos;
        ppl_snprintfTrunc(c->errStat.errMsgCmd, sizeof(c->errStat.errMsgCmd), "%s", c->errStat.errBuff);
    }
    return;
}

//! ppl_tbWrite - Report the error which has been recorded to the user, together with its traceback
//! \param [in] c - The Pyxplot context

void ppl_tbWrite(ppl_context *c) {
    char msg[LSTR_LENGTH];
    int i = 0, j, k;
    char *out = msg;
    int outLen = sizeof(msg);
    int h1 = -1, h2 = -1;
    out[i] = out[outLen - 1] = '\0';
    outLen--;

    if (!c->errStat.status) return; // No error!

    snprintf(out + i, outLen - i, "\n");
    i += strlen(out + i);

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

    for (j = c->errStat.tracebackDepth - 1; j >= 0; j--) {
        i = 0;
        h1 = -1;
        traceback *t = &c->errStat.tbLevel[j];
        snprintf(out + i, outLen - i, "In %s", (t->source[0] == '\0') ? "keyboard entry" : t->source);
        i += strlen(out + i);
        if (t->errLine >= 0) {
            snprintf(out + i, outLen - i, ", line %d", t->errLine);
            i += strlen(out + i);
        }
        if (t->errPos >= 0) {
            snprintf(out + i, outLen - i, ", position %d below", t->errPos);
            i += strlen(out + i);
        }
        if (t->context[0] != '\0') {
            snprintf(out + i, outLen - i, " (%s)", t->context);
            i += strlen(out + i);
        }
        if ((t->linetext != NULL) && (j > 0)) {
            snprintf(out + i, outLen - i, ":\n %s", t->linetext);
            h1 = i + 3 + t->errPos;
        } else snprintf(out + i, outLen - i, (j == 0) ? ":" : ".");
        ppl_error(&c->errcontext, ERR_PREFORMED, h1, -1, msg);
    }

    i = 1;
    strcpy(out, "\n");
    if ((c->errStat.tracebackDepth > 0) && (c->errStat.tbLevel[0].linetext != NULL)) {
        const int lp = c->errStat.errPosCmd;
        const int lp2 = c->errStat.errPosExpr;
        traceback *t = &c->errStat.tbLevel[0];
        if ((t->sourceId == c->errStat.sourceIdCmd) && (lp >= 0)) {
            ppl_snprintfTrunc(out + i, outLen - i, "%s\n", c->errStat.errMsgCmd);
            i += strlen(out + i);
            for (k = 0; ((k < lp) && (i < outLen - 8)); k++, i++) out[i] = ' ';
            snprintf(out + i, outLen - i, " |\n");
            i += strlen(out + i);
            for (k = 0; ((k < lp) && (i < outLen - 8)); k++, i++) out[i] = ' ';
            snprintf(out + i, outLen - i, "\\|/\n");
            i += strlen(out + i);
            h1 = i + 1 + lp;
        }
        h2 = i + 1 + lp2;
        snprintf(out + i, outLen - i, " %s\n", t->linetext);
        i += strlen(out + i);
        if ((t->sourceId == c->errStat.sourceIdExpr) && (lp2 >= 0)) {
            for (k = 0; ((k < lp2) && (i < outLen - 8)); k++, i++) out[i] = ' ';
            snprintf(out + i, outLen - i, "/|\\\n");
            i += strlen(out + i);
            for (k = 0; ((k < lp2) && (i < outLen - 8)); k++, i++) out[i] = ' ';
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
