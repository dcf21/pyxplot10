// parserExecute.c
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

// Executes commands which parserCompile.c has compiled. Each compiled command is a list of atoms, which are literals or
// expressions; executing it evaluates each atom, checks that it has the type which the command's grammar requires (e.g.
// a length, an angle or a color), and places it on an output stack. The output stack is then passed to parserShell.c,
// which runs the command.

#define _PARSEREXECUTE_C 1

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include <gsl/gsl_math.h>

#include "coreUtils/errorReport.h"
#include "userspace/stack.h"

#include "expressions/expEval.h"
#include "expressions/traceback_fns.h"

#include "settings/settingTypes.h"

#include "stringTools/asciidouble.h"
#include "stringTools/strConstants.h"

#include "parser/cmdList.h"
#include "parser/parser.h"

#include "userspace/context.h"
#include "userspace/garbageCollector.h"
#include "userspace/pplObj_fns.h"
#include "userspace/pplObjPrint.h"

#include "pplConstants.h"


//! ppl_parserLinePrint - Print a description of a compiled command line and its atoms, for debugging
//! \param [in] c - The Pyxplot context
//! \param [in] in - The compiled command line to print

void ppl_parserLinePrint(ppl_context *c, parserLine *in) {
    char msg[PPL_MSG_LENGTH];
    char *t = msg;
    parserAtom *a = in->firstAtom;

    // Describe the line as a whole
    ppl_snprintfTrunc(t, sizeof(msg), "Parser line -- stack length %d\nOriginal line was: %s", in->stackLen,
                      in->linetxt);
    ppl_report(&c->errcontext, msg);
    if (in->containsMacros) ppl_report(&c->errcontext, "Contains macros.");

    // Describe where each atom will be placed on the output stack
    for (a = in->firstAtom; a != NULL; a = a->next) {
        // A literal value, printed in full; or an expression, printed as typed
        if (a->literal != NULL) {
            int i = 0;
            ppl_strAppendf(t, sizeof(msg), &i, "Set %4d to literal of type %2d -- ", a->stackOutPos,
                           a->literal->objType);
            pplObjPrint(c, a->literal, NULL, t + i, sizeof(msg) - i, 0, 0);
            i += strlen(t + i);
            ppl_strAppendf(t, sizeof(msg), &i, ".");
        } else {
            ppl_snprintfTrunc(t, sizeof(msg), "Set %4d to expression -- %s.", a->stackOutPos, a->expr->ascii);
        }
        ppl_report(&c->errcontext, msg);
    }

    // End of the description
    ppl_report(&c->errcontext, "---------------");
    return;
}

//! ppl_parserOutFree - Free the output stack of an executed command, releasing references to the objects in it
//! \param [in] in - The parser output to free. The structure itself is not freed.

void ppl_parserOutFree(parserOutput *in) {
    int i;
    if (in == NULL) return;
    for (i = 0; i < in->stackLen; i++) ppl_garbageObject(&in->stk[i]);
    free(in->stk);
    in->stk = NULL;
    free(in->stkCharPos);
    in->stkCharPos = NULL;
    free(in);
    in = NULL;
    return;
}


//! ppl_parserExecute - Execute a compiled command line: evaluate its atoms to fill an output stack, and pass the
//! result to ppl_parserShell() to run the command
//! \param [in] c - The Pyxplot context
//! \param [in] in - The compiled command line to execute
//! \param [in] dirName - If not NULL, only commands of this type are allowed (used for sections of configuration
//! files, e.g. "set")
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppl_parserExecute(ppl_context *c, parserLine *in, const char *dirName, const int interactive,
                       const int iterDepth) {
    int i;
    const int stkLevelOld = c->stackPtr;
    parserOutput *out = NULL;
    pplObj *stk = NULL;
    char *eB = c->errStat.errBuff;
    int *stkCharPos = NULL;

    // Check that recursion depth has not been exceeded
    if (iterDepth > MAX_RECURSION_DEPTH) {
        strcpy(eB, "Maximum recursion depth exceeded.");
        ppl_tbAddLine(c, in, ERR_OVERFLOW, 0, NULL);
        return;
    }

    // If at bottom iteration depth, clean up stack now if there is any left-over junk
    if (iterDepth == 0) ppl_stackClean(c, 0);

    // Execute each compiled line in turn, stopping on an error, or if a break, continue, return or exit statement needs
    // to unwind the stack of loops and subroutines
    while ((in != NULL) && (!c->errStat.status) && (!c->shellExiting) && (!c->shellBroken) && (!c->shellContinued) &&
           (!c->shellReturned)) {
        parserAtom *item = in->firstAtom;

        //ppl_parserLinePrint(c,in);

        // If line contains macros, need to recompile it now
        if (in->containsMacros) {
            int stat = 0;
            parserStatus *ps = NULL;
            parserLine *pl = NULL;
            ppl_parserStatInit(&ps, &pl);
            // Failure to allocate memory is reported as an error
            if (ps == NULL) {
                strcpy(eB, "Out of memory.");
                ppl_tbAddLine(c, in, ERR_MEMORY, 0, NULL);
                return;
            }
            stat = ppl_parserCompile(c, ps, in->srcLineN, in->srcId, in->srcFname, in->linetxt, 1, iterDepth + 1);
            if (stat || c->errStat.status) break;
            ppl_parserExecute(c, ps->pl[iterDepth + 1], dirName, interactive, iterDepth + 1);
            ppl_parserLineFree(ps->pl[iterDepth + 1]);
            ppl_parserStatFree(&ps);
            in = in->next;
            continue;
        }

        // Initialise output stack
        out = (parserOutput *) malloc(sizeof(parserOutput));
        // Failure to allocate memory is reported as an error
        if (out == NULL) {
            strcpy(eB, "Out of memory.");
            ppl_tbAddLine(c, in, ERR_MEMORY, 0, NULL);
            return;
        }
        stk = (pplObj *) malloc(in->stackLen * sizeof(pplObj));
        // Failure to allocate memory is reported as an error
        if (stk == NULL) {
            free(out);
            out = NULL;
            strcpy(eB, "Out of memory.");
            ppl_tbAddLine(c, in, ERR_MEMORY, 0, NULL);
            return;
        }
        // Every slot on the output stack starts as a zombie, meaning that nothing was matched there
        for (i = 0; i < in->stackLen; i++) {
            stk[i].refCount = 1;
            pplObjZom(&stk[i], 0);
        }
        stkCharPos = (int *) malloc(in->stackLen * sizeof(int));
        // Failure to allocate memory is reported as an error. stkCharPos records where in the line each value came
        // from, for error messages.
        if (stkCharPos == NULL) {
            free(out);
            out = NULL;
            free(stk);
            stk = NULL;
            strcpy(eB, "Out of memory.");
            ppl_tbAddLine(c, in, ERR_MEMORY, 0, NULL);
            return;
        }
        for (i = 0; i < in->stackLen; i++) { stkCharPos[i] = -1; }
        out->stk = stk;
        out->stkCharPos = stkCharPos;
        out->stackLen = in->stackLen;

        // Loop over atoms
        while (item != NULL) {
            // Atoms are either literals, such as keywords matched in the command, or expressions to be evaluated now
            if (item->literal != NULL) {
                // Atom is a literal: simply copy it
                pplObjCpy(&stk[item->stackOutPos], item->literal, 0, 0, 1);
            } else {
                // Atom is an expression: evaluate it
                const int stkLevelOld = c->stackPtr;
                int lastOpAssign = 0, passed = 0, j = 0, k = 0, p = 0;
                pplObj *val = ppl_expEval(c, item->expr, &lastOpAssign, 1, iterDepth + 1);
                // The expression could not be evaluated
                if (c->errStat.status) {
                    if (item->options[1] ==
                        'E') // If expression has produced an error, and its second option is type E, store it for per-point evaluation
                    {
                        ppl_tbClear(c);
                        pplObjExpression(&stk[item->stackOutPos], 0, (void *) item->expr);
                        item->expr->refCount++;
                        goto gotTypeCexpression;
                    } else {
                        strcpy(c->errStat.errBuff, "");
                        ppl_tbAddLine(c, in, ERR_GENERIC, item->linePos, NULL);
                        break; // On error, stop
                    }
                }

                // Check type of pplObj produced by expression, and complain if the wrong type
                strcpy(eB + k, "Expression evaluates to the wrong type: needed");
                k += strlen(eB + k);
                // Check the value against each of the types which the grammar allows for this item, building up the
                // error message as we go, until one is accepted. The type letters are those of the %X codes in
                // buildScripts/parser_data.dat.
                for (j = 0; ((j < PARSER_TYPE_OPTIONS) && (item->options[j] != '\0') && (!passed)); j++) {
                    // Separate the alternatives in the error message
                    if (j != 0) {
                        strcpy(eB + k, ", or");
                        k += strlen(eB + k);
                    }
                    // Test whether the value is of this type, converting it into the form in which the command expects
                    // it
                    switch (item->options[j]) {
                        case 'a':
                            strcpy(eB + k, " an axis number");
                            k += strlen(eB + k);
                            if (val->objType != PPLOBJ_NUM) break;
                            if (val->flagComplex) break;
                            if (!val->dimensionless) break;
                            if ((val->real <= 0) || (val->real >= MAX_AXES)) break;
                            val->exponent[0] = (double) (item->options[++j] - '0');
                            passed = 1;
                            break;
                        case 'A':
                            strcpy(eB + k, " an angle");
                            k += strlen(eB + k);
                            if (val->objType != PPLOBJ_NUM) break;
                            if (val->flagComplex) break;
                            // Dimensionless angles are in degrees, and are converted to radians; otherwise the value
                            // must have dimensions of angle
                            if (val->dimensionless) {
                                val->real *= M_PI / 180;
                                passed = 1;
                                break;
                            }
                            for (p = 0; p < UNITS_MAX_BASEUNITS; p++)
                                // The exponent of every base unit must be zero, except angle, which must be one
                                if (val->exponent[p] != (p == UNIT_ANGLE)) {
                                    p = -1;
                                    break;
                                }
                            passed = (p > 0);
                            break;
                        case 'b':
                            ppl_castToBool(c, val);
                            passed = 1;
                            break;
                        case 'c':
                            strcpy(eB + k, " a color");
                            k += strlen(eB + k);
                            if ((val->objType != PPLOBJ_COL) &&
                                ((val->objType != PPLOBJ_NUM) || (!val->dimensionless) || (val->flagComplex) ||
                                 (val->real < INT_MIN) || (val->real > INT_MAX)))
                                break;
                            passed = 1;
                            break;
                        case 'd':
                            strcpy(eB + k, " an integer");
                            k += strlen(eB + k);
                            if (val->objType != PPLOBJ_NUM) break;
                            if (val->flagComplex) break;
                            if (!val->dimensionless) break;
                            if ((val->real < INT_MIN) || (val->real > INT_MAX)) break;
                            passed = 1;
                            break;
                        case 'D':
                            strcpy(eB + k, " a length");
                            k += strlen(eB + k);
                            if (val->objType != PPLOBJ_NUM) break;
                            if (val->flagComplex) break;
                            // Dimensionless lengths are in centimetres, and are converted to metres; otherwise the
                            // value must have dimensions of length
                            if (val->dimensionless) {
                                val->real *= 0.01;
                                passed = 1;
                                break;
                            }
                            for (p = 0; p < UNITS_MAX_BASEUNITS; p++)
                                // The exponent of every base unit must be zero, except length, which must be one
                                if (val->exponent[p] != (p == UNIT_LENGTH)) {
                                    p = -1;
                                    break;
                                }
                            passed = (p > 0);
                            break;
                        case 'E':
                        case 'F':
                            passed = 0;
                            break;
                        case 'f':
                            strcpy(eB + k, " a number");
                            k += strlen(eB + k);
                            if (val->objType != PPLOBJ_NUM) break;
                            if (val->flagComplex) break;
                            if (!val->dimensionless) break;
                            passed = 1;
                            break;
                        case 'o':
                            passed = 1;
                            break;
                        case 'p':
                        case 'P':
                            strcpy(eB + k, " a position vector");
                            k += strlen(eB + k);
                            // A position may be a vector of two components (or three for type P), which are unpacked
                            // into consecutive slots on the output stack
                            if (val->objType == PPLOBJ_VEC) {
                                double m = 1, x = 0, y = 0, z = 0;
                                gsl_vector *v = ((pplVector *) val->auxil)->v;
                                int l = v->size, rc;
                                if ((l < 2) || (l > ((item->options[j] == 'P') ? 3 : 2))) break; // wrong size
                                if (val->dimensionless) m = 0.01;
                                // Otherwise the vector must have dimensions of length
                                else {
                                    for (p = 0; p < UNITS_MAX_BASEUNITS; p++)
                                        // The exponent of every base unit must be zero, except length, which must be
                                        // one
                                        if (val->exponent[p] != (p == UNIT_LENGTH)) {
                                            p = -1;
                                            break;
                                        }
                                    if (p < 0) break; // vector has wrong units
                                }
                                if (l > 0) x = gsl_vector_get(v, 0) * m;
                                if (l > 1) y = gsl_vector_get(v, 1) * m;
                                if (l > 2) z = gsl_vector_get(v, 2) * m;
                                rc = val->refCount;
                                ppl_garbageObject(val);
                                val->refCount = rc;
                                pplObjNum(val, 0, x, 0);
                                pplObjNum(&stk[item->stackOutPos + 1], 0, y, 0);
                                if (item->options[j] == 'P') pplObjNum(&stk[item->stackOutPos + 2], 0, z, 0);
                                passed = 1;
                            } else if (val->objType == PPLOBJ_LIST) {
                                double x[3] = {0, 0, 0};
                                list *li = (list *) val->auxil;
                                int l = li->length, i, rc;
                                if ((l < 2) || (l > ((item->options[j] == 'P') ? 3 : 2))) break; // wrong size
                                // A position may also be a list of two (or three) numbers, which are unpacked into
                                // consecutive slots on the output stack
                                for (i = 0; i < l; i++) {
                                    double m = 1;
                                    pplObj *xo = ppl_listGetItem(li, i);
                                    if (xo->objType != PPLOBJ_NUM) break;
                                    if (xo->flagComplex) break;
                                    if (xo->dimensionless) m = 0.01;
                                    // Otherwise the coordinate must have dimensions of length
                                    else {
                                        for (p = 0; p < UNITS_MAX_BASEUNITS; p++)
                                            // Check the coordinate's units
                                            if (val->exponent[p] != UNIT_LENGTH) {
                                                p = -1;
                                                break;
                                            }
                                        if (p < 0) break; // vector has wrong units
                                    }
                                    x[i] = xo->real * m;
                                }
                                rc = val->refCount;
                                ppl_garbageObject(val);
                                val->refCount = rc;
                                pplObjNum(val, 0, x[0], 0);
                                pplObjNum(&stk[item->stackOutPos + 1], 0, x[1], 0);
                                if (item->options[j] == 'P') pplObjNum(&stk[item->stackOutPos + 2], 0, x[2], 0);
                                passed = 1;
                            }
                            break; // fail
                        case 'q':
                            strcpy(eB + k, " a string");
                            k += strlen(eB + k);
                            if (val->objType != PPLOBJ_STR) break;
                            passed = 1;
                            break;
                        case 'u':
                            strcpy(eB + k, " a physical quantity");
                            k += strlen(eB + k);
                            if (val->objType != PPLOBJ_NUM) break;
                            passed = 1;
                            break;
                        default:
                            ppl_fatal(&c->errcontext, __FILE__, __LINE__, "Illegal option encountered.");
                    }
                }

                // If expression has produced a value of the wrong type, throw an error
                if (!passed) {
                    strcpy(eB + k, ".");
                    ppl_tbAddLine(c, in, ERR_TYPE, item->linePos, NULL);
                    break;
                }

                // Copy value produced by atom
                pplObjCpy(&stk[item->stackOutPos], val, 0, 0, 1);
                ppl_stackClean(c, stkLevelOld); // Discard the expression's value, now that it has been copied
            }
        gotTypeCexpression:
            stkCharPos[item->stackOutPos] = item->linePos;
            item = item->next;
        }

        // If no error, pass command to shell
        if (!c->errStat.status) {
            char *cmd = (char *) stk[PARSE_arc_directive].auxil;
            if ((dirName != NULL) && (strcmp(dirName, cmd) != 0) &&
                ((strcmp(dirName, "set") != 0) || (strcmp(cmd, "set_error") != 0))) {
                sprintf(eB, "Command of type '%s' is not allowed in this section of a configuration file.", cmd);
                ppl_tbAddLine(c, in, ERR_SYNTAX, 0, NULL);
            } else if ((dirName != NULL) && (stk[PARSE_arc_editno].objType == PPLOBJ_NUM)) {
                sprintf(eB, "Settings may not be edited for canvas items within a configuration file.");
                ppl_tbAddLine(c, in, ERR_SYNTAX, 0, NULL);
            } else {
                ppl_parserShell(c, in, out, interactive, iterDepth);
            }
        }

        // Free output stack
        ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack
        ppl_parserOutFree(out);

        // Move on to the next line
        in = in->next;
    }
    return;
}
