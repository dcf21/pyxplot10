// flowctrl.c
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

// Implements Pyxplot's flow-control commands: the do, for, foreach, foreach datum and while loops, if statements, with
// blocks (which run commands inside a module's namespace), subroutine definitions, and break, continue and return. Loop
// bodies are compiled blocks of commands, executed with ppl_parserExecute(); break, continue and return set flags in
// the context, which each enclosing loop checks after running its body. Loops may be named, so that break and continue
// can be aimed at an outer loop.

#define _FLOWCTRL_C 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <glob.h>
#include <wordexp.h>

#include <gsl/gsl_math.h>
#include <gsl/gsl_vector.h>

#include "commands/flowctrl.h"
#include "userspace/stack.h"

#include "coreUtils/errorReport.h"

#include "expressions/expEval.h"
#include "expressions/traceback_fns.h"

#include "settings/settingTypes.h"
#include "settings/textConstants.h"

#include "stringTools/asciidouble.h"
#include "stringTools/strConstants.h"

#include "parser/cmdList.h"
#include "parser/parser.h"

#include "expressions/expEval.h"
#include "expressions/traceback_fns.h"

#include "userspace/context.h"
#include "userspace/contextVarDef.h"
#include "userspace/garbageCollector.h"
#include "userspace/pplObj_fns.h"
#include "userspace/pplObjFunc_fns.h"
#include "userspace/unitsArithmetic.h"
#include "userspace/unitsDisp.h"

#include "datafile.h"
#include "pplConstants.h"


//! ppl_directive_break - Implement the break command, which exits from the innermost loop, or from a named loop
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppl_directive_break(ppl_context *c, const parserLine *pl, const parserOutput *in, const int interactive,
                         const int iterDepth) {
    pplObj *stk = in->stk;
    const int named = (stk[PARSE_break_loopname].objType == PPLOBJ_STR);
    char *name = (char *) stk[PARSE_break_loopname].auxil;

    // If a loop name was given, check that we are inside a loop with that name, and aim the break at it
    if (named) {
        int i;
        for (i = iterDepth; i >= 0; i--)
            if ((c->shellLoopName[i] != NULL) && (strcmp(c->shellLoopName[i], name) == 0)) break;
        // No enclosing loop has the name given
        if (i < 0) {
            snprintf(c->errStat.errBuff, LSTR_LENGTH, "Not inside any loop with the name '%s'.", name);
            ppl_tbAddLine(c, pl, ERR_SYNTAX, in->stkCharPos[PARSE_break_loopname], "");
            return;
        }
        c->shellBreakLevel = i;
    } else {
        // Otherwise, we must be inside a loop
        if (!c->shellBreakable) {
            snprintf(c->errStat.errBuff, LSTR_LENGTH,
                     "The break statement can only be placed inside a loop structure.");
            ppl_tbAddLine(c, pl, ERR_SYNTAX, 0, "");
            return;
        }
        c->shellBreakLevel = -1;
    }

    // Flag that execution should break out of the loop
    c->shellBroken = 1;
    return;
}

//! ppl_directive_continue - Implement the continue command, which skips to the next iteration of the innermost
//! loop, or of a named loop
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppl_directive_continue(ppl_context *c, const parserLine *pl, const parserOutput *in, const int interactive,
                            const int iterDepth) {
    pplObj *stk = in->stk;
    const int named = (stk[PARSE_continue_loopname].objType == PPLOBJ_STR);
    char *name = (char *) stk[PARSE_continue_loopname].auxil;

    // If a loop name was given, check that we are inside a loop with that name, and aim the continue at it
    if (named) {
        int i;
        for (i = iterDepth; i >= 0; i--)
            if ((c->shellLoopName[i] != NULL) && (strcmp(c->shellLoopName[i], name) == 0)) break;
        // No enclosing loop has the name given
        if (i < 0) {
            snprintf(c->errStat.errBuff, LSTR_LENGTH, "Not inside any loop with the name '%s'.", name);
            ppl_tbAddLine(c, pl, ERR_SYNTAX, in->stkCharPos[PARSE_continue_loopname], "");
            return;
        }
        c->shellBreakLevel = i;
    } else {
        // Otherwise, we must be inside a loop
        if (!c->shellBreakable) {
            snprintf(c->errStat.errBuff, LSTR_LENGTH,
                     "The continue statement can only be placed inside a loop structure.");
            ppl_tbAddLine(c, pl, ERR_SYNTAX, 0, "");
            return;
        }
        c->shellBreakLevel = -1;
    }

    // Flag that execution should skip to the next iteration of the loop
    c->shellContinued = 1;
    return;
}

//! ppl_directive_return - Implement the return command, which returns a value from a subroutine
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppl_directive_return(ppl_context *c, const parserLine *pl, const parserOutput *in, const int interactive,
                          const int iterDepth) {
    pplObj *stk = in->stk;
    pplObj *retval = &stk[PARSE_return_return_value];
    const int gotval = (retval->objType != PPLOBJ_ZOM);

    // We must be inside a subroutine
    if (!c->shellReturnable) {
        snprintf(c->errStat.errBuff, LSTR_LENGTH, "The return statement can only be placed inside subroutines.");
        ppl_tbAddLine(c, pl, ERR_SYNTAX, 0, "");
        return;
    }

    // Flag that execution should return from the subroutine, breaking out of any loops within it
    c->shellBreakLevel = -1;
    c->shellReturned = 1;

    // Store the return value, if any
    if (gotval) pplObjCpy(&c->shellReturnVal, retval, 0, 0, 1);
    else pplObjNum(&c->shellReturnVal, 0, 0, 0);
    return;
}

//! ppl_directive_do - Implement the do ... while loop
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppl_directive_do(ppl_context *c, const parserLine *pl, const parserOutput *in, const int interactive,
                      const int iterDepth) {
    int criterion = 1;
    pplObj *stk = in->stk;
    parserLine *plc = (parserLine *) stk[PARSE_do_code].auxil;
    pplExpr *cond = (pplExpr *) stk[PARSE_do_condition].auxil;
    char *name = (char *) stk[PARSE_do_loopname].auxil;
    const int named = (stk[PARSE_do_loopname].objType == PPLOBJ_STR);
    const int shellBreakableOld = c->shellBreakable;
    const int stkLevelOld = c->stackPtr;

    // Record the loop's name, if any, for named break and continue statements, and allow break and continue inside it
    if (named) c->shellLoopName[iterDepth] = name;
    else c->shellLoopName[iterDepth] = NULL;
    c->shellBreakable = 1;

    // Run the body of the loop, then evaluate the condition, repeating while it is true
    do {
        int lastOpAssign, dollarAllowed = 1;
        pplObj *val;
        ppl_parserExecute(c, plc, NULL, interactive, iterDepth + 1);
        // Add this statement to the traceback of any error
        if (c->errStat.status) {
            strcpy(c->errStat.errBuff, "");
            ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "do loop");
            goto cleanup;
        }
        ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack
        // A continue statement aimed at this loop, or at the innermost loop, ends this iteration
        if ((c->shellContinued) && ((c->shellBreakLevel == iterDepth) || (c->shellBreakLevel < 0))) {
            c->shellContinued = 0;
            c->shellBreakLevel = 0;
            continue;
        }
        if ((c->shellBroken) || (c->shellContinued) || (c->shellReturned) || (c->shellExiting)) break;
        val = ppl_expEval(c, cond, &lastOpAssign, dollarAllowed, iterDepth + 1);
        // Add this statement to the traceback of any error
        if (c->errStat.status) {
            strcpy(c->errStat.errBuff, "");
            ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "do loop stopping criterion");
            goto cleanup;
        }
        ppl_castToBool(c, val);
        criterion = (val->real != 0);
        ppl_stackClean(c, stkLevelOld); // Discard the condition's value, now that it has been tested
    } while (criterion && !cancellationFlag);

// Tidy up when the loop finishes
cleanup:
    // A break statement aimed at this loop, or at the innermost loop, has now taken effect, so clear it
    if ((c->shellBroken) && ((c->shellBreakLevel == iterDepth) || (c->shellBreakLevel < 0))) {
        c->shellBroken = 0;
        c->shellBreakLevel = 0;
    }
    c->shellLoopName[iterDepth] = NULL;
    c->shellBreakable = shellBreakableOld;
    return;
}

//! ppl_directive_for - Implement the for loop, in both its BASIC-like (for i=0 to 10) and C-like
//! (for (i=0;i<10;i++)) forms
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppl_directive_for(ppl_context *c, const parserLine *pl, const parserOutput *in, const int interactive,
                       const int iterDepth) {
    pplObj *stk = in->stk;
    const int BASICstyle = (stk[PARSE_for_var_name].objType == PPLOBJ_STR);
    char *varname = (char *) stk[PARSE_for_var_name].auxil;
    pplObj *beginVal = &stk[PARSE_for_start_value];
    pplObj *endVal = &stk[PARSE_for_final_value];
    pplObj *stepVal = &stk[PARSE_for_step_size], stepTmp;
    const int stepGot = (stk[PARSE_for_step_size].objType == PPLOBJ_NUM);
    parserLine *plc = (parserLine *) stk[PARSE_for_code].auxil;
    pplExpr *begin = (pplExpr *) stk[PARSE_for_begin].auxil;
    const int beginGot = (stk[PARSE_for_begin].objType == PPLOBJ_EXP);
    pplExpr *cond = (pplExpr *) stk[PARSE_for_criterion].auxil;
    const int condGot = (stk[PARSE_for_criterion].objType == PPLOBJ_EXP);
    pplExpr *end = (pplExpr *) stk[PARSE_for_iterate].auxil;
    const int endGot = (stk[PARSE_for_iterate].objType == PPLOBJ_EXP);
    char *name = (char *) stk[PARSE_for_loopname].auxil;
    const int named = (stk[PARSE_for_loopname].objType == PPLOBJ_STR);
    const int shellBreakableOld = c->shellBreakable;
    const int stkLevelOld = c->stackPtr;

    // Record the loop's name, if any, for named break and continue statements, and allow break and continue inside it
    if (named) c->shellLoopName[iterDepth] = name;
    else c->shellLoopName[iterDepth] = NULL;
    c->shellBreakable = 1;

    // BASIC-style loops step a numeric variable from one value to another; C-style loops evaluate an initialiser, a
    // condition and an increment
    if (BASICstyle) {
        double iter;
        const int forwards = stepGot ? (stepVal->real > 0) : (endVal->real > beginVal->real);

        if (!stepGot) // If no step is specified, make it equal to one SI unit
        {
            memcpy(&stepTmp, beginVal, sizeof(pplObj));
            stepTmp.refCount = 1;
            stepTmp.amMalloced = 0;
            stepTmp.real = forwards ? 1 : -1;
            stepVal = &stepTmp;
        }
        // The start and end values must have the same units
        if (!ppl_unitsDimEqual(beginVal, endVal)) {
            ppl_snprintfTrunc(
                    c->errStat.errBuff, sizeof(c->errStat.errBuff),
                    "The start and end values of this for loop are dimensionally incompatible: the start value has dimensions of <%s> but the end value has dimensions of <%s>.",
                    ppl_printUnit(c, beginVal, NULL, NULL, 0, 1, 0), ppl_printUnit(c, endVal, NULL, NULL, 1, 1, 0));
            ppl_tbAddLine(c, pl, ERR_NUMERICAL, in->stkCharPos[PARSE_for_final_value], "");
            goto cleanup;
        }
        // The start value and step must have the same units
        if (!ppl_unitsDimEqual(beginVal, stepVal)) {
            ppl_snprintfTrunc(
                    c->errStat.errBuff, sizeof(c->errStat.errBuff),
                    "The start value and step size of this for loop are dimensionally incompatible: the start value has dimensions of <%s> but the step size has dimensions of <%s>.",
                    ppl_printUnit(c, beginVal, NULL, NULL, 0, 1, 0), ppl_printUnit(c, stepVal, NULL, NULL, 1, 1, 0));
            ppl_tbAddLine(c, pl, ERR_NUMERICAL, in->stkCharPos[PARSE_for_start_value], "");
            goto cleanup;
        }
        // The step must move from the start value towards the end value
        if ((stepVal->real == 0) || ((endVal->real != beginVal->real) && ((stepVal->real > 0) != forwards))) {
            strcpy(c->errStat.errBuff, "The number of iterations in this for loop is infinite.");
            ppl_tbAddLine(c, pl, ERR_NUMERICAL, in->stkCharPos[PARSE_for_start_value], "");
            goto cleanup;
        }

        // Run the body of the loop for each value of the variable, unless the user presses control-C
        for (iter = beginVal->real; (forwards ? (iter <= endVal->real) : (iter >= endVal->real)) && !cancellationFlag;
             iter += stepVal->real) {
            pplObj *varObj, vartmp;
            ppl_contextGetVarPointer(c, varname, &varObj, &vartmp);
            pplObjNum(varObj, varObj->amMalloced, iter, 0);
            ppl_unitsDimCpy(varObj, beginVal);
            ppl_parserExecute(c, plc, NULL, interactive, iterDepth + 1);
            vartmp.amMalloced = 0;
            ppl_garbageObject(&vartmp);
            // Add this statement to the traceback of any error
            if (c->errStat.status) {
                strcpy(c->errStat.errBuff, "");
                ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "for loop");
                goto cleanup;
            }
            ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack
            // A continue statement aimed at this loop, or at the innermost loop, ends this iteration
            if ((c->shellContinued) && ((c->shellBreakLevel == iterDepth) || (c->shellBreakLevel < 0))) {
                c->shellContinued = 0;
                c->shellBreakLevel = 0;
                continue;
            }
            if ((c->shellBroken) || (c->shellContinued) || (c->shellReturned) || (c->shellExiting)) break;
        }
    } else {
        // Evaluate the initialiser, if any
        if (beginGot) {
            int lastOpAssign, dollarAllowed = 1;
            ppl_expEval(c, begin, &lastOpAssign, dollarAllowed, iterDepth + 1);
            // Add this statement to the traceback of any error
            if (c->errStat.status) {
                strcpy(c->errStat.errBuff, "");
                ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "for loop initialisation expression");
                goto cleanup;
            }
            ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack
        }
        // Repeat until the condition becomes false
        while (1) {
            // Evaluate the condition, if any, and stop when it becomes false
            if (condGot) {
                int lastOpAssign, dollarAllowed = 1, criterion = 1;
                pplObj *val;
                val = ppl_expEval(c, cond, &lastOpAssign, dollarAllowed, iterDepth + 1);
                // Add this statement to the traceback of any error
                if (c->errStat.status) {
                    strcpy(c->errStat.errBuff, "");
                    ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "for loop stopping criterion");
                    goto cleanup;
                }
                ppl_castToBool(c, val);
                criterion = (val->real != 0);
                ppl_stackClean(c, stkLevelOld); // Discard the condition's value, now that it has been tested
                if (!criterion || cancellationFlag) break;
            }
            ppl_parserExecute(c, plc, NULL, interactive, iterDepth + 1);
            // Add this statement to the traceback of any error
            if (c->errStat.status) {
                strcpy(c->errStat.errBuff, "");
                ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "for loop");
                goto cleanup;
            }
            ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack
            // A continue statement aimed at this loop, or at the innermost loop, ends this iteration
            if ((c->shellContinued) && ((c->shellBreakLevel == iterDepth) || (c->shellBreakLevel < 0))) {
                c->shellContinued = 0;
                c->shellBreakLevel = 0;
            }
            if ((c->shellBroken) || (c->shellContinued) || (c->shellReturned) || (c->shellExiting)) break;
            // Evaluate the increment, if any
            if (endGot) {
                int lastOpAssign, dollarAllowed = 1;
                ppl_expEval(c, end, &lastOpAssign, dollarAllowed, iterDepth + 1);
                // Add this statement to the traceback of any error
                if (c->errStat.status) {
                    strcpy(c->errStat.errBuff, "");
                    ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "for loop stopping criterion");
                    goto cleanup;
                }
                ppl_stackClean(c, stkLevelOld); // Discard the condition's value, now that it has been tested
            }
        }
    }

// Tidy up when the loop finishes
cleanup:
    // A break statement aimed at this loop, or at the innermost loop, has now taken effect, so clear it
    if ((c->shellBroken) && ((c->shellBreakLevel == iterDepth) || (c->shellBreakLevel < 0))) {
        c->shellBroken = 0;
        c->shellBreakLevel = 0;
    }
    c->shellLoopName[iterDepth] = NULL;
    c->shellBreakable = shellBreakableOld;
    return;
}

//! foreachStop - Test whether a foreach loop should stop iterating, because an error occurred, or a break, continue,
//! return or exit statement was executed, or the user pressed Ctrl-C
//! \param [in] c - The Pyxplot context
//! \return True if the loop should stop

static int foreachStop(const ppl_context *c) {
    return c->errStat.status || c->shellBroken || c->shellContinued || c->shellReturned || c->shellExiting ||
            cancellationFlag;
}

//! ppl_directive_foreach - Implement the foreach loop, which iterates over the items in a list, vector or
//! dictionary, or over files matching a wildcard
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppl_directive_foreach(ppl_context *c, const parserLine *pl, const parserOutput *in, const int interactive,
                           const int iterDepth) {
    pplObj *stk = in->stk;
    pplObj *iter = &stk[PARSE_foreach_item_list];
    parserLine *plc = (parserLine *) stk[PARSE_foreach_code].auxil;
    char *varname = (char *) stk[PARSE_foreach_var_name].auxil;
    char *name = (char *) stk[PARSE_foreach_loopname].auxil;
    const int named = (stk[PARSE_foreach_loopname].objType == PPLOBJ_STR);
    const int shellBreakableOld = c->shellBreakable;
    const int stkLevelOld = c->stackPtr;

    // Record the loop's name, if any, for named break and continue statements, and allow break and continue inside it
    if (named) c->shellLoopName[iterDepth] = name;
    else c->shellLoopName[iterDepth] = NULL;
    c->shellBreakable = 1;

    if (iter->objType == PPLOBJ_STR) // Iterate over string: treat this as a filename to glob
    {
        int i, j;
        wordexp_t w;
        glob_t g;
        char *raw = (char *) iter->auxil, fName[FNAME_LENGTH];
        {
            int j, k;
            // Escape spaces in the filename pattern before passing it to wordexp
            for (j = k = 0; ((raw[j] != '\0') && (k < FNAME_LENGTH - 1));) {
                if (raw[j] == ' ') fName[k++] = '\\';
                fName[k++] = raw[j++];
            }
            fName[k++] = '\0';
        }
        if (wordexp(fName, &w, 0) != 0) goto cleanup; // No matches
        // Run the loop once for each file matching each word of the expanded pattern
        for (i = 0; i < w.we_wordc && !foreachStop(c); i++) {
            if (glob(w.we_wordv[i], 0, NULL, &g) != 0) continue;
            // Run the body of the loop once for each matching file, with the variable set to its filename
            for (j = 0; j < g.gl_pathc && !foreachStop(c); j++) {
                pplObj *varObj, vartmp;
                ppl_contextGetVarPointer(c, varname, &varObj, &vartmp);
                pplObjStr(varObj, varObj->amMalloced, 0, g.gl_pathv[j]);
                ppl_parserExecute(c, plc, NULL, interactive, iterDepth + 1);
                ppl_contextRestoreVarPointer(c, varname, &vartmp);
                // Add this statement to the traceback of any error
                if (c->errStat.status) {
                    strcpy(c->errStat.errBuff, "");
                    ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "foreach loop");
                }
                ppl_stackClean(c, stkLevelOld); // Discard the expression's value, now that it has been copied
                // A continue statement aimed at this loop, or at the innermost loop, ends this iteration
                if ((c->shellContinued) && ((c->shellBreakLevel == iterDepth) || (c->shellBreakLevel < 0))) {
                    c->shellContinued = 0;
                    c->shellBreakLevel = 0;
                    continue;
                }
            }
            globfree(&g);
        }
        wordfree(&w);
    } else if (iter->objType == PPLOBJ_VEC) // Iterate over vector
    {
        gsl_vector *vin = ((pplVector *) iter->auxil)->v;
        const int vinl = vin->size;
        int i;
        // Run the body of the loop once for each element of the vector
        for (i = 0; i < vinl && !foreachStop(c); i++) {
            pplObj *varObj, vartmp;
            ppl_contextGetVarPointer(c, varname, &varObj, &vartmp);
            pplObjNum(varObj, varObj->amMalloced, gsl_vector_get(vin, i), 0);
            ppl_unitsDimCpy(varObj, iter);
            ppl_parserExecute(c, plc, NULL, interactive, iterDepth + 1);
            ppl_contextRestoreVarPointer(c, varname, &vartmp);
            // Add this statement to the traceback of any error
            if (c->errStat.status) {
                strcpy(c->errStat.errBuff, "");
                ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "foreach loop");
            }
            ppl_stackClean(c, stkLevelOld); // Discard the expression's value, now that it has been copied
            // A continue statement aimed at this loop, or at the innermost loop, ends this iteration
            if ((c->shellContinued) && ((c->shellBreakLevel == iterDepth) || (c->shellBreakLevel < 0))) {
                c->shellContinued = 0;
                c->shellBreakLevel = 0;
                continue;
            }
            if ((c->shellBroken) || (c->shellContinued) || (c->shellReturned) || (c->shellExiting)) break;
        }
    } else if (iter->objType == PPLOBJ_LIST) // Iterate over list
    {
        list *lin = (list *) iter->auxil;
        listIterator *li = ppl_listIterateInit(lin);
        pplObj *obj;
        // Run the body of the loop once for each item of the list
        while (((obj = (pplObj *) ppl_listIterate(&li)) != NULL) && !foreachStop(c)) {
            pplObj *varObj, vartmp;
            ppl_contextGetVarPointer(c, varname, &varObj, &vartmp);
            pplObjCpy(varObj, obj, 0, varObj->amMalloced, 1);
            ppl_parserExecute(c, plc, NULL, interactive, iterDepth + 1);
            ppl_contextRestoreVarPointer(c, varname, &vartmp);
            // Add this statement to the traceback of any error
            if (c->errStat.status) {
                strcpy(c->errStat.errBuff, "");
                ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "foreach loop");
            }
            ppl_stackClean(c, stkLevelOld); // Discard the expression's value, now that it has been copied
            // A continue statement aimed at this loop, or at the innermost loop, ends this iteration
            if ((c->shellContinued) && ((c->shellBreakLevel == iterDepth) || (c->shellBreakLevel < 0))) {
                c->shellContinued = 0;
                c->shellBreakLevel = 0;
                continue;
            }
            if ((c->shellBroken) || (c->shellContinued) || (c->shellReturned) || (c->shellExiting)) break;
        }
    } else if ((iter->objType == PPLOBJ_DICT) || (iter->objType == PPLOBJ_MOD) ||
               (iter->objType == PPLOBJ_USER)) // Iterate over dictionary
    {
        dict *din = (dict *) iter->auxil;
        dictIterator *di = ppl_dictIterateInit(din);
        pplObj *obj;
        char *key, *cpy;
        // Run the body of the loop once for each key of the dictionary
        while (((obj = (pplObj *) ppl_dictIterate(&di, &key)) != NULL) && !foreachStop(c)) {
            pplObj *varObj, vartmp;
            ppl_contextGetVarPointer(c, varname, &varObj, &vartmp);
            cpy = (char *) malloc(strlen(key) + 1);
            // Stop if memory cannot be allocated for the key
            if (cpy == NULL) {
                ppl_contextRestoreVarPointer(c, varname, &vartmp);
                break;
            }
            strcpy(cpy, key);
            pplObjStr(varObj, varObj->amMalloced, 1, cpy);
            ppl_parserExecute(c, plc, NULL, interactive, iterDepth + 1);
            ppl_contextRestoreVarPointer(c, varname, &vartmp);
            // Add this statement to the traceback of any error
            if (c->errStat.status) {
                strcpy(c->errStat.errBuff, "");
                ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "foreach loop");
            }
            ppl_stackClean(c, stkLevelOld); // Discard the expression's value, now that it has been copied
            // A continue statement aimed at this loop, or at the innermost loop, ends this iteration
            if ((c->shellContinued) && ((c->shellBreakLevel == iterDepth) || (c->shellBreakLevel < 0))) {
                c->shellContinued = 0;
                c->shellBreakLevel = 0;
                continue;
            }
            if ((c->shellBroken) || (c->shellContinued) || (c->shellReturned) || (c->shellExiting)) break;
        }
    } else {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "Cannot iterate over an object of type <%s>.",
                          pplObjTypeNames[iter->objType]);
        ppl_tbAddLine(c, pl, ERR_TYPE, in->stkCharPos[PARSE_foreach_item_list], "");
        goto cleanup;
    }

// Tidy up when the loop finishes
cleanup:
    // A break statement aimed at this loop, or at the innermost loop, has now taken effect, so clear it
    if ((c->shellBroken) && ((c->shellBreakLevel == iterDepth) || (c->shellBreakLevel < 0))) {
        c->shellBroken = 0;
        c->shellBreakLevel = 0;
    }
    c->shellLoopName[iterDepth] = NULL;
    c->shellBreakable = shellBreakableOld;
    return;
}

//! ppl_directive_fordata - Implement the "foreach datum" loop, which iterates over the rows of a data file
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppl_directive_fordata(ppl_context *c, parserLine *pl, parserOutput *in, const int interactive,
                           const int iterDepth) {
    char msg[PPL_MSG_LENGTH];
    pplObj *stk = in->stk;
    parserLine *plc = (parserLine *) stk[PARSE_foreachdatum_code].auxil;
    int Nvars = 0, i, status = 0, errCount = 5;
    char *varname[USING_ITEMS_MAX];
    int pos = PARSE_foreachdatum_variables;
    char *name = (char *) stk[PARSE_foreachdatum_loopname].auxil;
    const int named = (stk[PARSE_foreachdatum_loopname].objType == PPLOBJ_STR);
    const int shellBreakableOld = c->shellBreakable;
    pplObj *varObj[USING_ITEMS_MAX], vartmp[USING_ITEMS_MAX];
    pplObj unit[USING_ITEMS_MAX];
    int minSet[USING_ITEMS_MAX], maxSet[USING_ITEMS_MAX];
    double min[USING_ITEMS_MAX], max[USING_ITEMS_MAX];
    dataTable *data = NULL;
    dataBlock *blk = NULL;
    parserLine *spool = NULL, **dataSpool = &spool;

    // Record the loop's name, if any, for named break and continue statements, and allow break and continue inside it
    if (named) c->shellLoopName[iterDepth] = name;
    else c->shellLoopName[iterDepth] = NULL;
    c->shellBreakable = 1;

    // Read variable names
    while (stk[pos].objType == PPLOBJ_NUM) {
        pos = (int) round(stk[pos].real);
        if (pos <= 0) break;
        // There is a fixed limit on the number of variables
        if (Nvars >= USING_ITEMS_MAX) {
            ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                              "Too many variable names; a maximum of %d may be specified.", USING_ITEMS_MAX);
            ppl_tbAddLine(c, pl, ERR_SYNTAX, in->stkCharPos[pos + PARSE_foreachdatum_variable_variables], "");
            goto cleanup;
        }
        varname[Nvars] = (char *) stk[pos + PARSE_foreachdatum_variable_variables].auxil;
        Nvars++;
    }

    // Fetch ordinate ranges
    {
        int i, pos = PARSE_foreachdatum_0range_list, nr = 0;
        const int o1 = PARSE_foreachdatum_min_0range_list;
        const int o2 = PARSE_foreachdatum_max_0range_list;
        for (i = 0; i < Nvars; i++) minSet[i] = 0;
        for (i = 0; i < Nvars; i++) maxSet[i] = 0;
        // Walk the linked list of ranges on the parser stack; there may be one for each variable
        while (stk[pos].objType == PPLOBJ_NUM) {
            pos = (int) round(stk[pos].real);
            if (pos <= 0) break;
            // There may be no more ranges than variables
            if (nr >= Nvars) {
                ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                                  "Too many ranges specified; there are only %d loop variables.", Nvars);
                ppl_tbAddLine(c, pl, ERR_SYNTAX, 0, "");
                goto cleanup;
            }
            if ((stk[pos + o1].objType == PPLOBJ_NUM) || (stk[pos + o1].objType == PPLOBJ_DATE) ||
                (stk[pos + o1].objType == PPLOBJ_BOOL)) {
                unit[nr] = stk[pos + o1];
                min[nr] = unit[nr].real;
                minSet[nr] = 1;
            }
            if ((stk[pos + o2].objType == PPLOBJ_NUM) || (stk[pos + o2].objType == PPLOBJ_DATE) ||
                (stk[pos + o2].objType == PPLOBJ_BOOL)) {
                // The maximum must have the same type as the minimum
                if ((minSet[nr]) && (unit[nr].objType != stk[pos + o2].objType)) {
                    ppl_snprintfTrunc(
                            c->errStat.errBuff, sizeof(c->errStat.errBuff),
                            "Minimum and maximum limits specified for variable %d (%s) have conflicting types of <%s> and <%s>.",
                            nr + 1, varname[nr], pplObjTypeNames[unit[nr].objType],
                            pplObjTypeNames[stk[pos + o2].objType]);
                    ppl_tbAddLine(c, pl, ERR_TYPE, in->stkCharPos[pos + PARSE_foreachdatum_min_0range_list], "");
                    goto cleanup;
                }
                // The maximum must have the same units as the minimum
                if ((minSet[nr]) && (!ppl_unitsDimEqual(&unit[nr], &stk[pos + o2]))) {
                    ppl_snprintfTrunc(
                            c->errStat.errBuff, sizeof(c->errStat.errBuff),
                            "Minimum and maximum limits specified for variable %d (%s) have conflicting physical units of <%s> and <%s>.",
                            nr + 1, varname[nr], ppl_printUnit(c, &unit[nr], NULL, NULL, 0, 0, 0),
                            ppl_printUnit(c, &stk[pos + o2], NULL, NULL, 1, 0, 0));
                    ppl_tbAddLine(c, pl, ERR_UNIT, in->stkCharPos[pos + PARSE_foreachdatum_min_0range_list], "");
                    goto cleanup;
                }
                unit[nr] = stk[pos + o2];
                max[nr] = unit[nr].real;
                maxSet[nr] = 1;
            }
            nr++;
        }
    }

    // Fetch data
    ppldata_fromCmd(c, &data, pl, in, 0, NULL, dataSpool, PARSE_TABLE_foreachdatum_, 0, Nvars, Nvars, min, minSet, max,
                    maxSet, unit, 0, &status, msg, &errCount, iterDepth);
    // Report a failure to read the data
    if (status || cancellationFlag) {
        ppl_error(&c->errcontext, ERR_GENERIC, -1, -1, msg);
        goto cleanup;
    }

    // Loop over data table
    blk = data->first;
    // Loop over the blocks of the data table
    while ((blk != NULL) && !foreachStop(c)) {
        int j;
        // Run the body of the loop once for each row of data, with the variables set to its columns
        for (j = 0; ((j < blk->blockPosition) && !foreachStop(c)); j++) {
            const int stkLevelOld = c->stackPtr;
            int k, inRange = 1;

            // Set each variable to the value in its column, skipping rows with any value outside its range
            for (k = 0; ((k < Nvars) && inRange); k++) {
                pplObj *obj = &blk->data_obj[k + j * Nvars];
                const int numeric = obj->objType == unit[k].objType;
                int om, rc;

                // Fetch variable pointer
                ppl_contextGetVarPointer(c, varname[k], &varObj[k], &vartmp[k]);

                // Check units of ranges
                if ((minSet[k] || maxSet[k]) && inRange) {
                    // A non-numeric value cannot be compared with a numeric range
                    if (!numeric) {
                        // Report an error, or skip the row silently if errors are switched off
                        if (c->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
                            ppl_snprintfTrunc(
                                    msg, sizeof(msg),
                                    "Minimum and maximum limits are specified for variable %d (%s), but it computes to a non-numeric quantity of type <%s>.",
                                    k + 1, varname[k], pplObjTypeNames[obj->objType]);
                            ppl_warning(&c->errcontext, ERR_TYPE, msg);
                        }
                        inRange = 0;
                    } else if (!ppl_unitsDimEqual(&unit[k], obj)) {
                        // Report an error, or skip the row silently if errors are switched off
                        if (c->set->term_current.ExplicitErrors == SW_ONOFF_ON) {
                            ppl_snprintfTrunc(
                                    msg, sizeof(msg),
                                    "The minimum and maximum limits specified in the 'foreach datum ...' command for variable %d (%s) have conflicting physical dimensions with the data returned from the data file. The limits have units of <%s>, whilst the data have units of <%s>.",
                                    k + 1, varname[k], ppl_printUnit(c, &unit[k], NULL, NULL, 0, 0, 0),
                                    ppl_printUnit(c, obj, NULL, NULL, 1, 0, 0));
                            ppl_warning(&c->errcontext, ERR_UNIT, msg);
                        }
                        inRange = 0;
                    }
                    if (minSet[k] && (obj->real < min[k])) inRange = 0;
                    if (maxSet[k] && (obj->real > max[k])) inRange = 0;
                }
                if ((obj->objType == PPLOBJ_STR) && (obj->auxil == NULL)) { inRange = 0; } // NULL strings are bad
                om = varObj[k]->amMalloced;
                rc = varObj[k]->refCount;
                varObj[k]->amMalloced = 0;
                ppl_garbageObject(varObj[k]);
                pplObjCpy(varObj[k], obj, 0, 0, 1);
                varObj[k]->amMalloced = om;
                varObj[k]->refCount = rc;
                ;
            }
            if (inRange) { ppl_parserExecute(c, plc, NULL, interactive, iterDepth + 1); }
            // Restore variable pointers
            for (i = 0; i < Nvars; i++) ppl_contextRestoreVarPointer(c, varname[i], &vartmp[i]);
            // Add this statement to the traceback of any error
            if (c->errStat.status) {
                strcpy(c->errStat.errBuff, "");
                ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "foreach datum statement");
                return;
            }
            ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack
            // A continue statement aimed at this loop, or at the innermost loop, ends this iteration
            if ((c->shellContinued) && ((c->shellBreakLevel == iterDepth) || (c->shellBreakLevel < 0))) {
                c->shellContinued = 0;
                c->shellBreakLevel = 0;
                continue;
            }
            if ((c->shellBroken) || (c->shellContinued) || (c->shellReturned) || (c->shellExiting)) break;
        }
        blk = blk->next;
        if (status) break;
    }

    // Free data tables
    blk = data->first;
    // Free the objects held in the data table
    while (blk != NULL) {
        int j;
        // Free each object in each row
        for (j = 0; j < blk->blockPosition; j++) {
            int k;
            for (k = 0; k < Nvars; k++) ppl_garbageObject(&blk->data_obj[k + j * Nvars]);
        }
        blk = blk->next;
    }

// Tidy up when the loop finishes
cleanup:
    // A break statement aimed at this loop, or at the innermost loop, has now taken effect, so clear it
    if ((c->shellBroken) && ((c->shellBreakLevel == iterDepth) || (c->shellBreakLevel < 0))) {
        c->shellBroken = 0;
        c->shellBreakLevel = 0;
    }
    c->shellLoopName[iterDepth] = NULL;
    c->shellBreakable = shellBreakableOld;
    return;
}

//! ppl_directive_if - Implement the if ... else if ... else conditional
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppl_directive_if(ppl_context *c, const parserLine *pl, const parserOutput *in, const int interactive,
                      const int iterDepth) {
    pplObj *stk = in->stk;
    parserLine *pl_if = (parserLine *) stk[PARSE_if_code].auxil;
    parserLine *pl_else = (parserLine *) stk[PARSE_if_code_else].auxil;
    const int gotelse = (stk[PARSE_if_code_else].objType == PPLOBJ_BYT);
    pplExpr *cond = (pplExpr *) stk[PARSE_if_criterion].auxil;
    pplObj *val;
    int lastOpAssign, dollarAllowed = 1, criterion;
    int pos = PARSE_if_0elifs;
    const int stkLevelOld = c->stackPtr;

    // Test criterion
    val = ppl_expEval(c, cond, &lastOpAssign, dollarAllowed, iterDepth + 1);
    // Add this statement to the traceback of any error
    if (c->errStat.status) {
        strcpy(c->errStat.errBuff, "");
        ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "if criterion");
        return;
    }
    ppl_castToBool(c, val);
    criterion = (val->real != 0);
    ppl_stackClean(c, stkLevelOld); // Discard the condition's value, now that it has been tested
    // If the condition is true, run the if block, and stop
    if (criterion) {
        ppl_parserExecute(c, pl_if, NULL, interactive, iterDepth + 1);
        // Add this statement to the traceback of any error
        if (c->errStat.status) {
            strcpy(c->errStat.errBuff, "");
            ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "if statement");
            return;
        }
        ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack before giving up
        return;
    }

    // loop over else ifs
    while (stk[pos].objType == PPLOBJ_NUM) {
        parserLine *pl_elif = NULL;
        pplExpr *cond_elif = NULL;
        pplObj *val;
        int lastOpAssign, dollarAllowed = 1, criterion;
        pos = (int) round(stk[pos].real);
        pl_elif = (parserLine *) stk[pos + PARSE_if_code_elif_0elifs].auxil;
        cond_elif = (pplExpr *) stk[pos + PARSE_if_criterion_0elifs].auxil;
        if (pos <= 0) break;
        val = ppl_expEval(c, cond_elif, &lastOpAssign, dollarAllowed, iterDepth + 1);
        // Add this statement to the traceback of any error
        if (c->errStat.status) {
            strcpy(c->errStat.errBuff, "");
            ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "else if criterion");
            return;
        }
        ppl_castToBool(c, val);
        criterion = (val->real != 0);
        ppl_stackClean(c, stkLevelOld); // Discard the condition's value, now that it has been tested
        // If this condition is true, run its block, and stop
        if (criterion) {
            ppl_parserExecute(c, pl_elif, NULL, interactive, iterDepth + 1);
            // Add this statement to the traceback of any error
            if (c->errStat.status) {
                strcpy(c->errStat.errBuff, "");
                ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "if statement");
                return;
            }
            ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack before giving up
            return;
        }
    }

    // else
    if (gotelse) {
        ppl_parserExecute(c, pl_else, NULL, interactive, iterDepth + 1);
        // Add this statement to the traceback of any error
        if (c->errStat.status) {
            strcpy(c->errStat.errBuff, "");
            ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "if statement");
            return;
        }
        ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack before giving up
        return;
    }
    return;
}

//! ppl_directive_subrt - Implement the subroutine command, which defines a new subroutine
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppl_directive_subrt(ppl_context *c, const parserLine *pl, const parserOutput *in, const int interactive,
                         const int iterDepth) {
    pplObj *stk = in->stk;
    parserLine *plsub = (parserLine *) stk[PARSE_subroutine_code].auxil;
    int pos, nArgs = 0, argListLen = 0, i, om, rc;
    pplFunc *f = NULL;
    pplObj *tmpObj;

    // Count number of arguments
    pos = PARSE_subroutine_0argument_list;
    // Walk the linked list of argument names on the parser stack, counting them
    while ((stk[pos].objType == PPLOBJ_NUM) && (stk[pos].real > 0)) {
        char *name;
        pos = (int) round(stk[pos].real);
        name = (char *) stk[pos + PARSE_subroutine_argument_name_0argument_list].auxil;
        nArgs++;
        argListLen += strlen(name) + 1;
    }
    // Refuse to define subroutines with too many arguments
    if (nArgs > FUNC_MAXARGS) {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                          "Too many arguments to function; the maximum allowed number is %d.", FUNC_MAXARGS);
        ppl_tbAddLine(c, pl, ERR_OVERFLOW, 0, 0);
        return;
    }

    // Allocate function descriptor
    f = (pplFunc *) malloc(sizeof(pplFunc));
    // Failure to allocate memory is reported as an error
    if (f == NULL) {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "Out of memory.");
        ppl_tbAddLine(c, pl, ERR_MEMORY, 0, 0);
        return;
    }
    f->functionType = PPL_FUNC_SUBROUTINE;
    f->refCount = 1;
    f->minArgs = nArgs;
    f->maxArgs = nArgs;
    f->functionPtr = (void *) plsub;
    f->argList = (char *) malloc(argListLen);
    f->min = NULL;
    f->max = NULL;
    f->minActive = NULL;
    f->maxActive = NULL;
    f->numOnly = 0;
    f->notNan = 0;
    f->realOnly = 0;
    f->dimlessOnly = 0;
    f->next = NULL;
    f->LaTeX = NULL;
    f->description = NULL;
    f->descriptionShort = NULL;
    // Failure to allocate memory is reported as an error
    if (f->argList == NULL) {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "Out of memory.");
        ppl_tbAddLine(c, pl, ERR_MEMORY, 0, 0);
        goto fail;
    }

    // Put argument names into structure
    pos = PARSE_subroutine_0argument_list;
    i = 0;
    // Store the argument names one after another, each followed by a zero byte
    while ((stk[pos].objType == PPLOBJ_NUM) && (stk[pos].real > 0)) {
        char *name;
        pos = (int) round(stk[pos].real);
        name = (char *) stk[pos + PARSE_subroutine_argument_name_0argument_list].auxil;
        strcpy(f->argList + i, name);
        i += strlen(name) + 1;
    }

    // Look up function or variable we are superseding
    ppl_contextVarHierLookup(c, pl->srcLineN, pl->srcId, pl->srcFname, pl->linetxt, stk, in->stkCharPos, &tmpObj,
                             PARSE_subroutine_subroutine_names, PARSE_subroutine_subroutine_name_subroutine_names);
    if ((c->errStat.status) || (tmpObj == NULL)) goto fail;
    // A global declaration at the top level cannot be overwritten
    if (tmpObj->objType == PPLOBJ_GLOB) {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                          "Variable declared global in global namespace.");
        ppl_tbAddLine(c, pl, ERR_NAMESPACE, 0, 0);
        goto fail;
    }

    // Replace the variable's old value with the subroutine, keeping its reference count and storage
    om = tmpObj->amMalloced;
    rc = tmpObj->refCount;
    tmpObj->amMalloced = 0;
    tmpObj->refCount = 1;
    ppl_garbageObject(tmpObj);
    pplObjFunc(tmpObj, om, 1, f);
    tmpObj->refCount = rc;
    if (plsub != NULL) __sync_add_and_fetch(&plsub->refCount, 1);
    return;

// On error, free the partially built function descriptor
fail:
    // Free the argument names and the descriptor
    if (f != NULL) {
        // The argument names
        if (f->argList != NULL) {
            free(f->argList);
            f->argList = NULL;
        }
        free(f);
        f = NULL;
    }
    return;
}

//! ppl_directive_while - Implement the while loop
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppl_directive_while(ppl_context *c, const parserLine *pl, const parserOutput *in, const int interactive,
                         const int iterDepth) {
    int criterion = 1;
    pplObj *stk = in->stk;
    parserLine *plc = (parserLine *) stk[PARSE_while_code].auxil;
    pplExpr *cond = (pplExpr *) stk[PARSE_while_criterion].auxil;
    char *name = (char *) stk[PARSE_while_loopname].auxil;
    const int named = (stk[PARSE_while_loopname].objType == PPLOBJ_STR);
    const int shellBreakableOld = c->shellBreakable;
    const int stkLevelOld = c->stackPtr;

    // Record the loop's name, if any, for named break and continue statements, and allow break and continue inside it
    if (named) c->shellLoopName[iterDepth] = name;
    else c->shellLoopName[iterDepth] = NULL;
    c->shellBreakable = 1;

    // Evaluate the condition, then run the body of the loop, repeating while the condition is true, unless the user
    // presses control-C
    while (!cancellationFlag) {
        int lastOpAssign, dollarAllowed = 1;
        pplObj *val;
        val = ppl_expEval(c, cond, &lastOpAssign, dollarAllowed, iterDepth + 1);
        // Add this statement to the traceback of any error
        if (c->errStat.status) {
            strcpy(c->errStat.errBuff, "");
            ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "while loop stopping criterion");
            goto cleanup;
        }
        ppl_castToBool(c, val);
        criterion = (val->real != 0);
        ppl_stackClean(c, stkLevelOld); // Discard the condition's value, now that it has been tested
        if (!criterion) break;
        ppl_parserExecute(c, plc, NULL, interactive, iterDepth + 1);
        // Add this statement to the traceback of any error
        if (c->errStat.status) {
            strcpy(c->errStat.errBuff, "");
            ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "while loop");
            goto cleanup;
        }
        ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack
        // A continue statement aimed at this loop, or at the innermost loop, ends this iteration
        if ((c->shellContinued) && ((c->shellBreakLevel == iterDepth) || (c->shellBreakLevel < 0))) {
            c->shellContinued = 0;
            c->shellBreakLevel = 0;
            continue;
        }
        if ((c->shellBroken) || (c->shellContinued) || (c->shellReturned) || (c->shellExiting)) break;
    }

// Tidy up when the loop finishes
cleanup:
    // A break statement aimed at this loop, or at the innermost loop, has now taken effect, so clear it
    if ((c->shellBroken) && ((c->shellBreakLevel == iterDepth) || (c->shellBreakLevel < 0))) {
        c->shellBroken = 0;
        c->shellBreakLevel = 0;
    }
    c->shellLoopName[iterDepth] = NULL;
    c->shellBreakable = shellBreakableOld;
    return;
}

//! ppl_directive_with - Implement the with command, which executes a block of commands with the members of a
//! dictionary or module available as local variables
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppl_directive_with(ppl_context *c, const parserLine *pl, const parserOutput *in, const int interactive,
                        const int iterDepth) {
    pplObj *stk = in->stk;
    parserLine *pl2 = (parserLine *) stk[PARSE_with_code].auxil;
    pplObj *obj = &stk[PARSE_with_namespace];
    dict *d;
    const int stkLevelOld = c->stackPtr;

    // Check that there's enough space on the stack
    if (c->ns_ptr > CONTEXT_DEPTH - 2) {
        strcpy(c->errStat.errBuff, "Stack overflow.");
        ppl_tbAddLine(c, pl, ERR_MEMORY, 0, "");
        return;
    }

    // Check requested namespace
    if ((obj->objType != PPLOBJ_MOD) && (obj->objType != PPLOBJ_USER)) {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                          "Requested namespace is not a module; it has type <%s>.", pplObjTypeNames[obj->objType]);
        ppl_tbAddLine(c, pl, ERR_TYPE, 0, "");
        return;
    }
    d = (dict *) obj->auxil;

    // Enter a new namespace
    d->refCount++;
    c->namespaces[++c->ns_ptr] = d;

    // Run the block of commands
    ppl_parserExecute(c, pl2, NULL, interactive, iterDepth + 1);

    // Leave namespace
    if ((--d->refCount) < 1) {
        ppl_dictFree(d);
        d = NULL;
    }
    c->ns_ptr--;

    // Add this statement to the traceback of any error
    if (c->errStat.status) {
        strcpy(c->errStat.errBuff, "");
        ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "with block");
        return;
    }
    ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack before giving up
    return;
}
